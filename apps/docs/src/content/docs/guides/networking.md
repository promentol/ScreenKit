---
title: Networking
description: fetch, XHR, WebSocket and EventSource over each platform's own HTTP client — and the divergences that follow from that.
---

Networking is **each platform's own HTTP client**, driven from native and surfaced as browser APIs.
ScreenKit owns no HTTP, WebSocket or cookie implementation on Apple or Android: name resolution,
connection pooling, HTTP/1.1 **and HTTP/2**, redirects, `Content-Encoding`, proxies, certificate
verification against the system trust store, and the cookie store are all the OS's.

| Platform | Client | Cookie store |
|---|---|---|
| Apple | NSURLSession | `NSHTTPCookieStorage` |
| Android | OkHttp, over JNI | `android.webkit.CookieManager` |
| Linux | a vendored cpp-httplib, behind the same seam | an in-memory RFC 6265 jar |

No networking or TLS library is bundled, and **no CA roots ship**.

## What you get

```js
const res = await fetch('https://api.example.com/shows', {
  headers: { Authorization: `Bearer ${token}` },
  signal: controller.signal,
})
const shows = await res.json()
```

`fetch` with `Headers`, `Request`, `Response`, `AbortController` / `AbortSignal`, `FormData`, and
streaming request and response bodies over `ReadableStream`. Also the whole of `XMLHttpRequest`
(upload progress included), `WebSocket`, `EventSource`, a cookie jar, `URL` and `URLSearchParams`.

Package-asset URLs read from the package rather than the network.

## Linux is the exception, and only for the protocol

React Native has no Linux target and the distributions have no client to point at, so Linux gets a
vendored cpp-httplib — one MIT header pinned by sha256 — behind the same seam. The rules that matter
still hold: **TLS is the image's own OpenSSL and the roots are the image's own CA bundle**, found at
run time and never shipped. An image without libssl fails at configure time rather than falling back
to plaintext.

What does not survive the substitution is recorded rather than worked around:

- **No HTTP/2 on Linux.**
- **No WebSocket extensions** there, so no `permessage-deflate` (NSURLSession and OkHttp both offer
  it).
- Cookies there are the one jar left in the tree, which also makes it the one place where cookie name
  prefixes and the secure-origin rule are ours to enforce.

## Absent

`WritableStream` and `TransformStream` (so no `pipeTo` / `pipeThrough`), byte streams and BYOB
readers, synchronous network XHR, and `document.cookie`.

HTTP/2 and proxies are no longer absent and no longer ours — they are whatever the platform's client
does.

:::note[Per-platform divergence is recorded, not hidden]
Where the three clients genuinely disagree, the difference is written down and asserted per platform
by the test suite rather than smoothed over in JavaScript. If your app depends on one of them, check
[targets](/reference/targets/).
:::
