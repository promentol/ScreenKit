// Copyright (c) ScreenKit contributors. MIT.
#include "LinuxCookieJar.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "Bytes.h"

namespace screenkit::net {
namespace {

/// Chromium's own caps, near enough: a page that sets cookies in a loop must not
/// grow this without bound on a device with 908 MB of RAM.
constexpr std::size_t kMaxPerDomain = 180;
constexpr std::size_t kMaxTotal = 3000;
/// RFC 6265 6.1 asks a store to keep at least 4096 bytes of name plus value.
/// Counting them is what keeps the caps above from bounding the *number* of
/// cookies while the memory they hold is unbounded -- 3000 of them at a
/// megabyte each is not a store a 908 MB device survives.
constexpr std::size_t kMaxNameAndValue = 4096;
/// A `Max-Age` far enough out is "never", and adding it to the clock must not
/// be allowed to overflow: signed overflow is undefined, and in practice wraps
/// a never-expiring cookie to one that expired long ago. A hundred years is
/// past anything a server means by it.
constexpr std::int64_t kMaxCookieLife = 100LL * 365 * 24 * 60 * 60;

std::int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool isCtl(unsigned char c) { return c < 0x20 || c == 0x7F; }

/// A `Set-Cookie` line carrying a control character is refused whole rather than
/// sanitised. There is no cookies.txt to forge a line in any more, but a name or
/// a Path with a newline in it is still a page trying to write something the
/// parser is not meant to see, and the safe answer is "no".
bool hasCtl(const std::string& text) {
  for (char c : text) {
    if (isCtl(static_cast<unsigned char>(c))) return true;
  }
  return false;
}

/// Days from 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
/// `days_from_civil`, so nothing here depends on `timegm` or on the process's
/// time zone -- a cookie's expiry is UTC by definition.
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int monthFromName(const std::string& token) {
  static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                  "jul", "aug", "sep", "oct", "nov", "dec"};
  if (token.size() < 3) return 0;
  const std::string head = lowerAscii(token.substr(0, 3));
  for (int i = 0; i < 12; ++i) {
    if (head == kMonths[i]) return i + 1;
  }
  return 0;
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

/// RFC 6265 5.1.1, which is the only date parser a cookie needs: it reads
/// IMF-fixdate, RFC 850 and asctime alike by tokenising on the delimiter set and
/// taking whichever token first looks like a time, a day, a month or a year.
/// Returns false when the value is not a date, which means "ignore the
/// attribute" rather than "reject the cookie".
bool parseCookieDate(const std::string& text, std::int64_t& out) {
  int hour = -1, minute = -1, second = -1;
  int day = -1, month = 0;
  int year = -1;

  std::size_t i = 0;
  const auto isDelimiter = [](unsigned char c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
  };
  while (i < text.size()) {
    while (i < text.size() && isDelimiter(static_cast<unsigned char>(text[i]))) ++i;
    const std::size_t start = i;
    while (i < text.size() && !isDelimiter(static_cast<unsigned char>(text[i]))) ++i;
    const std::string token = text.substr(start, i - start);
    if (token.empty()) continue;

    const auto firstColon = token.find(':');
    if (hour < 0 && firstColon != std::string::npos) {
      const auto secondColon = token.find(':', firstColon + 1);
      if (secondColon != std::string::npos) {
        const std::string h = token.substr(0, firstColon);
        const std::string m = token.substr(firstColon + 1, secondColon - firstColon - 1);
        std::string s = token.substr(secondColon + 1);
        // asctime and friends never put anything after the seconds, but a
        // trailing non-digit must not poison the parse.
        std::size_t digits = 0;
        while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') ++digits;
        s = s.substr(0, digits);
        if (allDigits(h) && allDigits(m) && allDigits(s) && h.size() <= 2 && m.size() <= 2 &&
            s.size() <= 2) {
          hour = std::atoi(h.c_str());
          minute = std::atoi(m.c_str());
          second = std::atoi(s.c_str());
          continue;
        }
      }
    }
    if (day < 0 && allDigits(token) && token.size() <= 2) {
      day = std::atoi(token.c_str());
      continue;
    }
    if (month == 0) {
      const int named = monthFromName(token);
      if (named != 0) {
        month = named;
        continue;
      }
    }
    if (year < 0 && allDigits(token) && token.size() >= 2 && token.size() <= 4) {
      year = std::atoi(token.c_str());
      continue;
    }
  }

  if (day < 1 || day > 31 || month == 0 || year < 0 || hour < 0) return false;
  if (year >= 70 && year <= 99) year += 1900;
  else if (year >= 0 && year <= 69) year += 2000;
  if (year < 1601) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  out = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
        hour * 3600 + minute * 60 + second;
  return true;
}

/// RFC 6265 5.1.3. An IP literal only ever matches itself, which is what keeps
/// `Domain=example.com` off a request to 127.0.0.1 -- the leak the retired
/// `net-cookie-public-suffix` row really guarded (spec-platform-http-clients.md).
bool domainMatches(const std::string& host, const std::string& domain) {
  if (host == domain) return true;
  if (isIpLiteral(host)) return false;
  if (host.size() <= domain.size()) return false;
  if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) return false;
  return host[host.size() - domain.size() - 1] == '.';
}

/// RFC 6265 5.1.4.
bool pathMatches(const std::string& requestPath, const std::string& cookiePath) {
  if (requestPath == cookiePath) return true;
  if (requestPath.size() < cookiePath.size()) return false;
  if (requestPath.compare(0, cookiePath.size(), cookiePath) != 0) return false;
  if (!cookiePath.empty() && cookiePath.back() == '/') return true;
  return requestPath[cookiePath.size()] == '/';
}

/// RFC 6265 5.1.4, the default-path of a request URI.
std::string defaultPath(const Url& url) {
  const std::string path = url.path();
  if (path.empty() || path[0] != '/') return "/";
  const auto last = path.find_last_of('/');
  if (last == 0) return "/";
  return path.substr(0, last);
}

}  // namespace

bool LinuxCookieJar::setFromResponse(const Url& url, const std::string& setCookie) {
  return store(url, setCookie, false);
}

bool LinuxCookieJar::setFromScript(const Url& url, const std::string& setCookie) {
  return store(url, setCookie, true);
}

bool LinuxCookieJar::store(const Url& url, const std::string& setCookie, bool fromScript) {
  if (setCookie.empty() || hasCtl(setCookie)) return false;

  // name=value, then the attributes (RFC 6265 5.2).
  const auto firstSemi = setCookie.find(';');
  std::string pair = trimWhitespace(setCookie.substr(0, firstSemi));
  const auto equals = pair.find('=');
  if (equals == std::string::npos) return false;  // a bare attribute is not a cookie
  Cookie cookie;
  cookie.name = trimWhitespace(pair.substr(0, equals));
  cookie.value = trimWhitespace(pair.substr(equals + 1));
  if (cookie.name.empty() && cookie.value.empty()) return false;
  if (cookie.name.size() + cookie.value.size() > kMaxNameAndValue) return false;
  // A name is a token: no separators, no spaces. Refusing here is what stops a
  // header like `a=b; Path=/` arriving through `setCookie` as one long name.
  for (char c : cookie.name) {
    if (c == ' ' || c == '\t' || c == ';' || c == ',' || c == '=') return false;
  }

  bool sawExpiry = false;
  std::int64_t expiresAt = 0;
  bool deleted = false;
  std::string domainAttribute;
  std::string pathAttribute;

  std::size_t at = firstSemi == std::string::npos ? setCookie.size() : firstSemi + 1;
  while (at < setCookie.size()) {
    const auto end = setCookie.find(';', at);
    const std::string attribute =
        setCookie.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? setCookie.size() : end + 1;
    const auto eq = attribute.find('=');
    const std::string name =
        lowerAscii(trimWhitespace(eq == std::string::npos ? attribute : attribute.substr(0, eq)));
    const std::string value = eq == std::string::npos ? std::string() : trimWhitespace(attribute.substr(eq + 1));
    if (name == "secure") {
      cookie.secure = true;
    } else if (name == "httponly") {
      cookie.httpOnly = true;
    } else if (name == "domain") {
      domainAttribute = value;
    } else if (name == "path") {
      pathAttribute = value;
    } else if (name == "max-age") {
      // Max-Age wins over Expires whichever order they arrive in (5.2.2), and a
      // zero or negative one is a deletion.
      if (!value.empty() && (value[0] == '-' || (value[0] >= '0' && value[0] <= '9'))) {
        const long long seconds = std::atoll(value.c_str());
        sawExpiry = true;
        if (seconds <= 0) {
          deleted = true;
        } else {
          deleted = false;
          // Clamped before the addition, not after: `now + seconds` on a
          // Max-Age of 2^63-1 is signed overflow, which is undefined and which
          // in practice wraps "never" round to "expired".
          const std::int64_t life =
              static_cast<long long>(kMaxCookieLife) < seconds
                  ? kMaxCookieLife
                  : static_cast<std::int64_t>(seconds);
          expiresAt = nowSeconds() + life;
        }
        // `sawExpiry` stays true for the rest of the line, which is how a later
        // `Expires` is kept from undoing this one (5.2.2).
        continue;
      }
    } else if (name == "expires") {
      if (sawExpiry) continue;  // Max-Age has already decided
      std::int64_t when = 0;
      if (parseCookieDate(value, when)) {
        if (when <= nowSeconds()) {
          deleted = true;
        } else {
          expiresAt = when;
        }
      }
    }
    // Anything else -- SameSite, Priority, Partitioned -- is ignored: this
    // runtime has one browsing context and no third-party requests to speak of.
  }

  // A `Secure` cookie may only be set from a secure origin (RFC 6265bis 5.5).
  // Without this a plaintext response -- or a page's own `setCookie` over
  // http -- can plant or *overwrite* a cookie the site marked Secure, which is
  // the attack the flag exists to stop; `matching()` refusing to send it later
  // does not help, because the damage is the value that is now stored.
  if (cookie.secure && !url.secure()) return false;

  // The `__Secure-` and `__Host-` prefixes (RFC 6265bis 4.1.3). They matter
  // most here because `setFromScript` is reachable from page JS: without them a
  // page can write `__Host-session` with any Domain and Path it likes, and a
  // server reading that name is entitled to assume it could not have been.
  if (cookie.name.rfind("__Secure-", 0) == 0 && !cookie.secure) return false;
  if (cookie.name.rfind("__Host-", 0) == 0) {
    if (!cookie.secure || !domainAttribute.empty() || pathAttribute != "/") return false;
  }

  // The origin's rules (5.3).
  if (!domainAttribute.empty()) {
    if (domainAttribute[0] == '.') domainAttribute.erase(0, 1);
    domainAttribute = lowerAscii(domainAttribute);
    if (domainAttribute.empty()) return false;
    if (!domainMatches(url.host, domainAttribute)) return false;
    // No public-suffix list ships with this runtime, so the cheap half of that
    // rule stands in for it: a single-label `Domain` (`com`, `local`) is refused
    // unless it is the host itself. Without it one site could set a cookie for
    // every site under a TLD.
    if (domainAttribute.find('.') == std::string::npos && domainAttribute != url.host) return false;
    cookie.hostOnly = false;
    cookie.domain = domainAttribute;
  } else {
    cookie.hostOnly = true;
    cookie.domain = url.host;
  }
  cookie.path = (!pathAttribute.empty() && pathAttribute[0] == '/') ? pathAttribute : defaultPath(url);
  cookie.expires = deleted ? 1 : expiresAt;

  if (fromScript && cookie.httpOnly) return false;  // JS may not mark one HttpOnly

  std::lock_guard<std::mutex> lock(mutex_);
  const std::int64_t now = nowSeconds();
  dropExpired(now);

  const auto same = [&](const Cookie& c) {
    return c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path;
  };
  const auto existing = std::find_if(cookies_.begin(), cookies_.end(), same);

  if (existing != cookies_.end() && fromScript && existing->httpOnly) {
    // The JS API can neither read an HttpOnly cookie nor replace one, which is
    // the whole of what "HttpOnly is invisible to JS" means here.
    return false;
  }

  if (deleted) {
    if (existing == cookies_.end()) return false;
    cookies_.erase(existing);
    return true;
  }

  if (existing != cookies_.end()) {
    // 5.3 step 11: an update keeps the original creation time, so re-setting a
    // cookie does not move it to the end of the `Cookie:` header.
    cookie.created = existing->created;
    *existing = std::move(cookie);
    return true;
  }

  cookie.created = nextCreated_++;
  const std::string domain = cookie.domain;
  cookies_.push_back(std::move(cookie));

  // Caps, oldest first, and only ever within the domain that overflowed.
  const auto perDomain = [&domain](const Cookie& c) { return c.domain == domain; };
  while (static_cast<std::size_t>(std::count_if(cookies_.begin(), cookies_.end(), perDomain)) >
         kMaxPerDomain) {
    const auto oldest = std::min_element(cookies_.begin(), cookies_.end(),
                                         [&](const Cookie& a, const Cookie& b) {
                                           if (a.domain != domain) return false;
                                           if (b.domain != domain) return true;
                                           return a.created < b.created;
                                         });
    if (oldest == cookies_.end()) break;
    cookies_.erase(oldest);
  }
  while (cookies_.size() > kMaxTotal) {
    const auto oldest = std::min_element(
        cookies_.begin(), cookies_.end(),
        [](const Cookie& a, const Cookie& b) { return a.created < b.created; });
    cookies_.erase(oldest);
  }
  return true;
}

std::vector<const LinuxCookieJar::Cookie*> LinuxCookieJar::matching(const Url& url,
                                                                    bool includeHttpOnly) const {
  const std::int64_t now = nowSeconds();
  const std::string path = url.path();
  const bool secure = url.secure();
  std::vector<const Cookie*> out;
  for (const Cookie& cookie : cookies_) {
    if (cookie.expires != 0 && cookie.expires <= now) continue;
    if (cookie.secure && !secure) continue;
    if (cookie.httpOnly && !includeHttpOnly) continue;
    if (cookie.hostOnly ? url.host != cookie.domain : !domainMatches(url.host, cookie.domain)) {
      continue;
    }
    if (!pathMatches(path, cookie.path)) continue;
    out.push_back(&cookie);
  }
  // RFC 6265 5.4: longer paths first, then oldest first. The order is part of
  // what a server sees, so it is the standard's rather than this vector's.
  std::stable_sort(out.begin(), out.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
    return a->created < b->created;
  });
  return out;
}

std::string LinuxCookieJar::requestHeader(const Url& url) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  for (const Cookie* cookie : matching(url, true)) {
    if (!out.empty()) out += "; ";
    out += cookie->name + "=" + cookie->value;
  }
  return out;
}

std::string LinuxCookieJar::scriptCookies(const Url& url) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  for (const Cookie* cookie : matching(url, false)) {
    if (!out.empty()) out += "; ";
    out += cookie->name + "=" + cookie->value;
  }
  return out;
}

void LinuxCookieJar::dropExpired(std::int64_t now) {
  cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                [now](const Cookie& c) {
                                  return c.expires != 0 && c.expires <= now;
                                }),
                 cookies_.end());
}

void LinuxCookieJar::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  cookies_.clear();
}

std::size_t LinuxCookieJar::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::int64_t now = nowSeconds();
  return static_cast<std::size_t>(std::count_if(
      cookies_.begin(), cookies_.end(),
      [now](const Cookie& c) { return c.expires == 0 || c.expires > now; }));
}

}  // namespace screenkit::net
