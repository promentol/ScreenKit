// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstdint>
#include <string>

namespace screenkit::net {

/// An absolute http(s) or ws(s) URL, split into what a connection and a request
/// line need. Not a WHATWG URL parser: the JS side resolves and normalises URLs
/// before they reach native, so this only has to be strict about what it accepts
/// and exact about what it sends.
struct Url {
  std::string scheme;       // "http", "https", "ws" or "wss", lower case
  std::string host;         // lower case; an IPv6 literal without its brackets
  std::uint16_t port = 0;   // always set, the scheme's default when absent
  std::string target = "/"; // path and query, percent-encoded, starting with '/'

  bool secure() const { return scheme == "https" || scheme == "wss"; }
  bool defaultPort() const;
  bool ipv6() const { return host.find(':') != std::string::npos; }

  /// `Host:` -- the port only when it is not the scheme's default.
  std::string hostHeader() const;
  /// scheme://host:port, the key the connection pool shares connections under.
  std::string origin() const;
  std::string href() const;
  /// The target without its query: what cookie paths are matched against.
  std::string path() const;
};

/// Parse an absolute URL. `schemes` is "http" (http, https) or "ws" (ws, wss).
/// A fragment is dropped; userinfo is refused, as fetch refuses it.
bool parseUrl(const std::string& input, const char* schemes, Url& out, std::string& error);

/// Resolve a `Location` value against the URL that answered with it (RFC 3986 5.2).
bool resolveUrl(const Url& base, const std::string& reference, Url& out, std::string& error);

/// True when `host` is a literal IPv4 or IPv6 address rather than a name: no SNI
/// is sent for one, and a cookie Domain attribute never applies to one.
bool isIpLiteral(const std::string& host);

}  // namespace screenkit::net
