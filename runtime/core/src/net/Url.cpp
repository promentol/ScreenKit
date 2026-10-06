// Copyright (c) ScreenKit contributors. MIT.
#include "Url.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace screenkit::net {
namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::uint16_t defaultPortFor(const std::string& scheme) {
  return (scheme == "https" || scheme == "wss") ? 443 : 80;
}

/// Percent-encode what may not travel raw in a request target: controls, space,
/// non-ASCII bytes and the few delimiters no server accepts unescaped. An
/// existing escape passes through untouched.
std::string encodeTarget(const std::string& raw) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' ||
        c == '}' || c == '|' || c == '\\' || c == '^') {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string removeDotSegments(const std::string& path) {
  std::vector<std::string> out;
  std::size_t start = 0;
  const bool trailing = !path.empty() && path.back() == '/';
  while (start <= path.size()) {
    std::size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string segment = path.substr(start, end - start);
    if (segment == "..") {
      if (!out.empty()) out.pop_back();
    } else if (segment != "." && !segment.empty()) {
      out.push_back(segment);
    }
    start = end + 1;
  }
  std::string joined = "/";
  for (std::size_t i = 0; i < out.size(); ++i) {
    joined += out[i];
    if (i + 1 < out.size()) joined += '/';
  }
  const std::string last = path.substr(path.find_last_of('/') + 1);
  if ((trailing || last == "." || last == "..") && joined.back() != '/') joined += '/';
  return joined;
}

}  // namespace

bool Url::defaultPort() const { return port == defaultPortFor(scheme); }

std::string Url::hostHeader() const {
  std::string h = ipv6() ? "[" + host + "]" : host;
  if (!defaultPort()) h += ":" + std::to_string(port);
  return h;
}

std::string Url::origin() const {
  return scheme + "://" + (ipv6() ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string Url::href() const { return scheme + "://" + hostHeader() + target; }

std::string Url::path() const {
  const auto q = target.find('?');
  return q == std::string::npos ? target : target.substr(0, q);
}

bool isIpLiteral(const std::string& host) {
  if (host.find(':') != std::string::npos) return true;
  if (host.empty()) return false;
  int dots = 0;
  for (char c : host) {
    if (c == '.') {
      ++dots;
    } else if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return dots == 3;
}

bool parseUrl(const std::string& input, const char* schemes, Url& out, std::string& error) {
  const auto colon = input.find("://");
  if (colon == std::string::npos) {
    error = "not an absolute URL: " + input;
    return false;
  }
  const std::string scheme = lower(input.substr(0, colon));
  const std::string family = schemes;
  const bool ok = family == "ws" ? (scheme == "ws" || scheme == "wss")
                                 : (scheme == "http" || scheme == "https");
  if (!ok) {
    error = "unsupported URL scheme \"" + scheme + ":\"" +
            (family == "ws" ? " (expected ws: or wss:)" : " (expected http: or https:)");
    return false;
  }

  std::string rest = input.substr(colon + 3);
  const auto hash = rest.find('#');
  if (hash != std::string::npos) rest.resize(hash);
  const auto pathStart = rest.find_first_of("/?");
  std::string authority = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
  std::string target = pathStart == std::string::npos ? "/" : rest.substr(pathStart);
  if (!target.empty() && target[0] == '?') target = "/" + target;

  if (authority.find('@') != std::string::npos) {
    error = "URL includes credentials: " + input;
    return false;
  }

  std::string host;
  std::string portText;
  if (!authority.empty() && authority[0] == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) {
      error = "invalid IPv6 host in URL: " + input;
      return false;
    }
    host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') {
        error = "invalid host in URL: " + input;
        return false;
      }
      portText = authority.substr(close + 2);
    }
  } else {
    const auto p = authority.rfind(':');
    host = p == std::string::npos ? authority : authority.substr(0, p);
    if (p != std::string::npos) portText = authority.substr(p + 1);
  }
  host = lower(host);
  if (host.empty()) {
    error = "URL has no host: " + input;
    return false;
  }
  for (unsigned char c : host) {
    if (c <= 0x20 || c >= 0x7f || c == '/' || c == '\\' || c == '%' || c == '#' || c == '?') {
      error = "invalid host in URL: " + input;
      return false;
    }
  }

  std::uint16_t port = defaultPortFor(scheme);
  if (!portText.empty()) {
    long value = 0;
    for (char c : portText) {
      if (!std::isdigit(static_cast<unsigned char>(c)) || value > 65535) {
        error = "invalid port in URL: " + input;
        return false;
      }
      value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 65535) {
      error = "invalid port in URL: " + input;
      return false;
    }
    port = static_cast<std::uint16_t>(value);
  }

  out.scheme = scheme;
  out.host = host;
  out.port = port;
  out.target = encodeTarget(target);
  return true;
}

bool resolveUrl(const Url& base, const std::string& reference, Url& out, std::string& error) {
  std::string ref = reference;
  while (!ref.empty() && (ref.front() == ' ' || ref.front() == '\t')) ref.erase(ref.begin());
  while (!ref.empty() && (ref.back() == ' ' || ref.back() == '\t')) ref.pop_back();

  // An absolute reference: a scheme before the first ':'. Only a valid scheme
  // counts -- `login?next=https://x/y` is a relative path whose query holds a URL.
  const auto colon = ref.find(':');
  bool hasScheme = colon != std::string::npos && colon > 0 &&
                   std::isalpha(static_cast<unsigned char>(ref[0]));
  for (std::size_t i = 1; hasScheme && i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(ref[i]);
    if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') hasScheme = false;
  }
  if (hasScheme) {
    return parseUrl(ref, base.scheme == "ws" || base.scheme == "wss" ? "ws" : "http", out, error);
  }
  if (ref.rfind("//", 0) == 0) {
    return parseUrl(base.scheme + ":" + ref, "http", out, error);
  }

  const auto hash = ref.find('#');
  if (hash != std::string::npos) ref.resize(hash);

  Url result = base;
  if (ref.empty()) {
    out = result;
    return true;
  }
  if (ref[0] == '?') {
    result.target = encodeTarget(base.path() + ref);
  } else if (ref[0] == '/') {
    const auto q = ref.find('?');
    const std::string path = q == std::string::npos ? ref : ref.substr(0, q);
    const std::string query = q == std::string::npos ? "" : ref.substr(q);
    result.target = encodeTarget(removeDotSegments(path) + query);
  } else {
    const std::string basePath = base.path();
    const std::string dir = basePath.substr(0, basePath.find_last_of('/') + 1);
    const auto q = ref.find('?');
    const std::string path = q == std::string::npos ? ref : ref.substr(0, q);
    const std::string query = q == std::string::npos ? "" : ref.substr(q);
    result.target = encodeTarget(removeDotSegments(dir + path) + query);
  }
  out = result;
  return true;
}

}  // namespace screenkit::net
