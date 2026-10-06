// Copyright (c) ScreenKit contributors. MIT.
//
// The error taxonomy every network client maps onto. It lived in `Connection.h`
// while the protocol stack was ours; the clients are the platform's now
// (NSURLSession, OkHttp) and this is what both fold their own error types into,
// so it has a header of its own.
#pragma once

namespace screenkit::net {

/// Why a request or socket failed. The JS side maps these onto web semantics:
/// every one of them is a network error (fetch's TypeError), and `Tls`, `Url` and
/// `Unsupported` are the three EventSource treats as fatal rather than retrying
/// -- a rejected certificate, a URL the client will not touch and a platform
/// with no client are none of them blips (`net-no-javavm` pins the last).
enum class NetError {
  None,
  Dns,          // the host name did not resolve
  Connect,      // refused, unreachable, timed out connecting
  Tls,          // handshake or certificate verification failed
  Network,      // the connection failed or closed mid-exchange
  Protocol,     // the peer spoke malformed HTTP or WebSocket
  Redirect,     // too many redirects, or a redirect the request mode forbids
  Decode,       // a truncated or corrupt chunked/gzip/deflate body
  Url,          // not a URL this client can reach
  Unsupported,  // no HTTP client on this platform
};

/// **These numbers are a wire format.** `dev/screenkit/net/HttpClient.java` sends
/// them across JNI as its `ERR_*` constants and `NetServiceAndroid.cpp` maps them
/// back, and neither the Java nor the JNI boundary can see this enum. Inserting a
/// kind in the middle would silently remap every error the Android client
/// reports, so the numbering is pinned here and the count is asserted where it is
/// translated. A new kind goes **at the end**, and `HttpClient.java`'s `ERR_*`
/// block and `ERR_COUNT` change with it.
inline constexpr int kNetErrorCount = 10;
static_assert(static_cast<int>(NetError::None) == 0, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Dns) == 1, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Connect) == 2, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Tls) == 3, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Network) == 4, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Protocol) == 5, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Redirect) == 6, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Decode) == 7, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Url) == 8, "NetError's numbering is a wire format");
static_assert(static_cast<int>(NetError::Unsupported) == kNetErrorCount - 1,
              "a new NetError kind goes at the end, and HttpClient.java's ERR_* change with it");

/// The name `__screenkit.net` hands JS as an error's `kind`. Inline, so the
/// taxonomy is one header and no platform has to link a translation unit for it.
inline const char* netErrorName(NetError error) {
  switch (error) {
    case NetError::None: return "none";
    case NetError::Dns: return "dns";
    case NetError::Connect: return "connect";
    case NetError::Tls: return "tls";
    case NetError::Network: return "network";
    case NetError::Protocol: return "protocol";
    case NetError::Redirect: return "redirect";
    case NetError::Decode: return "decode";
    case NetError::Url: return "url";
    case NetError::Unsupported: return "unsupported";
  }
  return "network";
}

}  // namespace screenkit::net
