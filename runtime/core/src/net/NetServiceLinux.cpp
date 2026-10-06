// Copyright (c) ScreenKit contributors. MIT.
//
// Linux's network client: cpp-httplib over the image's own OpenSSL, behind the
// `NetService` seam.
//
// Linux is the one target with no platform HTTP client to put behind that seam
// -- there is no NSURLSession and no OkHttp -- so this is the exception to
// "the client is the platform's": a single vendored MIT header
// (runtime/third_party/httplib/VENDOR.md) speaks HTTP/1.1 and RFC 6455, and
// everything that would be a security decision is still the image's. TLS is the
// image's OpenSSL and the roots are the image's CA bundle, found at run time;
// nothing about either ships in this repo.
//
// What this file owns that the other two clients do not, and why:
//
//   - **Redirects.** cpp-httplib can follow them itself, but then the jar below
//     would not see a hop's `Set-Cookie`, the final URL would be guesswork and
//     `redirect: 'manual'`/`'error'` would have nowhere to hook in. They are
//     followed here instead, by the Fetch rules.
//   - **Cookies.** `LinuxCookieJar`, because there is no platform store to point
//     at. In memory for the runtime's life (spec-linux-http-client.md).
//   - **Connection reuse and the six-per-origin limit.** NSURLSession and OkHttp
//     have dispatchers; here it is a pool of idle `httplib::Client`s per origin
//     plus an admission queue on the I/O queue.
//   - **Name resolution.** `getaddrinfo` cannot be interrupted, and cpp-httplib
//     calls it on the request's thread, so an abort or a shutdown used to wait
//     for the resolver. Names are resolved here instead, on a thread whose
//     answer can be abandoned (`startResolving`), and cpp-httplib is handed the
//     address through `set_hostname_addr_map` -- the host name is still the
//     SNI, the `Host` header and the name the certificate is checked against.
//   - **Content-Encoding.** cpp-httplib's decoder reads zlib and gzip but not a
//     raw DEFLATE stream, which servers send as `deflate` often enough that
//     every browser accepts it, so bodies are decoded here (`BodyDecoder`).
//
// Threading. cpp-httplib is synchronous: a request blocks its thread from the
// first byte written to the last byte read. So each admitted request gets a
// worker thread of its own and every sink call is posted back onto the runtime's
// one serial `IoQueue`, which is therefore still the only place a sink is
// touched, in order, without locks -- the same contract as the other two
// clients. **Nothing blocking ever runs on that queue**: a worker is never
// joined from it (see `reap`), and a socket's writes go to a thread of their
// own rather than stalling every other event behind a peer that stopped reading.
// Threads are bounded twice over for requests (`kMaxPerOrigin`, `kMaxInFlight`),
// because a page that opens fifty images must not open fifty threads on a Pi 3
// with 908 MB of RAM.
//
// Cancellation. Every blocking point a request or a socket can be in has a way
// out that another thread can take: a name lookup is abandoned, a flow-control
// or upload wait is woken, and a socket -- connecting, in its TLS handshake, or
// reading -- is shut down under cpp-httplib through a duplicate of its
// descriptor taken the moment it exists (`trackSocket`). That last one matters
// twice: cpp-httplib holds its socket mutex for a whole connect and handshake,
// so its own `stop()` *waits* for them, and called from the I/O queue that
// stalled every event in the runtime for as long as a connect to an address
// that never answers takes. The duplicate is shut down first, the connect fails
// at once, and `stop()` then has nothing to wait for.
//
// Backpressure is real here, and is the one thing this client does better than
// Apple's. The reader callback runs on the request's own thread, so blocking in
// it stops the socket being drained and the server stalls -- OkHttp's mechanism,
// reached a different way. (Returning false from the callback is *cancellation*
// in cpp-httplib, not a pause, so it is only used for an abort.)
//
// **What this client depends on that is not cpp-httplib's public API**, listed
// here because the vendoring story is cheap version bumps and these are what a
// bump has to be re-checked against (runtime/third_party/httplib/VENDOR.md):
//
//   - `Request::content_provider_`, `Request::content_length_` and
//     `Request::is_chunked_content_provider_`, plus
//     `detail::ContentProviderAdapter`. Every request body goes out through a
//     provider rather than `Request::body`, for two reasons: a streamed body has
//     to be pulled from JS as the socket takes it, and a body with no
//     `Content-Type` must go out with **no** such header, which the public
//     `Post` overloads cannot do -- they write `text/plain` into any request
//     that has a non-empty `Request::body` and no type of its own
//     (`ClientImpl::write_request`), which is a lie about the bytes.
//   - `WebSocketClient::peer_close_payload()`, which is not upstream's at all:
//     upstream discards a Close frame's payload, and tools/vendor/httplib.rules
//     patches the header to keep it. The one patch; VENDOR.md lists it.
//   - `set_hostname_addr_map` and `set_socket_options` doing what their names
//     say for both `Client` and `WebSocketClient`: the first is how an address
//     resolved here reaches the connect, the second how its socket is tracked.

// Set before the include, and only here: this is the single translation unit
// that compiles the vendored header. The one that changes its *class layouts* --
// OPENSSL_SUPPORT -- is set by CMake on the whole target instead, so a second
// file that ever includes this header cannot disagree with this one about it
// (an ODR violation that would link and then misbehave). ZLIB_SUPPORT is not
// set at all: bodies are decoded here (`BodyDecoder`), not by the library.
// EXPECT_100_THRESHOLD changes no layout and stays here with its reason:
// upstream adds `Expect: 100-continue` to any body over 1 KiB, browsers never
// send it, and a server that ignores it costs the request a second of dead time.
#define CPPHTTPLIB_EXPECT_100_THRESHOLD 0
#include <httplib.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "IoQueue.h"
#include "LinuxCookieJar.h"
#include "NetService.h"

namespace screenkit::net {
namespace {

constexpr const char* kTag = "screenkit.net";

/// Browsers open six connections per origin over HTTP/1.1, and `net-flow-control`
/// asserts exactly that number at the server.
constexpr std::size_t kMaxPerOrigin = 6;
/// ...and a ceiling across every origin, because each in-flight request here is
/// a thread. A page reaching twenty origins queues rather than spawning a
/// hundred threads on a device with 908 MB of RAM.
constexpr std::size_t kMaxInFlight = 16;
/// The Fetch redirect limit. `net-redirect` pins it: twenty hops arrive, the
/// twenty-first is a network error.
constexpr int kMaxRedirects = 20;
/// What may sit between a page writing a body -- a streamed request body or a
/// WebSocket's `send` -- and a peer that has stopped reading. Neither
/// `__screenkit.net.write` nor `send` can push back on the page, so the choice
/// is a bound or an unbounded queue, and a failed upload beats an OOM. Apple's
/// upload pump uses the same number for the same reason.
constexpr std::uint64_t kMaxPendingUpload = 8 * 1024 * 1024;
/// How long a socket's reader waits for a frame before looking at `cancelled`.
/// cpp-httplib's WebSocket has no cancellation of its own and destroying the
/// client under a blocked `read()` would be a use-after-free, so a bounded read
/// is what makes teardown prompt. Only a timeout set at run time is reported as
/// `Timeout` rather than closing the connection, which is exactly this.
constexpr time_t kSocketReadPollMs = 250;
/// An idle pooled connection is closed after this long. Six keep-alive sockets
/// per origin held for the runtime's life is a lot of file descriptors on a
/// device that has few.
constexpr auto kIdleConnectionLife = std::chrono::seconds(60);

// ---- the image's trust store ---------------------------------------------------
//
// Found at run time and never shipped: a CA bundle in the repo would be a set of
// roots nobody updates. The list is every place the distributions this runtime
// targets put theirs, Batocera's Buildroot layout first.

const char* const kCaBundleFiles[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Buildroot/Batocera, Debian, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",              // openSUSE
    "/etc/ssl/cert.pem",                   // Alpine, BSD
    "/usr/local/share/certs/ca-root-nss.crt",
    "/etc/certs/ca-certificates.crt",
};
const char* const kCaBundleDirs[] = {"/etc/ssl/certs", "/etc/pki/tls/certs"};

struct TrustPaths {
  std::string file;
  std::string dir;
};

bool isFile(const char* path) {
  if (path == nullptr || *path == '\0') return false;
  struct stat info {};
  return ::stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool isDirectory(const char* path) {
  if (path == nullptr || *path == '\0') return false;
  struct stat info {};
  return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/// Probed once. `SSL_CERT_FILE`/`SSL_CERT_DIR` win, as they do for every other
/// OpenSSL program on the image -- which is also how the suite points a runtime
/// with no test anchors at the fixture's own CA (`net-image-ca`).
const TrustPaths& trustPaths() {
  static const TrustPaths paths = [] {
    TrustPaths found;
    if (const char* file = std::getenv("SSL_CERT_FILE"); isFile(file)) found.file = file;
    if (const char* dir = std::getenv("SSL_CERT_DIR"); isDirectory(dir)) found.dir = dir;
    if (found.file.empty()) {
      for (const char* candidate : kCaBundleFiles) {
        if (isFile(candidate)) {
          found.file = candidate;
          break;
        }
      }
    }
    // A directory of hashed roots is only worth looking for when there is no
    // bundle file: an image that has both has them saying the same thing.
    if (found.file.empty() && found.dir.empty()) {
      for (const char* candidate : kCaBundleDirs) {
        if (isDirectory(candidate)) {
          found.dir = candidate;
          break;
        }
      }
    }
    if (found.file.empty() && found.dir.empty()) {
      log(LogLevel::Warn, kTag,
          "no CA bundle found on this image (looked in /etc/ssl/certs, /etc/pki/tls/certs and "
          "SSL_CERT_FILE); falling back to OpenSSL's built-in paths, and https may fail");
    } else {
      log(LogLevel::Log, kTag, "TLS roots: " + (found.file.empty() ? found.dir : found.file));
    }
    return found;
  }();
  return paths;
}

/// The test suite's extra anchors, **added** to the image's roots and never
/// replacing them: `testTlsAnchors` exists so the fixture's generated CA is
/// trusted alongside the real ones, not instead of them. Null when the store
/// cannot be built or an anchor cannot be added, and the caller then falls back
/// to the image's roots by path rather than carrying on with a half-built store.
X509_STORE* storeWithAnchors(const std::vector<Bytes>& anchors) {
  X509_STORE* store = X509_STORE_new();
  if (store == nullptr) {
    ERR_clear_error();
    return nullptr;
  }
  const TrustPaths& paths = trustPaths();
  bool loaded = false;
  if (!paths.file.empty() || !paths.dir.empty()) {
    loaded = X509_STORE_load_locations(store, paths.file.empty() ? nullptr : paths.file.c_str(),
                                       paths.dir.empty() ? nullptr : paths.dir.c_str()) == 1;
  }
  if (!loaded) loaded = X509_STORE_set_default_paths(store) == 1;
  if (!loaded) {
    // Nothing but the test anchors would be trusted, which is a different trust
    // decision from the one that was asked for.
    ERR_clear_error();
    X509_STORE_free(store);
    log(LogLevel::Warn, kTag, "could not load this image's CA roots into the test trust store");
    return nullptr;
  }
  for (const Bytes& der : anchors) {
    const unsigned char* at = der.data();
    X509* certificate = d2i_X509(nullptr, &at, static_cast<long>(der.size()));
    if (certificate == nullptr) {
      // Left in the thread's error queue, an OpenSSL error reappears at the next
      // handshake on this thread and is read as that handshake's failure.
      ERR_clear_error();
      X509_STORE_free(store);
      log(LogLevel::Warn, kTag, "a test trust anchor is not a DER certificate");
      return nullptr;
    }
    const int added = X509_STORE_add_cert(store, certificate);
    X509_free(certificate);
    if (added != 1) {
      ERR_clear_error();
      X509_STORE_free(store);
      log(LogLevel::Warn, kTag, "could not add a test trust anchor to the store");
      return nullptr;
    }
  }
  return store;
}

// ---- URLs and errors ------------------------------------------------------------

/// RFC 1035 2.3.4. A label over 63 octets cannot be put into a DNS message at
/// all, so no query can leave the machine and this is a URL this client will not
/// touch rather than a name that failed to resolve. OkHttp answers `url` here
/// too; `net-unreachable` accepts either.
bool hostIsSendable(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  if (isIpLiteral(host)) return true;
  std::size_t start = 0;
  while (start <= host.size()) {
    std::size_t end = host.find('.', start);
    if (end == std::string::npos) end = host.size();
    if (end - start > 63) return false;
    start = end + 1;
  }
  return true;
}

/// A name that does not resolve never gets this far -- it fails in
/// `resolveFor` as `dns` -- so a connection failure here is always `connect`.
NetError classify(httplib::Error error, bool headDelivered) {
  switch (error) {
    case httplib::Error::Connection:
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::ProxyConnection:
      return NetError::Connect;
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      return NetError::Tls;
    case httplib::Error::ExceedRedirectCount:
      return NetError::Redirect;
    case httplib::Error::Compression:
    case httplib::Error::UnsupportedContentEncoding:
      return NetError::Decode;
    case httplib::Error::InvalidRequestLine:
    case httplib::Error::InvalidHTTPMethod:
    case httplib::Error::InvalidHTTPVersion:
    case httplib::Error::InvalidHeaders:
    case httplib::Error::ExceedUriMaxLength:
    case httplib::Error::ExceedMaxPayloadSize:
      return NetError::Protocol;
    case httplib::Error::Read:
    case httplib::Error::Write:
    case httplib::Error::ConnectionClosed:
    case httplib::Error::Timeout:
    // This client's own aborts return before they classify anything, so a
    // `Canceled` that reaches here is a callback that gave up on a broken
    // connection -- a streamed body whose write failed, most likely.
    case httplib::Error::Canceled:
      return NetError::Network;
    default:
      return headDelivered ? NetError::Network : NetError::Connect;
  }
}

/// `httplib::to_string(error)` names the layer; for a refused certificate that
/// is the same sentence whatever was wrong with it, and the fixture offers three
/// different wrongnesses on three ports (untrusted, expired, wrong host). The
/// verify result the library carries out of the handshake is what tells them
/// apart, so it is folded in where there is one.
///
/// Where it is depends on the backend. On OpenSSL -- the only one this client
/// builds with -- cpp-httplib verifies *after* the handshake and fails with
/// `SSLServerVerification`, `ssl_error` 0 and the X509 verify result as the
/// backend code (`setup_client_tls_session`); only Mbed TLS and wolfSSL fail
/// the handshake itself with `tls::ErrorCode::CertVerifyFailed`. Both are read
/// here. X509 verify results are small (under a hundred in OpenSSL 3); the
/// other codes that can ride on this error are `ERR_get_error()`'s packed
/// values, which are not, and are left out rather than misnamed.
std::string describe(httplib::Error error, int sslError, std::uint64_t backend) {
  std::string text = httplib::to_string(error);
  if (text.empty()) text = "the request failed";
  const bool verifyResult =
      sslError == static_cast<int>(httplib::tls::ErrorCode::CertVerifyFailed) ||
      (error == httplib::Error::SSLServerVerification && sslError == 0 && backend > 0 && backend < 1024);
  if (error == httplib::Error::SSLServerHostnameVerification ||
      sslError == static_cast<int>(httplib::tls::ErrorCode::HostnameMismatch)) {
    text += ": the certificate is not valid for this host name";
  } else if (verifyResult) {
    text += ": " + httplib::tls::TlsError::verify_error_to_string(static_cast<long>(backend));
  }
  return text;
}

// ---- name resolution ---------------------------------------------------------------
//
// `getaddrinfo` is the one blocking call with no way to interrupt it: no
// cancellation, no descriptor to shut down, bounded only by the resolver's own
// timeouts. So a lookup runs on a thread of its own, the request waits for it on
// a condition variable a cancel can wake, and an abandoned lookup finishes on
// its own and throws its answer away. Nothing it touches but its own
// `Resolution` outlives the wait, so leaving it running is safe.

struct Resolution {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool abandoned = false;
  int status = 0;  // getaddrinfo's return: 0, or an EAI_* code
  /// Numeric, in the order the resolver gave them (RFC 6724), duplicates dropped.
  std::vector<std::string> addresses;
};

std::shared_ptr<Resolution> startResolving(const std::string& host) {
  auto resolution = std::make_shared<Resolution>();
  const auto resolve = [resolution, host] {
    // The hints cpp-httplib uses for a name of its own (`create_socket`), so
    // the answer is the one it would have connected to.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int status = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    std::vector<std::string> addresses;
    for (const addrinfo* at = result; at != nullptr; at = at->ai_next) {
      char text[NI_MAXHOST];
      if (::getnameinfo(at->ai_addr, at->ai_addrlen, text, sizeof(text), nullptr, 0, NI_NUMERICHOST) != 0) {
        continue;
      }
      if (std::find(addresses.begin(), addresses.end(), text) == addresses.end()) addresses.emplace_back(text);
    }
    if (result != nullptr) ::freeaddrinfo(result);
    {
      std::lock_guard<std::mutex> lock(resolution->mutex);
      resolution->done = true;
      resolution->status = status;
      resolution->addresses = std::move(addresses);
    }
    resolution->cv.notify_all();
  };
  try {
    std::thread(resolve).detach();
  } catch (const std::system_error&) {
    // No thread to be had: resolve right here, as cpp-httplib would have --
    // uninterruptible, but answered.
    resolve();
  }
  return resolution;
}

void abandon(const std::shared_ptr<Resolution>& resolution) {
  if (!resolution) return;
  {
    std::lock_guard<std::mutex> lock(resolution->mutex);
    resolution->abandoned = true;
  }
  resolution->cv.notify_all();
}

// ---- tracking a socket under cpp-httplib -----------------------------------------------
//
// The only hook cpp-httplib offers on a socket before it connects is
// `set_socket_options`, called with the descriptor the moment it is created.
// What is kept from it is a *duplicate*: shutting a descriptor down acts on the
// socket behind it, so `shutdown(copy)` aborts a connect, a TLS handshake or a
// read just as `shutdown(original)` would -- but the copy is ours, so it cannot
// be closed under us and handed to some other socket, which a bare descriptor
// number cached from a callback could be. `copyMutex` is its own lock because
// the callback runs under cpp-httplib's socket mutex, and `clientMutex` is held
// by the code that calls `stop()`, which takes that same mutex.

template <typename Live>
httplib::SocketOptions trackSocket(const std::shared_ptr<Live>& live) {
  std::weak_ptr<Live> weak = live;
  return [weak](socket_t fd) {
    auto held = weak.lock();
    if (!held) return;
    const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return;
    std::lock_guard<std::mutex> lock(held->copyMutex);
    if (held->socketCopy >= 0) ::close(held->socketCopy);
    held->socketCopy = copy;
    // Cancelled before this socket existed: nothing else will ever reach it.
    if (held->cancelled.load()) ::shutdown(copy, SHUT_RDWR);
  };
}

template <typename Live>
void shutDownSocket(Live& live) {
  std::lock_guard<std::mutex> lock(live.copyMutex);
  if (live.socketCopy >= 0) ::shutdown(live.socketCopy, SHUT_RDWR);
}

template <typename Live>
void releaseSocket(Live& live) {
  std::lock_guard<std::mutex> lock(live.copyMutex);
  if (live.socketCopy >= 0) ::close(live.socketCopy);
  live.socketCopy = -1;
}

/// Resolve `host` for `live`, where a cancel can abandon the wait. False when
/// `live` was cancelled first; otherwise `status` and `addresses` are the
/// resolver's answer.
template <typename Live>
bool resolveFor(const std::shared_ptr<Live>& live, const std::string& host, int& status,
                std::vector<std::string>& addresses) {
  auto resolution = startResolving(host);
  {
    // Published under the lock `cancel` takes after setting `cancelled`, so a
    // cancel either sees this lookup or is seen here -- never neither.
    std::lock_guard<std::mutex> lock(live->clientMutex);
    if (live->cancelled.load()) {
      abandon(resolution);
      return false;
    }
    live->resolving = resolution;
  }
  bool answered = false;
  {
    std::unique_lock<std::mutex> lock(resolution->mutex);
    resolution->cv.wait(lock, [&] { return resolution->done || resolution->abandoned; });
    answered = resolution->done && !resolution->abandoned;
    if (answered) {
      status = resolution->status;
      addresses = resolution->addresses;
    }
  }
  {
    std::lock_guard<std::mutex> lock(live->clientMutex);
    live->resolving.reset();
  }
  return answered && !live->cancelled.load();
}

std::string resolverError(const std::string& host, int status) {
  if (status == 0) return "no address for " + host;
  return "could not resolve " + host + ": " + ::gai_strerror(status);
}

bool isConnectFailure(httplib::Error error) {
  return error == httplib::Error::Connection || error == httplib::Error::ConnectionTimeout;
}

// ---- Content-Encoding ---------------------------------------------------------------------
//
// `gzip` and `deflate`, decoded as the body arrives so the flow window and the
// page both see decoded bytes. `deflate` is RFC 9110's zlib format, but a raw
// DEFLATE stream under the same name is common enough that every browser takes
// both; the first two bytes tell them apart. Whatever follows the end of the
// compressed stream is ignored, as Chrome ignores it -- a second gzip member
// included. An empty body is empty whatever it is labelled.
// A body that ends before its compressed stream does is *not* complete, and
// `finish` says so -- the failure OkHttp's `GzipSource` gives on Android too.

class BodyDecoder {
 public:
  enum class Kind { Identity, Gzip, Deflate, Unsupported };

  static Kind kindOf(const std::string& header) {
    std::string name = lowerAscii(header);
    const auto first = name.find_first_not_of(" \t");
    const auto last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (name.empty() || name == "identity") return Kind::Identity;
    if (name == "gzip" || name == "x-gzip") return Kind::Gzip;
    if (name == "deflate") return Kind::Deflate;
    // A coding this client never offered but knows compresses -- decoding it is
    // impossible, and handing the page compressed bytes as if they were the
    // body is worse than failing. Anything else is passed through as it came,
    // as cpp-httplib did: servers do put a charset in this header.
    if (name == "br" || name == "zstd" || name == "compress" || name == "x-compress") return Kind::Unsupported;
    return Kind::Identity;
  }

  explicit BodyDecoder(Kind kind) : kind_(kind) {}
  ~BodyDecoder() {
    if (initialised_) ::inflateEnd(&stream_);
  }
  BodyDecoder(const BodyDecoder&) = delete;
  BodyDecoder& operator=(const BodyDecoder&) = delete;

  /// Decode `size` more bytes, handing what comes out to `out(const char*,
  /// size_t)`, which returns false to stop. False on corrupt data (`corrupt()`)
  /// or when `out` stopped.
  template <typename Out>
  bool feed(const char* data, std::size_t size, Out&& out) {
    if (size == 0) return true;
    if (!initialised_) {
      // `deflate` needs two bytes to tell zlib from raw; hold one if that is all
      // there is.
      if (kind_ == Kind::Deflate && pending_.size() + size < 2) {
        pending_.append(data, size);
        return true;
      }
      if (!pending_.empty()) {
        pending_.append(data, size);
        std::string held;
        held.swap(pending_);
        if (!start(held.data())) return false;
        return inflateSome(held.data(), held.size(), out);
      }
      if (!start(data)) return false;
    }
    return inflateSome(data, size, out);
  }

  /// The body has ended. True when the compressed stream ended with it (or
  /// there never was one); false for a body cut short inside it.
  template <typename Out>
  bool finish(Out&& out) {
    if (!pending_.empty()) {
      // One byte of `deflate` and then nothing: raw, and certainly short.
      std::string held;
      held.swap(pending_);
      raw_ = true;
      if (!start(nullptr)) return false;
      if (!inflateSome(held.data(), held.size(), out)) return false;
    }
    return !initialised_ || ended_;
  }

  bool corrupt() const { return corrupt_; }

 private:
  bool start(const char* head) {
    int windowBits = 15;
    if (kind_ == Kind::Gzip) {
      windowBits = 16 + 15;
    } else if (!raw_ && head != nullptr) {
      const auto cmf = static_cast<unsigned char>(head[0]);
      const auto flg = static_cast<unsigned char>(head[1]);
      raw_ = !((cmf & 0x0f) == 8 && ((cmf << 8) | flg) % 31 == 0);
    }
    if (raw_) windowBits = -15;
    if (::inflateInit2(&stream_, windowBits) != Z_OK) {
      corrupt_ = true;
      return false;
    }
    initialised_ = true;
    return true;
  }

  template <typename Out>
  bool inflateSome(const char* data, std::size_t size, Out& out) {
    stream_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    stream_.avail_in = static_cast<uInt>(size);
    char buffer[64 * 1024];
    while (stream_.avail_in > 0 && !ended_) {
      stream_.next_out = reinterpret_cast<Bytef*>(buffer);
      stream_.avail_out = sizeof(buffer);
      const int rc = ::inflate(&stream_, Z_NO_FLUSH);
      if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        corrupt_ = true;
        return false;
      }
      const std::size_t produced = sizeof(buffer) - stream_.avail_out;
      if (produced > 0 && !out(buffer, produced)) return false;
      if (rc == Z_STREAM_END) ended_ = true;
      if (rc == Z_BUF_ERROR && produced == 0) break;  // needs more input
    }
    return true;
  }

  Kind kind_;
  z_stream stream_{};
  bool initialised_ = false;
  bool raw_ = false;
  bool ended_ = false;
  bool corrupt_ = false;
  std::string pending_;
};

// ---- idle connections ------------------------------------------------------------
//
// One `httplib::Client` holds one keep-alive connection, so reusing them is what
// makes `Connection: keep-alive` mean anything across two fetches -- and the
// TLS handshake is paid once per origin rather than once per request.

struct ClientSettings {
  std::string userAgent;
  std::vector<Bytes> testAnchors;
};

/// The image's roots, by path. Also the fallback when a test-anchor store cannot
/// be built: falling through to OpenSSL's compiled-in default paths instead
/// would quietly trust a different set of roots than the ones this client found.
template <typename Client>
void trustImageRoots(Client& client) {
  const TrustPaths& paths = trustPaths();
  if (!paths.file.empty() || !paths.dir.empty()) {
    client.set_ca_cert_path(paths.file, paths.dir);
  }
}

template <typename Client>
void applyTrust(Client& client, const ClientSettings& settings) {
  client.enable_server_certificate_verification(true);
  client.enable_server_hostname_verification(true);
  if (settings.testAnchors.empty()) {
    trustImageRoots(client);
    return;
  }
  if (X509_STORE* store = storeWithAnchors(settings.testAnchors); store != nullptr) {
    client.set_ca_cert_store(store);  // takes ownership
  } else {
    trustImageRoots(client);
  }
}

std::unique_ptr<httplib::Client> makeClient(const Url& url, const ClientSettings& settings) {
  auto client = std::make_unique<httplib::Client>(url.origin());
  if (!client->is_valid()) return nullptr;
  client->set_keep_alive(true);
  // Followed here, not there: see the file comment.
  client->set_follow_location(false);
  // Decoded here, not there: see `BodyDecoder`.
  client->set_decompress(false);
  // The target is already percent-encoded by the WHATWG parser on the JS side.
  // Re-encoding it would turn `%3A` into `%253A` and send a different URL than
  // the page asked for.
  client->set_path_encode(false);
  client->set_connection_timeout(30, 0);
  // The web has no default read timeout -- `AbortSignal.timeout` and
  // `xhr.timeout` are its answer, and both work -- so neither does this. What
  // ends a request nobody wants any more is `abortRequest`.
  client->set_read_timeout(7 * 24 * 60 * 60, 0);
  client->set_write_timeout(7 * 24 * 60 * 60, 0);
  client->set_default_headers({{"User-Agent", settings.userAgent}});
  if (url.secure()) applyTrust(*client, settings);
  return client;
}

class ClientPool {
 public:
  /// An idle connection to `url`'s origin, or a new client. Never null unless
  /// the URL is one cpp-httplib will not take.
  std::unique_ptr<httplib::Client> take(const Url& url, const ClientSettings& settings) {
    const std::string origin = url.origin();
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      expire(now);
      auto it = idle_.find(origin);
      if (it != idle_.end() && !it->second.empty()) {
        auto client = std::move(it->second.back().client);
        it->second.pop_back();
        if (it->second.empty()) idle_.erase(it);
        return client;
      }
    }
    return makeClient(url, settings);
  }

  void give(const Url& url, std::unique_ptr<httplib::Client> client) {
    if (!client) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    expire(now);
    auto& bucket = idle_[url.origin()];
    if (bucket.size() >= kMaxPerOrigin) return;  // destroyed with the unique_ptr
    bucket.push_back(Idle{std::move(client), now});
  }

  void clear() {
    std::unordered_map<std::string, std::vector<Idle>> taken;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      taken.swap(idle_);
    }
  }

 private:
  struct Idle {
    std::unique_ptr<httplib::Client> client;
    std::chrono::steady_clock::time_point since;
  };

  /// Callers hold `mutex_`. A connection nobody has wanted for a minute is a
  /// file descriptor held for nothing, and an empty bucket is a map entry held
  /// for nothing -- on a device with few of either.
  void expire(std::chrono::steady_clock::time_point now) {
    for (auto it = idle_.begin(); it != idle_.end();) {
      auto& bucket = it->second;
      bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                  [now](const Idle& idle) {
                                    return now - idle.since > kIdleConnectionLife;
                                  }),
                   bucket.end());
      it = bucket.empty() ? idle_.erase(it) : std::next(it);
    }
  }

  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<Idle>> idle_;
};

// ---- one request ------------------------------------------------------------------

struct LinuxCore;

struct LinuxRequest {
  std::uint64_t id = 0;
  HttpRequestSpec spec;
  std::shared_ptr<HttpSink> sink;
  /// The origin the admission counters charged this request to: its *first*
  /// one, which a redirect does not change.
  std::string origin;

  std::thread worker;
  /// Set by the worker as its very last act, so a reaper can tell a thread that
  /// has finished from one still unwinding from a cancel.
  std::atomic<bool> workerDone{false};
  /// I/O queue only.
  bool started = false;

  /// Set by an abort or by shutdown, read on the worker thread from inside
  /// cpp-httplib's callbacks: returning false out of one is how a blocking read
  /// or a blocking upload is unwound.
  std::atomic<bool> cancelled{false};

  /// The client the worker is using, if any. `stop()` from another thread is
  /// cpp-httplib's documented way to interrupt a blocking request, and this
  /// mutex is what keeps it from landing on a destroyed one.
  std::mutex clientMutex;
  httplib::Client* client = nullptr;
  /// The name lookup the worker is waiting on, so a cancel can stop the wait.
  /// Guarded by `clientMutex`.
  std::shared_ptr<Resolution> resolving;
  /// A duplicate of the socket the current send opened (`trackSocket`), or -1.
  std::mutex copyMutex;
  int socketCopy = -1;

  /// Flow control: bytes handed to the sink that the page has not read yet.
  std::mutex flowMutex;
  std::condition_variable flowCv;
  std::uint64_t unacknowledged = 0;

  /// A streamed request body, written by `appendRequestBody` and read by the
  /// content provider on the worker thread.
  std::mutex bodyMutex;
  std::condition_variable bodyCv;
  std::deque<std::shared_ptr<const Bytes>> bodyChunks;
  std::uint64_t bodyQueued = 0;
  bool bodyEnded = false;
  /// The page wrote more than `kMaxPendingUpload` past what the server took.
  /// Read on the worker, set on the queue.
  std::atomic<bool> bodyOverflowed{false};

  /// Exactly one party ever joins the worker. Both `shutdown` and a retirement
  /// job can hold the last reference to the same request -- the job runs on the
  /// queue while `shutdown` works through its own copy of the map -- and two
  /// threads calling `join()` on one `std::thread` is undefined behaviour that
  /// hangs in practice. Claiming it first is what makes the second caller a
  /// no-op instead.
  std::atomic<bool> joinClaimed{false};

  void joinWorker() {
    if (joinClaimed.exchange(true)) return;
    if (worker.joinable()) worker.join();
  }

  ~LinuxRequest() {
    // Never reached with an unclaimed live worker on any ordinary path -- `reap`
    // and `shutdown` join them -- but `~thread` on a joinable thread is
    // `std::terminate`, and the one path that can get here is a queue that went
    // away before its retirement job ran. Everything the worker still touches is
    // held by shared_ptr, so letting it finish unobserved is safe; taking the
    // process down is not.
    if (!joinClaimed.exchange(true) && worker.joinable()) worker.detach();
    if (socketCopy >= 0) ::close(socketCopy);
  }
};

// ---- one WebSocket -----------------------------------------------------------------
//
// cpp-httplib's `WebSocketClient` is synchronous in both directions, so a socket
// costs two threads: one blocked in `read()`, one draining the send queue.
// Sending from the I/O queue instead would stall every other event behind a peer
// that stopped reading, which is the failure the file comment forbids.

struct Outgoing {
  bool close = false;
  bool text = false;
  std::shared_ptr<const Bytes> payload;
  int code = 0;
  std::string reason;
};

struct LinuxSocket {
  std::uint64_t id = 0;
  Url url;
  std::vector<std::string> protocols;
  std::shared_ptr<SocketSink> sink;

  std::thread reader;
  std::thread writer;
  std::atomic<bool> cancelled{false};
  /// `onClose` has been handed to the sink; nothing follows it.
  std::atomic<bool> settled{false};

  /// Guards `client`, which the writer and the reader both reach for, and
  /// `resolving`.
  std::mutex clientMutex;
  httplib::ws::WebSocketClient* client = nullptr;
  std::shared_ptr<Resolution> resolving;
  /// A duplicate of the socket's descriptor (`trackSocket`), or -1: what lets a
  /// cancel end a connect, a handshake or a read at once.
  std::mutex copyMutex;
  int socketCopy = -1;
  /// The writer waits for the handshake before it sends anything.
  std::atomic<bool> open{false};

  std::mutex sendMutex;
  std::condition_variable sendCv;
  std::deque<Outgoing> sendQueue;
  std::uint64_t sendQueued = 0;
  bool sendFinished = false;

  /// Set by the reader as its very last act, once it has joined the writer, so
  /// the queue can tell a socket whose threads have finished from one still
  /// inside a handshake.
  std::atomic<bool> threadsDone{false};

  /// What *this* side closed with, when it did. The peer's code and reason are
  /// not observable through this library (see the file comment), so this is the
  /// only close that can be reported with its own code.
  std::atomic<bool> weClosed{false};
  int closeCode = 1000;
  std::string closeReason;

  /// As `LinuxRequest::joinClaimed`, twice: the reader is joined by whichever of
  /// `shutdown` and the retirement job gets there first, and the writer by
  /// whichever of the reader and `shutdown` does.
  std::atomic<bool> readerClaimed{false};
  std::atomic<bool> writerClaimed{false};

  void joinReader() {
    if (readerClaimed.exchange(true)) return;
    if (reader.joinable()) reader.join();
  }
  void joinWriter() {
    if (writerClaimed.exchange(true)) return;
    if (writer.joinable()) writer.join();
  }

  ~LinuxSocket() {
    if (!readerClaimed.exchange(true) && reader.joinable()) reader.detach();
    if (!writerClaimed.exchange(true) && writer.joinable()) writer.detach();
    if (socketCopy >= 0) ::close(socketCopy);
  }
};

struct LinuxCore {
  /// Weak, as on Android and for the same reason: every posted job holds the
  /// core, and a strong reference here would be a cycle through the queue.
  std::weak_ptr<IoQueue> queue;
  std::atomic<bool> stopped{false};
  ClientSettings settings;
  LinuxCookieJar cookies;
  ClientPool pool;

  // I/O queue only.
  std::unordered_map<std::uint64_t, std::shared_ptr<LinuxRequest>> requests;
  std::unordered_map<std::uint64_t, std::shared_ptr<LinuxSocket>> sockets;
  std::unordered_map<std::string, std::size_t> perOrigin;
  std::deque<std::uint64_t> waiting;
  std::size_t inFlight = 0;

  /// Requests whose worker has been let go but not yet joined. A cancel gives
  /// every worker a prompt way out, but "prompt" is still a thread switch and a
  /// socket teardown away, and the I/O queue waits for nothing: it only ever
  /// *hands over* a thread here and joins the ones that have already finished.
  std::mutex reaperMutex;
  std::vector<std::shared_ptr<LinuxRequest>> reaping;
  std::vector<std::shared_ptr<LinuxSocket>> reapingSockets;
};

/// I/O queue. Admit whatever has been waiting longest and now fits.
/// Forward-declared because the job that retires a finished request is posted
/// from its own worker thread, below.
void startWaiting(const std::shared_ptr<LinuxCore>& core);

/// Hand a finished-with request's thread over, and join whatever has already
/// run to completion. Safe to call from the I/O queue: it only ever joins a
/// thread whose body has returned, which costs the time to reap a TCB.
void reap(const std::shared_ptr<LinuxCore>& core, std::shared_ptr<LinuxRequest> done) {
  std::vector<std::shared_ptr<LinuxRequest>> finished;
  {
    std::lock_guard<std::mutex> lock(core->reaperMutex);
    if (done) core->reaping.push_back(std::move(done));
    auto live = std::partition(
        core->reaping.begin(), core->reaping.end(),
        [](const std::shared_ptr<LinuxRequest>& r) { return !r->workerDone.load(); });
    finished.assign(std::make_move_iterator(live), std::make_move_iterator(core->reaping.end()));
    core->reaping.erase(live, core->reaping.end());
  }
  for (auto& request : finished) {
    request->joinWorker();
  }
}

/// The same for a socket's two threads. The reader joins the writer itself
/// before it ends -- the client is that thread's stack local and the writer must
/// be finished with it before it goes -- so only the reader is left here.
void reapSockets(const std::shared_ptr<LinuxCore>& core, std::shared_ptr<LinuxSocket> done) {
  std::vector<std::shared_ptr<LinuxSocket>> finished;
  {
    std::lock_guard<std::mutex> lock(core->reaperMutex);
    if (done) core->reapingSockets.push_back(std::move(done));
    auto live = std::partition(
        core->reapingSockets.begin(), core->reapingSockets.end(),
        [](const std::shared_ptr<LinuxSocket>& s) { return !s->threadsDone.load(); });
    finished.assign(std::make_move_iterator(live),
                    std::make_move_iterator(core->reapingSockets.end()));
    core->reapingSockets.erase(live, core->reapingSockets.end());
  }
  for (auto& socket : finished) {
    socket->joinReader();
  }
}

/// A socket has stopped: drop it from the map so its id can come round again,
/// and hand its reader over to be joined. Called on the reader thread as its
/// last act, which is why `threadsDone` is set *after* the post.
void retireSocket(const std::shared_ptr<LinuxCore>& core,
                  const std::shared_ptr<LinuxSocket>& socket) {
  if (auto queue = core->queue.lock()) {
    queue->post([core, socket] {
      // By pointer, as requests are: an id reused by a later socket must not be
      // erased by an earlier one's retirement.
      const auto it = core->sockets.find(socket->id);
      if (it != core->sockets.end() && it->second == socket) core->sockets.erase(it);
      reapSockets(core, socket);
    });
  }
  socket->threadsDone.store(true);
}

/// Post a sink call onto the runtime's serial queue. Checked twice, on the way
/// in and again on the queue: a request that is aborted between the two must
/// still hear nothing more, which is what `abortRequest` promises. Every sink
/// call in this file goes through here, and that is the whole of the threading
/// contract.
template <typename Live, typename Work>
void onQueue(const std::shared_ptr<LinuxCore>& core, const std::shared_ptr<Live>& live, Work work) {
  if (core->stopped.load() || live->cancelled.load()) return;
  auto queue = core->queue.lock();
  if (!queue) return;
  queue->post([core, live, work]() mutable {
    if (core->stopped.load() || live->cancelled.load()) return;
    work();
  });
}

/// Tear the connection down under whatever the worker is doing on it: a name
/// lookup, a connect, a handshake, a write or a read.
///
/// The order is the point. The socket copy is shut down first and the lookup
/// abandoned, and only then is cpp-httplib's `stop()` called -- which takes the
/// client's socket mutex, held through a whole connect and TLS handshake. Called
/// the other way round from the I/O queue, `stop()` waited out a connect to an
/// address that never answers, and every event in the runtime waited with it.
void stopClient(const std::shared_ptr<LinuxRequest>& request) {
  shutDownSocket(*request);
  std::lock_guard<std::mutex> lock(request->clientMutex);
  abandon(request->resolving);
  if (request->client != nullptr) request->client->stop();
}

/// Wake a request out of whatever it is blocked on and tear its connection down.
/// Safe from any thread; the worker notices through `cancelled`.
void cancel(const std::shared_ptr<LinuxRequest>& request) {
  request->cancelled.store(true);
  {
    // Taken and dropped for the ordering alone: the reader checks `cancelled`
    // inside the wait predicate, and a notify that crosses with that check
    // without the lock in between is a lost wakeup and a permanent hang.
    std::lock_guard<std::mutex> lock(request->flowMutex);
  }
  request->flowCv.notify_all();
  {
    std::lock_guard<std::mutex> lock(request->bodyMutex);
    request->bodyEnded = true;
  }
  request->bodyCv.notify_all();
  stopClient(request);
}

/// The same, for a socket. There is no `stop()` on a `WebSocketClient`, and
/// destroying it under a blocked `read()` would be a use-after-free, so what
/// unblocks the reader is its socket being shut down under it -- a connect and a
/// handshake included -- with the bounded read timeout as the backstop.
void cancel(const std::shared_ptr<LinuxSocket>& socket) {
  socket->cancelled.store(true);
  {
    std::lock_guard<std::mutex> lock(socket->sendMutex);
    socket->sendFinished = true;
  }
  socket->sendCv.notify_all();
  shutDownSocket(*socket);
  std::lock_guard<std::mutex> lock(socket->clientMutex);
  abandon(socket->resolving);
}

// ---- the worker ---------------------------------------------------------------------

/// What one hop needs to know, carried across the redirect loop.
struct Hop {
  Url url;
  std::string method;
  HeaderList headers;
  std::shared_ptr<const Bytes> body;
  bool streaming = false;
};

/// True for a status the Fetch standard follows.
bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool sameOrigin(const Url& a, const Url& b) {
  return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

void dropBodyHeaders(HeaderList& headers) {
  headers.erase(std::remove_if(headers.begin(), headers.end(),
                               [](const std::pair<std::string, std::string>& header) {
                                 const std::string name = lowerAscii(header.first);
                                 return name == "content-length" || name == "content-type" ||
                                        name == "content-encoding" || name == "content-language" ||
                                        name == "content-location";
                               }),
                headers.end());
}

void dropHeader(HeaderList& headers, const char* name) {
  headers.erase(std::remove_if(headers.begin(), headers.end(),
                               [name](const std::pair<std::string, std::string>& header) {
                                 return equalsIgnoreCase(header.first, name);
                               }),
                headers.end());
}

ResponseHead headOf(const httplib::Response& response, const Url& url, bool redirected) {
  ResponseHead head;
  head.status = response.status;
  // cpp-httplib keeps the reason phrase the server actually wrote, which is one
  // fidelity Apple's client cannot offer.
  head.statusText = response.reason;
  head.url = url.href();
  head.redirected = redirected;
  head.headers.reserve(response.headers.size());
  for (const auto& [name, value] : response.headers) {
    head.headers.emplace_back(lowerAscii(name), value);
  }
  return head;
}

/// One request, start to finish, on a thread of its own. Everything it tells the
/// page goes out through `onQueue`; the only thing it returns is the completion
/// job that retires it.
void runRequest(std::shared_ptr<LinuxCore> core, std::shared_ptr<LinuxRequest> request) {
  auto sink = request->sink;
  const HttpRequestSpec& spec = request->spec;

  Hop hop;
  hop.url = spec.url;
  hop.method = spec.method;
  hop.headers = spec.headers;
  hop.body = spec.body;
  hop.streaming = spec.streamingBody;

  int hops = 0;
  bool settled = false;
  std::uint64_t uploaded = 0;

  const auto fail = [&](NetError kind, std::string message) {
    if (settled) return;
    settled = true;
    onQueue(core, request, [sink, kind, message] { sink->onError(kind, message); });
  };

  while (!settled) {
    if (request->cancelled.load()) break;

    if (!hostIsSendable(hop.url.host)) {
      fail(NetError::Url, "not a URL this client can reach: " + hop.url.href());
      break;
    }

    auto client = core->pool.take(hop.url, core->settings);
    if (!client) {
      fail(NetError::Url, "not a URL this client can reach: " + hop.url.href());
      break;
    }

    // The name, resolved here where a cancel can abandon the wait -- unless the
    // pooled connection is still open, when there is nothing to connect, or the
    // host is an address already.
    std::vector<std::string> addresses;
    if (!isIpLiteral(hop.url.host) && !client->is_socket_open()) {
      int status = 0;
      if (!resolveFor(request, hop.url.host, status, addresses)) break;  // cancelled
      if (status != 0 || addresses.empty()) {
        fail(NetError::Dns, resolverError(hop.url.host, status));
        break;
      }
    }

    httplib::Request wire;
    wire.method = hop.method;
    wire.path = hop.url.target;
    for (const auto& [name, value] : hop.headers) wire.headers.emplace(name, value);
    // cpp-httplib only adds these two when there is no content receiver, and
    // there always is one here.
    if (!wire.has_header("Accept")) wire.headers.emplace("Accept", "*/*");
    if (!wire.has_header("Accept-Encoding")) {
      wire.headers.emplace("Accept-Encoding", "gzip, deflate");
    }
    if (spec.useCookies) {
      const std::string cookies = core->cookies.requestHeader(hop.url);
      if (!cookies.empty()) wire.headers.emplace("Cookie", cookies);
    }

    // Every body goes out through a provider, never `wire.body` -- see the
    // file comment: a body in `wire.body` with no `Content-Type` of its own
    // gets `text/plain` written into it by cpp-httplib, and a browser sends no
    // such header at all. `Transfer-Encoding` is set for the streamed case
    // only, and only here: cpp-httplib does not add one of its own, so the
    // multimap cannot end up with two.
    if (hop.streaming) {
      wire.content_length_ = 0;
      wire.is_chunked_content_provider_ = true;
      wire.set_header("Transfer-Encoding", "chunked");
      auto pump = [core, request, sink, &uploaded, &spec](std::size_t, httplib::DataSink& out) {
        std::shared_ptr<const Bytes> chunk;
        bool ended = false;
        {
          // Held only long enough to take the next chunk: writing to the socket
          // under it would make `appendRequestBody` -- which runs on the I/O
          // queue -- wait on a stalled server, and with it every other event.
          std::unique_lock<std::mutex> lock(request->bodyMutex);
          request->bodyCv.wait(lock, [&] {
            return request->cancelled.load() || request->bodyOverflowed.load() ||
                   !request->bodyChunks.empty() || request->bodyEnded;
          });
          // Overflow is checked *before* the queue: what is still queued must
          // not be drained and finished off as if the body had ended, or the
          // server would be handed a truncated upload that looks complete.
          if (request->cancelled.load() || request->bodyOverflowed.load()) return false;
          if (request->bodyChunks.empty()) {
            ended = true;
          } else {
            chunk = std::move(request->bodyChunks.front());
            request->bodyChunks.pop_front();
            request->bodyQueued -= chunk->size();
          }
        }
        if (ended) {
          // Nothing left and the page said it was done: the terminating chunk
          // goes out here, and that -- not a byte count -- is what tells a
          // streamed upload it is complete.
          out.done();
          if (spec.reportUpload) {
            const std::uint64_t total = uploaded;
            onQueue(core, request, [sink, total] { sink->onUploadProgress(total, -1, true); });
          }
          return true;
        }
        if (!out.write(reinterpret_cast<const char*>(chunk->data()), chunk->size())) return false;
        uploaded += chunk->size();
        if (spec.reportUpload) {
          const std::uint64_t sent = uploaded;
          onQueue(core, request, [sink, sent] { sink->onUploadProgress(sent, -1, false); });
        }
        return true;
      };
      wire.content_provider_ = httplib::detail::ContentProviderAdapter(std::move(pump));
    } else if (hop.body && !hop.body->empty()) {
      auto body = hop.body;
      wire.content_length_ = body->size();
      wire.is_chunked_content_provider_ = false;
      wire.content_provider_ = [body](std::size_t offset, std::size_t length,
                                      httplib::DataSink& out) {
        if (offset >= body->size()) {
          out.done();
          return true;
        }
        const std::size_t take = std::min(length, body->size() - offset);
        return out.write(reinterpret_cast<const char*>(body->data()) + offset, take);
      };
      if (spec.reportUpload) {
        wire.upload_progress = [core, request, sink](std::size_t current, std::size_t total) {
          if (request->cancelled.load()) return false;
          const auto sent = static_cast<std::uint64_t>(current);
          const auto expected = static_cast<std::int64_t>(total);
          onQueue(core, request, [sink, sent, expected] {
            sink->onUploadProgress(sent, expected, sent >= static_cast<std::uint64_t>(expected));
          });
          return true;
        };
      }
    }

    // The head, and the decision the head forces: a hop we are going to follow
    // must not reach the page, and neither must its body.
    bool headDelivered = false;
    bool following = false;
    bool refusedRedirect = false;
    std::unique_ptr<BodyDecoder> decoder;
    bool undecodable = false;

    wire.response_handler = [&](const httplib::Response& response) {
      if (request->cancelled.load()) return false;
      if (isRedirect(response.status) && !response.get_header_value("Location").empty()) {
        if (spec.redirect == RedirectMode::Follow) {
          following = true;
          return true;  // the body is read and dropped, so the connection survives
        }
        if (spec.redirect == RedirectMode::Error) {
          refusedRedirect = true;
          return false;
        }
        // Manual: the page gets the 3xx itself, exactly as it arrived.
      }
      const auto kind = BodyDecoder::kindOf(response.get_header_value("Content-Encoding"));
      if (kind == BodyDecoder::Kind::Unsupported) {
        undecodable = true;  // fails at its first byte; an empty body is fine
      } else if (kind != BodyDecoder::Kind::Identity) {
        decoder = std::make_unique<BodyDecoder>(kind);
      }
      ResponseHead head = headOf(response, hop.url, hops > 0);
      headDelivered = true;
      onQueue(core, request, [sink, head]() mutable { sink->onHead(std::move(head)); });
      return true;
    };

    // Decoded bytes, to the page, under the flow window.
    const auto emit = [&](const char* data, std::size_t size) {
      Bytes chunk(reinterpret_cast<const std::uint8_t*>(data),
                  reinterpret_cast<const std::uint8_t*>(data) + size);
      if (spec.flowWindow > 0) {
        // Counted **before** the chunk is posted, or an acknowledgement racing
        // in between the two would be clamped to zero and lost, and the reader
        // below would then wait for bytes the page has already read.
        std::lock_guard<std::mutex> lock(request->flowMutex);
        request->unacknowledged += size;
      }
      onQueue(core, request,
              [sink, chunk = std::move(chunk)]() mutable { sink->onData(std::move(chunk)); });
      if (spec.flowWindow > 0) {
        // Not returning is the whole mechanism: this is the thread reading the
        // socket, so while it waits the receive window closes and the server
        // stalls. That is OkHttp's guarantee, reached a different way.
        std::unique_lock<std::mutex> lock(request->flowMutex);
        request->flowCv.wait(lock, [&] {
          return request->cancelled.load() || request->unacknowledged < spec.flowWindow;
        });
      }
      return !request->cancelled.load();
    };

    wire.content_receiver = [&](const char* data, std::size_t size, std::size_t, std::size_t) {
      if (request->cancelled.load()) return false;
      if (following || size == 0) return true;
      if (undecodable) return false;
      if (decoder) return decoder->feed(data, size, emit);
      return emit(data, size);
    };

    // One address at a time, in the resolver's order, moving on only when the
    // connect itself failed -- nothing has been written then, so the next one is
    // safe to try. That is the loop cpp-httplib ran over `getaddrinfo`'s answer
    // before names were resolved here.
    client->set_socket_options(trackSocket(request));
    httplib::Result outcome;
    bool abandoned = false;
    for (std::size_t attempt = 0;; ++attempt) {
      if (!addresses.empty()) client->set_hostname_addr_map({{hop.url.host, addresses[attempt]}});
      {
        std::lock_guard<std::mutex> lock(request->clientMutex);
        if (request->cancelled.load()) {
          abandoned = true;
          break;
        }
        request->client = client.get();
      }
      // Re-checked with the client published and the lock dropped. A `stop()`
      // that landed before the call is *lost*: with no request in flight it
      // closes an idle socket, and the call would simply reconnect and run a
      // request the cancelled page will never see.
      //
      // The `Result` overload rather than `send(Request&, Response&, Error&)`:
      // only this one carries the TLS error out, and without it every refused
      // certificate reads the same (see `describe`).
      if (request->cancelled.load()) {
        std::lock_guard<std::mutex> lock(request->clientMutex);
        request->client = nullptr;
        abandoned = true;
        break;
      }
      outcome = client->send(wire);
      {
        std::lock_guard<std::mutex> lock(request->clientMutex);
        request->client = nullptr;
      }
      releaseSocket(*request);
      if (outcome || request->cancelled.load() || !isConnectFailure(outcome.error()) ||
          attempt + 1 >= addresses.size()) {
        break;
      }
    }
    // A pooled client must not keep a callback that names this request.
    client->set_socket_options(nullptr);

    if (abandoned || request->cancelled.load()) break;

    const bool ok = static_cast<bool>(outcome);
    if (ok && spec.useCookies) {
      // Every hop's cookies, which is the reason redirects are followed here and
      // not inside cpp-httplib.
      const auto range = outcome->headers.equal_range("Set-Cookie");
      for (auto it = range.first; it != range.second; ++it) {
        core->cookies.setFromResponse(hop.url, it->second);
      }
    }

    if (ok) {
      // Only a connection that finished cleanly goes back in the pool; anything
      // else may have bytes of a half-read body still on it.
      core->pool.give(hop.url, std::move(client));
    } else {
      client.reset();
    }

    if (!ok) {
      if (refusedRedirect) {
        fail(NetError::Redirect,
             "the response was a redirect and redirect: 'error' was asked for");
      } else if (request->bodyOverflowed.load()) {
        // The page outran the server by more than the cap. Reported from here
        // rather than from the I/O queue, so it can never arrive after `onEnd`.
        fail(NetError::Network, "the request body outran the server by more than 8 MiB");
      } else if (undecodable) {
        fail(NetError::Decode, "the response uses a Content-Encoding this client cannot decode");
      } else if (decoder && decoder->corrupt()) {
        fail(NetError::Decode, "the response body could not be decoded");
      } else {
        fail(classify(outcome.error(), headDelivered),
             describe(outcome.error(), outcome.ssl_error(), outcome.ssl_backend_error()));
      }
      break;
    }
    const httplib::Response& response = *outcome;

    if (!following) {
      if (!headDelivered) {
        // HEAD, 204 and 304 never reach the response handler: cpp-httplib skips
        // the whole body block for them, head included.
        ResponseHead head = headOf(response, hop.url, hops > 0);
        onQueue(core, request, [sink, head]() mutable { sink->onHead(std::move(head)); });
      }
      if (decoder && !decoder->finish(emit)) {
        // The framing ended cleanly and the compressed stream inside it did not:
        // a body cut short, which must not reach the page as a whole one.
        if (decoder->corrupt()) {
          fail(NetError::Decode, "the response body could not be decoded");
        } else {
          fail(NetError::Network, "the response body ended before its compressed data did");
        }
        break;
      }
      settled = true;
      onQueue(core, request, [sink] { sink->onEnd(); });
      break;
    }

    // ---- follow it (Fetch, "HTTP-redirect fetch") ----
    if (hops >= kMaxRedirects) {
      fail(NetError::Redirect, "too many redirects");
      break;
    }
    Url next;
    std::string why;
    if (!resolveUrl(hop.url, response.get_header_value("Location"), next, why)) {
      fail(NetError::Redirect, "the redirect went somewhere this client cannot follow: " + why);
      break;
    }
    const bool crossOrigin = !sameOrigin(hop.url, next);
    const int status = response.status;
    const bool dropsBody =
        (status == 303 && hop.method != "GET" && hop.method != "HEAD") ||
        ((status == 301 || status == 302) && hop.method == "POST");
    if (dropsBody) {
      hop.method = "GET";
      hop.body.reset();
      dropBodyHeaders(hop.headers);
      if (hop.streaming) {
        hop.streaming = false;
        // The page is still writing into a body nobody will send now. Left
        // queued, those chunks reach the 8 MiB cap and cancel a redirect that
        // was meant to succeed, so the queue is emptied and the writer is told
        // the body is over.
        {
          std::lock_guard<std::mutex> lock(request->bodyMutex);
          request->bodyChunks.clear();
          request->bodyQueued = 0;
          request->bodyEnded = true;
        }
        request->bodyCv.notify_all();
      }
    } else if (hop.streaming) {
      // A streamed body is consumed once and cannot be replayed, so a 307/308
      // that wants it again is a network error rather than a silent empty body.
      fail(NetError::Redirect, "a streamed request body cannot be re-sent to a redirect");
      break;
    }
    // Fetch drops `Authorization` when the redirect leaves the origin: without
    // it a bearer token written for one host is handed to whatever the redirect
    // names. `net-redirect` asserts both halves of this.
    if (crossOrigin) dropHeader(hop.headers, "Authorization");
    // The jar decides the next hop's cookies; a stale one from this hop must not
    // ride along.
    dropHeader(hop.headers, "Cookie");
    hop.url = next;
    ++hops;
  }

  // Retire on the queue, whatever happened -- including the paths that unwind
  // because the request was cancelled, which is why those break out of the loop
  // rather than returning. An abort has usually done this bookkeeping already
  // and the job then finds nothing, but nothing else would do it for a request
  // cancelled from inside (an upload that outran the server), and the slot and
  // the thread would be held for the runtime's life.
  auto queue = core->queue.lock();
  if (queue) {
    queue->post([core, request] {
      // By pointer, never by id alone: `bindings/Net.cpp` starts at 1 in every
      // runtime and a retirement that ran late could otherwise erase and join a
      // *different*, live request that had since been given the same id.
      const auto it = core->requests.find(request->id);
      if (it == core->requests.end() || it->second != request) return;
      core->requests.erase(it);
      if (request->started) {
        if (core->inFlight > 0) --core->inFlight;
        auto counted = core->perOrigin.find(request->origin);
        if (counted != core->perOrigin.end()) {
          if (counted->second > 0) --counted->second;
          if (counted->second == 0) core->perOrigin.erase(counted);
        }
      }
      reap(core, request);
      startWaiting(core);
    });
  }
  // Last, and after the post: `reap` joins only threads that have set this, and
  // the queue job above may run the moment it is posted.
  request->workerDone.store(true);
}

void startRequestNow(const std::shared_ptr<LinuxCore>& core,
                     const std::shared_ptr<LinuxRequest>& request) {
  request->started = true;
  ++core->inFlight;
  ++core->perOrigin[request->origin];
  request->worker = std::thread(runRequest, core, request);
}

/// I/O queue. Admit whatever has been waiting longest and now fits, in order.
void startWaiting(const std::shared_ptr<LinuxCore>& core) {
  if (core->stopped.load()) return;
  bool progress = true;
  while (progress && core->inFlight < kMaxInFlight) {
    progress = false;
    for (auto it = core->waiting.begin(); it != core->waiting.end(); ++it) {
      const auto found = core->requests.find(*it);
      if (found == core->requests.end()) {
        // Aborted while it waited.
        core->waiting.erase(it);
        progress = true;
        break;
      }
      if (core->perOrigin[found->second->origin] >= kMaxPerOrigin) continue;
      auto request = found->second;
      core->waiting.erase(it);
      startRequestNow(core, request);
      progress = true;
      break;
    }
  }
}

// ---- the socket ------------------------------------------------------------------------

/// Settle a socket exactly once, for anything but a Close frame from the peer
/// (`settlePeerClose`): a close this side sent is reported with its own code
/// and reason, and everything else is a failure, 1006.
void settleSocket(const std::shared_ptr<LinuxCore>& core, const std::shared_ptr<LinuxSocket>& socket,
                  bool failed, NetError kind, std::string message) {
  if (socket->settled.exchange(true)) return;
  auto sink = socket->sink;
  if (sink == nullptr) return;
  if (socket->weClosed.load()) {
    const int code = socket->closeCode;
    const std::string reason = socket->closeReason;
    onQueue(core, socket, [sink, code, reason] { sink->onClose(code, reason, true); });
    return;
  }
  onQueue(core, socket, [sink, failed, kind, message] {
    if (failed) sink->onError(kind, message);
    sink->onClose(1006, "", false);
  });
}

/// The peer closed with a Close frame: its code and reason, and a clean close
/// (RFC 6455 7.1.5, 7.1.6). A frame with no payload carries no code, which is
/// 1005 (7.4.1); a one-byte payload is not a valid Close frame at all (5.5.1).
/// Only reachable because tools/vendor/httplib.rules keeps the payload that
/// upstream cpp-httplib throws away.
void settlePeerClose(const std::shared_ptr<LinuxCore>& core, const std::shared_ptr<LinuxSocket>& socket,
                     const std::string& payload) {
  if (payload.size() == 1) {
    settleSocket(core, socket, true, NetError::Protocol, "the peer's Close frame was malformed");
    return;
  }
  if (socket->settled.exchange(true)) return;
  auto sink = socket->sink;
  if (sink == nullptr) return;
  int code = 1005;
  std::string reason;
  if (payload.size() >= 2) {
    code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
    reason = payload.substr(2);
  }
  onQueue(core, socket, [sink, code, reason] { sink->onClose(code, reason, true); });
}

void runSocketWriter(std::shared_ptr<LinuxCore> core, std::shared_ptr<LinuxSocket> socket) {
  auto sink = socket->sink;
  for (;;) {
    Outgoing item;
    {
      std::unique_lock<std::mutex> lock(socket->sendMutex);
      // `open` is in the predicate because a frame written before the handshake
      // finished would go out on a connection that does not exist yet: the
      // client is published before `connect()` so that a cancel can reach it.
      socket->sendCv.wait(lock, [&] {
        return socket->cancelled.load() || socket->sendFinished ||
               (socket->open.load() && !socket->sendQueue.empty());
      });
      if (socket->cancelled.load()) return;
      if (!socket->open.load() || socket->sendQueue.empty()) {
        if (socket->sendFinished) return;
        continue;
      }
      item = std::move(socket->sendQueue.front());
      socket->sendQueue.pop_front();
      if (item.payload) socket->sendQueued -= item.payload->size();
    }
    // The pointer is read under the lock and used outside it. Holding it across
    // a blocking send would make the reader -- which takes the same lock on its
    // way out -- wait for a peer that stopped reading. What keeps the client
    // alive meanwhile is that the reader joins *this* thread before letting it
    // go out of scope.
    httplib::ws::WebSocketClient* client = nullptr;
    {
      std::lock_guard<std::mutex> lock(socket->clientMutex);
      client = socket->client;
    }
    if (client == nullptr || socket->cancelled.load()) return;
    if (item.close) {
      socket->closeCode = item.code;
      socket->closeReason = item.reason;
      socket->weClosed.store(true);
      // The frame goes out; the peer echoes it and the reader's `read()` ends.
      client->close(static_cast<httplib::ws::CloseStatus>(item.code), item.reason);
      return;
    }
    const auto size = static_cast<std::uint64_t>(item.payload ? item.payload->size() : 0);
    const char* data =
        item.payload && size > 0 ? reinterpret_cast<const char*>(item.payload->data()) : "";
    // The overload *is* the opcode: `send(std::string)` writes a Text frame and
    // `send(const char*, size_t)` a Binary one. Nothing in the signature says
    // so, and picking the wrong one turns every `socket.send('...')` into a
    // Blob at the far end.
    if (item.text) {
      client->send(std::string(data, static_cast<std::size_t>(size)));
    } else {
      client->send(data, static_cast<std::size_t>(size));
    }
    // Sent or refused, `bufferedAmount` has to stop counting these bytes: a
    // failure ends the connection through the reader, and a page waiting for
    // `bufferedAmount` to reach zero must not wait for bytes that will never go.
    onQueue(core, socket, [sink, size] { sink->onSent(size); });
  }
}

void runSocket(std::shared_ptr<LinuxCore> core, std::shared_ptr<LinuxSocket> socket) {
  auto sink = socket->sink;
  // Whatever way this returns, the copy of the socket goes with it.
  struct Release {
    LinuxSocket& socket;
    ~Release() { releaseSocket(socket); }
  } release{*socket};

  // The writer is started first and parks on its queue, so *every* way out of
  // this function has to tell it to stop and wait for it: it is the client on
  // this stack that it sends through, and that client goes out of scope here.
  const auto stopWriter = [&] {
    {
      std::lock_guard<std::mutex> lock(socket->sendMutex);
      socket->sendFinished = true;
    }
    socket->sendCv.notify_all();
    socket->joinWriter();
  };

  httplib::Headers headers;
  if (!socket->protocols.empty()) {
    std::string joined;
    for (const auto& protocol : socket->protocols) {
      if (!joined.empty()) joined += ", ";
      joined += protocol;
    }
    headers.emplace("Sec-WebSocket-Protocol", joined);
  }
  headers.emplace("User-Agent", core->settings.userAgent);
  // The handshake is an ordinary request and carries the jar's cookies, which is
  // what `net-cookies` asserts.
  const std::string cookies = core->cookies.requestHeader(socket->url);
  if (!cookies.empty()) headers.emplace("Cookie", cookies);

  httplib::ws::WebSocketClient client(socket->url.href(), headers);
  if (!client.is_valid()) {
    stopWriter();
    settleSocket(core, socket, true, NetError::Url,
                 "not a URL this client can reach: " + socket->url.href());
    retireSocket(core, socket);
    return;
  }
  if (socket->url.secure()) applyTrust(client, core->settings);
  client.set_connection_timeout(30, 0);
  // Bounded, so the reader comes back to look at `cancelled`; a timeout set at
  // run time leaves the connection open and usable, unlike the compile-time
  // default.
  client.set_read_timeout(0, kSocketReadPollMs * 1000);
  client.set_write_timeout(30, 0);
  client.set_socket_options(trackSocket(socket));

  // The name, as for a request: resolved where a cancel can abandon the wait.
  std::vector<std::string> addresses;
  if (!isIpLiteral(socket->url.host)) {
    int status = 0;
    if (!resolveFor(socket, socket->url.host, status, addresses)) {
      stopWriter();
      retireSocket(core, socket);
      return;
    }
    if (status != 0 || addresses.empty()) {
      stopWriter();
      settleSocket(core, socket, true, NetError::Dns, resolverError(socket->url.host, status));
      retireSocket(core, socket);
      return;
    }
  }

  {
    std::unique_lock<std::mutex> lock(socket->clientMutex);
    if (socket->cancelled.load()) {
      lock.unlock();
      stopWriter();
      retireSocket(core, socket);
      return;
    }
    socket->client = &client;
  }

  // Address by address, as a request does, while the connect itself fails.
  httplib::ws::Result handshake;
  for (std::size_t attempt = 0;; ++attempt) {
    if (!addresses.empty()) client.set_hostname_addr_map({{socket->url.host, addresses[attempt]}});
    handshake = client.connect();
    if (handshake || socket->cancelled.load() || !isConnectFailure(handshake.error()) ||
        attempt + 1 >= addresses.size()) {
      break;
    }
  }
  if (!handshake || socket->cancelled.load()) {
    stopWriter();
    {
      std::lock_guard<std::mutex> lock(socket->clientMutex);
      socket->client = nullptr;
    }
    if (!socket->cancelled.load()) {
      settleSocket(core, socket, true, classify(handshake.error(), handshake.status() > 0),
                   describe(handshake.error(), handshake.ssl_error(),
                            handshake.ssl_backend_error()));
    }
    retireSocket(core, socket);
    return;
  }

  // RFC 6455 4.1: a subprotocol the client never offered, or any extension --
  // this client offers none -- fails the connection. cpp-httplib checks the
  // accept key and neither of these. Dropped rather than closed: a failed
  // handshake owes the peer no closing handshake.
  const std::string protocol = client.subprotocol();
  std::string refused;
  if (!protocol.empty() &&
      std::find(socket->protocols.begin(), socket->protocols.end(), protocol) == socket->protocols.end()) {
    refused = "the server chose a subprotocol that was not offered: " + protocol;
  } else if (handshake.has_header("Sec-WebSocket-Extensions")) {
    refused = "the server answered with an extension that was not offered: " +
              handshake.get_header_value("Sec-WebSocket-Extensions");
  }
  if (!refused.empty()) {
    stopWriter();
    shutDownSocket(*socket);
    {
      std::lock_guard<std::mutex> lock(socket->clientMutex);
      socket->client = nullptr;
    }
    settleSocket(core, socket, true, NetError::Protocol, refused);
    retireSocket(core, socket);
    return;
  }

  socket->open.store(true);
  onQueue(core, socket, [sink, protocol] { sink->onOpen(protocol, std::string()); });
  // Only now: a send queued while the handshake was still running would have
  // gone out on a connection that did not exist yet.
  socket->sendCv.notify_all();

  std::string message;
  for (;;) {
    const httplib::ws::ReadResult result = client.read(message);
    if (socket->cancelled.load()) break;
    if (result == httplib::ws::Timeout) continue;
    if (result == httplib::ws::Fail) break;
    const bool text = result == httplib::ws::Text;
    Bytes payload(reinterpret_cast<const std::uint8_t*>(message.data()),
                  reinterpret_cast<const std::uint8_t*>(message.data()) + message.size());
    onQueue(core, socket, [sink, text, payload = std::move(payload)]() mutable {
      sink->onMessage(text, std::move(payload));
    });
    message.clear();
  }

  // The writer first, and before anything else: `client` is this function's
  // stack local, so it must not go out of scope while another thread is inside
  // `send()` on it.
  stopWriter();
  {
    std::lock_guard<std::mutex> lock(socket->clientMutex);
    socket->client = nullptr;
  }
  if (!socket->cancelled.load()) {
    // The peer's Close frame, when that is what ended it and this side had not
    // closed first -- a close this side sent is reported with its own code.
    const std::string* peer = client.peer_close_payload();
    if (peer != nullptr && !socket->weClosed.load()) {
      settlePeerClose(core, socket, *peer);
    } else {
      settleSocket(core, socket, true, NetError::Network, "the socket closed");
    }
  }
  retireSocket(core, socket);
}

// ---- the service -----------------------------------------------------------------------

class LinuxNetService final : public NetService {
 public:
  explicit LinuxNetService(NetConfig config) {
    queue_ = makeIoQueue(config.name + ".net");
    core_ = std::make_shared<LinuxCore>();
    core_->queue = queue_;
    core_->settings.userAgent = std::move(config.userAgent);
    core_->settings.testAnchors = std::move(config.testTlsAnchors);
  }

  ~LinuxNetService() override { shutdown(); }

  void startRequest(std::uint64_t id, HttpRequestSpec spec, std::shared_ptr<HttpSink> sink) override {
    if (stopped_ || sink == nullptr) return;
    auto core = core_;
    auto request = std::make_shared<LinuxRequest>();
    request->id = id;
    request->spec = std::move(spec);
    request->sink = std::move(sink);
    request->origin = request->spec.url.origin();
    queue_->post([core, request] {
      if (core->stopped.load()) return;
      // An id already in use is a caller error, and taking it would strand the
      // request that holds it -- its worker would then find a stranger under its
      // own id. Refusing settles the new one instead of losing the old one.
      if (core->requests.count(request->id) != 0) {
        auto sink = request->sink;
        if (sink) sink->onError(NetError::Url, "that request id is already in use");
        return;
      }
      core->requests[request->id] = request;
      if (core->inFlight < kMaxInFlight && core->perOrigin[request->origin] < kMaxPerOrigin) {
        startRequestNow(core, request);
      } else {
        // Waiting its turn, and abortable from there: an aborted request that
        // never started must not hold a slot it was never given.
        core->waiting.push_back(request->id);
      }
    });
  }

  void appendRequestBody(std::uint64_t id, std::shared_ptr<const Bytes> chunk) override {
    if (stopped_ || !chunk || chunk->empty()) return;
    auto core = core_;
    queue_->post([core, id, chunk] {
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      bool overflowed = false;
      {
        std::lock_guard<std::mutex> lock(request->bodyMutex);
        if (request->bodyEnded) return;
        if (request->bodyQueued + chunk->size() > kMaxPendingUpload) {
          overflowed = true;
        } else {
          // The chunk is shared, not copied: `bindings/Net.cpp` already made
          // this buffer and nothing mutates it.
          request->bodyChunks.push_back(chunk);
          request->bodyQueued += chunk->size();
        }
      }
      if (!overflowed) {
        request->bodyCv.notify_all();
        return;
      }
      // The server has stopped taking the body and the page keeps writing.
      // Dropping the chunk would corrupt the upload in silence, so the request
      // fails instead -- and the memory stays bounded. The *worker* reports it,
      // on its way out, so the error can never land after an `onEnd` that was
      // already on the queue (`fail` is once-only, and `onEnd` settles too).
      //
      // Deliberately **not** `cancel()`: cancelling is what silences a sink,
      // and this is the one failure the page still has to hear about. The
      // provider returns false instead, which unwinds the request through
      // cpp-httplib and tears the connection down on the way.
      request->bodyOverflowed.store(true);
      request->bodyCv.notify_all();
      // ...and the socket goes too. A server that stopped reading leaves the
      // worker parked inside `DataSink::write`, not on `bodyCv`, so the flag
      // alone would never be looked at: the write has to fail for the provider
      // to return at all. `cancelled` is deliberately *not* set -- that is what
      // silences a sink, and this is the one failure the page must still hear.
      stopClient(request);
    });
  }

  void finishRequestBody(std::uint64_t id) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id] {
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      {
        std::lock_guard<std::mutex> lock(request->bodyMutex);
        request->bodyEnded = true;
      }
      request->bodyCv.notify_all();
    });
  }

  void abortRequest(std::uint64_t id) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id] {
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      core->requests.erase(it);
      // Cancelled whether it had started or not: a request still waiting its
      // turn must not be admitted a moment later, and `cancelled` is what stops
      // any event already on its way to the sink -- checked again on the queue,
      // so nothing already posted is delivered either.
      cancel(request);
      if (request->started) {
        if (core->inFlight > 0) --core->inFlight;
        auto counted = core->perOrigin.find(request->origin);
        if (counted != core->perOrigin.end()) {
          if (counted->second > 0) --counted->second;
          if (counted->second == 0) core->perOrigin.erase(counted);
        }
        // Handed over, never joined here: `cancel` has given the worker its way
        // out, but the queue does not wait for it to take it.
        reap(core, request);
      }
      startWaiting(core);
    });
  }

  void acknowledgeResponseData(std::uint64_t id, std::uint64_t bytes) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id, bytes] {
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      {
        std::lock_guard<std::mutex> lock(request->flowMutex);
        request->unacknowledged =
            bytes >= request->unacknowledged ? 0 : request->unacknowledged - bytes;
      }
      request->flowCv.notify_all();
    });
  }

  void openSocket(std::uint64_t id, Url url, std::vector<std::string> protocols,
                  std::shared_ptr<SocketSink> sink) override {
    if (stopped_ || sink == nullptr) return;
    auto core = core_;
    auto socket = std::make_shared<LinuxSocket>();
    socket->id = id;
    socket->url = std::move(url);
    socket->protocols = std::move(protocols);
    socket->sink = std::move(sink);
    queue_->post([core, socket] {
      if (core->stopped.load()) return;
      if (core->sockets.count(socket->id) != 0) {
        auto sink = socket->sink;
        sink->onError(NetError::Url, "that socket id is already in use");
        sink->onClose(1006, "", false);
        return;
      }
      core->sockets[socket->id] = socket;
      socket->writer = std::thread(runSocketWriter, core, socket);
      socket->reader = std::thread(runSocket, core, socket);
    });
  }

  void sendSocket(std::uint64_t id, bool text, std::shared_ptr<const Bytes> payload) override {
    if (stopped_ || !payload) return;
    auto core = core_;
    queue_->post([core, id, text, payload] {
      const auto it = core->sockets.find(id);
      if (it == core->sockets.end()) return;
      auto socket = it->second;
      {
        std::lock_guard<std::mutex> lock(socket->sendMutex);
        if (socket->sendFinished) return;
        // The same cap the streamed upload has, and for the same reason: `send`
        // cannot push back on the page, and a peer that stopped reading must not
        // grow this without bound.
        if (socket->sendQueued + payload->size() > kMaxPendingUpload) {
          socket->sendQueue.push_back(Outgoing{true, false, nullptr, 1009, std::string()});
          socket->sendFinished = true;
        } else {
          socket->sendQueue.push_back(Outgoing{false, text, payload, 0, std::string()});
          socket->sendQueued += payload->size();
        }
      }
      socket->sendCv.notify_all();
    });
  }

  void closeSocket(std::uint64_t id, int code, std::string reason) override {
    if (stopped_) return;
    auto core = core_;
    queue_->post([core, id, code, reason] {
      const auto it = core->sockets.find(id);
      if (it == core->sockets.end()) return;
      auto socket = it->second;
      if (!socket->open.load()) {
        // close() while CONNECTING. There is no frame to send and the writer
        // would park until a handshake that nobody wants any more; the shim has
        // already dispatched its own error and close, so this only has to stop
        // the threads (dom-shim.js, "close() while connecting").
        cancel(socket);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(socket->sendMutex);
        if (socket->sendFinished) return;
        // Queued behind whatever is still to go out, as a browser does: a
        // close() after a send() must not overtake it.
        socket->sendQueue.push_back(
            Outgoing{true, false, nullptr, code == 0 ? 1000 : code, reason});
        socket->sendFinished = true;
      }
      socket->sendCv.notify_all();
    });
  }

  // Both go straight at the jar: it has a mutex of its own, exactly as the
  // platform stores the other clients use do, so `NetService.h`'s "safe from any
  // thread" holds without a queue hop -- and there is no window in which a
  // teardown could free what these are reading.
  std::string cookiesFor(const Url& url) override {
    if (stopped_) return std::string();
    return core_->cookies.scriptCookies(url);
  }

  bool setCookie(const Url& url, const std::string& setCookie) override {
    if (stopped_) return false;
    return core_->cookies.setFromScript(url, setCookie);
  }

  void shutdown() override {
    {
      // The flag alone is taken under the lock, and the lock is dropped before
      // any of the work: a sink destroyed below could reach back into this
      // object, and finding the mutex held by its own thread would be a
      // deadlock rather than the second call being the no-op it is meant to be.
      std::lock_guard<std::mutex> lock(shutdownMutex_);
      if (stopped_.exchange(true)) return;
    }
    auto core = core_;
    std::unordered_map<std::uint64_t, std::shared_ptr<LinuxRequest>> requests;
    std::unordered_map<std::uint64_t, std::shared_ptr<LinuxSocket>> sockets;
    std::vector<std::shared_ptr<LinuxRequest>> reaping;
    std::vector<std::shared_ptr<LinuxSocket>> reapingSockets;

    const auto stopOnQueue = [&] {
      core->stopped.store(true);
      requests = std::move(core->requests);
      sockets = std::move(core->sockets);
      core->requests.clear();
      core->sockets.clear();
      core->waiting.clear();
      core->perOrigin.clear();
      core->inFlight = 0;
      for (auto& [id, request] : requests) {
        (void)id;
        cancel(request);
      }
      for (auto& [id, socket] : sockets) {
        (void)id;
        cancel(socket);
      }
      {
        std::lock_guard<std::mutex> lock(core->reaperMutex);
        reaping.swap(core->reaping);
        reapingSockets.swap(core->reapingSockets);
      }
      core->pool.clear();
      core->cookies.clear();
    };
    // `sync` from the queue's own thread is a documented deadlock (IoQueue.h),
    // and teardown is the one path that can be reached from a job -- a sink
    // dropped on the queue may hold the last reference to this service.
    if (queue_->onQueueThread()) {
      stopOnQueue();
    } else {
      queue_->sync(stopOnQueue);
    }

    // Off the queue. Every worker was cancelled above -- its lookup abandoned,
    // its socket shut down, its waits woken -- so these joins are prompt
    // (`net-cancel-reach` times one with two connects in flight). Synchronous
    // all the same: when this returns, no thread of ours is running and no sink
    // will be touched again.
    for (auto& request : reaping) {
      request->joinWorker();
    }
    for (auto& [id, request] : requests) {
      (void)id;
      request->joinWorker();
      request->sink.reset();
    }
    for (auto& socket : reapingSockets) {
      socket->joinReader();
    }
    for (auto& [id, socket] : sockets) {
      (void)id;
      // The reader joins the writer itself, but a socket torn down before it
      // ever got that far may still have one parked.
      socket->joinReader();
      socket->joinWriter();
      socket->sink.reset();
    }
  }

 private:
  std::shared_ptr<IoQueue> queue_;
  std::shared_ptr<LinuxCore> core_;
  std::atomic<bool> stopped_{false};
  std::mutex shutdownMutex_;
};

}  // namespace

std::shared_ptr<NetService> NetService::create(NetConfig config) {
  return std::make_shared<LinuxNetService>(std::move(config));
}

bool networkAvailable() { return true; }

}  // namespace screenkit::net
