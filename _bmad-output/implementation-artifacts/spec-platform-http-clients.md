---
title: 'Per-platform HTTP clients: NSURLSession and OkHttp behind NetService, as React Native does'
type: 'refactor'
created: '2026-09-18'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'NO_VCS'
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-runtime-networking.md', '{project-root}/_bmad-output/implementation-artifacts/spec-android-network-backend.md', '{project-root}/runtime/core/src/net/Http.h']
---

<frozen-after-approval reason="human-owned intent -- do not modify unless human renegotiates">

## Intent

**Problem:** ScreenKit owns its HTTP/1.1, WebSocket and cookie implementation -- `Http.cpp` (948),
`WebSocket.cpp` (415), `CookieJar.cpp` (413). That is a protocol stack to maintain forever, it has no
HTTP/2 and no proxy support, and every future platform inherits the maintenance rather than the
platform's own battle-tested client.

**Approach:** Do what React Native does: the JS layer stays, and the native side becomes a
per-platform HTTP client. `NetService` becomes the seam -- an interface with one implementation per
platform, NSURLSession on Apple (RN's `RCTHTTPRequestHandler`) and OkHttp on Android (RN's
`NetworkingModule`). The portable protocol layer and the `Connection` seam beneath it are deleted.

## Boundaries & Constraints

**Always:**
- **`NetService` is the seam.** It becomes an interface; `bindings/Net.cpp` and `__screenkit.net` keep
  calling it unchanged, and the DOM shim's `fetch`, XHR, WebSocket and EventSource are untouched.
- **Streaming survives, with Apple's limits recorded** (amended 2026-09-18, see the decision below).
  RN has no `response.body` because its bridge moves data as base64 strings; ScreenKit calls JSI
  directly, so chunks are delivered as they arrive. On Android that is fully true. On Apple it is
  true for every MIME type except `text/plain`, and backpressure is best-effort.
- **The `net-*` rows are the contract.** Every row that passes today passes afterwards on both Apple
  and Android, or its change is recorded as a deliberate divergence with the reason.
- **No bundled TLS and no CA roots**, on any platform: the client is the platform's, and so is its
  trust store. `testTlsAnchors` stays honoured for the test fixture only, and never settable from JS.
- **Linux gets nothing**, as it has nothing today: it builds the Unavailable backend, and M11 decides
  its client separately. RN has no Linux target to copy.

**Never:**
- Changes to `__screenkit.net`, the DOM shim's networking, or JS-visible API shape.
- A bundled HTTP or TLS library on Apple or Android.
- Keeping a second, portable implementation alive "just in case" -- the point is to stop owning one.
- Edits to vendored code.

**Decision (2026-09-18):** this reverses the seam decision taken earlier the same day
(spec-android-network-backend.md), which put the backend at `Connection` and kept the protocol layer
portable. The human was shown the cost -- deleting ~1,776 tested lines, accepting per-platform
divergence, and leaving Linux without a story -- and chose RN's model for HTTP/2, proxies and not
owning a protocol stack.

**Decision (cookies, 2026-09-18):** the platform stores, fully RN's model --
`NSHTTPCookieStorage` on Apple, `android.webkit.CookieManager` on Android. `CookieJar.cpp` is deleted
with the rest of the protocol layer, `cookies.txt` persistence ends, and cookie behaviour is allowed
to differ between platforms exactly as it does in RN. `net-cookies` and `net-cookie-public-suffix`
change or are retired accordingly.

**Decision (response head, 2026-09-18):** exactly as RN. Apple passes NSURLSession's
`allHeaderFields` through, so on Apple `Headers` iteration order is unspecified, duplicate headers --
including multiple `Set-Cookie` -- arrive comma-joined into one value, and `statusText` is the
canonical phrase for the status code rather than what the server sent. Recorded, not worked around.

**Decision (Apple streaming, 2026-09-18, amends "Streaming survives"):** two guarantees the
`Connection` seam provided do not survive NSURLSession, and are accepted as divergences rather than
worked around.

1. **Backpressure is best-effort on Apple.** CFNetwork's read-ahead is unbounded relative to its
   delegate: the body is off the socket before `[task suspend]` takes effect. `net-flow-control`
   passes in the shipped RelWithDebInfo build on timing, not on mechanism, and fails deterministically
   under ASan. A page that fetches a large body and does not read it will buffer it in memory on
   Apple -- which is the failure `flowWindow` was built to prevent (deferred-work.md:314). Android
   keeps real backpressure, because OkHttp is pull-based.
2. **`text/plain` does not stream on Apple.** CFNetwork buffers such a body whole, head included. The
   fixture's `/slow` route is `application/octet-stream` for this reason, with the comment saying so.
   Every other MIME type streams; Android streams `text/plain` too.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| The existing rows | every `net-*` row, on macOS and on `screenkit-tv` | the transcripts they produce today | a row that cannot pass is a recorded divergence, not a silent edit |
| Streaming both ways | `net-streaming-request`, `net-streaming-response` | unchanged: bodies stream, `response.body` works | N/A |
| Backpressure (Android) | `net-flow-control`: a 64 MiB body nobody reads | the server stalls, then the body reads in full | N/A |
| Backpressure (Apple) | the same row | best-effort: the body reads in full, but the server is not reliably stalled | the ASan build shows the read-ahead; recorded, not worked around |
| `text/plain` streaming | a `text/plain` response sent in pieces | Android delivers pieces; **Apple delivers it whole** | N/A -- recorded divergence |
| Manual redirect | `redirect: 'manual'` | the real 3xx, not the followed response | N/A |
| Bad certificates | untrusted, expired, wrong-host | `NetError::Tls`, no bypass | connection fails |
| Cookies | `net-cookies`, `net-cookie-public-suffix` | the platform store's behaviour, per platform; no `cookies.txt` | a row that no longer holds is retired with its reason |
| Response head on Apple | a reply with two `Set-Cookie` headers and a custom reason phrase | one comma-joined value, unspecified order, canonical `statusText` | N/A -- this is the recorded divergence |
| Linux | a Batocera build | still compiles, still `unsupported` | N/A |

</frozen-after-approval>

## Code Map

**What is deleted:** `runtime/core/src/net/Http.{h,cpp}` (948 lines: redirects `:746-789`, default
headers `:498-536`, inflate `:352-373`, retry `:896-900`, `ConnectionPool` `:433-491`),
`WebSocket.{h,cpp}` (415), `Connection.h` (the seam: `:64` `ConnectionListener`, `:79` `Connection`,
`:121-124` the three free functions) -- **except `NetError` (`:29-40`) and `netErrorName`, which both
clients still map onto and which must move to a header of their own before `Connection.h` goes** --
`NetworkFramework.mm` (340), `NetworkAndroid.{h,cpp}` (740) and
`Transport.java` -- today's Android backend, finished hours ago -- `CookieJar.{h,cpp}` (413) and `NetContext.h`.

**What survives:** `Url.cpp` (234, WHATWG parsing the shim needs), `Bytes.cpp` (178),
`bindings/Net.cpp` (the JS binding, unchanged), `runtime/js/dom-shim.js` (fetch/XHR/WebSocket/
EventSource, unchanged).

**The seam to build:** `runtime/core/src/net/NetService.h:34` is concrete with **no virtual methods**
(`create()` at `:36`, private ctor `:66`, pimpl `Core` at `:65`). It must become an interface. Its
methods are the RN `Networking` module's surface: `startRequest` `:42`, `appendRequestBody` `:43`,
`finishRequestBody` `:44`, `abortRequest` `:46`, `acknowledgeResponseData` `:48`, `openSocket` `:50`,
`sendSocket` `:52`, `closeSocket` `:53`, `cookiesFor` `:57`, `setCookie` `:58`, `shutdown` `:62`.

**The contracts each client must emit** (unchanged, this is what keeps the shim working):
`Http.h:54` `HttpSink` -- `onHead(ResponseHead)`, `onData`, `onEnd`, `onError(NetError, msg)`,
`onUploadProgress(sent, total, complete)`; `WebSocket.h:23` `SocketSink` -- `onOpen(protocol,
extensions)`, `onMessage(text, Bytes)`, `onSent(payloadBytes)`, `onError`, `onClose(code, reason,
wasClean)`. `NetError` stays as the error taxonomy both clients map onto, in its new home.

**The features that make this non-trivial:**
- `HttpRequestSpec::flowWindow` (`Http.h:43`) + `acknowledgeResponseData`: OkHttp is pull-based so
  backpressure is natural; NSURLSession pushes through its delegate and needs `suspend()`/`resume()`.
- `RedirectMode::{Follow,Manual,Error}` (`Http.h:29`): `willPerformHTTPRedirection` returning nil on
  Apple, `followRedirects(false)` on Android.
- `onUploadProgress`: `didSendBodyData` on Apple; a wrapping `RequestBody` on Android.
- `onSent` for `bufferedAmount`: OkHttp's `queueSize()`; a send-completion tally on Apple.
- `testTlsAnchors`: a `URLSessionDelegate` trust callback on Apple; the `X509ExtendedTrustManager`
  written today on Android -- **including its three-argument `checkServerTrusted`**, without which
  Conscrypt skips host-name verification entirely (`spec-android-network-backend.md` review pass 1).

**Android build:** OkHttp is a Kotlin library, so `runtime/android/settings.gradle:2` ("nothing in
this build is Kotlin") stops being true, and `kotlin-stdlib` plus okio join
`gradle/verification-metadata.xml`, which pins a sha256 per artifact and fails an unpinned
dependency. `app/build.gradle:131-137` is where dependencies go.

**Tests:** `runtime/tests/CMakeLists.txt:253-269` (16 `net-*` rows), `RuntimeTests.cpp:4321-4347`
`netFixture()`, `tools/android/android.sh` `NET_ROWS` + `check_rows` + the `host` row.

## Tasks & Acceptance

Sequenced so the tree builds and the rows pass at every step; Apple first, because it can be done
while Android still runs today's backend behind the same interface.

**Execution:**
- [x] `runtime/core/src/net/NetService.{h,cpp}` -- make it an interface with a per-platform
  `create()`, keeping every method signature so `bindings/Net.cpp` is untouched.
- [x] `runtime/core/src/net/NetServiceApple.mm` (new) -- NSURLSession: requests, streaming bodies both
  ways, `suspend()`/`resume()` for the flow window, redirect modes, upload progress, the trust
  callback for `testTlsAnchors`, and `URLSessionWebSocketTask` for sockets.
- [x] `runtime/android/app/src/main/java/dev/screenkit/net/HttpClient.java` (new) + `NetServiceAndroid.cpp`
  (new) -- OkHttp over JNI, reusing today's `JNI_OnLoad`, class-loader and global-ref discipline, and
  the `X509ExtendedTrustManager` with its three-argument overload.
- [x] `runtime/android/app/build.gradle`, `gradle/verification-metadata.xml`, `settings.gradle` --
  OkHttp, okio and `kotlin-stdlib` pinned by sha256; correct the "nothing is Kotlin" comment.
- [x] `runtime/core/src/net/NetError.h` (new) -- move `NetError` and `netErrorName` out of
  `Connection.h` first, so the taxonomy survives the seam it currently lives in.
- [x] Delete `Http.{h,cpp}`, `WebSocket.{h,cpp}`, `CookieJar.{h,cpp}`, `Connection.h`, `NetContext.h`,
  `NetworkFramework.mm`, `NetworkAndroid.{h,cpp}`, `Transport.java`, and their CMake entries; point
  `cookiesFor`/`setCookie` at the platform store on each side.
- [x] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`, `tools/android/android.sh` --
  keep every row running on both platforms; record any row that changes as a divergence.
- [x] Docs -- `Architecture.md` §3.2 and the linking table, `runtime/README.md`,
  `runtime/js/README.md`, and `deferred-work.md`: the protocol layer is gone and HTTP/2 is now
  whatever the platform does.

**Acceptance Criteria:**
- Given macOS, when the suite runs, then every `net-*` row passes except those retired under the
  cookie decision, each retirement recorded with its reason, and no other row is lost.
- Given `screenkit-tv`, when `android.sh test` runs, then every row passes, none skipped.
- Given a Batocera build, when it compiles, then it still builds the Unavailable backend.
- Given a page on Android, when it uses `fetch` with a streamed response, then `response.body`
  delivers chunks under real backpressure.
- Given a page on Apple, when it uses `fetch` with a streamed non-`text/plain` response, then
  `response.body` delivers chunks as they arrive, backpressure being best-effort per the decision.

## Implementation Notes

**Shape.** `runtime/core/src/net/` is 4,000 lines lighter and has no protocol code in it. What is
left: `NetError.h` (the taxonomy, now inline and standalone), `NetService.h` (the interface, plus
`HttpRequestSpec`, `ResponseHead`, `HttpSink` and `SocketSink`, which had to move out of the deleted
`Http.h`/`WebSocket.h`), `NetServiceApple.mm`, `NetServiceAndroid.{h,cpp}` +
`dev/screenkit/net/HttpClient.java`, `NetServiceUnavailable.cpp`, and `Url`/`Bytes` as before.
Deleted: `Http.{h,cpp}`, `WebSocket.{h,cpp}`, `CookieJar.{h,cpp}`, `Connection.h`, `NetContext.h`,
`NetService.cpp`, `NetworkFramework.mm`, `NetworkAndroid.{h,cpp}`, `NetworkUnavailable.cpp`,
`Transport.java`. `bindings/Net.cpp` and `runtime/js/dom-shim.js` are untouched.

**One thing had to survive `Connection.h` besides `NetError`: `IoQueue`.** `bindings/Net.cpp:534`
uses `makeIoQueue` as a general-purpose serial queue for image decode, and that file was not to be
edited, so the interface and a portable `ThreadIoQueue` moved to `IoQueue.{h,cpp}` -- the reviewed
one from `NetworkAndroid.cpp`, which already handled the delay clamp, the drain-on-destruction and
the throwing job. It carries no bytes and touches no socket, so there is nothing left for a platform
to specialise.

**Apple.** One serial dispatch queue per runtime is the `NSURLSession`'s delegate queue, so every
delegate callback and every command from JS runs on it in order. The features the spec called
non-trivial, and how each landed: the flow window is `[task suspend]`/`[task resume]`, and it works
(the server stalls under 1.2 MiB of a 64 MiB body) as long as the delegate keeps up -- see the ASan
note below; redirect modes are `willPerformHTTPRedirection` returning the new request, nil, or nil
with the request marked refused; upload progress is `didSendBodyData`; a streamed request body is a
`CFStreamCreateBoundPair` fed from a run loop on one shared thread, and aborting it closes the *read*
end first so the server never sees a body that ends cleanly; `testTlsAnchors` is a task-level trust
challenge, and refusing one cancels the task, so the reason is remembered -- without that the row
simply hung, because a refusal and this client's own abort are the same `NSURLErrorCancelled`.
Cookies are an `NSHTTPCookieStorage` of the runtime's own (an ephemeral configuration's), not the
process-wide one: a shared store would write the suite's 127.0.0.1 cookies into the developer's own
cookie file and make the rows depend on each other.

**Android.** `HttpClient.java` is the whole client and `NetServiceAndroid.cpp` is the translation:
ids are process-wide handles in a registry, because `bindings/Net.cpp` starts at 1 in every runtime;
every Java callback looks its handle up, posts onto the runtime's serial queue and is dropped if the
slot went; `JNI_OnLoad` still resolves the class where the app's class loader is reachable and
`RegisterNatives` the ten callbacks. Backpressure is a blocking reader loop -- OkHttp is pull-based,
so not pulling *is* the mechanism -- and the per-origin limit is `Dispatcher.setMaxRequestsPerHost(6)`,
because OkHttp's own default is five and browsers use six. The `X509ExtendedTrustManager` with its
three-argument `checkServerTrusted` came across unchanged from `Transport.java`, host-name hole and
all. OkHttp 4.12.0 rather than 5.x: it is the line that still supports minSdk 24 and Java 8 bytecode.
`kotlin-stdlib` 1.9.10, okio 3.6.0 and okhttp itself are pinned by sha256, written by
`./gradlew --write-verification-metadata sha256`.

**The bet mostly held, and the places it did not are in the rows.** Fourteen of the sixteen `net-*`
rows have one transcript that passes on both platforms (`net-image` is Apple-only, as before). Two
carry both (`net-encoded-body`,
`net-cookies`) through a `NET_PER_PLATFORM` macro rather than being weakened to what both could
agree on -- the differences are the point, and they are visible in the test file. A third,
`net-flow-control`, does the same for the one guarantee only Android's client makes (below). Two lines were
normalised because the clients name the same failure differently: an unencodable host label is `dns`
on Apple and `url` on Android, and the `Cookie:` header's order is each store's. What changed, all of
it recorded in deferred-work.md: `Content-Encoding` handling (OkHttp offers gzip only and strips the
header off what it decoded), truncated and mis-framed bodies, the `Set-Cookie`/`statusText`/header-order
fidelity on Apple, cookie persistence, `text/plain` streaming on Apple, and two WebSocket handshake
refusals neither client makes any more.

**Rows retired, with reasons.** `net-cookie-public-suffix` -- the rule is the store's now and the two
stores answer differently (Chromium has a list, Apple has none), so there is no one transcript; the
leak it really guarded is still covered by `net-cookies`. `host-storage-directory` and
`host-window-storage-directory` -- the only thing they could observe was the `cookies.txt` the jar
wrote, and the jar is gone. macOS is 160 rows rather than 163.

**Two things found on the way that no compile check would have.** A block captures a C++ reference as
a reference, so `receiveNext`'s `shared_ptr` parameters had to be by value -- the same bug the
Network.framework backend's comments warn about, and it segfaulted four rows before it was fixed.
And `android.webkit.CookieManager.getCookie` returns HttpOnly cookies, so "HttpOnly is never visible
to JS" had to be kept by a name list the client maintains; the device run is what showed it.

**Backpressure is a guarantee on one platform and best effort on the other, and the row says so.**
OkHttp is pull-based, so the reader loop stopping *is* backpressure: `net-flow-control` still asserts
that a 64 MiB body nobody reads leaves the server under 24 MiB, on `screenkit-tv`, unchanged.
NSURLSession pushes through its delegate and all this client can do is suspend the task, which holds
the server back while the delegate keeps up and does not when it cannot -- in
`runtime/build/macos-asan` the whole body is off the socket before the first `didReceiveData` can
suspend anything, 4/4 runs, while the same row stalls the server under 1.2 MiB in the RelWithDebInfo
build, also 4/4. That is not a debug-build artefact but a guarantee NSURLSession does not make, so
that line carries a transcript per platform (`NET_PER_PLATFORM`, as `net-encoded-body` and
`net-cookies` do) and the runtime tells the row which it is through `__net.stallsUnreadBodies`.
The part of the window that *is* true of both is asserted on both, in both builds: against a server
offering at most 128 KiB per 10 ms, a 16 MiB body nobody reads is still short of the end after 1.5 s,
and deleting `[task suspend]` finishes it -- sabotage-checked in `macos` and in `macos-asan`. The gap
itself is in deferred-work.md, against the entry that closed backpressure in the first place: on tvOS
an unread body can still download into memory when the server can outrun the runtime.

## Spec Change Log

- 2026-09-18 -- implemented. Two amendments to what the tasks said, both forced and both recorded:
  `IoQueue` had to survive `Connection.h` alongside `NetError`, because `bindings/Net.cpp` -- which
  the spec forbids editing -- uses `makeIoQueue` for image decode; and two rows carry a transcript
  per platform rather than one, because the cookie stores and the `Content-Encoding` handling
  genuinely differ and a single transcript could only be had by asserting less. A third,
  `net-flow-control`, followed once the human accepted the Apple streaming divergences: Android still
  asserts the server stalling under 24 MiB, Apple asserts only that the body arrives in full. `net-cookie-public-suffix`
  is retired under the cookie decision, and `host-storage-directory` /
  `host-window-storage-directory` with it -- they could only see the storage directory through the
  jar's `cookies.txt`. The frozen block is otherwise untouched.

## Review Triage Log

Review pass 1, 2026-09-18. Three layers: blind-hunter (17), edge-case-hunter (29), verification-gap
(3 + 5 other). `[Gn]` groups a shared root cause.

**Note on the staged diff:** it omitted `bindings/Net.cpp`, the `RuntimeTests.cpp` regions,
`HostMain.cpp` and the gradle `dependencies` block, so three blind-hunter findings ("no tests
accompany the change", "the seam's only consumer is outside the change set", "the OkHttp dependency
is unreviewable") are artefacts of that staging rather than defects. The coordinator staged the diff.

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| 1 | [G1] `@()` on a non-UTF-8 `std::string` yields nil into a dictionary literal / `URLWithString:` | **high** | Verified at `NetServiceApple.mm:805,968,1032`. A JS string with a lone surrogate reaches `setCookie`/`url`; `@{k: nil}` and `URLWithString:nil` raise. A page can crash the process. |
| 2 | [G2] `NewStringUTF` is handed JS text that is not modified UTF-8 | **high** | Any astral character (an emoji in a close reason) aborts under CheckJNI; `GetStringUTFChars` returns CESU-8 back. The `byte[]` message path already avoids this. |
| 3 | [G2] `screenkitNetHead` indexes `values` by `names`' length with no `ExceptionCheck` | **high** | A short array raises and the pending exception makes the next JNI call abort. |
| 4 | [G2] `NewObjectArray`/`NewStringUTF`/`toJavaBytes` null returns unchecked | **high** | A pending OutOfMemoryError plus `SetObjectArrayElement` on null aborts; a null body is sent as no body and the request appears to succeed. |
| 5 | [G3] A WebSocket that cannot get a `JNIEnv` never settles | **high** | `openSocket`'s `!env` path is a bare `return`; the JS socket stays CONNECTING for ever. The branch above emits `onError`+`onClose(1006)`. |
| 6 | [G3] `HttpClient.startRequest`/`openSocket` return silently when stopped | **high** | The call is never retired and its sink hears nothing. |
| 7 | [G3] An `Error` escaping `deliver`'s catch leaves the exchange in the map | **medium** | Only `Exception` is caught; an `OutOfMemoryError` never settles the request. |
| 8 | [G3] A close reason over 123 bytes or a reserved code throws out of `close()` | **medium** | `IllegalArgumentException` escapes; the socket never closes and never settles. |
| 9 | [G4] A lost acknowledgement stalls a flow-controlled response for ever | **high** | `Exchange.pump` increments `unacknowledged` *after* `nativeData`; an ack landing in that window is clamped to 0 and discarded, then the reader blocks on it. With `flowWindow <= 64 KiB` the response hangs permanently. |
| 10 | [G5] `core->handles[id]` is never erased on normal completion | **high** | Verified: `screenkitNetEnd`/`Error`/`SocketClose` call `forgetCall` (process-wide `gCalls`) but not `core->handles`; only abort and shutdown erase. A polling TV app grows it without bound. Apple's `retire:` has no such hole. |
| 11 | [G5] A failed `ScopedEnv` in shutdown skips `DeleteGlobalRef` and the refcount | **medium** | Leaks the client global ref and `liveJavaRefCount()` never returns to zero -- the one leak detector this code has. |
| 12 | [G6] `cookiesFor`/`setCookie` race shutdown's `DeleteGlobalRef` | **high** | Both check `stopped_` then call into `core_->client`, which shutdown nulls on the I/O queue. `NetService.h` promises every entry point posts to its queue or uses a thread-safe platform object; these do neither. Apple's are safe because `NSHTTPCookieStorage` is. |
| 13 | [G7] `shutdown` cancels flow-suspended tasks without resuming them | **high** | `cancelRequest` says "a suspended task ignores cancel until it is resumed"; the shutdown loop calls `[task cancel]` directly, so the connection outlives the runtime -- contradicting `NetService.h`. Two layers. |
| 14 | [G8] Apple truncates WebSocket text at the first NUL | **medium** | `strlen` on `UTF8String` drops everything from an embedded U+0000; legal on the wire and in JS, preserved by Android. Two layers. |
| 15 | [G8] Invalid UTF-8 in a text send becomes an empty frame while `onSent` reports the full size | **medium** | Apple substitutes `@""`; JS sees a successful send of data that never went. Android substitutes U+FFFD -- a third behaviour. |
| 16 | [G9] Upload progress never reports `complete` for a streamed body on Apple | **medium** | `complete = total > 0 && totalSent >= total`, and a streamed request's `total` is unknown. XHR upload `load`/`loadend` fire on Android, never on Apple. Two layers. |
| 17 | [G10] `NSURLErrorCancelled` on a still-live request retires it without settling | **medium** | A system-initiated cancel leaves the JS promise pending for ever. |
| 18 | [G11] All-DER-anchors-failed falls back to the system store in silence | **medium** | `testTlsAnchors` dropped and the chain evaluated against platform roots -- the same shape as the Android trust bug caught last round. |
| 19 | [G12] `UploadBody` is not one-shot, so an OkHttp retry re-sends an empty body | **medium** | A retried connection calls `writeTo` again on a drained stream; the server sees a complete, empty upload. |
| 20 | [G12] `Cookie.Builder` is handed untrimmed name/value from the platform store | **medium** | A pair with space around `=` throws out of the jar, failing every request to that host. |
| 21 | [G12] `redirect:'error'` treats a 3xx with no `Location` as a redirect | **low** | Android rejects a response Apple and `fetch` deliver normally. |
| 22 | [G13] `bufferedAmount` never drains if the socket closes with bytes queued | **medium** | Queued entries are abandoned without `nativeSocketSent`; a page waiting on `bufferedAmount` hangs. |
| 23 | [G14] The Apple upload queue `_pending` has no cap | **medium** | A slow upload to a stalled server grows it without bound. |
| 24 | [G15] Three hand-maintained copies of `NetError` and `RedirectMode` | **medium** | `NetError.h`, `HttpClient.java`'s `ERR_*` and `toNetError`'s `case 0..9` must stay in lockstep with nothing enforcing it; inserting a kind silently remaps every Android error. |
| 25 | [G16] `net-flow-control`'s Apple branch asserts a constant and checks nothing | **high** | Verified: `stalled` is fetched on both branches, inspected only on Android. Deleting the whole `[task suspend]` block leaves macOS -- the only platform CTest runs -- green. **Caused by the coordinator's own instruction** to make that branch a constant. |
| 26 | [G16] No row combines a flow-suspended request with abort or shutdown | **medium** | Pre-verified: deleting the resume-before-cancel block fails nothing, because `net-abort-timeout` only aborts `/hang` and a kept-up-with `/slow`, and `net-flow-control` never aborts the firehose it suspended. |
| 27 | [G17] `storageDirectory` is dead plumbing | **medium** | Verified: read by none of the three implementations, and the two rows that observed it were retired with their fixtures. Found independently by all three layers and the coordinator. |
| 28 | [G18] `IoQueue.h` documents the opposite teardown order from `IoQueue.cpp` | **low** | Header says drain-then-join; the code joins at `:22` and drains at `:27`, so leftover jobs run on the destroying thread. |
| 29 | [G18] `IoQueue::postAfter` has no caller anywhere in the repo | **low** | Dead API from the deleted stack; its overflow clamp is unreachable. |
| 30 | [G18] `NetError.h` says only `Tls` is fatal for EventSource | **low** | The shim treats `tls`, `unsupported` and `url` as fatal, and `net-no-javavm` pins `unsupported`. |
| 31 | [G18] Comments still name `Transport.java` and `NetworkAndroid.cpp` | **low** | Both deleted here; `AndroidManifest.xml:16` and five `RuntimeTests.cpp` sites point at files that no longer exist. |
| 32 | [G19] `UnavailableNetService::stopped_` is a plain bool read across threads | **low** | Both other implementations declare it `std::atomic<bool>`. |
| 33 | [G20] Android `HttpOnly` protection is best-effort and its Javadoc self-contradicts | **medium**, deferred | Already an open risk; the name list cannot see a cookie set before the runtime started. The two comments contradict each other and should be fixed together. |
| 34 | [G21] The platforms make opposite cookie-store decisions, each documented as correct | **medium**, deferred | Apple uses a private per-runtime store *because* a shared one leaks across runtimes; Android uses the device-wide `CookieManager`. A consequence of the cookie decision, not a coding error -- it needs the human. |
| 35 | [G22] `setMaxRequestsPerHost(6)` plus a blocking pump stalls the seventh streaming request | **medium**, deferred | Six concurrent streaming responses block six dispatcher threads; NSURLSession does not. Real divergence, but the fix is a dispatcher redesign. |
| 36 | [G23] `onOpen`'s `extensions` is always empty on both clients | **low**, deferred | `WebSocket.extensions` is permanently `""`; nothing negotiates today since `permessage-deflate` is never offered. |
| 37 | [G24] The Unavailable client is executed by no test on any platform | **medium**, deferred | Pre-verified: Linux builds with tests off and every net row skips on `!networkAvailable()`. Dropping its `queue_->post` would hang every `fetch` on Batocera with nothing failing. Needs a Linux test target the repo lacks. |
| 38 | [G25] `IoQueue::sync` from the queue's own thread deadlocks | **low** | `IoQueue.h:33` documents "Never call it from the queue", so this is a documented contract rather than a defect -- but it hangs silently instead of failing loudly. A debug assert is the direct fix. |
| 39 | -- | **false** | "No tests accompany the change / the seam's consumer is outside it / the OkHttp dependency is unreviewable": artefacts of the coordinator's diff staging. The tests exist and pass. |

**Patch round, 2026-09-18.** All 32 patched; re-verified by the coordinator, not from the report:
macOS 160/160, macos-asan 154/154, device 18/18, Linux builds `NetServiceUnavailable`. Two items came
back with evidence against the instruction rather than complied with, and both were right:

- The suggested `stalled.written < size` bound for the Apple branch would not have worked: measured,
  `macos` holds the unpaced server at ~1.2 MiB while `macos-asan` writes all 67,108,864 -- identical
  to deleting `[task suspend]`, so no bound separates the two. A **paced** server was used instead
  (16 MiB at 128 KiB/10 ms), asserted on both platforms in both builds and sabotage-checked: removing
  the suspend now fails the row on macOS, which is what finding 25 existed for.
- Resume-before-cancel turned out to be defensive rather than load-bearing -- instrumenting showed a
  suspended data task does act on `cancel` on macOS 26. The change was made as asked, and the
  comments claiming otherwise were corrected rather than left asserting something the code does not
  support.

No `intent_gap` or `bad_spec`: the spec was followed, and the Apple divergences it could not have
predicted were escalated and recorded in the frozen block before review. 1-32 route to **patch**,
33-38 to **defer**, 39 rejected.

## Design Notes

The whole change is a bet that two vendor clients agree closely enough that one JS layer sits on both.
Where they will not agree, and what the rows will show first: response-head fidelity on Apple and
cookie semantics (both settled as deliberate divergences in the frozen block), the error taxonomy (`NSURLError*` and OkHttp's
exception hierarchy both have to fold into `NetError`'s ten kinds), and `net-abort-timeout`, since the
two clients time out on different clocks.

## Verification

**Commands:**
- `ctest --test-dir runtime/build/macos --output-on-failure` -- every `net-*` row passes.
  **Run: 160/160, all 16 `net-*` rows among them, and 154/154 under ASan.** 163 before; the three that are gone are
  `net-cookie-public-suffix`, `host-storage-directory` and `host-window-storage-directory`, each
  retired above with its reason.
- `sh tools/android/android.sh test` -- every row passes on the emulator, none skipped.
  **Run on `screenkit-tv`: 18/18, "every row passed"** -- the 16 `net-*` rows minus `net-image`
  (Apple only, no drawable), plus `net-no-javavm`, `net-shutdown-mid-call` and the `host` row.
- `sh tools/batocera/pi.sh build` -- linux-arm64 still builds the Unavailable client. **Run: it
  compiles `NetServiceUnavailable.cpp` and `IoQueue.cpp` and the host links.**
- `sh runtime/scripts/build-tvos-simulator.sh && sh runtime/scripts/run-tvos-simulator.sh` -- tvOS
  still runs, since NSURLSession is the one client both Apple targets share. **Run: builds clean and
  the app renders on the simulator. `otool -L` on the app binary: Foundation and Security, and
  Network.framework is gone.**

**Also run:**
- `ctest --test-dir runtime/build/macos-asan` -- **154/154.**
- `./gradlew assembleDebug` with and without `-Pscreenkit.netTests`, arm64-v8a and armeabi-v7a --
  clean.
- `javap -s` on the built `HttpClient.class` -- every one of the twelve methods and ten `native*`
  callbacks matches the descriptor `prepareAndroidNetwork` and `RegisterNatives` ask for.
