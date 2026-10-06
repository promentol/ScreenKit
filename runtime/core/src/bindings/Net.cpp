// Copyright (c) ScreenKit contributors. MIT.
//
// `__screenkit.net`, the handle API over net::NetService. Browser semantics --
// Headers, Request/Response, streams, XHR's and EventSource's state machines,
// WebSocket's readyState -- are JavaScript (runtime/js/dom-shim.js); this file
// only moves bytes and events across the thread boundary.
#include "Net.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "../loop/EventLoop.h"
#include "../net/NetService.h"
#include "EventChannel.h"
#include "ImageDecode.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

class OwnedArrayBuffer : public jsi::MutableBuffer {
 public:
  explicit OwnedArrayBuffer(net::Bytes bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t* data() override { return bytes_.data(); }

 private:
  net::Bytes bytes_;
};

jsi::Value arrayBuffer(jsi::Runtime& rt, net::Bytes bytes) {
  return jsi::ArrayBuffer(rt, std::make_shared<OwnedArrayBuffer>(std::move(bytes)));
}

/// A finite, non-negative integer that fits a size_t and a double exactly.
bool isCount(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

/// An RFC 7230 token: what a WebSocket subprotocol must be.
bool isToken(const std::string& text) {
  if (text.empty()) return false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) continue;
    if (std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != 0) continue;
    return false;
  }
  return true;
}

/// An ArrayBuffer, a typed array or a DataView, copied.
bool bytesFrom(jsi::Runtime& rt, const jsi::Value& value, net::Bytes& out) {
  if (!value.isObject()) return false;
  jsi::Object object = value.getObject(rt);
  if (object.isArrayBuffer(rt)) {
    jsi::ArrayBuffer buffer = object.getArrayBuffer(rt);
    const uint8_t* data = buffer.data(rt);
    out.assign(data, data + buffer.size(rt));
    return true;
  }
  jsi::Value inner = object.getProperty(rt, "buffer");
  if (!inner.isObject() || !inner.getObject(rt).isArrayBuffer(rt)) return false;
  jsi::ArrayBuffer buffer = inner.getObject(rt).getArrayBuffer(rt);
  const double offset = object.getProperty(rt, "byteOffset").asNumber();
  const double length = object.getProperty(rt, "byteLength").asNumber();
  const size_t total = buffer.size(rt);
  // NaN passes every comparison below as false, so finiteness is checked first.
  if (!isCount(offset) || !isCount(length) || offset + length > static_cast<double>(total)) return false;
  const uint8_t* data = buffer.data(rt) + static_cast<size_t>(offset);
  out.assign(data, data + static_cast<size_t>(length));
  return true;
}

std::string stringProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name,
                           const std::string& fallback) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isString() ? value.getString(rt).utf8(rt) : fallback;
}

bool boolProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, bool fallback) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isBool() ? value.getBool() : fallback;
}

std::uint64_t idArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, const char* fn) {
  if (count < 1 || !args[0].isNumber() || !isCount(args[0].getNumber())) {
    throw jsi::JSError(rt, std::string("__screenkit.net.") + fn + " requires an id (a non-negative integer)");
  }
  return static_cast<std::uint64_t>(args[0].getNumber());
}

jsi::Object errorPayload(jsi::Runtime& rt, net::NetError kind, const std::string& message) {
  jsi::Object payload(rt);
  payload.setProperty(rt, "kind", jsi::String::createFromAscii(rt, net::netErrorName(kind)));
  payload.setProperty(rt, "message", jsi::String::createFromUtf8(rt, message));
  return payload;
}

}  // namespace

/// JS-thread state: the callback for every open request and socket.
class NetBinding : public ChannelTarget, public std::enable_shared_from_this<NetBinding> {
 public:
  std::shared_ptr<net::NetService> service;
  /// Where `decodeImage` decodes: its own serial queue, so a large image holds
  /// up neither the JS thread nor the network. Made on first use.
  std::shared_ptr<net::IoQueue> decodeQueue;
  std::shared_ptr<JsExecutor> executor;
  std::weak_ptr<EventLoop> loop;
  std::string tag;
  std::unordered_map<std::uint64_t, std::shared_ptr<jsi::Function>> callbacks;
  std::uint64_t nextId = 1;
  bool stopped = false;

  std::uint64_t add(std::shared_ptr<jsi::Function> callback) {
    const std::uint64_t id = nextId++;
    callbacks.emplace(id, std::move(callback));
    if (auto live = loop.lock()) live->holdWork();
    return id;
  }

  void forget(std::uint64_t id) {
    if (callbacks.erase(id) == 0) return;
    if (auto live = loop.lock()) live->releaseWork();
  }

  bool channelStopped() const override { return stopped; }
  const std::string& channelTag() const override { return tag; }

  /// One event, on the JS thread. A terminal event forgets the id first, so
  /// whatever the callback does next (a new request, say) sees the old one gone.
  void deliver(jsi::Runtime& rt, std::uint64_t id, const char* type, const jsi::Value& payload,
               bool terminal) override {
    if (stopped) return;
    const auto it = callbacks.find(id);
    if (it == callbacks.end()) return;
    std::shared_ptr<jsi::Function> callback = it->second;
    if (terminal) forget(id);
    try {
      callback->call(rt, jsi::String::createFromAscii(rt, type), payload);
    } catch (const jsi::JSError& e) {
      log(LogLevel::Error, tag,
          std::string("uncaught error in a network ") + type + " callback: " + e.getMessage() + "\n" + e.getStack());
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, tag, std::string("JSI error in a network ") + type + " callback: " + e.what());
    } catch (const std::exception& e) {
      log(LogLevel::Error, tag, std::string("exception in a network ") + type + " callback: " + e.what());
    }
  }

  void shutdown() {
    if (stopped) return;
    stopped = true;
    if (service) service->shutdown();
    if (auto live = loop.lock()) {
      for (size_t i = 0; i < callbacks.size(); ++i) live->releaseWork();
    }
    callbacks.clear();
  }
};

namespace {

class JsHttpSink final : public net::HttpSink {
 public:
  explicit JsHttpSink(Poster poster) : poster_(std::move(poster)) {}

  void onHead(net::ResponseHead head) override {
    auto shared = std::make_shared<net::ResponseHead>(std::move(head));
    poster_.post("head", false, [shared](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object payload(rt);
      payload.setProperty(rt, "status", shared->status);
      payload.setProperty(rt, "statusText", jsi::String::createFromUtf8(rt, shared->statusText));
      payload.setProperty(rt, "url", jsi::String::createFromUtf8(rt, shared->url));
      payload.setProperty(rt, "redirected", shared->redirected);
      jsi::Array headers(rt, shared->headers.size());
      for (size_t i = 0; i < shared->headers.size(); ++i) {
        jsi::Array pair(rt, 2);
        pair.setValueAtIndex(rt, 0, jsi::String::createFromUtf8(rt, shared->headers[i].first));
        pair.setValueAtIndex(rt, 1, jsi::String::createFromUtf8(rt, shared->headers[i].second));
        headers.setValueAtIndex(rt, i, std::move(pair));
      }
      payload.setProperty(rt, "headers", std::move(headers));
      return payload;
    });
  }

  void onData(net::Bytes chunk) override {
    auto shared = std::make_shared<net::Bytes>(std::move(chunk));
    poster_.post("data", false,
                 [shared](jsi::Runtime& rt) -> jsi::Value { return arrayBuffer(rt, std::move(*shared)); });
  }

  void onEnd() override {
    poster_.post("end", true, [](jsi::Runtime&) -> jsi::Value { return jsi::Value::undefined(); });
  }

  void onError(net::NetError kind, std::string message) override {
    poster_.post("error", true, [kind, message](jsi::Runtime& rt) -> jsi::Value {
      return errorPayload(rt, kind, message);
    });
  }

  void onUploadProgress(std::uint64_t sent, std::int64_t total, bool complete) override {
    poster_.post("upload", false, [sent, total, complete](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object payload(rt);
      payload.setProperty(rt, "loaded", static_cast<double>(sent));
      payload.setProperty(rt, "total", static_cast<double>(total));
      payload.setProperty(rt, "complete", complete);
      return payload;
    });
  }

 private:
  Poster poster_;
};

class JsSocketSink final : public net::SocketSink {
 public:
  explicit JsSocketSink(Poster poster) : poster_(std::move(poster)) {}

  void onOpen(std::string protocol, std::string extensions) override {
    poster_.post("open", false, [protocol, extensions](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object payload(rt);
      payload.setProperty(rt, "protocol", jsi::String::createFromUtf8(rt, protocol));
      payload.setProperty(rt, "extensions", jsi::String::createFromUtf8(rt, extensions));
      return payload;
    });
  }

  void onMessage(bool text, net::Bytes data) override {
    auto shared = std::make_shared<net::Bytes>(std::move(data));
    poster_.post("message", false, [shared, text](jsi::Runtime& rt) -> jsi::Value {
      if (text) return jsi::String::createFromUtf8(rt, shared->data(), shared->size());
      return arrayBuffer(rt, std::move(*shared));
    });
  }

  void onSent(std::uint64_t payloadBytes) override {
    poster_.post("sent", false,
                 [payloadBytes](jsi::Runtime&) -> jsi::Value { return jsi::Value(static_cast<double>(payloadBytes)); });
  }

  void onError(net::NetError kind, std::string message) override {
    poster_.post("error", false, [kind, message](jsi::Runtime& rt) -> jsi::Value {
      return errorPayload(rt, kind, message);
    });
  }

  void onClose(int code, std::string reason, bool wasClean) override {
    poster_.post("close", true, [code, reason, wasClean](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object payload(rt);
      payload.setProperty(rt, "code", code);
      payload.setProperty(rt, "reason", jsi::String::createFromUtf8(rt, reason));
      payload.setProperty(rt, "wasClean", wasClean);
      return payload;
    });
  }

 private:
  Poster poster_;
};

std::shared_ptr<jsi::Function> callbackArg(jsi::Runtime& rt, const jsi::Value& value, const char* fn) {
  if (!value.isObject() || !value.getObject(rt).isFunction(rt)) {
    throw jsi::JSError(rt, std::string("__screenkit.net.") + fn + " requires an event callback");
  }
  return std::make_shared<jsi::Function>(value.getObject(rt).getFunction(rt));
}

void setFunction(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned params,
                 jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name), params,
                                                           std::move(body)));
}

}  // namespace

std::shared_ptr<NetBinding> installNet(jsi::Runtime& runtime, std::shared_ptr<JsExecutor> executor,
                                       std::shared_ptr<EventLoop> loop, const RuntimeConfig& config) {
  auto binding = std::make_shared<NetBinding>();
  net::NetConfig netConfig;
  netConfig.name = config.name;
  netConfig.testTlsAnchors = config.testTlsAnchors;
  binding->service = net::NetService::create(std::move(netConfig));
  binding->executor = std::move(executor);
  binding->loop = loop;
  binding->tag = config.name;

  std::weak_ptr<NetBinding> weak = binding;
  jsi::Object api(runtime);

  setFunction(runtime, api, "request", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (!b || b->stopped) throw jsi::JSError(rt, "the network layer has shut down");
                if (count < 2 || !args[0].isObject()) {
                  throw jsi::JSError(rt, "__screenkit.net.request requires options and a callback");
                }
                jsi::Object options = args[0].getObject(rt);
                auto callback = callbackArg(rt, args[1], "request");

                net::HttpRequestSpec spec;
                const std::string url = stringProperty(rt, options, "url", "");
                std::string error;
                if (!net::parseUrl(url, "http", spec.url, error)) throw jsi::JSError(rt, error);
                spec.method = stringProperty(rt, options, "method", "GET");
                if (spec.method.empty() || spec.method.find_first_of(" \r\n\t") != std::string::npos) {
                  throw jsi::JSError(rt, "invalid HTTP method");
                }
                jsi::Value headers = options.getProperty(rt, "headers");
                if (headers.isObject() && headers.getObject(rt).isArray(rt)) {
                  jsi::Array list = headers.getObject(rt).getArray(rt);
                  const size_t n = list.size(rt);
                  for (size_t i = 0; i < n; ++i) {
                    jsi::Value entry = list.getValueAtIndex(rt, i);
                    if (!entry.isObject() || !entry.getObject(rt).isArray(rt)) continue;
                    jsi::Array pair = entry.getObject(rt).getArray(rt);
                    if (pair.size(rt) < 2) continue;
                    spec.headers.emplace_back(pair.getValueAtIndex(rt, 0).toString(rt).utf8(rt),
                                              pair.getValueAtIndex(rt, 1).toString(rt).utf8(rt));
                  }
                }
                jsi::Value body = options.getProperty(rt, "body");
                if (!body.isUndefined() && !body.isNull()) {
                  net::Bytes bytes;
                  if (!bytesFrom(rt, body, bytes)) {
                    throw jsi::JSError(rt, "__screenkit.net.request: body must be an ArrayBuffer or a view");
                  }
                  spec.body = std::make_shared<const net::Bytes>(std::move(bytes));
                }
                spec.streamingBody = boolProperty(rt, options, "stream", false);
                if (spec.streamingBody) spec.body.reset();
                const std::string redirect = stringProperty(rt, options, "redirect", "follow");
                spec.redirect = redirect == "manual" ? net::RedirectMode::Manual
                                : redirect == "error" ? net::RedirectMode::Error
                                                      : net::RedirectMode::Follow;
                spec.useCookies = boolProperty(rt, options, "cookies", true);
                spec.reportUpload = boolProperty(rt, options, "upload", false);
                jsi::Value window = options.getProperty(rt, "flowWindow");
                if (!window.isUndefined()) {
                  if (!window.isNumber() || !isCount(window.getNumber())) {
                    throw jsi::JSError(rt, "__screenkit.net.request: flowWindow must be a non-negative integer");
                  }
                  spec.flowWindow = static_cast<std::uint64_t>(window.getNumber());
                }

                const std::uint64_t id = b->add(std::move(callback));
                auto sink = std::make_shared<JsHttpSink>(Poster(weak, b->executor, id));
                b->service->startRequest(id, std::move(spec), std::move(sink));
                return jsi::Value(static_cast<double>(id));
              });

  setFunction(runtime, api, "write", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "write");
                net::Bytes bytes;
                if (count < 2 || !bytesFrom(rt, args[1], bytes)) {
                  throw jsi::JSError(rt, "__screenkit.net.write requires bytes");
                }
                if (b && !b->stopped && !bytes.empty()) {
                  b->service->appendRequestBody(id, std::make_shared<const net::Bytes>(std::move(bytes)));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "finish", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "finish");
                if (b && !b->stopped) b->service->finishRequestBody(id);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "abort", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "abort");
                if (b && !b->stopped) {
                  b->forget(id);
                  b->service->abortRequest(id);
                }
                return jsi::Value::undefined();
              });

  // Encoded image bytes -> `{width, height, data}` RGBA, decoded off the JS
  // thread and delivered as one "image" or "error" event -- how a downloaded
  // image or a Blob reaches pixels without costing a frame. The bytes are copied
  // here, so the caller's buffer may change afterwards.
  setFunction(runtime, api, "decodeImage", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (!b || b->stopped) throw jsi::JSError(rt, "the network layer has shut down");
                net::Bytes bytes;
                if (count < 2 || !bytesFrom(rt, args[0], bytes)) {
                  throw jsi::JSError(rt, "__screenkit.net.decodeImage requires bytes and a callback");
                }
                auto callback = callbackArg(rt, args[1], "decodeImage");
                if (!b->decodeQueue) b->decodeQueue = net::makeIoQueue(b->tag + ".decode");
                const std::uint64_t id = b->add(std::move(callback));
                Poster poster(weak, b->executor, id);
                auto shared = std::make_shared<net::Bytes>(std::move(bytes));
                b->decodeQueue->post([poster, shared]() mutable {
                  auto image = std::make_shared<DecodedImage>();
                  std::string error;
                  const bool ok = decodeImage(shared->data(), shared->size(), *image, error);
                  shared.reset();
                  if (!ok) {
                    poster.post("error", true, [error](jsi::Runtime& rt2) -> jsi::Value {
                      return errorPayload(rt2, net::NetError::Decode, error);
                    });
                    return;
                  }
                  poster.post("image", true, [image](jsi::Runtime& rt2) -> jsi::Value {
                    return imageObject(rt2, *image);
                  });
                });
                return jsi::Value(static_cast<double>(id));
              });

  // A request made with a flowWindow reads no further ahead of the page than
  // that: the page reports here what it has read.
  setFunction(runtime, api, "acknowledge", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "acknowledge");
                if (count < 2 || !args[1].isNumber() || !isCount(args[1].getNumber())) {
                  throw jsi::JSError(rt, "__screenkit.net.acknowledge requires a byte count (a non-negative integer)");
                }
                if (b && !b->stopped) {
                  b->service->acknowledgeResponseData(id, static_cast<std::uint64_t>(args[1].getNumber()));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "openSocket", 3,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (!b || b->stopped) throw jsi::JSError(rt, "the network layer has shut down");
                if (count < 3 || !args[0].isString()) {
                  throw jsi::JSError(rt, "__screenkit.net.openSocket requires a URL, protocols and a callback");
                }
                net::Url url;
                std::string error;
                if (!net::parseUrl(args[0].getString(rt).utf8(rt), "ws", url, error)) {
                  throw jsi::JSError(rt, error);
                }
                std::vector<std::string> protocols;
                if (args[1].isObject() && args[1].getObject(rt).isArray(rt)) {
                  jsi::Array list = args[1].getObject(rt).getArray(rt);
                  for (size_t i = 0; i < list.size(rt); ++i) {
                    std::string protocol = list.getValueAtIndex(rt, i).toString(rt).utf8(rt);
                    if (!isToken(protocol)) {
                      throw jsi::JSError(rt, "__screenkit.net.openSocket: \"" + protocol +
                                                 "\" is not a subprotocol token (RFC 7230)");
                    }
                    protocols.push_back(std::move(protocol));
                  }
                }
                auto callback = callbackArg(rt, args[2], "openSocket");
                const std::uint64_t id = b->add(std::move(callback));
                auto sink = std::make_shared<JsSocketSink>(Poster(weak, b->executor, id));
                b->service->openSocket(id, std::move(url), std::move(protocols), std::move(sink));
                return jsi::Value(static_cast<double>(id));
              });

  setFunction(runtime, api, "send", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "send");
                if (count < 2) throw jsi::JSError(rt, "__screenkit.net.send requires data");
                bool text = false;
                net::Bytes bytes;
                if (args[1].isString()) {
                  const std::string utf8 = args[1].getString(rt).utf8(rt);
                  bytes.assign(utf8.begin(), utf8.end());
                  text = true;
                } else if (!bytesFrom(rt, args[1], bytes)) {
                  throw jsi::JSError(rt, "__screenkit.net.send requires a string or bytes");
                }
                if (b && !b->stopped) {
                  b->service->sendSocket(id, text, std::make_shared<const net::Bytes>(std::move(bytes)));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "close", 3,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "close");
                int code = 0;
                if (count > 1 && !args[1].isUndefined()) {
                  const double raw = args[1].isNumber() ? args[1].getNumber() : -1;
                  if (!isCount(raw) || !(raw == 0 || raw == 1000 || (raw >= 3000 && raw <= 4999))) {
                    throw jsi::JSError(rt, "__screenkit.net.close: the code must be 0 (none), 1000, or 3000-4999");
                  }
                  code = static_cast<int>(raw);
                }
                const std::string reason = count > 2 && args[2].isString() ? args[2].getString(rt).utf8(rt) : "";
                if (reason.size() > 123) {
                  throw jsi::JSError(rt, "__screenkit.net.close: the reason must be at most 123 UTF-8 bytes");
                }
                if (b && !b->stopped) b->service->closeSocket(id, code, reason);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "getCookies", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (count < 1 || !args[0].isString()) throw jsi::JSError(rt, "getCookies requires a URL");
                net::Url url;
                std::string error;
                if (!net::parseUrl(args[0].getString(rt).utf8(rt), "http", url, error)) throw jsi::JSError(rt, error);
                return jsi::String::createFromUtf8(rt, b && !b->stopped ? b->service->cookiesFor(url) : "");
              });

  setFunction(runtime, api, "setCookie", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (count < 2 || !args[0].isString() || !args[1].isString()) {
                  throw jsi::JSError(rt, "setCookie requires a URL and a cookie string");
                }
                net::Url url;
                std::string error;
                if (!net::parseUrl(args[0].getString(rt).utf8(rt), "http", url, error)) throw jsi::JSError(rt, error);
                return jsi::Value(b && !b->stopped && b->service->setCookie(url, args[1].getString(rt).utf8(rt)));
              });

  jsi::Value io = runtime.global().getProperty(runtime, "__screenkit");
  if (!io.isObject()) {
    throw jsi::JSError(runtime, "installNet: __screenkit is missing -- installHostIO runs first");
  }
  io.getObject(runtime).setProperty(runtime, "net", std::move(api));
  return binding;
}

void shutdownNet(NetBinding& binding) { binding.shutdown(); }

}  // namespace screenkit
