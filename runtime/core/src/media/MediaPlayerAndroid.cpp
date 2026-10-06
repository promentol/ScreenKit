// Copyright (c) ScreenKit contributors. MIT.
//
// Android's player: Media3 ExoPlayer over JNI, through
// `runtime/android/app/src/main/java/dev/screenkit/media/VideoPlayer.java`.
//
// There is no media code here. ExoPlayer parses HLS and DASH, adapts the
// bitrate, decodes through MediaCodec, renders into a SurfaceView beneath SDL's
// surface, and runs Widevine and ClearKey through MediaDrm. What this file does
// is translate: a seam call into a call on the Java object, and the Java
// callbacks back into `MediaSink`.
//
// Threading. Every call out of here is a JNI call that only posts to the Java
// player's own looper (VideoPlayer.java), made from whichever thread calls the
// seam -- the JS thread, or the thread that paused the runtime -- attached to
// the VM for the call. Every callback in arrives on a Java thread (the player's
// looper, or ExoPlayer's DRM thread for a licence request), finds its player
// through the registry by handle, and hands the event to the sink under the
// player's sink lock. `shutdown()` takes that same lock to drop the sink, so an
// event already on its way either lands before shutdown returns or not at all.
#include "MediaPlayerAndroid.h"

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

#include "../net/IoQueue.h"
#include "MediaPlayer.h"

namespace screenkit::media {
namespace {

constexpr const char* kTag = "screenkit.media";
constexpr const char* kPlayerClass = "dev/screenkit/media/VideoPlayer";
/// VideoPlayer.ERR_COUNT, checked when the class is resolved: the error kinds
/// cross JNI as plain ints.
constexpr int kJavaErrorCount = kMediaErrorKindCount;

JavaVM* gVm = nullptr;
jclass gPlayerClass = nullptr;  // global ref
jmethodID gCtor = nullptr;
jmethodID gLoad = nullptr;
jmethodID gPlay = nullptr;
jmethodID gPause = nullptr;
jmethodID gSeek = nullptr;
jmethodID gSetRate = nullptr;
jmethodID gSetVolume = nullptr;
jmethodID gSetMuted = nullptr;
jmethodID gSetPlane = nullptr;
jmethodID gSelectVariant = nullptr;
jmethodID gSetAbr = nullptr;
jmethodID gSelectAudioLanguage = nullptr;
jmethodID gSelectText = nullptr;
jmethodID gProvideLicence = nullptr;
jmethodID gUnload = nullptr;
jmethodID gShutdown = nullptr;
jmethodID gCapabilitiesStatic = nullptr;  // static
std::atomic<bool> gReady{false};
std::mutex gPrepareMutex;
std::atomic<std::size_t> gLiveRefs{0};

// ---- attaching to the VM ------------------------------------------------------------
//
// The seam is called from native threads -- the JS thread, whatever thread
// paused the runtime -- and each needs a JNIEnv of its own. Attaching costs
// nothing after the first time; the thread_local detaches when the thread ends.

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

/// True when a call threw. Logged and cleared: a pending exception left in
/// place makes the next JNI call abort the process.
bool checkException(JNIEnv* env, const char* what) {
  if (env->ExceptionCheck() == JNI_FALSE) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  log(LogLevel::Warn, kTag, std::string("a JNI call threw: ") + what);
  return true;
}

// ---- strings cross as UTF-8 bytes ---------------------------------------------------------
//
// Never as jstring: JNI's string calls speak *modified* UTF-8, on which an emoji
// in a cue or a URL aborts the process under CheckJNI. VideoPlayer.java decodes
// and encodes standard UTF-8 itself.

jbyteArray toBytes(JNIEnv* env, const std::string& text) {
  jbyteArray out = env->NewByteArray(static_cast<jsize>(text.size()));
  if (out == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  if (!text.empty()) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
  }
  return out;
}

jbyteArray toBytes(JNIEnv* env, const Bytes& data) {
  jbyteArray out = env->NewByteArray(static_cast<jsize>(data.size()));
  if (out == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  if (!data.empty()) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte*>(data.data()));
  }
  return out;
}

std::string fromBytes(JNIEnv* env, jbyteArray array) {
  if (array == nullptr) return std::string();
  const jsize length = env->GetArrayLength(array);
  std::string out(static_cast<size_t>(length), '\0');
  if (length > 0) env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(&out[0]));
  return out;
}

Bytes rawBytes(JNIEnv* env, jbyteArray array) {
  Bytes out;
  if (array == nullptr) return out;
  const jsize length = env->GetArrayLength(array);
  out.resize(static_cast<size_t>(length));
  if (length > 0) env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(out.data()));
  return out;
}

std::string stringAt(JNIEnv* env, jobjectArray array, jsize index) {
  auto element = static_cast<jbyteArray>(env->GetObjectArrayElement(array, index));
  std::string out = fromBytes(env, element);
  if (element != nullptr) env->DeleteLocalRef(element);
  return out;
}

std::vector<jint> ints(JNIEnv* env, jintArray array) {
  std::vector<jint> out;
  if (array == nullptr) return out;
  out.resize(static_cast<size_t>(env->GetArrayLength(array)));
  if (!out.empty()) env->GetIntArrayRegion(array, 0, static_cast<jsize>(out.size()), out.data());
  return out;
}

std::vector<jdouble> doubles(JNIEnv* env, jdoubleArray array) {
  std::vector<jdouble> out;
  if (array == nullptr) return out;
  out.resize(static_cast<size_t>(env->GetArrayLength(array)));
  if (!out.empty()) env->GetDoubleArrayRegion(array, 0, static_cast<jsize>(out.size()), out.data());
  return out;
}

// ---- ClearKey: the configured keys as the JWK set a ClearKey CDM takes as its licence

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string base64Url(const std::string& hex) {
  Bytes raw;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    raw.push_back(static_cast<std::uint8_t>(hexValue(hex[i]) * 16 + hexValue(hex[i + 1])));
  }
  static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  for (; i + 2 < raw.size(); i += 3) {
    const std::uint32_t v = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i + 1 == raw.size()) {
    const std::uint32_t v = raw[i] << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
  } else if (i + 2 == raw.size()) {
    const std::uint32_t v = (raw[i] << 16) | (raw[i + 1] << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
  }
  return out;
}

std::string clearKeysJson(const std::vector<std::pair<std::string, std::string>>& keys) {
  if (keys.empty()) return std::string();
  std::string json = "{\"keys\":[";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) json += ",";
    json += "{\"kty\":\"oct\",\"kid\":\"" + base64Url(keys[i].first) + "\",\"k\":\"" + base64Url(keys[i].second) +
            "\"}";
  }
  json += "],\"type\":\"temporary\"}";
  return json;
}

// ---- the players ------------------------------------------------------------------------

class AndroidPlayer;

std::mutex gRegistryMutex;
std::unordered_map<jlong, std::weak_ptr<AndroidPlayer>> gPlayers;
jlong gNextHandle = 1;

class AndroidPlayer final : public MediaPlayer, public std::enable_shared_from_this<AndroidPlayer> {
 public:
  explicit AndroidPlayer(MediaConfig config) : name_(std::move(config.name)), sink_(std::move(config.sink)) {}

  ~AndroidPlayer() override { shutdown(); }

  /// After make_shared: registers the handle and builds the Java player.
  void start() {
    {
      std::lock_guard<std::mutex> lock(gRegistryMutex);
      handle_ = gNextHandle++;
      gPlayers[handle_] = weak_from_this();
    }
    if (!gReady.load()) return;
    ScopedEnv env;
    if (!env) return;
    jobject local = env->NewObject(gPlayerClass, gCtor, static_cast<jlong>(handle_));
    if (checkException(env.get(), "new VideoPlayer") || local == nullptr) {
      log(LogLevel::Warn, kTag, "could not build the video player: loads will fail");
      return;
    }
    java_ = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (java_ != nullptr) gLiveRefs.fetch_add(1);
  }

  /// Hand one event to the sink, unless shut down. Any thread.
  template <typename Deliver>
  void deliver(Deliver&& deliver) {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_ && !stopped_.load()) deliver(*sink_);
  }

  void load(LoadRequest request) override {
    if (stopped_) return;
    if (java_ == nullptr) {
      // No Java player: fail on a later turn, as a real load would.
      if (!fallback_) fallback_ = net::makeIoQueue(name_ + ".media");
      std::weak_ptr<AndroidPlayer> weak = weak_from_this();
      const std::uint32_t serial = request.serial;
      fallback_->post([weak, serial] {
        if (auto self = weak.lock()) {
          self->deliver([serial](MediaSink& sink) {
            sink.onError(serial, MediaErrorKind::Unavailable, 0,
                         "the Android media player is unavailable (dev.screenkit.media.VideoPlayer did not load)");
          });
        }
      });
      return;
    }
    ScopedEnv env;
    if (!env) return;
    jbyteArray url = toBytes(env.get(), request.url);
    jbyteArray mime = toBytes(env.get(), request.mimeType);
    jbyteArray keySystem = toBytes(env.get(), request.drm.keySystem);
    jbyteArray server = toBytes(env.get(), request.drm.licenceServer);
    const std::string keys = clearKeysJson(request.drm.clearKeys);
    jbyteArray clearKeys = keys.empty() ? nullptr : toBytes(env.get(), keys);
    jbyteArray certificate =
        request.drm.serverCertificate ? toBytes(env.get(), *request.drm.serverCertificate) : nullptr;
    jbyteArray audio = toBytes(env.get(), request.audioLanguage);
    jbyteArray text = toBytes(env.get(), request.textLanguage);
    jdoubleArray abr = abrArray(env.get(), request.abr);
    env->CallVoidMethod(java_, gLoad, static_cast<jint>(request.serial), url, request.startTime, mime, keySystem,
                        server, clearKeys, certificate, audio, text, abr);
    checkException(env.get(), "VideoPlayer.load");
    for (jobject ref : {static_cast<jobject>(url), static_cast<jobject>(mime), static_cast<jobject>(keySystem),
                        static_cast<jobject>(server), static_cast<jobject>(clearKeys),
                        static_cast<jobject>(certificate), static_cast<jobject>(audio), static_cast<jobject>(text),
                        static_cast<jobject>(abr)}) {
      if (ref != nullptr) env->DeleteLocalRef(ref);
    }
  }

  void play() override { callVoid(gPlay, "play"); }
  void pause() override { callVoid(gPause, "pause"); }

  void seek(double position) override {
    call("seek", [&](JNIEnv* env) { env->CallVoidMethod(java_, gSeek, static_cast<jdouble>(position)); });
  }

  void setRate(double rate) override {
    call("setRate", [&](JNIEnv* env) { env->CallVoidMethod(java_, gSetRate, static_cast<jdouble>(rate)); });
  }

  void setVolume(double volume) override {
    call("setVolume", [&](JNIEnv* env) { env->CallVoidMethod(java_, gSetVolume, static_cast<jdouble>(volume)); });
  }

  void setMuted(bool muted) override {
    call("setMuted",
         [&](JNIEnv* env) { env->CallVoidMethod(java_, gSetMuted, static_cast<jboolean>(muted ? JNI_TRUE : JNI_FALSE)); });
  }

  void setPlane(PlaneRect rect, bool visible) override {
    // The drawable the rect is measured in: Android always draws at a fixed
    // size, scaled into the window (GlSurfaceSdl.cpp), and the Java side maps
    // the plane the same way (media::planeInWindow) against the layout's size.
    const VideoHost host = videoHost();
    call("setPlane", [&](JNIEnv* env) {
      env->CallVoidMethod(java_, gSetPlane, rect.x, rect.y, rect.width, rect.height,
                          static_cast<jboolean>(visible ? JNI_TRUE : JNI_FALSE), static_cast<jint>(rect.order),
                          static_cast<jint>(host.drawableWidth), static_cast<jint>(host.drawableHeight));
    });
  }

  void selectVariant(int variantId) override {
    call("selectVariant", [&](JNIEnv* env) { env->CallVoidMethod(java_, gSelectVariant, static_cast<jint>(variantId)); });
  }

  void setAbr(AbrConfig abr) override {
    call("setAbr", [&](JNIEnv* env) {
      jdoubleArray array = abrArray(env, abr);
      env->CallVoidMethod(java_, gSetAbr, array);
      if (array != nullptr) env->DeleteLocalRef(array);
    });
  }

  void selectAudioLanguage(std::string language, std::string role) override {
    call("selectAudioLanguage", [&](JNIEnv* env) {
      jbyteArray lang = toBytes(env, language);
      jbyteArray r = toBytes(env, role);
      env->CallVoidMethod(java_, gSelectAudioLanguage, lang, r);
      if (lang != nullptr) env->DeleteLocalRef(lang);
      if (r != nullptr) env->DeleteLocalRef(r);
    });
  }

  void selectText(int textTrackId) override {
    call("selectText", [&](JNIEnv* env) { env->CallVoidMethod(java_, gSelectText, static_cast<jint>(textTrackId)); });
  }

  void provideLicence(std::uint64_t requestId, std::shared_ptr<const Bytes> licence) override {
    call("provideLicence", [&](JNIEnv* env) {
      jbyteArray bytes = licence && !licence->empty() ? toBytes(env, *licence) : nullptr;
      env->CallVoidMethod(java_, gProvideLicence, static_cast<jlong>(requestId), bytes);
      if (bytes != nullptr) env->DeleteLocalRef(bytes);
    });
  }

  void unload() override { callVoid(gUnload, "unload"); }

  void shutdown() override {
    if (stopped_.exchange(true)) return;
    {
      std::lock_guard<std::mutex> lock(gRegistryMutex);
      gPlayers.erase(handle_);
    }
    {
      // Waits out an event being delivered right now; none starts after this.
      std::lock_guard<std::mutex> lock(sinkMutex_);
      sink_.reset();
    }
    if (java_ != nullptr) {
      ScopedEnv env;
      if (env) {
        // Only posts: the Java player releases ExoPlayer on its own looper and
        // removes its SurfaceView on the UI thread, which this never waits for.
        env->CallVoidMethod(java_, gShutdown);
        checkException(env.get(), "VideoPlayer.shutdown");
        env->DeleteGlobalRef(java_);
        gLiveRefs.fetch_sub(1);
      }
      java_ = nullptr;
    }
    if (fallback_) fallback_->sync([] {});
  }

 private:
  static jdoubleArray abrArray(JNIEnv* env, const AbrConfig& abr) {
    const jdouble values[7] = {abr.enabled ? 1.0 : 0.0,
                               abr.minBandwidth,
                               abr.maxBandwidth,
                               static_cast<jdouble>(abr.minWidth),
                               static_cast<jdouble>(abr.maxWidth),
                               static_cast<jdouble>(abr.minHeight),
                               static_cast<jdouble>(abr.maxHeight)};
    jdoubleArray array = env->NewDoubleArray(7);
    if (array == nullptr) {
      env->ExceptionClear();
      return nullptr;
    }
    env->SetDoubleArrayRegion(array, 0, 7, values);
    return array;
  }

  template <typename Body>
  void call(const char* what, Body&& body) {
    if (stopped_ || java_ == nullptr) return;
    ScopedEnv env;
    if (!env) return;
    body(env.get());
    checkException(env.get(), what);
  }

  void callVoid(jmethodID method, const char* what) {
    call(what, [&](JNIEnv* env) { env->CallVoidMethod(java_, method); });
  }

  std::string name_;
  jlong handle_ = 0;
  jobject java_ = nullptr;
  std::mutex sinkMutex_;
  std::shared_ptr<MediaSink> sink_;
  std::atomic<bool> stopped_{false};
  std::shared_ptr<net::IoQueue> fallback_;
};

/// The player a callback names, or null once it has shut down.
std::shared_ptr<AndroidPlayer> playerFor(jlong handle) {
  std::lock_guard<std::mutex> lock(gRegistryMutex);
  auto it = gPlayers.find(handle);
  return it == gPlayers.end() ? nullptr : it->second.lock();
}

template <typename Deliver>
void toSink(jlong handle, Deliver&& deliver) {
  if (auto player = playerFor(handle)) player->deliver(std::forward<Deliver>(deliver));
}

std::uint32_t serialOf(jint serial) { return static_cast<std::uint32_t>(serial); }

}  // namespace

// ---- the callbacks, registered in prepareAndroidMedia -------------------------------------

extern "C" {

JNIEXPORT void JNICALL screenkitMediaMetadata(JNIEnv* env, jclass, jlong handle, jint serial, jdouble duration,
                                              jboolean live, jdouble seekStart, jdouble seekEnd, jint width,
                                              jint height, jboolean hasVideo, jboolean hasAudio, jbyteArray manifest) {
  MediaInfo info;
  info.duration = duration;
  info.live = live == JNI_TRUE;
  info.seekStart = seekStart;
  info.seekEnd = seekEnd;
  info.width = width;
  info.height = height;
  info.hasVideo = hasVideo == JNI_TRUE;
  info.hasAudio = hasAudio == JNI_TRUE;
  info.manifest = fromBytes(env, manifest);
  toSink(handle, [&](MediaSink& sink) { sink.onMetadata(serialOf(serial), info); });
}

JNIEXPORT void JNICALL screenkitMediaState(JNIEnv*, jclass, jlong handle, jint serial, jint state) {
  if (state < 0 || state > static_cast<jint>(PlaybackState::Ended)) return;
  toSink(handle, [&](MediaSink& sink) { sink.onState(serialOf(serial), static_cast<PlaybackState>(state)); });
}

JNIEXPORT void JNICALL screenkitMediaTime(JNIEnv*, jclass, jlong handle, jint serial, jdouble position,
                                          jdouble seekStart, jdouble seekEnd) {
  toSink(handle, [&](MediaSink& sink) { sink.onTime(serialOf(serial), position, seekStart, seekEnd); });
}

JNIEXPORT void JNICALL screenkitMediaSeeked(JNIEnv*, jclass, jlong handle, jint serial, jdouble position) {
  toSink(handle, [&](MediaSink& sink) { sink.onSeeked(serialOf(serial), position); });
}

JNIEXPORT void JNICALL screenkitMediaBuffered(JNIEnv* env, jclass, jlong handle, jint serial, jdoubleArray ranges) {
  const std::vector<jdouble> flat = doubles(env, ranges);
  std::vector<TimeRange> list;
  for (size_t i = 0; i + 1 < flat.size(); i += 2) list.push_back(TimeRange{flat[i], flat[i + 1]});
  toSink(handle, [&](MediaSink& sink) { sink.onBuffered(serialOf(serial), list); });
}

JNIEXPORT void JNICALL screenkitMediaTracks(JNIEnv* env, jclass, jlong handle, jint serial, jintArray variantInts,
                                            jdoubleArray variantDoubles, jobjectArray variantStrings,
                                            jintArray audioInts, jobjectArray audioStrings, jintArray textInts,
                                            jobjectArray textStrings) {
  TrackList tracks;
  const std::vector<jint> vi = ints(env, variantInts);
  const std::vector<jdouble> vd = doubles(env, variantDoubles);
  for (size_t i = 0; i * 6 + 5 < vi.size() && i * 2 + 1 < vd.size(); ++i) {
    VariantTrack v;
    v.id = vi[i * 6];
    v.width = vi[i * 6 + 1];
    v.height = vi[i * 6 + 2];
    v.audioId = vi[i * 6 + 3];
    v.channels = vi[i * 6 + 4];
    v.active = vi[i * 6 + 5] != 0;
    v.bandwidth = vd[i * 2];
    v.frameRate = vd[i * 2 + 1];
    const auto base = static_cast<jsize>(i * 4);
    v.videoCodec = stringAt(env, variantStrings, base);
    v.audioCodec = stringAt(env, variantStrings, base + 1);
    v.language = stringAt(env, variantStrings, base + 2);
    v.label = stringAt(env, variantStrings, base + 3);
    tracks.variants.push_back(std::move(v));
  }
  const std::vector<jint> ai = ints(env, audioInts);
  for (size_t i = 0; i * 3 + 2 < ai.size(); ++i) {
    AudioTrack a;
    a.id = ai[i * 3];
    a.channels = ai[i * 3 + 1];
    a.active = ai[i * 3 + 2] != 0;
    const auto base = static_cast<jsize>(i * 4);
    a.language = stringAt(env, audioStrings, base);
    a.label = stringAt(env, audioStrings, base + 1);
    a.role = stringAt(env, audioStrings, base + 2);
    a.codec = stringAt(env, audioStrings, base + 3);
    tracks.audio.push_back(std::move(a));
  }
  const std::vector<jint> ti = ints(env, textInts);
  for (size_t i = 0; i * 3 + 2 < ti.size(); ++i) {
    TextTrack t;
    t.id = ti[i * 3];
    t.forced = ti[i * 3 + 1] != 0;
    t.active = ti[i * 3 + 2] != 0;
    const auto base = static_cast<jsize>(i * 4);
    t.language = stringAt(env, textStrings, base);
    t.label = stringAt(env, textStrings, base + 1);
    t.kind = stringAt(env, textStrings, base + 2);
    t.mimeType = stringAt(env, textStrings, base + 3);
    tracks.text.push_back(std::move(t));
  }
  toSink(handle, [&](MediaSink& sink) { sink.onTracks(serialOf(serial), tracks); });
}

JNIEXPORT void JNICALL screenkitMediaVariant(JNIEnv*, jclass, jlong handle, jint serial, jint id) {
  toSink(handle, [&](MediaSink& sink) { sink.onVariantChanged(serialOf(serial), id); });
}

JNIEXPORT void JNICALL screenkitMediaSize(JNIEnv*, jclass, jlong handle, jint serial, jint width, jint height) {
  toSink(handle, [&](MediaSink& sink) { sink.onSize(serialOf(serial), width, height); });
}

JNIEXPORT void JNICALL screenkitMediaCues(JNIEnv* env, jclass, jlong handle, jint serial, jint track,
                                          jdoubleArray times, jobjectArray texts) {
  const std::vector<jdouble> flat = doubles(env, times);
  const jsize count = texts == nullptr ? 0 : env->GetArrayLength(texts);
  std::vector<Cue> cues;
  for (jsize i = 0; i < count && static_cast<size_t>(i) * 2 + 1 < flat.size(); ++i) {
    Cue cue;
    cue.start = flat[static_cast<size_t>(i) * 2];
    cue.end = flat[static_cast<size_t>(i) * 2 + 1];
    cue.text = stringAt(env, texts, i);
    cues.push_back(std::move(cue));
  }
  toSink(handle, [&](MediaSink& sink) { sink.onCues(serialOf(serial), track, cues); });
}

JNIEXPORT void JNICALL screenkitMediaLicence(JNIEnv* env, jclass, jlong handle, jint serial, jlong requestId,
                                             jbyteArray keySystem, jbyteArray challenge) {
  const std::string system = fromBytes(env, keySystem);
  Bytes data = rawBytes(env, challenge);
  toSink(handle, [&](MediaSink& sink) {
    sink.onLicenceRequest(serialOf(serial), static_cast<std::uint64_t>(requestId), system, data, std::string());
  });
}

JNIEXPORT void JNICALL screenkitMediaStats(JNIEnv*, jclass, jlong handle, jint serial, jint width, jint height,
                                           jdouble frameRate, jdouble streamBandwidth, jdouble estimatedBandwidth,
                                           jlong decoded, jlong dropped, jlong corrupted) {
  MediaStats stats;
  stats.width = width;
  stats.height = height;
  stats.frameRate = frameRate;
  stats.streamBandwidth = streamBandwidth;
  stats.estimatedBandwidth = estimatedBandwidth;
  stats.decodedFrames = static_cast<std::uint64_t>(decoded < 0 ? 0 : decoded);
  stats.droppedFrames = static_cast<std::uint64_t>(dropped < 0 ? 0 : dropped);
  stats.corruptedFrames = static_cast<std::uint64_t>(corrupted < 0 ? 0 : corrupted);
  toSink(handle, [&](MediaSink& sink) { sink.onStats(serialOf(serial), stats); });
}

JNIEXPORT void JNICALL screenkitMediaError(JNIEnv* env, jclass, jlong handle, jint serial, jint kind,
                                           jint httpStatus, jbyteArray message) {
  const MediaErrorKind error =
      kind >= 0 && kind < kMediaErrorKindCount ? static_cast<MediaErrorKind>(kind) : MediaErrorKind::Media;
  const std::string text = fromBytes(env, message);
  toSink(handle, [&](MediaSink& sink) { sink.onError(serialOf(serial), error, httpStatus, text); });
}

}  // extern "C"

namespace {

const JNINativeMethod kCallbacks[] = {
    {"nativeMetadata", "(JIDZDDIIZZ[B)V", reinterpret_cast<void*>(screenkitMediaMetadata)},
    {"nativeState", "(JII)V", reinterpret_cast<void*>(screenkitMediaState)},
    {"nativeTime", "(JIDDD)V", reinterpret_cast<void*>(screenkitMediaTime)},
    {"nativeSeeked", "(JID)V", reinterpret_cast<void*>(screenkitMediaSeeked)},
    {"nativeBuffered", "(JI[D)V", reinterpret_cast<void*>(screenkitMediaBuffered)},
    {"nativeTracks", "(JI[I[D[[B[I[[B[I[[B)V", reinterpret_cast<void*>(screenkitMediaTracks)},
    {"nativeVariant", "(JII)V", reinterpret_cast<void*>(screenkitMediaVariant)},
    {"nativeSize", "(JIII)V", reinterpret_cast<void*>(screenkitMediaSize)},
    {"nativeCues", "(JII[D[[B)V", reinterpret_cast<void*>(screenkitMediaCues)},
    {"nativeLicence", "(JIJ[B[B)V", reinterpret_cast<void*>(screenkitMediaLicence)},
    {"nativeStats", "(JIIIDDDJJJ)V", reinterpret_cast<void*>(screenkitMediaStats)},
    {"nativeError", "(JIII[B)V", reinterpret_cast<void*>(screenkitMediaError)},
};

std::once_flag gCapabilitiesOnce;
MediaCapabilities gCapabilities;

}  // namespace

bool prepareAndroidMedia(JavaVM* vm, JNIEnv* env, std::string& error) {
  std::lock_guard<std::mutex> lock(gPrepareMutex);
  if (gReady.load()) return true;
  if (vm == nullptr || env == nullptr) {
    error = "no JavaVM or JNIEnv";
    return false;
  }
  gVm = vm;

  jclass local = env->FindClass(kPlayerClass);
  if (local == nullptr) {
    env->ExceptionClear();
    error = std::string("cannot find ") + kPlayerClass +
            " -- this has to run where the app's class loader is reachable";
    return false;
  }
  jclass localBytes = env->FindClass("[B");
  if (localBytes == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(local);
    error = "cannot find byte[]";
    return false;
  }

  // The error kinds are a wire format: VideoPlayer.ERR_COUNT must agree.
  jfieldID countField = env->GetStaticFieldID(local, "ERR_COUNT", "I");
  if (countField == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(localBytes);
    error = "VideoPlayer has no ERR_COUNT";
    return false;
  }
  const jint count = env->GetStaticIntField(local, countField);
  if (count != kJavaErrorCount) {
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(localBytes);
    error = "VideoPlayer.ERR_COUNT is " + std::to_string(count) + ", MediaPlayer.h has " +
            std::to_string(kMediaErrorKindCount) + " error kinds";
    return false;
  }

  struct Wanted {
    jmethodID* out;
    const char* name;
    const char* signature;
  };
  const Wanted wanted[] = {
      {&gCtor, "<init>", "(J)V"},
      {&gLoad, "load", "(I[BD[B[B[B[B[B[B[B[D)V"},
      {&gPlay, "play", "()V"},
      {&gPause, "pause", "()V"},
      {&gSeek, "seek", "(D)V"},
      {&gSetRate, "setRate", "(D)V"},
      {&gSetVolume, "setVolume", "(D)V"},
      {&gSetMuted, "setMuted", "(Z)V"},
      {&gSetPlane, "setPlane", "(DDDDZIII)V"},
      {&gSelectVariant, "selectVariant", "(I)V"},
      {&gSetAbr, "setAbr", "([D)V"},
      {&gSelectAudioLanguage, "selectAudioLanguage", "([B[B)V"},
      {&gSelectText, "selectText", "(I)V"},
      {&gProvideLicence, "provideLicence", "(J[B)V"},
      {&gUnload, "unload", "()V"},
      {&gShutdown, "shutdown", "()V"},
  };
  // One at a time: a pending NoSuchMethodError makes the next JNI call illegal.
  for (const Wanted& method : wanted) {
    *method.out = env->GetMethodID(local, method.name, method.signature);
    if (*method.out == nullptr) {
      env->ExceptionClear();
      env->DeleteLocalRef(local);
      env->DeleteLocalRef(localBytes);
      error = std::string("VideoPlayer has no ") + method.name + method.signature;
      return false;
    }
  }
  gCapabilitiesStatic = env->GetStaticMethodID(local, "capabilities", "()[Ljava/lang/String;");
  if (gCapabilitiesStatic == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(localBytes);
    error = "VideoPlayer has no static capabilities()";
    return false;
  }

  if (env->RegisterNatives(local, kCallbacks, static_cast<jint>(sizeof(kCallbacks) / sizeof(kCallbacks[0]))) != 0) {
    env->ExceptionClear();
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(localBytes);
    error = "RegisterNatives failed for VideoPlayer";
    return false;
  }

  gPlayerClass = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  env->DeleteLocalRef(localBytes);
  if (gPlayerClass == nullptr) {
    error = "cannot hold the VideoPlayer class";
    return false;
  }
  gReady.store(true);
  return true;
}

std::size_t liveMediaJavaRefCount() { return gLiveRefs.load(); }

std::shared_ptr<MediaPlayer> MediaPlayer::create(MediaConfig config) {
  auto player = std::make_shared<AndroidPlayer>(std::move(config));
  player->start();
  return player;
}

bool mediaAvailable() { return gReady.load(); }

MediaCapabilities mediaCapabilities() {
  std::call_once(gCapabilitiesOnce, [] {
    MediaCapabilities caps;
    caps.platform = "android";
    if (!gReady.load()) {
      caps.videoOutputProblem = "the Android media player is unavailable";
      gCapabilities = caps;
      return;
    }
    caps.available = true;
    caps.hls = true;
    caps.dash = true;
    caps.progressive = true;
    caps.videoOutput = true;
    caps.containers = {"video/mp4", "audio/mp4", "video/webm", "audio/webm", "video/mp2t",
                       "application/vnd.apple.mpegurl", "application/x-mpegurl", "application/dash+xml"};
    ScopedEnv env;
    if (env) {
      auto list = static_cast<jobjectArray>(env->CallStaticObjectMethod(gPlayerClass, gCapabilitiesStatic));
      if (!checkException(env.get(), "VideoPlayer.capabilities") && list != nullptr) {
        const jsize n = env->GetArrayLength(list);
        for (jsize i = 0; i < n; ++i) {
          auto item = static_cast<jstring>(env->GetObjectArrayElement(list, i));
          if (item == nullptr) continue;
          // ASCII only (key system and codec names), so plain UTF chars are safe.
          const char* chars = env->GetStringUTFChars(item, nullptr);
          const std::string entry = chars != nullptr ? chars : "";
          if (chars != nullptr) env->ReleaseStringUTFChars(item, chars);
          env->DeleteLocalRef(item);
          if (entry.rfind("keySystem:", 0) == 0) caps.keySystems.push_back(entry.substr(10));
          if (entry.rfind("codec:", 0) == 0) caps.codecs.push_back(entry.substr(6));
        }
        env->DeleteLocalRef(list);
      }
    }
    gCapabilities = caps;
  });
  return gCapabilities;
}

}  // namespace screenkit::media
