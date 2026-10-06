// Copyright (c) ScreenKit contributors. MIT.
//
// The seam. One runtime's network layer, as an interface with one
// implementation per platform: NSURLSession on Apple (NetServiceApple.mm),
// OkHttp over JNI on Android (NetServiceAndroid.cpp), a vendored cpp-httplib on
// Linux, which has no platform client (NetServiceLinux.cpp), and an Unavailable
// one anywhere else. This is React Native's shape -- the JS layer above is a
// browser API, the native side is the platform's own HTTP client -- and the
// method list below is RN's `Networking` module's surface.
//
// Ids are the caller's, and unique among a service's live requests and among
// its live sockets: every client refuses one already in use -- the new call
// fails with `Url` -- rather than strand the call that holds it
// (`net-seam-contract`).
//
// `bindings/Net.cpp` is the only caller. Every entry point is safe from any
// thread (in practice the JS thread): an implementation either posts to its own
// serial queue or hands the work straight to a thread-safe platform object.
//
// Nothing here knows about JSI. Events leave through `HttpSink` and
// `SocketSink`, which the binding implements by posting event-loop tasks; that
// is what makes completions wait behind a paused runtime's freeze gate.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Bytes.h"
#include "IoQueue.h"
#include "NetError.h"
#include "Url.h"

namespace screenkit::net {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

enum class RedirectMode { Follow, Manual, Error };

/// Pinned for the same reason `NetError`'s numbering is (NetError.h): this
/// crosses JNI as a plain int and `HttpClient.java`'s `REDIRECT_*` constants are
/// the other half of it.
static_assert(static_cast<int>(RedirectMode::Follow) == 0, "RedirectMode is a wire format");
static_assert(static_cast<int>(RedirectMode::Manual) == 1, "RedirectMode is a wire format");
static_assert(static_cast<int>(RedirectMode::Error) == 2, "RedirectMode is a wire format");

struct HttpRequestSpec {
  std::string method = "GET";  // already upper-cased where the method is a known one
  Url url;
  HeaderList headers;          // already filtered of forbidden names by JS
  std::shared_ptr<const Bytes> body;  // a complete body, or null
  bool streamingBody = false;  // body arrives through appendBody/endBody, sent chunked
  RedirectMode redirect = RedirectMode::Follow;
  bool useCookies = true;      // false for credentials: 'omit'
  bool reportUpload = false;   // XHR's upload progress
  /// Response bytes the receiver may hold unread before reading from the network
  /// stops, resuming as `acknowledge` reports them read. 0: read as fast as the
  /// data arrives -- for receivers that consume it as it comes.
  std::uint64_t flowWindow = 0;
};

struct ResponseHead {
  int status = 0;
  std::string statusText;
  std::string url;             // after redirects
  bool redirected = false;
  HeaderList headers;          // names lower-cased
};

/// What a client emits for one request, from its head to its last byte. The JS
/// binding turns each call into an event-loop task.
class HttpSink {
 public:
  virtual ~HttpSink() = default;
  virtual void onHead(ResponseHead head) = 0;
  virtual void onData(Bytes chunk) = 0;
  virtual void onEnd() = 0;
  virtual void onError(NetError kind, std::string message) = 0;
  /// `total` is -1 for a streamed body. `complete` once every body byte has
  /// been handed to the network.
  virtual void onUploadProgress(std::uint64_t sent, std::int64_t total, bool complete) = 0;
};

/// What a client emits for one WebSocket.
class SocketSink {
 public:
  virtual ~SocketSink() = default;
  virtual void onOpen(std::string protocol, std::string extensions) = 0;
  virtual void onMessage(bool text, Bytes data) = 0;
  /// `payloadBytes` of application data has been handed to the network;
  /// `bufferedAmount` goes down by that much.
  virtual void onSent(std::uint64_t payloadBytes) = 0;
  /// The connection failed. Always followed by onClose with 1006.
  virtual void onError(NetError kind, std::string message) = 0;
  virtual void onClose(int code, std::string reason, bool wasClean) = 0;
};

struct NetConfig {
  std::string name = "screenkit";
  /// Test-only extra TLS trust anchors (DER), from `RuntimeConfig`. Never
  /// reachable from JS.
  std::vector<Bytes> testTlsAnchors;
  std::string userAgent = "Mozilla/5.0 (ScreenKit)";
};

class NetService {
 public:
  /// The platform's client. One per runtime.
  static std::shared_ptr<NetService> create(NetConfig config);

  virtual ~NetService() = default;

  NetService(const NetService&) = delete;
  NetService& operator=(const NetService&) = delete;

  virtual void startRequest(std::uint64_t id, HttpRequestSpec spec, std::shared_ptr<HttpSink> sink) = 0;
  virtual void appendRequestBody(std::uint64_t id, std::shared_ptr<const Bytes> chunk) = 0;
  virtual void finishRequestBody(std::uint64_t id) = 0;
  /// Close the request's connection and forget it; its sink hears nothing more.
  virtual void abortRequest(std::uint64_t id) = 0;
  /// The receiver has read `bytes` more of the response (HttpRequestSpec::flowWindow).
  virtual void acknowledgeResponseData(std::uint64_t id, std::uint64_t bytes) = 0;

  virtual void openSocket(std::uint64_t id, Url url, std::vector<std::string> protocols,
                          std::shared_ptr<SocketSink> sink) = 0;
  virtual void sendSocket(std::uint64_t id, bool text, std::shared_ptr<const Bytes> payload) = 0;
  virtual void closeSocket(std::uint64_t id, int code, std::string reason) = 0;

  /// The JS API's view of the platform cookie store: HttpOnly cookies are
  /// invisible and cannot be set from there.
  virtual std::string cookiesFor(const Url& url) = 0;
  virtual bool setCookie(const Url& url, const std::string& setCookie) = 0;

  /// Close every request and socket and drop every sink. Synchronous and
  /// idempotent; nothing is delivered after it returns.
  virtual void shutdown() = 0;

 protected:
  NetService() = default;
};

/// False on a platform with no client yet, where every request fails with
/// `Unsupported`.
bool networkAvailable();

}  // namespace screenkit::net
