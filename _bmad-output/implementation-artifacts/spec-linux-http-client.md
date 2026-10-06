---
title: 'Linux network client: cpp-httplib over the system OpenSSL, with WebSocket, a Linux-only cookie jar'
type: 'feature'
created: '2026-09-18'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'NO_VCS'
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-platform-http-clients.md', '{project-root}/runtime/core/src/net/NetService.h']
---

<frozen-after-approval reason="human-owned intent -- do not modify unless human renegotiates">

## Intent

**Problem:** Linux is the one target with no HTTP client. Apple has NSURLSession and Android has
OkHttp behind `NetService`; Batocera builds `NetServiceUnavailable`, so every `fetch`, XHR,
WebSocket, EventSource and network image fails with `unsupported`. The Blits example app's TMDB demo
cannot run there.

**Approach:** A Linux implementation of `NetService` over **cpp-httplib** (yhirose/cpp-httplib), a
single MIT-licensed header, with TLS from the image's own OpenSSL and its CA bundle. WebSocket goes
through the library's own `WebSocketClient` (amended 2026-09-18 with the WebSocket decision below;
this paragraph originally said the library had none). Cookies come from a small jar written for this
platform,
because Linux has no platform store to point at.

## Boundaries & Constraints

**Always:**
- **`NetService` is the seam, unchanged.** `bindings/Net.cpp`, `__screenkit.net` and the DOM shim are
  untouched, as they were for Apple and Android.
- **TLS is the image's.** cpp-httplib is built with `CPPHTTPLIB_OPENSSL_SUPPORT` against the
  distribution's OpenSSL and its CA bundle, taken through `SCREENKIT_SYSROOT` the way SDL3 already
  is. Nothing is bundled and no CA roots ship. `testTlsAnchors` is honoured for the fixture only and
  is never settable from JS.
- **Backpressure is real here.** cpp-httplib's `ContentReceiver` returns false to stop reading, so
  `flowWindow`/`acknowledgeResponseData` work as they do on Android -- not best-effort as on Apple.
- **The `net-*` rows are the contract**, every row must pass on Linux, `net-websocket`
  included, and any row that changes is recorded as a divergence.
- **The dependency is pinned by sha256**, as the SDL headers and the expo-gl import already are.

**Never:**
- A bundled TLS library, a bundled CA bundle, or a certificate-verification bypass.
- Changes to `__screenkit.net`, the DOM shim's networking, or JS-visible API shape.
- Reviving the deleted portable HTTP or WebSocket stack for this platform.
- Edits to vendored code.

**Decision (WebSocket, superseded 2026-09-18):** originally "not supported on Linux, because
cpp-httplib has no WebSocket". **That reason was false.** The coordinator asserted it from memory of
older releases without checking the header it was about to pin; the review found
`class WebSocketClient` at `runtime/third_party/httplib/httplib.h:4508` in the vendored v0.56.0, with
`connect`, `read`, `send`, `close`, `is_open`, `subprotocol`, a ping interval and a missed-pong limit.
The human re-made the decision on correct information.

**Decision (WebSocket, 2026-09-18, replaces the above):** Linux supports WebSocket through
cpp-httplib's own `WebSocketClient`. `openSocket`/`sendSocket`/`closeSocket` and `SocketSink` behave
as they do on Apple and Android, and `net-websocket` runs on the device as a real socket row rather
than asserting `unsupported`. The deleted RFC 6455 code stays deleted: the library supplies the
implementation, so nothing is re-owned. The superseded text read: cpp-httplib has no WebSocket, and the
RFC 6455 implementation deleted earlier today is unrecoverable -- there is no version control and no
copy in the tree. `openSocket` fails with `NetError::Unsupported` followed by `onClose(1006)`,
exactly as `NetServiceUnavailable` does, so a page sees the same behaviour it sees today rather than
a hang. Recorded as a platform divergence.

**Decision (cookies, 2026-09-18):** a small jar for this platform only, held **in memory** for the
lifetime of the runtime. It is not persisted, because `RuntimeConfig::storageDirectory` and its
plumbing were deleted this afternoon and persisting would mean restoring them; Apple already keeps a
private per-runtime store that does not survive a restart, so this matches it rather than Android.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| The rows that do not need a socket | every `net-*` row except `net-websocket`, on a Batocera build | the transcripts macOS produces | a row that cannot pass is a recorded divergence, not a silent edit |
| WebSocket | `new WebSocket(...)` to the fixture | opens, exchanges text and binary frames, closes with the peer's code | a refused handshake fails with `onClose(1006)`, never a hang |
| Backpressure | a large body nobody reads | the server stalls, then the body reads in full -- as on Android | N/A |
| TLS | the fixture's untrusted, expired and wrong-host certificates | `NetError::Tls` | connection fails, no bypass |
| No OpenSSL in the image | the sysroot has no libssl | the build fails loudly at configure time | never a silent plaintext fallback |
| Cookies | a `Set-Cookie` then a later request to the same origin | the cookie is sent back; it does not survive a restart | N/A |

</frozen-after-approval>

## Code Map

**The seam (unchanged, three implementations today):** `runtime/core/src/net/NetService.h` --
`create(NetConfig)` `:105`, then `startRequest` `:112`, `appendRequestBody` `:113`,
`finishRequestBody` `:114`, `abortRequest` `:116`, `acknowledgeResponseData` `:118`, `openSocket`
`:120`, `sendSocket`, `closeSocket`, `cookiesFor`, `setCookie`, `shutdown`. Sinks: `HttpSink` `:70-77`
(`onHead`/`onData`/`onEnd`/`onError`/`onUploadProgress`) and `SocketSink` `:83-91`.

**The skeleton to start from:** `runtime/core/src/net/NetServiceUnavailable.cpp` (76 lines) -- it
already answers every method and settles both sinks with `Unsupported`; the WebSocket half of this
work is what it already does.

**The two working references:** `NetServiceAndroid.cpp` for the shape that has real backpressure (a
blocking reader loop plus `acknowledgeResponseData`), and `NetServiceApple.mm` for redirect modes,
upload progress and the test-anchor trust path. Both were reviewed on 2026-09-18; their triage log is
in `spec-platform-http-clients.md`.

**Threading:** `runtime/core/src/net/IoQueue.{h,cpp}` -- one serial queue per runtime, already used by
both clients and by image decode. cpp-httplib is **synchronous**: a request blocks its thread until
the response ends, so each in-flight request needs a thread of its own, with callbacks posted back
onto the `IoQueue`. That is a different model from NSURLSession and OkHttp and is the main design
risk.

**How a system library reaches the Linux build:** `runtime/cmake/LinuxSystem.cmake:20-37` --
`SCREENKIT_SYSROOT` holds `include/` and `lib/` for the target's libraries, `find_library(... PATHS
"${SCREENKIT_SYSROOT}/lib" NO_DEFAULT_PATH REQUIRED)`, with a loud `FATAL_ERROR` when a header is
missing. `tools/batocera/pi.sh sysroot` fills it by copying the libraries off the Pi and taking
headers from pinned source releases by sha256. OpenSSL follows that path; the CA bundle is the
image's own file, found at runtime, never copied into the repo.

**Where the backend is chosen:** `runtime/CMakeLists.txt` -- the `if(APPLE)/elseif(ANDROID)/else()`
fork that currently sends Linux to `NetServiceUnavailable.cpp`.

**Tests:** `runtime/tests/CMakeLists.txt` (the `net-*` rows and the `net-server` fixture);
`RuntimeTests.cpp` `netFixture()` skips on `!networkAvailable()`; `runtime/tests/net/server.mjs` is a
host-side node process. **The suite does not build for Linux today** -- `runtime/CMakeLists.txt` gates
it on `APPLE AND NOT tvOS`, and `tools/batocera/pi.sh` configures `-DSCREENKIT_BUILD_TESTS=OFF`. The
Android work solved the same problem with a net-only target plus an `android.sh test` subcommand
(`tools/android/android.sh`), and that is the pattern to copy.

**Unverified, and to confirm first:** whether the Batocera image carries `libssl`/`libcrypto` and a CA
bundle at all. The Pi was unreachable when this was written, so nothing here is measured. If the image
has no OpenSSL, the approach needs revisiting before any code is written.

## Tasks & Acceptance

**Execution:**
- [ ] **NOT DONE -- the device was unreachable for the whole of this implementation.** Confirm on the
  device that Batocera ships `libssl`/`libcrypto` and a CA bundle, and record the
  versions -- everything below assumes it. `sh tools/batocera/pi.sh probe` is the one command that
  answers it, and `pi.sh sysroot` refuses an image that fails it.
- [x] `tools/batocera/pi.sh` -- extend `sysroot` to take OpenSSL's libraries off the Pi with headers
  from the pinned source release, as it already does for SDL3 and SDL3_ttf. **Amended: the libraries
  come off the Pi, the headers cannot come from a source release** (see the change log).
- [x] Vendor cpp-httplib as a single header pinned by sha256, consistent with how the SDL headers and
  the expo-gl import are pinned; record the version. **v0.56.0.**
- [x] `runtime/core/src/net/NetServiceLinux.cpp` (new) -- `NetService` over `httplib::Client`:
  a thread per in-flight request with callbacks posted onto the `IoQueue`, `ContentReceiver` driving
  `flowWindow`, the three redirect modes, upload progress, response-head capture with duplicate
  headers intact, and `openSocket`/`sendSocket`/`closeSocket` over `WebSocketClient` (superseding the
  original `Unsupported` + `onClose(1006)`, per the WebSocket decision).
- [x] `runtime/core/src/net/LinuxCookieJar.{h,cpp}` (new) -- an in-memory jar for this platform:
  store on `Set-Cookie`, return a `Cookie` header for an origin, honour Secure/HttpOnly/expiry and
  the domain/path rules, and keep `HttpOnly` out of `cookiesFor`.
- [x] TLS -- `CPPHTTPLIB_OPENSSL_SUPPORT` against the sysroot's OpenSSL, the image's CA bundle, and
  `testTlsAnchors` added to the store only when non-empty, never replacing it.
- [x] `runtime/CMakeLists.txt`, `runtime/cmake/LinuxSystem.cmake` -- select the new client for Linux
  and fail at configure time when OpenSSL is absent.
- [x] Tests -- a net-only test target that cross-compiles for linux-arm64 and a `pi.sh test`
  subcommand that starts the fixture, reaches it from the device, and runs every row that does not
  need a WebSocket; mirror `tools/android/android.sh test`, including its nonce binding so a stale
  log line cannot be read as a pass. **Written and the target is proven; `pi.sh test` itself has
  never been run, because it needs the device.**
- [x] Docs -- `Architecture.md` §3.2 and the linking table, `runtime/README.md`,
  `runtime/js/README.md`'s "Where the platforms differ" table, and `deferred-work.md`.

**Acceptance Criteria:**
- Given a Batocera build, when the net rows run on the device, then every row that does not need a
  WebSocket passes, none skipped.
- Given a page on the device, when it opens a `WebSocket` to the fixture, then it connects, exchanges
  text and binary frames, and closes -- and a refused handshake fails with a 1006 close rather than
  hanging. (Replaces the original "fails at once with `unsupported`", per the WebSocket decision.)
- Given a large response body left unread, when it is measured at the server, then the server stalls
  -- real backpressure, as on Android.
- Given macOS and Android builds, when their suites run, then they are unchanged (160/160, 154/154,
  18/18).
- Given an image with no OpenSSL, when the build is configured, then it fails with a message naming
  what is missing.

## Implementation Notes

**Device verification, 2026-09-18 (coordinator).** The Pi was unreachable while this was implemented;
it came back afterwards and every untested criterion was then settled on the real device:

- `pi.sh probe` -- the spec's precondition holds: **OpenSSL 3.4.0**, `libssl.so.3` / `libcrypto.so.3`,
  a 213,777-byte CA bundle at `/etc/ssl/certs/ca-certificates.crt`, glibc 2.40 (Buildroot).
- `pi.sh sysroot` -- OpenSSL's libraries taken off the device; headers from `libssl-dev` in the
  pinned build image, as GLES and EGL already do.
- `pi.sh build` -- linux-arm64 builds and links `NetServiceLinux.cpp`, `LinuxCookieJar.cpp` and
  `IoQueue.cpp` with the real toolchain for the first time.
- `pi.sh test` -- **all 15 rows pass on the device**, none skipped: net-http-get, net-https,
  net-request-body, net-streaming-request, net-streaming-response, net-http-error-status,
  net-redirect, net-encoded-body, net-abort-timeout, net-flow-control, net-unreachable, net-cookies,
  net-websocket, net-eventsource, net-shutdown-idle. (`net-image` excluded, as on Android.)
- macOS 160/160, macos-asan 154/154, android 18/18 -- all unchanged, re-run by the coordinator.

So criteria 1, 2 and 3 are met on hardware rather than in a container, and the container run's value
was in finding a fault in itself: its first harness absorbed 30.9 MB and failed both
`net-flow-control` stall lines while the client was correct.

**Shape.** `runtime/core/src/net/` gains `NetServiceLinux.cpp` (1,057 lines) and
`LinuxCookieJar.{h,cpp}` (84 + 395), and `runtime/third_party/httplib/` holds one vendored MIT
header (cpp-httplib v0.56.0, commit `278c2979`, 22,875 lines) with its `LICENSE` and a `VENDOR.md`.
`tools/vendor/httplib.sh` re-imports it at the pin and `--check`s it; `runtime/cmake/HttplibVendored.cmake`
re-checks both sha256s at configure time, so a hand-edit fails the Linux build rather than shipping.
`NetServiceUnavailable.cpp` stays, for a platform that is neither Apple, Android nor Linux.
`bindings/Net.cpp`, `__screenkit.net` and `runtime/js/dom-shim.js` are untouched, as the boundaries
required.

**Backpressure is real, but not by the mechanism the spec named.** Returning false from
cpp-httplib's `ContentReceiver` is *cancellation* -- it fails the request with `Error::Canceled` --
not a pause, so it could not drive `flowWindow`. What drives it instead is stronger and simpler:
each request runs on a worker thread of its own, so the receiver callback **is** the thread reading
the socket, and blocking in it on a condition variable stops the socket being drained. The receive
window closes and the server stalls -- OkHttp's guarantee, reached a different way. Returning false
is kept for the one thing it means: an abort. `net-flow-control` asserts the same under-24-MiB stall
on Linux that it asserts on Android, and `stallsUnreadBodies` is now a property of the client rather
than a list of platforms.

**Threading, and the two bounds.** Every sink call is posted onto the runtime's one serial `IoQueue`
and nothing touches a sink anywhere else, which is the same contract the other two clients keep. A
request is admitted on that queue against two counters -- six per origin (what browsers do over
HTTP/1.1, and what `net-flow-control` asserts at the server) and sixteen in all, because on this
platform an in-flight request is a thread and a page that opens fifty images must not open fifty
threads on a Pi 3. A request that does not fit waits in a FIFO and can be aborted from there without
ever holding a slot. Idle `httplib::Client`s are pooled per origin, which is what makes
`Connection: keep-alive` mean anything across two fetches and pays for the TLS handshake once.

**Redirects and cookies are followed here, not inside the library.** cpp-httplib can follow a
redirect itself, but then the jar would not see a hop's `Set-Cookie`, the final URL would be
guesswork, and `redirect: 'manual'`/`'error'` would have nowhere to hook in. The loop implements the
Fetch rules (301/302 POST and 303 non-GET become GET and drop the body and its headers, 307/308 keep
both, `Authorization` is dropped cross-origin, twenty hops and the twenty-first is a network error)
and asks the jar for a `Cookie` header on every hop.

**Three small decisions worth naming.** `Expect: 100-continue` is disabled
(`CPPHTTPLIB_EXPECT_100_THRESHOLD 0`): upstream adds it to any body over 1 KiB and browsers never
send it. `set_path_encode(false)`: the target arrives already percent-encoded from the WHATWG parser
on the JS side, and re-encoding it would turn `%3A` into `%253A`. And a body whose type the page did
not give goes out with an empty `Content-Type`, exactly as the Apple client does and for the same
reason -- cpp-httplib would otherwise write `text/plain` into it, which is a lie about the bytes.

**A host label over 63 octets is `NetError::Url`**, refused before anything is resolved (RFC 1035
2.3.4), which is what OkHttp does and what `net-unreachable` already accepts. A real resolution
failure is told apart from a refused connection on the error path only, by asking `getaddrinfo`
whether the name resolves at all: cpp-httplib reports both as `Error::Connection`, and the two mean
different things to `EventSource`, which retries one and gives up on the other.

**Two bugs found by reading rather than by a row**, both in the first draft and both fixed: the
worker copies `request->sink` when it starts, so clearing that member from the I/O queue during an
abort was a plain data race on a `shared_ptr` (the sink is dropped after the join now, and
`cancelled` is what silences delivery); and `unacknowledged` has to be incremented *before* the
chunk is posted, or an acknowledgement landing between the two is clamped to zero and lost and the
reader then waits for bytes the page has already read -- the same hole review pass 1 found in the
Android client (finding 9).

**Every row that runs on the other two platforms runs here, and all fifteen pass.** Twelve share the
one transcript the other platforms use. Three carry a Linux branch: `net-encoded-body` (cpp-httplib's
own `Content-Encoding` behaviour), `net-cookies` (the jar's) and `net-websocket`. *This paragraph
describes the first round, before WebSocket was implemented -- see the 2026-09-19 note below.* `net-https` and
`net-shutdown-idle` guard their WebSocket segments on a new `__net.webSockets`, the same shape as
`__net.stallsUnreadBodies`. `net-image` is left out, as it is on Android: it uploads to a texture and
this binary has no drawable. **That is measured on a linux/arm64 container, not on the device** --
see Verification.

## Spec Change Log

**Patch round and state, 2026-09-19 (coordinator).** All 32 review items are in, WebSocket included,
verified from the tree rather than from the report. The previous session ended mid-round; the tree was
checked for half-applied edits first and was consistent.

- WebSocket runs over `WebSocketClient`: two threads per socket, a bounded send queue, and a 250 ms
  read timeout so the reader notices cancellation. **One line stays per-platform:** a close the *peer*
  sends reports as 1006, because cpp-httplib's `WebSocket::read` echoes the Close frame and discards
  its payload with no accessor for it. Closes this side sends keep their code and reason.
- New rows: `net-system-ca` (image roots must *succeed*, via `SSL_CERT_FILE`), `net-cookie-rules`
  (single-label `Domain`, `Secure` from plaintext, `__Secure-`/`__Host-`), `net-upload-overflow`.
- Three bugs found beyond the list, **two of them on the Pi and invisible to the container**: every
  WebSocket send went out as Binary (in cpp-httplib the overload picks the opcode), and a double
  `join()` race between `shutdown` and the retirement job hung `net-shutdown-idle`.
- Pushbacks upheld: the same-origin `Authorization` control became "sent when nothing redirects",
  since NSURLSession strips it on every redirect; the Apple empty `Content-Type` stays, because
  CFNetwork otherwise invents `application/x-www-form-urlencoded`; the private httplib members are
  documented rather than replaced, because the public `Post` overloads force `text/plain`.
- Verified by the coordinator on the final build: macOS **161/161**, macos-asan **155/155** (after
  the `IoQueue` interface change that Apple and Android share), Android **18/18**, Linux compiles.
- **Device, final build, 2026-09-19: `pi.sh test` 18/18 on the Pi**, none skipped -- the fifteen
  shared rows (`net-websocket` as a real socket row, `net-shutdown-idle` included, the row the
  double-`join()` race used to hang) plus `net-system-ca`, `net-cookie-rules` and
  `net-upload-overflow`. All five acceptance criteria are now met on hardware.


- 2026-09-18 -- implemented, with three amendments to what the tasks said and one task not done.
  The frozen block is untouched.

  **Not done: the first task.** `batocera.local` did not resolve at any point during this work, and
  was polled roughly every forty-five seconds throughout. So nothing in "Unverified, and to confirm
  first" was confirmed, and *every* acceptance criterion that names the device is untested. What was
  built instead is the means to answer it in one command -- `sh tools/batocera/pi.sh probe` reports
  the image's SDL3, `libssl`, `libcrypto`, OpenSSL version and CA bundle -- and the failure paths for
  each way the answer could be "no": `pi.sh sysroot` refuses an image with no `libssl.so.3` and names
  what it found instead, `cmake/LinuxSystem.cmake` fails at configure time with the same message, and
  the client logs a warning naming every path it looked in when an image has no CA bundle.

  **Amendment 1 -- OpenSSL's headers cannot come from a pinned source release.** The task asked for
  the SDL3 arrangement: libraries off the Pi, headers from the release the Pi runs, pinned by sha256.
  OpenSSL's `opensslconf.h` and `configuration.h` are *generated* by its own Configure script and are
  in no source tarball, so there is nothing to extract. The libraries still come off the device; the
  headers come from `libssl-dev` in the pinned Debian 12 build image, which is the arrangement GLES
  and EGL already use in the same file. It is safe in one direction only -- OpenSSL 3.x keeps its
  ABI, so Debian's 3.0 headers against a newer 3.x on the image are fine -- and `pi.sh sysroot`
  refuses anything that is not `libssl.so.3` rather than letting a 1.1.1 mismatch reach the linker.
  Filed in deferred-work.md.

  **Amendment 2 -- `ContentReceiver` returning false is cancellation, not a pause.** The frozen
  block's *requirement* (real backpressure, as on Android) holds and is asserted; the mechanism named
  in it does not exist. See the Implementation Notes: blocking in the receiver on its own thread is
  what stops the socket being drained.

  **Amendment 3 -- `net-websocket` runs on Linux rather than being excluded.** The task list says
  "every row that does not need a WebSocket", which would have dropped this one. It carries a Linux
  transcript instead, because the frozen block's own I/O matrix makes a claim that nothing else
  would check -- that a socket fails *at once* and never hangs -- and acceptance criterion 2 is
  exactly that claim. Two other rows, `net-https` and `net-cookies`, have a WebSocket segment inside
  an otherwise portable transcript; those segments are guarded on `__net.webSockets` rather than the
  rows being dropped.

  **Recorded divergences beyond the frozen block's:** no HTTP/2 on this platform; a *raw*-deflate
  `Content-Encoding: deflate` body fails rather than being decoded (zlib's header detection reads the
  wrapped form only); and a worker blocked in `getaddrinfo` cannot be interrupted, so an abort or a
  shutdown waits for the resolver. All three are in deferred-work.md.

## Review Triage Log

## Review Triage Log

Review pass 1, 2026-09-18. Three layers: blind-hunter (16), edge-case-hunter (13), verification-gap
(5 + 2 other). The staged diff again omitted `RuntimeTests.cpp`, `tests/CMakeLists.txt`, `VENDOR.md`
and the `Dockerfile`; blind-hunter findings that the tests, VENDOR.md or the Dockerfile change "are
not in the change set" are artefacts of that staging and are rejected on it. `pi.sh test` was run on
hardware and every row passed, so "it fails out of the box" is refuted directly.

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| 1 | cpp-httplib v0.56.0 **does** have a WebSocket client | **high** | Verified: `class WebSocketClient` at `httplib.h:4508` with connect/read/send/close/is_open/subprotocol/ping. The frozen "not supported" decision rested on a coordinator error; the human re-made it and Linux now implements WebSocket. |
| 2 | `Authorization` is dropped cross-origin by code nothing asserts | **high** | Verified: the drop is `NetServiceLinux.cpp:787`; `grep -rni authorization runtime/tests` returns nothing. Delete the line and a bearer token follows a redirect to any host with the suite green. |
| 3 | The image's CA roots are only ever asserted to fail | **high** | `net-https`'s production block asserts a rejection; every other row supplies its own anchor. Make `trustPaths()` return nothing and https to every real host breaks on device, suite green. |
| 4 | The single-label `Domain` refusal is unreachable by any fixture host | **high** | Verified: the only `Domain=` values are `127.0.0.1` (IP literal, short-circuited) and `example.com` (has a dot). Delete `LinuxCookieJar.cpp:262` and one site can set a cookie for a whole TLD. |
| 5 | A `Secure` cookie can be set over plaintext | **high** | `url.secure()` is read in `matching()` and never in `store()`, so a plaintext hop can plant or overwrite a cookie later sent over https. |
| 6 | Workers are joined on the serial I/O queue | **high** | `abortRequest` and the retirement job call `worker.join()` inside a queue job and `shutdown()` does it inside `sync`, with a 7-day read timeout; a worker in `getaddrinfo`, a TLS handshake or connect stalls every other request. |
| 7 | `cancel()` can be lost between publishing the client and `send()` | **high** | `clientMutex` is released before `send`, `cancelled` is not re-checked, and httplib's `stop()` has no sticky flag; the traced path is reachable in `net-flow-control`. Outcome is a hung queue and a timeout, not a clean failure. |
| 8 | `shutdown()` can deadlock against its own mutex | **high** | `sync` from the queue thread, or `sink.reset()` re-entering shutdown while `shutdownMutex_` is held, hangs permanently. `IoQueue.h:33` forbids sync-from-queue but nothing detects it. |
| 9 | The 8 MiB streamed-upload overflow path has no row | **medium** | `net-streaming-request` moves tens of bytes; `grep "outran"` finds no test. Unverified on Apple too, where the same message exists. |
| 10 | `Max-Age` overflow is undefined behaviour | **medium** | `atoll` then `nowSeconds() + seconds` signed-overflows; a "never expires" cookie wraps negative and vanishes. |
| 11 | Cookie sizes are unbounded; only counts are capped | **medium** | Up to 3000 cookies of arbitrary size held in RAM on a 908 MB device. |
| 12 | `__Secure-` / `__Host-` prefixes are not enforced | **medium** | `setFromScript` is reachable from page JS; without the prefixes a page can forge `__Host-session` with an arbitrary Domain/Path. This is the one place the project owns cookie semantics. |
| 13 | The vendored pin is checked only on a Linux configure | **medium** | `HttplibVendored.cmake` is included only in the Linux branch and `httplib.sh --check` has no caller; a hand-edited vendored header passes every macOS `ctest`. `gl-vendor` shows the pattern. |
| 14 | The `CPPHTTPLIB_*` defines live in a `.cpp` while the include path is target-wide | **medium** | Verified: they are `#define`s at the top of `NetServiceLinux.cpp`; a second includer would get different class layouts -- a silent ODR violation, not a link error. |
| 15 | Empty `Content-Type: ` is sent for a typeless body | **medium** | Verified at `NetServiceLinux.cpp:573`. The identical defect was already an open risk on Apple; it was reproduced rather than avoided. A browser sends no header. |
| 16 | The streamed-upload path uses httplib private members | **medium** | `content_length_`, `is_chunked_content_provider_`, `content_provider_` and `detail::ContentProviderAdapter` -- exactly what a version bump may rename, against a vendoring story built for cheap bumps. |
| 17 | The OpenSSL error queue is left dirty by `storeWithAnchors` | **medium** | A duplicate-certificate failure is discarded without `ERR_clear_error()`, on the same thread that then performs the handshake. |
| 18 | TLS failures collapse to one message | **medium** | Untrusted, expired and wrong-host are indistinguishable to the page though the fixture forwards three separate ports for them. |
| 19 | `resolves()` is called with a bracketed IPv6 literal | **medium** | `getaddrinfo("[::1]")` fails, so every IPv6 connect failure is reported as `Dns` rather than `Connect`. |
| 20 | `perOrigin` and the idle client pool never age out | **medium** | Buckets are never erased at zero and there is no idle timeout; six keep-alive sockets per origin are held for the runtime's life on a device with limited fds. |
| 21 | A `LinuxRequest` destroyed with a joinable worker calls `std::terminate` | **medium** | The "queue is gone" path never joins, and the worker can be the last owner -- process abort rather than a leak. |
| 22 | A 303/301/302 redirect does not drain the streamed body | **medium** | Undrained chunks hit the 8 MiB cap and cancel an otherwise successful redirect. |
| 23 | The retirement job re-finds by id and `startRequest` accepts a duplicate | **medium** | A different live request can be erased and its thread joined on-queue. |
| 24 | `onError` can reach a sink that has already had `onEnd` | **medium** | The body-overflow path does not check `cancelled`/`stopped` or route through `onQueue`. |
| 25 | A null `storeWithAnchors` falls through to OpenSSL's built-in paths | **medium** | Not the discovered image roots -- the same silent-fallback shape as finding 18 in the Apple round. |
| 26 | Response and request chunks are copied once more than needed | **low** | `[sink, chunk]` copies into the closure before moving out; `appendRequestBody` deep-copies a `shared_ptr<const Bytes>`. Doubles peak upload memory against an 8 MiB accounted cap. |
| 27 | `openssl::openssl` is linked PUBLIC while `httplib::httplib` is PRIVATE | **low** | Only `NetServiceLinux.cpp` includes OpenSSL headers; PUBLIC leaks them to every consumer of `screenkit-core`. |
| 28 | `HttplibVendored.cmake` resolves its path from `CMAKE_CURRENT_SOURCE_DIR` | **low** | Silently depends on being included from one place; `CMAKE_CURRENT_LIST_DIR` is location-independent. |
| 29 | The vendor pin is duplicated in three places and `COMMIT` protects nothing | **low** | Both fetches are by tag despite the header arguing a tag can move; `COMMIT` is used only in a log line, and for an annotated tag it is not even a commit sha. |
| 30 | `--check` combined with `--version` always reports a mismatch | **low** | Compares against empty sha256s. |
| 31 | Shell papercuts in `pi.sh` | **low** | `probe`'s `ABSENT` fallback is unreachable (takes `paste`'s status); `bench` names the wrong output path under `VARIANT=jit`; `headers()` hardcodes `shasum` so `sysroot` fails on a Linux dev box; the usage `sed` range truncates mid-sentence. |
| 32 | `resolves()`'s stated justification is contradicted by the taxonomy | **low** | The comment says `EventSource` retries `dns` but not `connect`; `NetError.h` and `dom-shim.js:5951` treat only `tls`/`url`/`unsupported` as fatal, so both are retried and the extra blocking lookup changes only a message string. |
| 33 | -- | **false** | "No tests or spec files in the change set", "VENDOR.md missing", "the Dockerfile change is missing", "`pi.sh test` fails out of the box": artefacts of the coordinator's diff staging. `pi.sh test` was run on hardware and every row passed. |

No `bad_spec`: the spec was followed. Finding 1 is an `intent_gap` in origin -- the frozen decision
rested on a coordinator error -- but it was taken back to the human and re-decided before any code
was written against it, so it routes to patch with the rest rather than to a loopback.
1-32 route to **patch**; 33 rejected.

## Design Notes

cpp-httplib being synchronous is the one place this client cannot copy either of the others. A
thread per in-flight request is the straightforward reading, and the six-slot-per-origin behaviour the
other clients get from their dispatchers has to come from somewhere here -- a bounded pool rather than
an unbounded thread per request, or a page that opens fifty images spawns fifty threads on a Pi 3
with 908 MB of RAM.

## Verification

**The device was unreachable for the whole of this work**, so everything below that names it is
**NOT RUN**. `batocera.local` failed to resolve on every attempt, polled every forty-five seconds
from the first minute to the last.

**Commands:**
- `sh tools/batocera/pi.sh test` -- every non-WebSocket row passes on the device, none skipped.
  **NOT RUN: no device.** The subcommand is written and syntax-checked, and its row list is
  cross-checked against `runtime/tests/CMakeLists.txt` on every run (verified by hand: no CTest
  `net-*` row is missing from it but `net-image`). Its two untried parts are the ssh reverse tunnel
  -- Batocera's sshd must allow `TcpForwarding`, which is the default but is unconfirmed on this
  image -- and `timeout` being present in the device's busybox.
- `ctest --test-dir runtime/build/macos` -- 160/160, unchanged. **Run: 160/160.**
- `ctest --test-dir runtime/build/macos-asan` -- 154/154, unchanged. **Run: 154/154.**
- `sh tools/android/android.sh test` -- 18/18, unchanged. **Run on `screenkit-tv`: 18/18, "every row
  passed".**
- `sh tools/batocera/pi.sh build` -- linux-arm64 builds the new client. **NOT RUN as written**, since
  it starts by fetching the sysroot off the device. The build it wraps was run, in the same pinned
  `linux/arm64` Debian 12 container, against the device's cached SDL3 and a stand-in `libssl`: the
  host and `screenkit-net-tests` compile and link clean, with no warnings from either new file.

**Also run, and the reason it is not the device run:**
- **The fifteen net rows, on linux-arm64, in a container.** `screenkit-net-tests` built for
  linux-arm64 and run under `--platform linux/arm64` with the node fixture in the same container, so
  nothing is proxied: `net-http-get net-https net-request-body net-streaming-request
  net-streaming-response net-http-error-status net-redirect net-encoded-body net-abort-timeout
  net-flow-control net-unreachable net-cookies net-websocket net-eventsource net-shutdown-idle` --
  **15/15, twice, after the last change.** This is the same binary, the same rows and the same
  transcripts the device would run, on the same architecture, but it is **not** the device: the
  kernel, the OpenSSL, the CA bundle, the resolver and the SDL3 are Debian 13's and not Batocera's,
  and `pi.sh test`'s own plumbing (ssh `-R`, the push, the nonce line) is not exercised at all.
- **Acceptance criterion 5, directly.** Configuring against a sysroot with SDL3 and no OpenSSL fails
  at configure time: `no libssl in ... -- the Linux HTTP client is cpp-httplib over the image's
  OpenSSL, and without it there is no TLS and no https`, naming `pi.sh sysroot` as the fix.
- **The vendored pin.** Appending one line to `third_party/httplib/httplib.h` fails the configure
  with both sha256s printed; restoring it passes. `sh tools/vendor/httplib.sh --check` agrees.
- **A first measurement that was wrong, and why it is worth recording.** The first container harness
  reached the fixture through an ssh-less two-hop proxy, and `net-flow-control` failed on both stall
  lines. The client was not at fault: a probe that read exactly 1 MiB and then stopped still let the
  fixture write **30.9 MB**, so the harness itself was absorbing thirty megabytes. Running the
  fixture inside the container removed the proxy and the row passed. The same shape of error is
  possible on the device -- `ssh -R` has a 2 MB channel window, which is well inside the row's 24 MiB
  bound but is still slack the macOS suite does not have.

**Acceptance criteria, one by one:**

| # | Criterion | Status |
|---|---|---|
| 1 | every non-WebSocket net row passes on the device, none skipped | **untested** -- 15/15 on linux-arm64 in a container, which is not the device |
| 2 | `new WebSocket(...)` fails at once with `unsupported` and never hangs | **untested on the device**; asserted by `net-websocket`'s Linux transcript and passing in the container |
| 3 | a large unread body stalls the server -- real backpressure | **untested on the device**; `net-flow-control` asserts the Android bound (under 24 MiB of 64 MiB) and passes in the container |
| 4 | macOS and Android suites unchanged (160/160, 154/154, 18/18) | **met** -- all three run and unchanged |
| 5 | an image with no OpenSSL fails the build with a message naming what is missing | **met** -- run directly |
