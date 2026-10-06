// Copyright (c) ScreenKit contributors. MIT.
//
// The cookie store for the Linux client, and the one place in the tree where
// ScreenKit still owns cookie semantics.
//
// Apple and Android point at a platform store -- `NSHTTPCookieStorage`,
// `android.webkit.CookieManager` -- and Linux has none to point at: there is no
// system cookie jar a headless runtime may share, and cpp-httplib brings none.
// So this is a small RFC 6265 jar written for this platform, and it is **held in
// memory for the lifetime of the runtime** rather than persisted: persisting
// would mean restoring `RuntimeConfig::storageDirectory` and its plumbing, which
// were deleted with the portable stack, and Apple's private per-runtime store
// does not survive a restart either (spec-linux-http-client.md, the cookie
// decision).
//
// Thread-safe on its own, like the platform stores the other two clients use:
// `NetService.h` promises every entry point is safe from any thread, and this is
// touched both from the runtime's I/O queue (`cookiesFor`, `setCookie`) and from
// each request's worker thread (the `Cookie:` it sends and the `Set-Cookie` it
// stores, once per redirect hop).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Url.h"

namespace screenkit::net {

class LinuxCookieJar {
 public:
  /// A `Set-Cookie` line from a response, for the URL that answered it. Stores,
  /// replaces or -- for an expiry in the past -- removes one cookie. HttpOnly is
  /// allowed here and nowhere else. False when the line is not a cookie this jar
  /// will hold.
  bool setFromResponse(const Url& url, const std::string& setCookie);

  /// The JS API's half (`__screenkit.net.setCookie`). Refuses to set an HttpOnly
  /// cookie and refuses to replace one, so a page can neither forge nor read
  /// back what the server marked HttpOnly.
  bool setFromScript(const Url& url, const std::string& setCookie);

  /// The `Cookie:` header for a request, HttpOnly included, in RFC 6265 5.4
  /// order: longer paths first, then oldest first. Empty when nothing matches.
  std::string requestHeader(const Url& url) const;

  /// What `__screenkit.net.getCookies` answers: the same, minus HttpOnly.
  std::string scriptCookies(const Url& url) const;

  /// Drop everything. `shutdown` calls it, so a torn-down runtime keeps nothing.
  void clear();

  /// Live (unexpired) cookies. Tests only.
  std::size_t size() const;

 private:
  struct Cookie {
    std::string name;
    std::string value;
    std::string domain;   // no leading dot; the host itself when hostOnly
    std::string path;     // always starts with '/'
    bool hostOnly = true;
    bool secure = false;
    bool httpOnly = false;
    /// Seconds since the epoch, or 0 for a session cookie (this runtime's life).
    std::int64_t expires = 0;
    /// A tie-break for the `Cookie:` order, and what an update keeps (RFC 6265
    /// 5.3 step 11): re-setting a cookie must not move it to the end.
    std::uint64_t created = 0;
  };

  bool store(const Url& url, const std::string& setCookie, bool fromScript);
  /// Callers hold `mutex_`.
  std::vector<const Cookie*> matching(const Url& url, bool includeHttpOnly) const;
  void dropExpired(std::int64_t now);

  mutable std::mutex mutex_;
  std::vector<Cookie> cookies_;
  std::uint64_t nextCreated_ = 1;
};

}  // namespace screenkit::net
