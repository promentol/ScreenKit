// Copyright (c) ScreenKit contributors. MIT.
//
// Android's network client: OkHttp over JNI, through
// `runtime/android/app/src/main/java/dev/screenkit/net/HttpClient.java`.
//
// There is no protocol code here. OkHttp resolves names, pools connections,
// speaks HTTP/1.1 and HTTP/2, follows redirects, decodes `Content-Encoding`,
// verifies certificates against the platform trust store, and cookies live in
// `android.webkit.CookieManager` -- the client the OS and Google maintain, which
// is what React Native's `NetworkingModule` uses too. What this file does is
// translate: a `HttpRequestSpec` into a call on the Java object, and the Java
// callbacks back into `HttpSink` and `SocketSink`.
//
// Threading, and why the shape looks like NetServiceApple.mm: every call out of
// this file is made from the runtime's one serial I/O queue, and every callback
// in from a Java thread does nothing but copy its bytes and post onto that same
// queue. The sinks are therefore only ever touched there, in order, with no
// locks -- and a callback that crosses with a teardown finds its slot gone twice
// over: the registry lookup fails, and the posted job checks `stopped` again.
#include "NetServiceAndroid.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "IoQueue.h"
#include "NetService.h"

namespace screenkit::net {
namespace {

constexpr const char* kTag = "screenkit.net";
constexpr const char* kClientClass = "dev/screenkit/net/HttpClient";

JavaVM* gVm = nullptr;
jclass gClientClass = nullptr;      // global ref
jclass gStringClass = nullptr;      // global ref, "java/lang/String"
jclass gByteArrayClass = nullptr;   // global ref, "[B"
jmethodID gCtor = nullptr;
jmethodID gStartRequest = nullptr;
jmethodID gAppendBody = nullptr;
jmethodID gFinishBody = nullptr;
jmethodID gAbortRequest = nullptr;
jmethodID gAcknowledge = nullptr;
jmethodID gOpenSocket = nullptr;
jmethodID gSendSocket = nullptr;
jmethodID gCloseSocket = nullptr;
jmethodID gCookiesFor = nullptr;
jmethodID gSetCookie = nullptr;
jmethodID gShutdown = nullptr;
std::atomic<bool> gReady{false};
std::mutex gPrepareMutex;

/// Global references and call registrations outstanding (NetServiceAndroid.h).
std::atomic<std::size_t> gLiveRefs{0};

// ---- attaching to the VM ----------------------------------------------------------
//
// The I/O queue is a plain std::thread, so every call into Java from it needs a
// JNIEnv of its own. Attaching costs nothing after the first time, and the
// thread_local below detaches when the thread ends -- a thread that stayed
// attached would keep the VM from shutting down and leak its thread-local Java
// state.

struct Detacher {
  bool attached = false;
  ~Detacher() {
    if (attached && gVm != nullptr) gVm->DetachCurrentThread();
  }
};
thread_local Detacher tDetacher;

class ScopedEnv {
 public:
  ScopedEnv() {
    if (gVm == nullptr) return;
    void* raw = nullptr;
    if (gVm->GetEnv(&raw, JNI_VERSION_1_6) == JNI_OK) {
      env_ = static_cast<JNIEnv*>(raw);
      return;
    }
    if (gVm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
      env_ = nullptr;
      return;
    }
    tDetacher.attached = true;
  }

  explicit operator bool() const { return env_ != nullptr; }
  JNIEnv* operator->() const { return env_; }
  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
};

/// True when a call threw. The exception is logged and cleared: a pending
/// exception left in place makes the next JNI call abort the process.
bool checkException(JNIEnv* env, const char* what) {
  if (env->ExceptionCheck() == JNI_FALSE) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  log(LogLevel::Warn, kTag, std::string("a JNI call threw: ") + what);
  return true;
}

// ---- UTF-8 across the JNI boundary --------------------------------------------
//
// JNI's string calls do not speak standard UTF-8. `NewStringUTF` takes *modified*
// UTF-8: a four-byte sequence -- any emoji, and so any WebSocket close reason or
// URL path a page can write -- is illegal there and ART aborts the process on it
// under CheckJNI, and an embedded NUL has to be C0 80. `GetStringUTFChars` hands
// back the same encoding. Everything that crosses here comes from JS, so both
// directions are converted explicitly rather than hoped about.

/// Standard UTF-8 -> modified UTF-8. False when the input is not valid UTF-8,
/// which is a JS string with a lone surrogate in it: the caller fails the call.
bool toModifiedUtf8(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size() + 8);
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const auto* end = p + in.size();
  const auto encodeThree = [&out](std::uint32_t cp) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  };
  while (p < end) {
    const unsigned char lead = *p;
    if (lead == 0x00) {
      out.push_back(static_cast<char>(0xC0));
      out.push_back(static_cast<char>(0x80));
      ++p;
      continue;
    }
    std::size_t width = 0;
    std::uint32_t cp = 0;
    if (lead < 0x80) {
      out.push_back(static_cast<char>(lead));
      ++p;
      continue;
    } else if ((lead & 0xE0) == 0xC0) {
      width = 2;
      cp = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      width = 3;
      cp = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      width = 4;
      cp = lead & 0x07u;
    } else {
      return false;
    }
    if (static_cast<std::size_t>(end - p) < width) return false;
    for (std::size_t i = 1; i < width; ++i) {
      const unsigned char next = p[i];
      if ((next & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3Fu);
    }
    // No overlongs, no surrogates, nothing above U+10FFFF: each would encode to
    // something a Java String cannot hold.
    if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800) || (width == 4 && cp < 0x10000)) {
      return false;
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (width < 4) {
      out.append(reinterpret_cast<const char*>(p), width);
    } else {
      // A surrogate pair, each half as its own three-byte sequence.
      const std::uint32_t rest = cp - 0x10000;
      encodeThree(0xD800 + (rest >> 10));
      encodeThree(0xDC00 + (rest & 0x3FF));
    }
    p += width;
  }
  return true;
}

/// Modified UTF-8 -> standard UTF-8: C0 80 back to a NUL, and a surrogate pair
/// back to one four-byte sequence. Anything it cannot read is copied through, so
/// a malformed Java string costs a mangled message rather than a lost one.
std::string fromModifiedUtf8(const char* in, std::size_t size) {
  std::string out;
  out.reserve(size);
  const auto* p = reinterpret_cast<const unsigned char*>(in);
  const auto* end = p + size;
  const auto readThree = [](const unsigned char* at) {
    return ((at[0] & 0x0Fu) << 12) | ((at[1] & 0x3Fu) << 6) | (at[2] & 0x3Fu);
  };
  while (p < end) {
    if (p[0] == 0xC0 && end - p >= 2 && p[1] == 0x80) {
      out.push_back('\0');
      p += 2;
      continue;
    }
    if ((p[0] & 0xF0) == 0xE0 && end - p >= 6 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      const std::uint32_t high = readThree(p);
      if (high >= 0xD800 && high <= 0xDBFF && (p[3] & 0xF0) == 0xE0 && (p[4] & 0xC0) == 0x80 &&
          (p[5] & 0xC0) == 0x80) {
        const std::uint32_t low = readThree(p + 3);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          const std::uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
          out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          p += 6;
          continue;
        }
      }
    }
    out.push_back(static_cast<char>(*p));
    ++p;
  }
  return out;
}

std::string toString(JNIEnv* env, jstring value) {
  if (value == nullptr) return std::string();
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) {
    env->ExceptionClear();
    return std::string();
  }
  std::string out = fromModifiedUtf8(utf, std::strlen(utf));
  env->ReleaseStringUTFChars(value, utf);
  return out;
}

/// Null when the text cannot be a Java string, or when the VM is out of memory.
/// Either way the caller fails the call rather than carrying on with a null.
jstring toJavaString(JNIEnv* env, const std::string& text) {
  std::string modified;
  if (!toModifiedUtf8(text, modified)) return nullptr;
  jstring value = env->NewStringUTF(modified.c_str());
  if (value == nullptr) env->ExceptionClear();
  return value;
}

Bytes toBytes(JNIEnv* env, jbyteArray value) {
  Bytes out;
  if (value == nullptr) return out;
  const jsize size = env->GetArrayLength(value);
  if (size <= 0) return out;
  out.resize(static_cast<std::size_t>(size));
  env->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte*>(out.data()));
  if (env->ExceptionCheck() != JNI_FALSE) {
    env->ExceptionClear();
    out.clear();
  }
  return out;
}

jbyteArray toJavaBytes(JNIEnv* env, const std::uint8_t* data, std::size_t size) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
  if (array == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  if (size > 0) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte*>(data));
  }
  return array;
}

NetError toNetError(jint kind) {
  // If this fires, a kind was added to NetError and the cases below, and
  // `HttpClient.java`'s ERR_* block and ERR_COUNT, have to follow it.
  static_assert(kNetErrorCount == 10, "toNetError's cases must cover every NetError");
  switch (kind) {
    case 0: return NetError::None;
    case 1: return NetError::Dns;
    case 2: return NetError::Connect;
    case 3: return NetError::Tls;
    case 4: return NetError::Network;
    case 5: return NetError::Protocol;
    case 6: return NetError::Redirect;
    case 7: return NetError::Decode;
    case 8: return NetError::Url;
    case 9: return NetError::Unsupported;
    default: return NetError::Network;
  }
}

// ---- the registry ------------------------------------------------------------------
//
// Ids come from `bindings/Net.cpp` and start at 1 in every runtime, so two
// runtimes in one process would collide. The handle Java sees is a process-wide
// one; this is what maps it back to a sink and the queue its events belong on.

struct AndroidCore;

struct CallSlot {
  std::weak_ptr<AndroidCore> core;
  std::shared_ptr<HttpSink> http;
  std::shared_ptr<SocketSink> socket;
  /// The id `bindings/Net.cpp` knows this call by, so a terminal event can drop
  /// the core's handle entry too rather than leaving one behind per request.
  std::uint64_t localId = 0;
};

std::mutex gCallsMutex;
std::unordered_map<jlong, std::shared_ptr<CallSlot>> gCalls;
std::atomic<jlong> gNextHandle{1};

struct AndroidCore {
  /// Weak on purpose. Every posted job captures the `shared_ptr<AndroidCore>`,
  /// so a strong reference here would be a cycle: the queue would own a job that
  /// owns the state that owns the queue, and nothing would ever destroy either.
  /// The service owns the queue; `NwShared` held its dispatch queue the same way.
  std::weak_ptr<IoQueue> queue;
  jobject client = nullptr;  // global ref
  /// Written on the queue, read from Java threads deciding whether to post.
  std::atomic<bool> stopped{false};
  /// The runtime's own ids, as `bindings/Net.cpp` hands them out, to the
  /// process-wide handle Java knows them by. I/O queue only.
  std::unordered_map<std::uint64_t, jlong> handles;
};

std::shared_ptr<CallSlot> findCall(jlong handle) {
  std::lock_guard<std::mutex> lock(gCallsMutex);
  const auto it = gCalls.find(handle);
  return it == gCalls.end() ? nullptr : it->second;
}

void forgetCall(jlong handle) {
  std::lock_guard<std::mutex> lock(gCallsMutex);
  if (gCalls.erase(handle) > 0) gLiveRefs.fetch_sub(1);
}

jlong registerCall(const std::shared_ptr<AndroidCore>& core, std::uint64_t localId,
                   std::shared_ptr<HttpSink> http, std::shared_ptr<SocketSink> socket) {
  auto slot = std::make_shared<CallSlot>();
  slot->core = core;
  slot->http = std::move(http);
  slot->socket = std::move(socket);
  slot->localId = localId;
  const jlong handle = gNextHandle.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(gCallsMutex);
    gCalls.emplace(handle, std::move(slot));
  }
  gLiveRefs.fetch_add(1);
  return handle;
}

/// Run `work` on the slot's own queue with the slot still live, or drop it.
/// Every Java callback goes through here, and that is the whole of the threading
/// contract: nothing below it runs anywhere but the runtime's serial queue.
template <typename Work>
void onQueue(jlong handle, Work work) {
  auto slot = findCall(handle);
  if (!slot) return;
  auto core = slot->core.lock();
  if (!core || core->stopped.load()) return;
  auto queue = core->queue.lock();
  if (!queue) return;
  queue->post([slot, core, handle, work]() mutable {
    if (core->stopped.load()) return;
    // Re-checked on the queue: a terminal event or an abort may have retired the
    // slot between the lookup above and this job.
    if (!findCall(handle)) return;
    work(*slot, *core);
  });
}

/// A call has delivered its last event: drop the registry slot *and* the core's
/// id -> handle entry, or a long-lived runtime's map grows by one per request.
/// I/O queue only.
void retireCall(AndroidCore& core, jlong handle, std::uint64_t localId) {
  core.handles.erase(localId);
  forgetCall(handle);
}

}  // namespace

// ---- the callbacks Java makes ----------------------------------------------------

extern "C" {

JNIEXPORT void JNICALL screenkitNetHead(JNIEnv* env, jclass, jlong handle, jint status,
                                        jstring statusText, jobjectArray names, jobjectArray values,
                                        jstring url, jboolean redirected) {
  ResponseHead head;
  head.status = static_cast<int>(status);
  head.statusText = toString(env, statusText);
  head.url = toString(env, url);
  head.redirected = redirected == JNI_TRUE;
  // Both lengths: indexing `values` by `names`' count would raise on a short
  // array, and the pending exception would make the next JNI call abort.
  const jsize named = names == nullptr ? 0 : env->GetArrayLength(names);
  const jsize valued = values == nullptr ? 0 : env->GetArrayLength(values);
  const jsize count = named < valued ? named : valued;
  head.headers.reserve(static_cast<std::size_t>(count));
  for (jsize i = 0; i < count; ++i) {
    auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
    auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
    if (checkException(env, "reading a response header")) {
      if (name != nullptr) env->DeleteLocalRef(name);
      if (value != nullptr) env->DeleteLocalRef(value);
      break;
    }
    head.headers.emplace_back(toString(env, name), toString(env, value));
    if (name != nullptr) env->DeleteLocalRef(name);
    if (value != nullptr) env->DeleteLocalRef(value);
  }
  auto shared = std::make_shared<ResponseHead>(std::move(head));
  onQueue(handle, [shared](CallSlot& slot, AndroidCore&) {
    if (slot.http) slot.http->onHead(*shared);
  });
}

JNIEXPORT void JNICALL screenkitNetData(JNIEnv* env, jclass, jlong handle, jbyteArray chunk) {
  auto shared = std::make_shared<Bytes>(toBytes(env, chunk));
  onQueue(handle, [shared](CallSlot& slot, AndroidCore&) {
    if (slot.http) slot.http->onData(std::move(*shared));
  });
}

JNIEXPORT void JNICALL screenkitNetEnd(JNIEnv*, jclass, jlong handle) {
  onQueue(handle, [handle](CallSlot& slot, AndroidCore& core) {
    auto sink = slot.http;
    retireCall(core, handle, slot.localId);
    if (sink) sink->onEnd();
  });
}

JNIEXPORT void JNICALL screenkitNetError(JNIEnv* env, jclass, jlong handle, jint kind,
                                         jstring message) {
  const NetError error = toNetError(kind);
  const std::string text = toString(env, message);
  onQueue(handle, [handle, error, text](CallSlot& slot, AndroidCore& core) {
    auto sink = slot.http;
    retireCall(core, handle, slot.localId);
    if (sink) sink->onError(error, text);
  });
}

JNIEXPORT void JNICALL screenkitNetUpload(JNIEnv*, jclass, jlong handle, jlong sent, jlong total,
                                          jboolean complete) {
  const bool done = complete == JNI_TRUE;
  onQueue(handle, [sent, total, done](CallSlot& slot, AndroidCore&) {
    if (slot.http) {
      slot.http->onUploadProgress(static_cast<std::uint64_t>(sent), static_cast<std::int64_t>(total),
                                  done);
    }
  });
}

JNIEXPORT void JNICALL screenkitNetSocketOpen(JNIEnv* env, jclass, jlong handle, jstring protocol,
                                              jstring extensions) {
  const std::string text = toString(env, protocol);
  const std::string negotiated = toString(env, extensions);
  onQueue(handle, [text, negotiated](CallSlot& slot, AndroidCore&) {
    if (slot.socket) slot.socket->onOpen(text, negotiated);
  });
}

JNIEXPORT void JNICALL screenkitNetSocketMessage(JNIEnv* env, jclass, jlong handle, jboolean text,
                                                 jbyteArray data) {
  auto shared = std::make_shared<Bytes>(toBytes(env, data));
  const bool isText = text == JNI_TRUE;
  onQueue(handle, [shared, isText](CallSlot& slot, AndroidCore&) {
    if (slot.socket) slot.socket->onMessage(isText, std::move(*shared));
  });
}

JNIEXPORT void JNICALL screenkitNetSocketSent(JNIEnv*, jclass, jlong handle, jlong bytes) {
  onQueue(handle, [bytes](CallSlot& slot, AndroidCore&) {
    if (slot.socket) slot.socket->onSent(static_cast<std::uint64_t>(bytes));
  });
}

JNIEXPORT void JNICALL screenkitNetSocketError(JNIEnv* env, jclass, jlong handle, jint kind,
                                               jstring message) {
  const NetError error = toNetError(kind);
  const std::string text = toString(env, message);
  onQueue(handle, [error, text](CallSlot& slot, AndroidCore&) {
    if (slot.socket) slot.socket->onError(error, text);
  });
}

JNIEXPORT void JNICALL screenkitNetSocketClose(JNIEnv* env, jclass, jlong handle, jint code,
                                               jstring reason, jboolean wasClean) {
  const std::string text = toString(env, reason);
  const bool clean = wasClean == JNI_TRUE;
  const int status = static_cast<int>(code);
  onQueue(handle, [handle, status, text, clean](CallSlot& slot, AndroidCore& core) {
    auto sink = slot.socket;
    retireCall(core, handle, slot.localId);
    if (sink) sink->onClose(status, text, clean);
  });
}

}  // extern "C"

namespace {

const JNINativeMethod kCallbacks[] = {
    {"nativeHead",
     "(JILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Z)V",
     reinterpret_cast<void*>(screenkitNetHead)},
    {"nativeData", "(J[B)V", reinterpret_cast<void*>(screenkitNetData)},
    {"nativeEnd", "(J)V", reinterpret_cast<void*>(screenkitNetEnd)},
    {"nativeError", "(JILjava/lang/String;)V", reinterpret_cast<void*>(screenkitNetError)},
    {"nativeUpload", "(JJJZ)V", reinterpret_cast<void*>(screenkitNetUpload)},
    {"nativeSocketOpen", "(JLjava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(screenkitNetSocketOpen)},
    {"nativeSocketMessage", "(JZ[B)V", reinterpret_cast<void*>(screenkitNetSocketMessage)},
    {"nativeSocketSent", "(JJ)V", reinterpret_cast<void*>(screenkitNetSocketSent)},
    {"nativeSocketError", "(JILjava/lang/String;)V",
     reinterpret_cast<void*>(screenkitNetSocketError)},
    {"nativeSocketClose", "(JILjava/lang/String;Z)V",
     reinterpret_cast<void*>(screenkitNetSocketClose)},
};

// ---- the service ---------------------------------------------------------------------

class AndroidNetService final : public NetService {
 public:
  explicit AndroidNetService(NetConfig config) : userAgent_(std::move(config.userAgent)) {
    queue_ = makeIoQueue(config.name + ".net");
    core_ = std::make_shared<AndroidCore>();
    core_->queue = queue_;
    if (!gReady.load()) {
      // No VM, or the class never resolved: every call below finds no client and
      // fails `unsupported`, as on a platform with none at all.
      return;
    }
    ScopedEnv env;
    if (!env) return;
    jobjectArray anchors = makeAnchors(env.get(), config.testTlsAnchors);
    if (anchors == nullptr) return;
    jstring agent = env->NewStringUTF(userAgent_.c_str());
    jobject client = env->NewObject(gClientClass, gCtor, anchors, agent);
    env->DeleteLocalRef(anchors);
    if (agent != nullptr) env->DeleteLocalRef(agent);
    if (checkException(env.get(), "new HttpClient") || client == nullptr) {
      log(LogLevel::Warn, kTag, "could not build the HTTP client: requests will fail");
      return;
    }
    core_->client = env->NewGlobalRef(client);
    env->DeleteLocalRef(client);
    if (core_->client != nullptr) gLiveRefs.fetch_add(1);
  }

  ~AndroidNetService() override { shutdown(); }

  void startRequest(std::uint64_t id, HttpRequestSpec spec, std::shared_ptr<HttpSink> sink) override {
    if (stopped_) return;
    auto core = core_;
    auto shared = std::make_shared<HttpRequestSpec>(std::move(spec));
    queue_->post([core, id, shared, sink] {
      if (core->stopped.load()) return;
      if (core->client == nullptr) {
        if (sink) sink->onError(NetError::Unsupported, "this device has no HTTP client");
        return;
      }
      // An id already in use is a caller error. Taking it would re-point the id
      // at the new call and leave the old one running where no abort can reach
      // it; refusing settles the new one instead (as the Linux client does).
      if (core->handles.count(id) != 0) {
        if (sink) sink->onError(NetError::Url, "that request id is already in use");
        return;
      }
      ScopedEnv env;
      if (!env) {
        if (sink) sink->onError(NetError::Unsupported, "no JNIEnv for the HTTP client");
        return;
      }
      // Every allocation below can answer null -- a string JS wrote that is not
      // valid UTF-8, or an OutOfMemoryError -- and carrying on with one would
      // either send the wrong request or abort the process on the next JNI call.
      jstring url = toJavaString(env.get(), shared->url.href());
      jstring method = toJavaString(env.get(), shared->method);
      const auto headerCount = static_cast<jsize>(shared->headers.size());
      jobjectArray names = env->NewObjectArray(headerCount, gStringClass, nullptr);
      jobjectArray values = env->NewObjectArray(headerCount, gStringClass, nullptr);
      if (names == nullptr || values == nullptr) env->ExceptionClear();
      bool ok = url != nullptr && method != nullptr && names != nullptr && values != nullptr;
      for (jsize i = 0; ok && i < headerCount; ++i) {
        jstring name = toJavaString(env.get(), shared->headers[static_cast<std::size_t>(i)].first);
        jstring value = toJavaString(env.get(), shared->headers[static_cast<std::size_t>(i)].second);
        if (name == nullptr || value == nullptr) {
          ok = false;
        } else {
          env->SetObjectArrayElement(names, i, name);
          env->SetObjectArrayElement(values, i, value);
        }
        if (name != nullptr) env->DeleteLocalRef(name);
        if (value != nullptr) env->DeleteLocalRef(value);
      }
      jbyteArray body = nullptr;
      if (ok && shared->body && !shared->body->empty()) {
        body = toJavaBytes(env.get(), shared->body->data(), shared->body->size());
        // A null here would go out as no body at all, and the request would look
        // as if it had succeeded.
        ok = body != nullptr;
      }
      bool threw = !ok;
      if (ok) {
        const jlong handle = registerCall(core, id, sink, nullptr);
        core->handles[id] = handle;
        env->CallVoidMethod(core->client, gStartRequest, handle, url, method, names, values, body,
                            shared->streamingBody ? JNI_TRUE : JNI_FALSE,
                            static_cast<jint>(shared->redirect),
                            shared->useCookies ? JNI_TRUE : JNI_FALSE,
                            shared->reportUpload ? JNI_TRUE : JNI_FALSE,
                            static_cast<jlong>(shared->flowWindow));
        threw = checkException(env.get(), "HttpClient.startRequest");
        if (threw) retireCall(*core, handle, id);
      }
      if (url != nullptr) env->DeleteLocalRef(url);
      if (method != nullptr) env->DeleteLocalRef(method);
      if (names != nullptr) env->DeleteLocalRef(names);
      if (values != nullptr) env->DeleteLocalRef(values);
      if (body != nullptr) env->DeleteLocalRef(body);
      if (threw && sink) {
        sink->onError(ok ? NetError::Network : NetError::Url,
                      ok ? "the HTTP client refused the request"
                         : "the request could not be handed to the HTTP client");
      }
    });
  }

  void appendRequestBody(std::uint64_t id, std::shared_ptr<const Bytes> chunk) override {
    if (stopped_ || !chunk || chunk->empty()) return;
    auto core = core_;
    queue_->post([core, id, chunk] {
      jlong handle = 0;
      if (!live(core, id, handle)) return;
      ScopedEnv env;
      if (!env) return;
      jbyteArray bytes = toJavaBytes(env.get(), chunk->data(), chunk->size());
      if (bytes == nullptr) return;
      env->CallVoidMethod(core->client, gAppendBody, handle, bytes);
      checkException(env.get(), "HttpClient.appendRequestBody");
      env->DeleteLocalRef(bytes);
    });
  }

  void finishRequestBody(std::uint64_t id) override {
    callWithHandle(id, gFinishBody, "HttpClient.finishRequestBody");
  }

  void abortRequest(std::uint64_t id) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id] {
      const auto it = core->handles.find(id);
      if (it == core->handles.end()) return;
      const jlong handle = it->second;
      core->handles.erase(it);
      // Forgotten first: whatever Java is in the middle of, its callback finds
      // nothing to deliver to.
      forgetCall(handle);
      if (core->client == nullptr) return;
      ScopedEnv env;
      if (!env) return;
      env->CallVoidMethod(core->client, gAbortRequest, handle);
      checkException(env.get(), "HttpClient.abortRequest");
    });
  }

  void acknowledgeResponseData(std::uint64_t id, std::uint64_t bytes) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id, bytes] {
      jlong handle = 0;
      if (!live(core, id, handle)) return;
      ScopedEnv env;
      if (!env) return;
      env->CallVoidMethod(core->client, gAcknowledge, handle, static_cast<jlong>(bytes));
      checkException(env.get(), "HttpClient.acknowledgeResponseData");
    });
  }

  void openSocket(std::uint64_t id, Url url, std::vector<std::string> protocols,
                  std::shared_ptr<SocketSink> sink) override {
    if (stopped_) return;
    auto core = core_;
    auto shared = std::make_shared<std::pair<Url, std::vector<std::string>>>(std::move(url),
                                                                            std::move(protocols));
    queue_->post([core, id, shared, sink] {
      if (core->stopped.load()) return;
      if (core->client == nullptr) {
        if (sink) {
          sink->onError(NetError::Unsupported, "this device has no HTTP client");
          sink->onClose(1006, "", false);
        }
        return;
      }
      if (core->handles.count(id) != 0) {
        if (sink) {
          sink->onError(NetError::Url, "that socket id is already in use");
          sink->onClose(1006, "", false);
        }
        return;
      }
      ScopedEnv env;
      if (!env) {
        // A bare return here would leave the JS WebSocket CONNECTING for ever.
        if (sink) {
          sink->onError(NetError::Unsupported, "no JNIEnv for the HTTP client");
          sink->onClose(1006, "", false);
        }
        return;
      }
      jstring url = toJavaString(env.get(), shared->first.href());
      const auto protocolCount = static_cast<jsize>(shared->second.size());
      jobjectArray protocols = env->NewObjectArray(protocolCount, gStringClass, nullptr);
      if (protocols == nullptr) env->ExceptionClear();
      bool ok = url != nullptr && protocols != nullptr;
      for (jsize i = 0; ok && i < protocolCount; ++i) {
        jstring protocol = toJavaString(env.get(), shared->second[static_cast<std::size_t>(i)]);
        if (protocol == nullptr) {
          ok = false;
        } else {
          env->SetObjectArrayElement(protocols, i, protocol);
          env->DeleteLocalRef(protocol);
        }
      }
      bool threw = !ok;
      if (ok) {
        const jlong handle = registerCall(core, id, nullptr, sink);
        core->handles[id] = handle;
        env->CallVoidMethod(core->client, gOpenSocket, handle, url, protocols);
        threw = checkException(env.get(), "HttpClient.openSocket");
        if (threw) retireCall(*core, handle, id);
      }
      if (url != nullptr) env->DeleteLocalRef(url);
      if (protocols != nullptr) env->DeleteLocalRef(protocols);
      if (threw && sink) {
        sink->onError(ok ? NetError::Network : NetError::Url,
                      ok ? "the HTTP client refused the socket"
                         : "the socket could not be handed to the HTTP client");
        sink->onClose(1006, "", false);
      }
    });
  }

  void sendSocket(std::uint64_t id, bool text, std::shared_ptr<const Bytes> payload) override {
    if (stopped_ || !payload) return;
    auto core = core_;
    queue_->post([core, id, text, payload] {
      jlong handle = 0;
      if (!live(core, id, handle)) return;
      ScopedEnv env;
      if (!env) return;
      jbyteArray bytes = toJavaBytes(env.get(), payload->data(), payload->size());
      if (bytes == nullptr) return;
      env->CallVoidMethod(core->client, gSendSocket, handle, text ? JNI_TRUE : JNI_FALSE, bytes);
      checkException(env.get(), "HttpClient.sendSocket");
      env->DeleteLocalRef(bytes);
    });
  }

  void closeSocket(std::uint64_t id, int code, std::string reason) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id, code, reason] {
      jlong handle = 0;
      if (!live(core, id, handle)) return;
      ScopedEnv env;
      if (!env) return;
      // The reason is whatever the page passed to close(): an emoji in it is a
      // four-byte sequence, which NewStringUTF cannot carry.
      jstring text = toJavaString(env.get(), reason);
      if (text == nullptr) text = env->NewStringUTF("");
      if (text == nullptr) {
        env->ExceptionClear();
        return;
      }
      env->CallVoidMethod(core->client, gCloseSocket, handle, static_cast<jint>(code), text);
      checkException(env.get(), "HttpClient.closeSocket");
      env->DeleteLocalRef(text);
    });
  }

  // Both of these are synchronous JS calls, and both run on the queue like every
  // other entry point: `shutdown` nulls `client` and deletes its global
  // reference there, so reading it from the caller's thread would race a
  // teardown with a JNI call on a freed reference.
  std::string cookiesFor(const Url& url) override {
    if (stopped_) return std::string();
    auto core = core_;
    const std::string href = url.href();
    std::string out;
    queue_->sync([core, &href, &out] {
      if (core->stopped.load() || core->client == nullptr) return;
      ScopedEnv env;
      if (!env) return;
      jstring target = toJavaString(env.get(), href);
      if (target == nullptr) return;
      auto value = static_cast<jstring>(env->CallObjectMethod(core->client, gCookiesFor, target));
      if (!checkException(env.get(), "HttpClient.cookiesFor")) out = toString(env.get(), value);
      if (value != nullptr) env->DeleteLocalRef(value);
      env->DeleteLocalRef(target);
    });
    return out;
  }

  bool setCookie(const Url& url, const std::string& setCookie) override {
    if (stopped_) return false;
    auto core = core_;
    const std::string href = url.href();
    bool stored = false;
    queue_->sync([core, &href, &setCookie, &stored] {
      if (core->stopped.load() || core->client == nullptr) return;
      ScopedEnv env;
      if (!env) return;
      jstring target = toJavaString(env.get(), href);
      jstring value = toJavaString(env.get(), setCookie);
      if (target != nullptr && value != nullptr) {
        const jboolean ok = env->CallBooleanMethod(core->client, gSetCookie, target, value);
        stored = !checkException(env.get(), "HttpClient.setCookie") && ok == JNI_TRUE;
      }
      if (target != nullptr) env->DeleteLocalRef(target);
      if (value != nullptr) env->DeleteLocalRef(value);
    });
    return stored;
  }

  void shutdown() override {
    std::lock_guard<std::mutex> lock(shutdownMutex_);
    if (stopped_.exchange(true)) return;
    auto core = core_;
    // Synchronous, so no request outlives the runtime: every call is cancelled
    // and every sink dropped before this returns.
    queue_->sync([core] {
      core->stopped.store(true);
      for (const auto& [id, handle] : core->handles) {
        (void)id;
        forgetCall(handle);
      }
      core->handles.clear();
      if (core->client == nullptr) return;
      ScopedEnv env;
      if (env) {
        env->CallVoidMethod(core->client, gShutdown);
        checkException(env.get(), "HttpClient.shutdown");
        env->DeleteGlobalRef(core->client);
      } else {
        // The reference is unreachable either way, so the count has to come down
        // either way -- otherwise one failed attach leaves liveJavaRefCount()
        // permanently non-zero and the leak it exists to show invisible.
        log(LogLevel::Warn, kTag, "no JNIEnv at shutdown: the client reference is dropped unreleased");
      }
      gLiveRefs.fetch_sub(1);
      core->client = nullptr;
    });
  }

 private:
  /// I/O queue. The call is still registered and the client is usable.
  static bool live(const std::shared_ptr<AndroidCore>& core, std::uint64_t id, jlong& handle) {
    if (core->stopped.load() || core->client == nullptr) return false;
    const auto it = core->handles.find(id);
    if (it == core->handles.end()) return false;
    if (!findCall(it->second)) return false;
    handle = it->second;
    return true;
  }

  void callWithHandle(std::uint64_t id, jmethodID method, const char* what) {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id, method, what] {
      jlong handle = 0;
      if (!live(core, id, handle)) return;
      ScopedEnv env;
      if (!env) return;
      env->CallVoidMethod(core->client, method, handle);
      checkException(env.get(), what);
    });
  }

  static jobjectArray makeAnchors(JNIEnv* env, const std::vector<Bytes>& anchors) {
    jobjectArray array =
        env->NewObjectArray(static_cast<jsize>(anchors.size()), gByteArrayClass, nullptr);
    if (array == nullptr) {
      env->ExceptionClear();
      log(LogLevel::Warn, kTag, "could not allocate the test trust anchors");
      return nullptr;
    }
    for (std::size_t i = 0; i < anchors.size(); ++i) {
      jbyteArray der = toJavaBytes(env, anchors[i].data(), anchors[i].size());
      if (der == nullptr) {
        env->DeleteLocalRef(array);
        // Silently falling back to the platform store would trust a different
        // root than the one that was configured.
        log(LogLevel::Warn, kTag, "could not copy a test trust anchor");
        return nullptr;
      }
      env->SetObjectArrayElement(array, static_cast<jsize>(i), der);
      env->DeleteLocalRef(der);
    }
    return array;
  }

  std::shared_ptr<IoQueue> queue_;
  std::shared_ptr<AndroidCore> core_;
  std::string userAgent_;
  std::atomic<bool> stopped_{false};
  std::mutex shutdownMutex_;
};

}  // namespace

// ---- the host's half -----------------------------------------------------------------

void setJavaVm(JavaVM* vm) { gVm = vm; }

bool prepareAndroidNetwork(JNIEnv* env, std::string& error) {
  std::lock_guard<std::mutex> lock(gPrepareMutex);
  if (gReady.load()) return true;
  if (gVm == nullptr) {
    error = "no JavaVM: setJavaVm has not been called";
    return false;
  }
  if (env == nullptr) {
    error = "no JNIEnv";
    return false;
  }

  // Every failure below leaves nothing behind: a half-prepared set of globals
  // would be re-resolved by the next call and leak the first pair.
  jclass localClient = env->FindClass(kClientClass);
  if (localClient == nullptr) {
    env->ExceptionClear();
    error = std::string("cannot find ") + kClientClass +
            " -- this has to run where the app's class loader is reachable";
    return false;
  }
  jclass localString = env->FindClass("java/lang/String");
  jclass localBytes = env->FindClass("[B");
  if (localString == nullptr || localBytes == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(localClient);
    if (localString != nullptr) env->DeleteLocalRef(localString);
    if (localBytes != nullptr) env->DeleteLocalRef(localBytes);
    error = "cannot find java.lang.String or byte[]";
    return false;
  }

  struct Wanted {
    jmethodID* out;
    const char* name;
    const char* signature;
  };
  const Wanted wanted[] = {
      {&gCtor, "<init>", "([[BLjava/lang/String;)V"},
      {&gStartRequest, "startRequest",
       "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[BZIZZJ)V"},
      {&gAppendBody, "appendRequestBody", "(J[B)V"},
      {&gFinishBody, "finishRequestBody", "(J)V"},
      {&gAbortRequest, "abortRequest", "(J)V"},
      {&gAcknowledge, "acknowledgeResponseData", "(JJ)V"},
      {&gOpenSocket, "openSocket", "(JLjava/lang/String;[Ljava/lang/String;)V"},
      {&gSendSocket, "sendSocket", "(JZ[B)V"},
      {&gCloseSocket, "closeSocket", "(JILjava/lang/String;)V"},
      {&gCookiesFor, "cookiesFor", "(Ljava/lang/String;)Ljava/lang/String;"},
      {&gSetCookie, "setCookie", "(Ljava/lang/String;Ljava/lang/String;)Z"},
      {&gShutdown, "shutdown", "()V"},
  };
  // Checked one at a time: a pending NoSuchMethodError makes the *next* JNI call
  // illegal, and CheckJNI turns that into an abort.
  for (const Wanted& method : wanted) {
    *method.out = env->GetMethodID(localClient, method.name, method.signature);
    if (*method.out == nullptr) {
      env->ExceptionClear();
      env->DeleteLocalRef(localClient);
      env->DeleteLocalRef(localString);
      env->DeleteLocalRef(localBytes);
      error = std::string("HttpClient has no ") + method.name + method.signature;
      return false;
    }
  }

  if (env->RegisterNatives(localClient, kCallbacks,
                           static_cast<jint>(sizeof(kCallbacks) / sizeof(kCallbacks[0]))) != 0) {
    env->ExceptionClear();
    env->DeleteLocalRef(localClient);
    env->DeleteLocalRef(localString);
    env->DeleteLocalRef(localBytes);
    error = "RegisterNatives failed for HttpClient";
    return false;
  }

  gClientClass = static_cast<jclass>(env->NewGlobalRef(localClient));
  gStringClass = static_cast<jclass>(env->NewGlobalRef(localString));
  gByteArrayClass = static_cast<jclass>(env->NewGlobalRef(localBytes));
  env->DeleteLocalRef(localClient);
  env->DeleteLocalRef(localString);
  env->DeleteLocalRef(localBytes);
  if (gClientClass == nullptr || gStringClass == nullptr || gByteArrayClass == nullptr) {
    if (gClientClass != nullptr) env->DeleteGlobalRef(gClientClass);
    if (gStringClass != nullptr) env->DeleteGlobalRef(gStringClass);
    if (gByteArrayClass != nullptr) env->DeleteGlobalRef(gByteArrayClass);
    gClientClass = gStringClass = gByteArrayClass = nullptr;
    error = "cannot hold the HttpClient classes";
    return false;
  }
  gReady.store(true);
  return true;
}

std::size_t liveJavaRefCount() { return gLiveRefs.load(); }

std::shared_ptr<NetService> NetService::create(NetConfig config) {
  return std::make_shared<AndroidNetService>(std::move(config));
}

bool networkAvailable() { return gReady.load(); }

}  // namespace screenkit::net
