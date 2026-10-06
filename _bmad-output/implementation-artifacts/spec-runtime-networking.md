---
title: 'Runtime networking: fetch, XHR, WebSocket, EventSource, streams and cookies over each OS native network stack'
type: 'feature'
created: '2026-09-17'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/runtime/js/README.md', '{project-root}/runtime/README.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** The JS runtime has no network. `fetch` and `XMLHttpRequest` only read package assets and
reject every `http(s)` URL; `WebSocket`, `EventSource`, `Headers`, `Request`, `AbortController`,
`ReadableStream` and cookies do not exist. Architecture §3.2 promises networking, real apps need it
(remote images, TMDB), and the M7 dev loop needs a WebSocket and HTTP to reach `vite dev`.

**Approach:** A native networking layer on each OS's own network stack — Network.framework on Apple for
every connection, plain and TLS — driven by one serial I/O queue per runtime and surfaced through
browser-standard APIs in the DOM shim: `fetch` with `Headers`/`Request`/`Response`/`AbortController`/
`FormData` and streaming bodies, a full `XMLHttpRequest`, `WebSocket`, `EventSource`, and a cookie jar.
Package-asset URLs keep resolving to the confined asset reader.

**Decisions (2026-09-17):**
- **Each OS's native network stack, no SDL3_net** (revised by the human during implementation,
  2026-09-17). On Apple (tvOS, macOS) every connection — `http:`, `https:`, `ws:`, `wss:` — runs on
  Network.framework, TLS with the system trust store. Android and Linux get their own native backends
  with their hosts. No networking or TLS library is added, and no CA roots are bundled.
- **Full scope in one spec:** `fetch` (with request and response streams), `XMLHttpRequest`,
  `WebSocket`, `EventSource`, cookies — in release builds too. Planned before the M7 dev loop, which
  builds on it (`spec-m7-module-runner-hmr-go-client.md`, draft).

## Boundaries & Constraints

**Always:**
- Web semantics: `fetch` rejects with `TypeError` only for network failures (an HTTP 404/500 resolves
  with `ok: false`); an abort rejects with an `AbortError` `DOMException`; XHR, WebSocket and
  EventSource fire events in the order their specs define.
- TLS certificates are always verified by the OS stack (trust, validity, host name). A bad certificate
  fails the connection; no bypass is reachable from JS.
- Network I/O never blocks the JS thread. Completions arrive as event-loop tasks, so they wait behind a
  paused runtime's freeze gate. An in-flight request, an open WebSocket or an open EventSource keeps
  `Runtime::idle()` false.
- Package-asset loading is unchanged and still confined (`dom-asset-confinement` passes).
- Shutdown with requests, streams or sockets open closes them and drops their callbacks without a crash.
- Every new behaviour gets a ctest row against a local fixture server, sabotage-checked; no test reaches
  the public internet.

**Never:**
- No SDL3_net and no third-party networking or TLS library; no edits to vendored expo-gl or
  `node_modules`.
- No HTTP/2, proxies, WebSocket `permessage-deflate`, `WritableStream`/`TransformStream`, or service
  workers — absent and documented.
- No change to `screenkit bundle`, the `.skpkg` format, or how package assets resolve.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| HTTP GET | `fetch('http://host/data.json')` | `ok`, `status`, `headers`, `json()` as sent | — |
| HTTPS | certificate trusted by the OS | same as HTTP | untrusted / expired / wrong host: `TypeError`; XHR `error`; WebSocket `error` + `close` 1006; EventSource `error`, closed |
| Request body | POST/PUT with string, JSON, `ArrayBuffer`/view, `Blob`, `URLSearchParams`, `FormData`, custom headers | server receives exact bytes, `Content-Type` and headers | — |
| Streaming request | body is a `ReadableStream` with `duplex: 'half'` | sent chunked as the stream yields | stream errors: fetch rejects, connection closed |
| Streaming response | server sends chunks slowly | `response.body.getReader().read()` yields each chunk before the response ends; `for await` works; `tee()` and `clone()` give both readers every byte | `cancel()` closes the connection |
| HTTP error status | 404 / 500 | resolves `ok: false`, body readable | — |
| Redirect | 301/302/303/307/308 chain | followed (≤ 20), `response.url` final, `redirected` true; 303 becomes GET; `redirect: 'manual'`/`'error'` honoured | loop or > 20: `TypeError` |
| Encoded body | `Transfer-Encoding: chunked`, `Content-Encoding: gzip`/`deflate` | body decoded | truncated: `TypeError` |
| Abort / timeout | `controller.abort()`, `AbortSignal.timeout(ms)`, XHR `abort()`, `xhr.timeout` | fetch rejects `AbortError`/`TimeoutError`; XHR `abort` or `timeout` then `loadend`; connection closed | — |
| Unreachable | DNS failure, refused connection | fetch `TypeError`; XHR `error`; WebSocket `error` + `close` 1006 | — |
| Cookies | response `Set-Cookie` (Domain, Path, Expires/Max-Age, Secure, HttpOnly, SameSite) | stored; sent on later matching requests (fetch, XHR, WebSocket, EventSource); `Secure` only over https; expired removed; persistent cookies survive a runtime restart | `credentials: 'omit'` sends and stores none |
| WebSocket | `ws:`/`wss:` echo; text, binary (`arraybuffer`, `blob`), close code + reason from either side, server ping, subprotocol | `open`, `message`, `close` with `code`/`reason`/`wasClean`; pong sent; `protocol`, `bufferedAmount` | handshake refused: `error` + `close` 1006 |
| EventSource | server sends `data`, named `event`, `id`, `retry` over `text/event-stream` | `open`, `message` and named events with `data`/`lastEventId`; after a dropped connection reconnects after `retry` with `Last-Event-ID`; `close()` stops it | non-200 or wrong content type: `error`, closed |
| Network image | `new Image().src` or `createImageBitmap(blob)` from an https PNG | decoded from memory, uploads to a texture | undecodable: `error` / rejection |
| Package asset | relative, `/abs`, `screenkit:` URL | read from the package as today, confined | missing: 404 as today |
| Shutdown / idle | request, stream, WebSocket or EventSource open | idle false while open; shutdown closes all, no callback, no crash | — |

</frozen-after-approval>

## Code Map

- `runtime/js/dom-shim.js` -- loading section 2104–2115 ("There is no network"); `resolveResource` 2121–2147 (http(s) throws 2128); `blobFromAsset` 2152; `XMLHttpRequest` 2185–2270 (GET-only 2240, no progress/abort/timeout); `Response` 2272–2289 (no constructor/clone, content-type-only headers); `fetch` 2291–2310 (ignores init); `Blob` 1914–1932 (`text()` Latin-1); `createImageBitmap` 2338–2367 and `Image.src` 2378–2401 need an asset path; `DOMException` 1803–1830; event dispatch/EventTarget from M6 (`dispatcher`, `EventTarget`). Undefined today: `Headers`, `Request`, `AbortController`, `AbortSignal`, `FormData`, `WebSocket`, `EventSource`, `ReadableStream`.
- Image decode -- `texImage2D` decodes `localUri` files via `stbi_load` (`runtime/third_party/gl/SKGLImageUtils.cpp:128`); `readImageSource` (`runtime/core/src/gfx/VendoredWebGL.cpp:397–421`) already takes `{data, width, height}` RGBA, so network images decode from memory natively (vendored stb_image) and upload through it, with `gl-premultiplied-alpha` rules.
- Native async -- `HostIO.cpp` `readFile`/`imageInfo` are synchronous (129–176); promises are made only in JS. `JsExecutor::invokeAsync` (`Runtime.h:38`, `HermesHost.cpp:351`) posts tasks from any thread (callers `EventLoop.cpp:48`, `Input.cpp:433`). `jsi/EventEmitter.h`, `NativeModule.h`, `SharedObject.h` exist for native→JS events and lifetime. `idle()` counts busy, queue, timers, frames only (`HermesHost.cpp:297`, `EventLoop.cpp:334`).
- Threads -- one `SDL_CreateThread` JS thread per runtime (`HermesHost.cpp:76`), SDL timer thread; `SdlSync.h` wrappers; no pool.
- Apple networking -- Network.framework for every connection: `nw_connection_create` with `nw_parameters_create_secure_tcp` (plain: `NW_PARAMETERS_DISABLE_PROTOCOL` for TLS; TLS: SNI via `sec_protocol_options_set_tls_server_name`, ALPN `http/1.1`, system trust; a test-only verify block adding anchors through `SecTrustSetAnchorCertificates` + `SecTrustEvaluateWithError`, fed from a `RuntimeConfig` field never reachable from JS), `nw_connection_send`/`nw_connection_receive` on a dispatch queue, DNS inside the connection. Low-level Network.framework is not subject to ATS.
- tvOS embedding -- only `hermesvm.framework` is copied and signed into the app (`runtime/apple/CMakeLists.txt:44–54`, rpath 41–42); **SDL3.framework is not embedded** (absolute build rpath: works on the simulator, fails on a device).
- Hosts -- `runtime/apple/HostMain.mm` creates runtimes (`runWindowed` 829, tvOS `SDL_AppInit` 713) and exits on `idle()` (575, 779, 896); a storage directory for persistent cookies comes from the host (macOS Application Support, tvOS Caches).
- Tests pinning today's behaviour -- `dom-fetch` (`RuntimeTests.cpp:1272–1292`) expects `https://example.com/x` to reject; `dom-coverage` 1343, `dom-identity-and-absence` 1357, `dom-texture-chain` 1239, `dom-image-element` 1294, `dom-asset-confinement` 1221, `dom-navigator` 1761.
- Docs -- `runtime/js/README.md:215, 253–258` ("No network", JS Blob cannot become an image); `Architecture.md:42, 54` (io layer), `160–161` (§3.2 provided list), `605` (says SDL3 links static — it is a dynamic framework).

## Tasks & Acceptance

**Execution:**
- [x] `runtime/CMakeLists.txt`, `runtime/apple/CMakeLists.txt` -- link Network.framework (and Security.framework) into `screenkit-core` on Apple; embed and sign `SDL3.framework` in the tvOS app with the `@executable_path/Frameworks` rpath, which is missing today.
- [x] `runtime/core/src/net/` (new) -- one serial I/O queue per runtime (a dispatch queue on Apple); a `Connection` interface with a Network.framework implementation on Apple (plain and TLS), compiled per platform so Android/Linux backends slot in later; HTTP/1.1 client (request serialisation incl. chunked upload, keep-alive pool per origin, chunked and gzip/deflate responses through the system zlib, redirects); RFC 6455 client (handshake, masking, fragmentation, ping/pong, close handshake); incremental body delivery for streams and EventSource; cookie jar (RFC 6265 storage model, persistent cookies in a host-supplied directory); completions via `JsExecutor`; open-work count feeding `idle()`; everything closed at shutdown.
- [x] `runtime/core/src/bindings/Net.cpp` (new) + `HostIO.cpp` -- the narrow handle API JS builds on (`__screenkit.net`: start/abort/stream request with head, chunk, end, error events; open/send/close socket with events; cookie get/set for a URL) and `decodeImage(bytes)` → `{width, height, data}` RGBA.
- [x] `runtime/js/dom-shim.js` -- `ReadableStream` (default reader, `read`/`cancel`/`releaseLock`, `tee`, async iteration), `Headers`, `Request`, `Response` (constructor, `clone`, `body`, body mixins, `redirected`, `type`), `fetch` (method, headers, body types incl. streams with `duplex:'half'`, `redirect`, `signal`, `credentials`), `AbortController`/`AbortSignal` (`abort`, `reason`, `timeout`, `any`), `FormData` (string/Blob fields, multipart), full `XMLHttpRequest` (all methods, headers, `responseType` text/json/arraybuffer/blob, `progress`/`abort`/`timeout`/`loadend`, `upload` progress), `WebSocket` (`binaryType`, `protocol`, `bufferedAmount`, codes/reasons), `EventSource` (named events, `lastEventId`, `retry`, reconnection, `withCredentials`); `resolveResource` routes http(s) to the network, package URLs unchanged; network images through `decodeImage`; `Blob.text()` UTF-8.
- [x] `runtime/tests/` + a Node fixture server started by ctest (HTTP, HTTPS with a generated test CA passed through the test-only anchor field, WS, WSS, redirects, chunked, gzip, slow/streaming, dropped connections, Set-Cookie, event-stream) -- one row per matrix row; update `dom-fetch`'s https expectation; existing rows keep passing.
- [x] `Architecture.md` §3.2, §10 (native network stack per OS, framework embedding, line 605), `runtime/js/README.md` (network section, cookie semantics, absent list), `runtime/README.md`, `deferred-work.md` (Android/Linux native network backends; absent features) -- match what ships.

**Acceptance Criteria:**
- Given the full ctest suite (macOS and ASan) with its local fixture servers and `node --test`, when run, then every matrix row passes and nothing reaches the public internet.
- Given the example app on the tvOS simulator, when its Images page loads remote https images, then they render.
- Given the tvOS app built, when its binary is inspected, then it links Network.framework, carries `SDL3.framework` in `Frameworks/` loaded through `@rpath`, and links no SDL3_net or third-party networking library.

## Implementation Notes

- **Shape.** `runtime/core/src/net/`: `Connection`/`IoQueue` seam (Connection.h), `NetworkFramework.mm` (Apple, plain and TLS, ARC, serial dispatch queue per runtime), `NetworkUnavailable.cpp` (every other platform: connections fail `unsupported`), `Http.cpp` (keep-alive pool, chunked upload, chunked + gzip/deflate via system zlib, redirects, 1xx skipped, one retry on a dead pooled connection), `WebSocket.cpp` (RFC 6455), `CookieJar.cpp` (RFC 6265, `cookies.txt` in `RuntimeConfig::storageDirectory`), `NetService` (synchronous shutdown). `bindings/Net.cpp` is `__screenkit.net`; `HostIO` gains `decodeImage`. `RuntimeConfig` gains `storageDirectory` and test-only `testTlsAnchors`; `EventLoop::holdWork/releaseWork` keeps `idle()` false while a request or socket is open.
- **Event delivery.** One queued task per request/socket (`EventChannel`), draining its backlog with a microtask checkpoint between events, so a paused runtime's backlog cannot overflow SDL's 65,535-event queue (`net-shutdown-idle` sends 70,000 messages while paused).
- **Decisions made in implementation** (each logged in deferred-work.md): `redirect: 'manual'` resolves with the real 3xx; cookies without a public-suffix list and with SameSite stored only, one jar per host bundle id; no backpressure and no per-origin connection cap; decode of downloaded images on the JS thread; ASan suite runs with `detect_container_overflow=0` (false positive from the uninstrumented prebuilt ANGLE); the `.invalid` DNS row goes through the system resolver.
- **Also.** `dom-shim.hbc` 107 KB -> 196 KB (Architecture §3 budget 500 KB). tvOS app now embeds and signs `SDL3.framework` with `@executable_path/Frameworks` as the only rpath (`BUILD_WITH_INSTALL_RPATH`).
- **Verified at step-03 (orchestrator, against the diff):** ctest macOS 121/121 and ASan 121/121 (15 `net-*` rows with the fixture server); `node --test` 63/63; `NetworkUnavailable.cpp` compiles with `clang++ -std=c++17 -fsyntax-only`; `otool -L` on the tvOS app: `@rpath/SDL3.framework`, `@rpath/hermesvm.framework`, Network.framework, Security.framework, no SDL3_net; the example app's Images page on the tvOS simulator renders its remote https images (`runtime/build/tvos-simulator/images-page.png`).
- **Review patches (step-04), verified by the orchestrator:** ctest macOS 121/121 and ASan 121/121, `node --test` 63/63, the non-Apple stub still compiles, the tvOS app links Network/Security and embeds SDL3 + hermesvm with no SDL3_net, and the example app's Images page renders its remote https images on the tvOS simulator after the decode changes. Untested by a row (compile-checked only): the 1xx cap, TCP keepalive, EOF-before-error ordering, drain exception safety, handle-API argument validation, the decode dimension caps, and dropping a Request's signal from its parent.

## Spec Change Log

- 2026-09-17, during implementation, by the human: "just use network.framework, no SDL3_net". The
  networking decision, Approach, the Never rule, Code Map, tasks, the third acceptance criterion and the
  design note now name Network.framework for every Apple connection (plain and TLS) and drop SDL3_net
  entirely (no prebuilt entry, no `Sdl3NetPrebuilt.cmake`, no framework to embed). Known-bad state
  avoided: two connection paths on Apple. KEEP: the matrix, the JS API surface, cookies/EventSource/
  streams, the I/O-thread and idle rules, and embedding `SDL3.framework`.

## Review Triage Log

| Finding | Verdict | Evidence | Route |
|---|---|---|---|
| BH1 `resolveUrl` treats `login?next=https://x` as absolute (a `:` before the first `/`) | medium | Url.cpp resolveUrl: the colon test takes the query's scheme; `parseUrl` then refuses scheme `login?next=https` and the redirect fails. Direct fix: a valid scheme with no `/?#` before the colon. | patch |
| BH2/ECH9 1xx interim responses recurse with no cap | low | Http.cpp handleHead calls onConnectionData(leftover) per 1xx; a server sending thousands overflows the dispatch thread's stack. Loop + cap is direct. | patch |
| BH3a no read timeout on a stalled request | false | Browser fetch/XHR have no default read timeout either; `AbortSignal.timeout` and `xhr.timeout` are the web's answer and both work (net-abort-timeout). | reject |
| BH3b open WebSocket/EventSource never notices a dead peer (Wi-Fi drop) | medium | NetworkFramework.mm sets no TCP keepalive and no viability handling, so a silently dead connection stays open and holds `idle()` false. TCP keepalive options on the connection are a few lines. | patch |
| BH4/ECH15/VG-O1 WebSocket `open` fires after `close()` during CONNECTING | medium | dom-shim.js openSocket handler sets `WS_OPEN` unconditionally when native `open` was already queued; readyState goes CLOSING -> OPEN. | patch |
| BH5 WebSocket `readyState` not CLOSED when `error` fires | low | The WHATWG "fail the WebSocket connection" steps set CLOSED before `error`; the shim sets it only on `close`. Direct fix. | patch |
| BH6/ECH2 a non-JSError exception in a drain leaves `scheduled_` true forever | low | Net.cpp NetBinding::deliver catches only jsi::JSError; anything else escapes drain with `scheduled_` set, so the channel never drains again and the loop hold is never released. RAII reset + broader catch is direct. | patch |
| ECH1 `invokeAsync` dropping the drain task leaves the channel stuck | low | Real only when SDL's queue is full or the host is stopping; the channel design exists to keep the queue from filling, and a stopping host shuts the binding down. Fix needs `invokeAsync` to report failure (an API change). | reject |
| BH7/ECH7 cookie `Path` is not checked for control characters | medium | CookieJar.cpp store: `pathAttr` is taken raw; `\n`/tab from a server or `__screenkit.net.setCookie` writes forged lines into cookies.txt, which reload as HttpOnly cookies for any domain. Direct `hasControl` check. | patch |
| BH7b no per-cookie size or per-domain count limit | low | Only a global 3000 cap; unlikely to matter for one app's jar, and limits add policy. | reject |
| BH8 dependent AbortSignals are never removed from their parent | low | Request construction and AbortSignal.any push onto `dependents` forever; an app reusing one controller across many fetches grows it. A fetch can drop its request signal from the parent when it settles. | patch |
| BH9/ECH24 `<img>` src change: the old download keeps running, and a later failed load leaves the previous pixels | medium | makeImage/downloadBytes keeps no request id to abort; `failed()` leaves `_pixels`/`_assetPath`, so `decode()` resolves and texImage2D uploads the stale image. Direct fixes. | patch |
| BH10a `decodeImage` has no dimension cap and copies the decoded buffer | medium | HostIO.cpp decodes whatever stb reports and copies into a vector: a small PNG claiming huge dimensions can OOM the process. `stbi_info_from_memory` cap + an owning buffer is direct. | patch |
| BH10b `Inflater::run` inflates one read into one allocation | low | A 256 KB read at a high ratio becomes one large `Bytes`; emitting per 64 KB output buffer is direct. | patch |
| BH11a a reused connection's retry re-sends a POST | low | Chromium also retries a request on a reused socket that failed before any response byte; browser-consistent. | reject |
| BH11b streaming-body requests take pooled connections they can never retry on | low | mayRetry is false for streams, so losing the race with a server's idle close fails the upload; skipping the pool for streams is direct. | patch |
| BH12/ECH14 a reset after FIN fails a close-delimited body, contrary to the comment | low | NetworkFramework.mm receiveNext fails on any error even with `isComplete`; direct reorder. | patch |
| BH13 `cache`/`integrity` accepted and ignored; no HTTP cache; not documented | low | `integrity` silently unenforced is a quiet failure the spec forbids; rejecting a non-empty `integrity` loudly and documenting the absent HTTP cache is direct. | patch |
| BH14a net rows fail rather than skip where `networkAvailable()` is false | low | No non-Apple build runs today; the skip is a one-line check. | patch |
| BH15 no `Origin` header on WebSocket handshakes or requests | low | Browsers send one; some servers (ActionCable) check it. There is no real origin (`screenkit://`); the deviation goes in the documented differences. | patch |
| VG1 net-image cannot tell natural-size upload from a crop | medium | Pre-verified: flat opaque 13x7 fixture, texel (0,0) only; reverting the naturalWidth read passes. | patch |
| VG2 no test of the microtask checkpoint between network events | medium | Pre-verified: deleting Net.cpp's checkpoint passes every row. | patch |
| VG3 EventSource `retry:` never observed | low | Pre-verified: the reconnect lands inside the 10 s wait at the 3 s default. | patch |
| VG4/ECH19 HEAD, 204, 304 never requested; `response.body` is a stream for them | medium | Pre-verified test gap; and dom-shim.js networkFetch builds a ReadableStream body for null-body statuses and HEAD, where the spec says null. | patch |
| VG5 WebSocket handshake refusals (bad Accept, unrequested protocol, extensions) untested though README says asserted | medium | Pre-verified: the fixture always handshakes correctly. | patch |
| VG6 fragmented incoming WebSocket messages untested | medium | Pre-verified: the fixture always sets FIN. | patch |
| VG7 a future cookie `Expires` untested | low | Pre-verified: only a 1970 Expires is sent. | patch |
| VG8 Blob UTF-8 only tested with ASCII | low | Pre-verified. | patch |
| VG9 missing-asset async XHR contract (404, error, loadend) untested | medium | Pre-verified: Lightning depends on it. | patch |
| VG10 hosts passing a storage directory untested | low | Pre-verified gap; needs the fixture server plus an isolated home or simulator loopback. | defer |
| VG-O2/ECH25 body reads alias shared bytes (clones, blob: URLs) | low | dom-shim.js consumeBody returns `body.bytes` uncopied and teeBody shares it; mutating one result changes the other or the registered Blob. Direct copy. | patch |
| ECH3 NaN `byteOffset`/`byteLength` pass the range check | medium | Net.cpp bytesFrom and HostIO decodeImage compare with `<`/`>`, which NaN passes, then cast to size_t: undefined behaviour and an out-of-bounds read reachable from JS. Direct `isfinite` check. | patch |
| ECH4/ECH5/ECH6 the handle API trusts ids, close codes, close reasons and subprotocol strings | low | Non-integral/NaN ids and codes are cast undefined; an over-123-byte reason makes an illegal control frame; CR/LF in a subprotocol injects handshake header lines. The JS APIs validate, `__screenkit.net` does not; direct checks. | patch |
| ECH8 two runtimes sharing a storage directory race on `cookies.txt.tmp` | low | One runtime per host today (deferred-work already records one jar per host bundle id); a merge/lock scheme is new machinery. | reject |
| ECH10 duplicate `Content-Length` header lines that disagree: the first wins | low | Http.cpp reads `head.get` (first line only); RFC 9112 requires rejecting conflicting lengths. Direct. | patch |
| ECH11 an empty gzip/deflate body (Content-Length 0, empty chunked) fails as decode | medium | handleHead creates the inflater for any framed body; finish() then sees it unfinished and fails. Direct: only a body that fed input must finish. | patch |
| ECH12 a second gzip member arriving in a later read is ignored | low | Multi-member gzip split exactly at a read boundary is rare, and handling it adds state. | reject |
| ECH13 an early response before the request body is flushed pools a desynchronised connection | low | finish() pools on `bodyWritten_`, not on the body being flushed; direct check against `bytesFlushed() >= bodyEnd_`. | patch |
| ECH16 `close(undefined, reason)` drops the reason | low | The WebSockets standard sets code 1000 when a reason is given without a code. Direct. | patch |
| ECH17 EventSource reconnects forever on `unsupported`/`url` errors | low | dom-shim.js treats only `tls` as fatal; on a platform without a backend it loops and never idles. Direct. | patch |
| ECH18 abort/error while the pump's reader holds the request body stream | low | networkFetch calls `body.stream.cancel`, which rejects on a locked stream, so the source's cancel never runs. Direct: cancel through the reader. | patch |
| ECH20 FormData parsing reads `name=` out of `filename=` | low | The regex matches inside `filename="..."` when it comes first. Direct. | patch |
| ECH21 a Blob/File type with CR/LF injects multipart part headers | low | The File API normalises a type with non-printable characters to ''. Direct. | patch |
| ECH22 XHR HEAD on a package asset returns the body | low | sendAsset feeds the bytes for HEAD. Direct. | patch |
| ECH23 `xhr.timeout` set after `send()` is not applied | low | Legal but unusual; applying it needs the send time tracked. | reject |
| ECH26 the fixture's closed port can be reassigned to a later listen | low | server.mjs picks it before the listens; direct reorder and distinctness check. | patch |
| ECH27 `fetch({url})` objects no longer work | false | Browsers stringify a non-Request input (`[object Object]`); the old acceptance was non-standard and nothing measured passes such an object. | reject |

## Design Notes

**Why JS on a narrow native handle.** Architecture §2 keeps browser-shaped APIs in JS; native owns only
what needs the OS — sockets, TLS, cookies' storage, decode. `Headers`/`Request`/`Response`/`FormData`,
streams, XHR's and EventSource's state machines are spec bookkeeping, easier to get right and test in JS.

**Cookies without an origin.** The document is `screenkit://`, so under browser rules every request is
cross-origin and `credentials: 'same-origin'` would never send a cookie. A TV app is closer to a native
client (React Native sends cookies by default): the jar applies to every request unless
`credentials: 'omit'`; XHR's `withCredentials` is accepted and does not gate cookies. `HttpOnly` cookies
are never exposed to JS, and there is no `document.cookie` view of the jar.

**One connection path per OS.** HTTP and WebSocket framing sit above the `Connection` interface and do
not care which stack carries the bytes; on Apple that is Network.framework for plain and TLS alike, so
there is one path and one set of connection bugs.

## Verification

**Commands:**
- `ctest --test-dir runtime/build/macos` and `--test-dir runtime/build/macos-asan` -- expected: 100% pass.
- `node --test packages/@screenkit/cli/test packages/@screenkit/vite-plugin/test` -- expected: all pass.

**Manual checks:**
- tvOS simulator, example app: Images page renders remote images; `otool -L` on the app binary shows `SDL3.framework` through `@rpath` inside the app, Network.framework, and no SDL3_net.
