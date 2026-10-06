---
title: 'Android network backend: TCP and TLS on the Connection seam, over JNI'
type: 'feature'
created: '2026-09-17'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'NO_VCS'
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-runtime-networking.md', '{project-root}/_bmad-output/implementation-artifacts/spec-m10-android-host.md', '{project-root}/runtime/core/src/net/Connection.h']
---

<frozen-after-approval reason="human-owned intent -- do not modify unless human renegotiates">

## Intent

**Problem:** Outside Apple, the runtime has no network: `NetworkUnavailable.cpp` fails every request,
WebSocket and EventSource with `unsupported` (deferred-work.md:302). Android TV and Fire TV apps need
`fetch`, XHR, WebSocket, EventSource and network images.

**Approach:** On Android, a JNI backend implements the `Connection` seam -- TCP and TLS byte streams
only -- over `java.net.Socket` and `javax.net.ssl.SSLSocket`. Everything above that seam (HTTP/1.1,
WebSocket framing, redirects, cookies, connection pooling, flow control, inflate) is the existing
portable C++ and is reused unchanged. `__screenkit.net`, the DOM shim and every JS API stay
unchanged. Apple keeps Network.framework.

## Boundaries & Constraints

**Always:**
- **The seam is `Connection.h:118-124`**: a backend provides `networkAvailable()`, `makeIoQueue()` and
  `makeConnection()`, and implements `Connection` (:79) and `ConnectionListener` (:64). Nothing above
  it changes.
- **Behaviour matches the existing networking spec** (spec-runtime-networking.md): every `net-*` row
  passes unchanged. Because the protocol layer is reused rather than reimplemented, this follows from
  the seam being correct -- HTTP/1.1, `Accept-Encoding`, redirect policy, the cookie jar,
  `ConnectionPool` (six per origin), `flowWindow` backpressure and WebSocket semantics are all
  portable code that is already tested.
- **The threading contract** (`Connection.h:8-11`): every call in and every callback out happens on
  the one serial `IoQueue`. Java read/write threads must re-post onto it, never call the listener
  directly, and no listener call may arrive after `close()` returns.
- **Backpressure is real:** `setReceiving(false)` must stop reading from the socket, not buffer.
- **TLS:** the platform trust store by default. `TlsSettings::testAnchors` is honoured only when
  non-empty, through a custom `X509TrustManager`, and can never be set from JS.
- **The class loader:** any Java class this backend uses is resolved where the app's class loader is
  reachable and held as a `NewGlobalRef` -- the JS and I/O threads are native threads attached to the
  VM and their `FindClass` cannot see app classes (`HostMain.cpp:108-151`).
- **Language and dependencies:** Java only (Java 11, package `dev.screenkit.net`), no third-party
  networking dependency, nothing bundled. `android.permission.INTERNET` is added.
- **Build selection:** at build time in `runtime/CMakeLists.txt`, alongside the existing Apple branch.

**Never:**
- A third-party HTTP client, OkHttp included -- see the decision below.
- Changes to `__screenkit.net`, `dom-shim.js` networking, or JS-visible behaviour.
- Reimplementing HTTP, WebSocket framing, redirects, cookies, pooling or flow control.
- A bundled TLS library, or a certificate-verification bypass.
- HTTP/2, proxies, or `permessage-deflate`.
- Kotlin, in sources or dependencies.
- Edits to vendored code.

**Decision (sequencing, satisfied):** the Android host (Architecture M10) was built first; this
backend is built and verified on it, not against a desktop JVM.

**Decision (seam, 2026-09-18, supersedes the OkHttp decision):** the backend implements `Connection`,
not a whole HTTP client. Investigation found the earlier premise wrong: `NetService.h:34` is a
concrete class with no virtual methods, so there was no interface to put OkHttp behind, and OkHttp
cannot serve a raw-byte seam. Using it would have required making `NetService` abstract and
re-deriving redirect, cookie, pooling and flow-control semantics in Java against 16 existing rows,
and would have pulled `kotlin-stdlib` into a build that states it has none (`settings.gradle:2`).
This restores the plan recorded at deferred-work.md:305.

**Decision (verification, 2026-09-18):** the `net-*` rows are run on the `screenkit-tv` emulator
against the existing node fixture reached over `adb reverse` -- the same rows and assertions as Apple.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Existing rows | 15 `net-*` rows plus `net-cookie-public-suffix`, run on the emulator | identical transcripts to Network.framework | a row skipped for "network unavailable" counts as a failure |
| `net-image` | the same row on Apple only | covered on Apple; **not** run on Android | it uploads its download to a texture and the net test binary has no drawable (human decision, 2026-09-18) |
| No JavaVM | the backend is built but no JavaVM was captured | `networkAvailable()` false; requests fail `unsupported` | no crash |
| Untrusted / expired / wrong-host certificate | the fixture's bad certs, without test anchors | `NetError::Tls` | connection fails, no bypass |
| Backpressure | `setReceiving(false)` during a large download | the socket stops being read and the server stalls | resumes on `setReceiving(true)` |
| Shutdown mid-call | teardown during a streaming response with an open WebSocket | calls cancelled, no callback after `shutdown()` returns, no JNI global ref leaked | N/A |

</frozen-after-approval>

## Code Map

**The seam (what this implements):**
- `runtime/core/src/net/Connection.h` -- `:8-11` threading contract; `:29-40` `NetError` kinds
  (None, Dns, Connect, Tls, Network, Protocol, Redirect, Decode, Url, Unsupported); `:46-54` `IoQueue`
  (`post`/`postAfter`/`sync`); `:60` `TlsSettings.testAnchors` (DER); `:64-73` `ConnectionListener`
  (ready/data/eof/failed/flushed); `:79-110` `Connection` (setListener, start, write, close,
  `setReceiving`, ready, closed, bytesWritten, bytesFlushed, url); **`:121-124` the three free
  functions.**
- `runtime/core/src/net/NetworkFramework.mm` -- the reference to mirror: `:32` `DispatchIoQueue`;
  `:67` `NwShared`, one shared_ptr holding all mutable state that every callback checks for `closed`
  first -- this is what guarantees no listener call after `close()`; `:96-110` `classify()`, the one
  piece each backend rewrites; `:153`/`:309-315` backpressure by not asking for the next read;
  `:248-255` a TLS verify path installed **only** when `testAnchors` is non-empty; `:112-119`
  shutdown; `:329-335` the three entry points.
- `runtime/core/src/net/NetworkUnavailable.cpp:19,77,113-117` -- the skeleton to copy. Note `:39`
  `postAfter` ignores its delay; the real backend must honour it, because `bindings/Net.cpp:534`
  uses `makeIoQueue` as a general-purpose serial queue for image decode.

**Portable, reused, not to be touched:**
- `runtime/core/src/net/NetService.h:34` -- concrete, no virtuals; `runtime/CMakeLists.txt:155-164`
  compiles it and `Http.cpp`, `WebSocket.cpp`, `CookieJar.cpp` once for every platform.
- `runtime/core/src/net/Http.cpp` -- redirects `:746-789` (limit `:12`), default headers `:498-536`,
  inflate `:352-373`, cookies `:525-527`/`:939-944`, idempotent retry `:896-900`, pool `:433-491`.

**Android host as M10 built it:**
- `runtime/CMakeLists.txt:188-197` -- the fork point. Mirror the font block at `:199+`, which already
  has an `elseif(ANDROID)` branch.
- `runtime/android/jni/HostMain.cpp:108-151` `warmIntl` -- **the class-loader trap.** The JS thread is
  a native thread attached to the VM, so `FindClass` there reaches the system class loader and cannot
  see app classes; M10 worked around it by warming fbjni's cache from SDL's Java-started thread.
  `deferred-work.md:388-390` records the robust fix. `:534,538` are the only JNI functions, resolved
  by name mangling: **there is no `JNI_OnLoad`, no captured `JavaVM*` and no `RegisterNatives`
  anywhere in the repo.**
- `runtime/android/app/src/main/AndroidManifest.xml:6,16` -- no `INTERNET` permission, with a comment
  saying why.
- `runtime/android/app/build.gradle:58,66,93-94` -- minSdk 24, `arm64-v8a`+`armeabi-v7a`, Java 11.
- `runtime/android/settings.gradle:2` -- "nothing in this build is Kotlin".

**Tests:**
- `runtime/tests/CMakeLists.txt:253-269` the 16 `net-*` rows; `:271-286` the `net-server` fixture,
  `SKIP_RETURN_CODE 77`.
- `runtime/tests/RuntimeTests.cpp:4321-4347` `netFixture()` -- reads `servers.json`, skips when
  `net::networkAvailable()` is false; `:6538-6554` the dispatch table.
- `runtime/tests/net/server.mjs` -- host-side node on 127.0.0.1, ephemeral ports in `servers.json`,
  fresh DER certs per start (`certs.mjs`). An emulator reaches it only via `adb reverse`.
- `runtime/CMakeLists.txt:335` -- the suite builds on `APPLE AND NOT tvOS` only, and it drives the
  macOS host and ANGLE offscreen surfaces, so the **whole** binary will not cross-compile; the net
  rows need a target that excludes the GL-dependent cases.
- `tools/android/android.sh:229-240` -- `sdk avd build install push run shot key logs`, AVD
  `screenkit-tv` (`:35`), `adb` plumbing to copy into the app's files dir (`:147-160`).

**Toolchain (the spec's earlier "missing" note was stale):** NDK 27.3.13750724, cmdline-tools,
emulator with `screenkit-tv`, JDK 23, Gradle 9.0.0 (wrapper 8.14.3). **1.8 GB free disk.**

## Tasks & Acceptance

**Execution:**
- [x] `runtime/android/app/src/main/java/dev/screenkit/net/Transport.java` (new) -- a socket the native
  side drives by handle: connect (plain or `SSLSocket`) with a timeout, a reader thread delivering
  bytes to native, write, close, `setReceiving` pausing the reader, and a trust manager used only when
  test anchors are supplied. Java 11, no dependencies.
- [x] `runtime/core/src/net/NetworkAndroid.cpp` (new) -- the three entry points; a serial `IoQueue`
  honouring `postAfter` delays; `Connection` over JNI, with every Java upcall re-posted onto the queue
  and all mutable state in one shared block checked for `closed`, as `NwShared` does; exception and
  socket-error mapping to `NetError` kinds; `net::setJavaVm(JavaVM*)` for a non-Android embedder.
- [x] `runtime/android/jni/HostMain.cpp` -- add the repo's first `JNI_OnLoad`: capture `JavaVM*`,
  resolve the `dev.screenkit.net` classes there (where the app's class loader is reachable) into
  `NewGlobalRef`s, cache their method ids, and `RegisterNatives` the callbacks -- without this the
  I/O threads fail exactly as Intl did.
- [x] `runtime/CMakeLists.txt` -- an `elseif(ANDROID)` branch at `:188-197` selecting
  `NetworkAndroid.cpp`, mirroring the font block at `:199+`.
- [x] `runtime/android/app/src/main/AndroidManifest.xml` -- add `android.permission.INTERNET`, replace
  the "no network backend" comment.
- [x] `runtime/tests/CMakeLists.txt`, `runtime/CMakeLists.txt:335` -- a net-only test target that
  cross-compiles for android-arm64 (the GL-dependent cases excluded), keeping the macOS suite as it is.
- [x] `tools/android/android.sh` -- a `test` subcommand: start the node fixture, `adb reverse` its
  ports, push and run the net binary on `screenkit-tv`, and fail on a skip.
- [x] Docs -- `Architecture.md:170-171,580,643`, `runtime/README.md:16,234-236`,
  `runtime/js/README.md:374,434,467-468`, and a `deferred-work.md` entry: all currently assert Android
  has no network backend.

**Acceptance Criteria:**
- Given the emulator and the fixture over `adb reverse`, when the net rows run, then all 16 listed in
  `NET_ROWS` pass with none skipped, matching the Apple transcripts.
- Given the macOS build, when the full suite runs, then it still passes 163/163 on Network.framework.
- Given the Linux (Batocera) build, when it compiles, then it still builds the Unavailable backend.
- Given a page on a real Android build, when it uses `fetch`, XHR, WebSocket and EventSource, then
  they behave as on Apple.

## Implementation Notes

**Shape.** `Transport.java` (one file, no dependency) owns a reader thread and a writer thread per
connection; `NetworkAndroid.cpp` owns the serial queue, the `Connection` and all mutable state in one
`AndroidShared` block, exactly as `NwShared` does. Every Java callback is a static native taking the
connection's id, and does nothing but copy its bytes and post onto the queue; the lambda there checks
`closed` before touching the listener, and `close()` unregisters the id first, so a callback racing a
close finds nothing twice over. `classify()` is the one rewritten piece: `UnknownHostException` is
`Dns`, anything `javax.net.ssl.SSL*` or `java.security.cert.*` before the handshake finished is `Tls`
and after it `Network`, everything else is `Connect` before ready and `Network` after.

**Backpressure needed a handshake the Apple backend gets for free.** Network.framework asks for the
next read from *inside* the delivery callback, so a listener that stops receiving stops the reads. A
Java reader thread has no such coupling: left alone it would read as fast as the socket allows and
pile the bytes up in the queue's backlog, which is the buffering the spec's Always list forbids. So
the reader parks after each chunk until the queue calls `Transport.delivered()` — one read
outstanding at a time — and `setReceiving(false)` parks it before the next read on top of that.

**TLS.** The default `SSLSocketFactory` (platform trust store) unless `testAnchors` is non-empty, in
which case an `SSLContext` with `AddedAnchors`: the system trust manager first, the anchors-only one
only if that refuses. Both do the full chain check, so an expired certificate still fails. Host names
are verified through `SSLParameters.setEndpointIdentificationAlgorithm("HTTPS")` — without it
`SSLSocket` checks the chain and *not* the name, which is the one gap between it and
`HttpsURLConnection`. SNI is sent for names and not for literals, as on Apple.

**`AddedAnchors` has to extend `X509ExtendedTrustManager`,** and that was not obvious: written as a
plain `X509TrustManager` it passed every row except the one that matters, and `net-https` on the
device resolved the wrong-host certificate with status 200. Conscrypt does the host-name check inside
the `checkServerTrusted(chain, authType, socket)` overload; a manager that implements only the
two-argument form is called through that instead and no name is ever checked, whatever the socket's
endpoint identification algorithm says. Production never took this path — the platform's own manager
is extended — but a wrapper that is not is the classic Android TLS hole. Recorded in
deferred-work.md.

**Where the device rows run, and why not where the spec said.** The spec's task says "push and run
the net binary". A binary run from `adb shell` has no JavaVM, so `FindClass` could never reach
`dev.screenkit.net.Transport`, `networkAvailable()` would be false and every row would skip — which
the matrix counts as a failure. The rows therefore load `libscreenkit-net-tests.so` (the same
`RuntimeTests.cpp`, built by Gradle under `-Pscreenkit.netTests`) inside the debug APK, through
`dev.screenkit.net.NetTests` in `app/src/nettests/`, where the VM, the app's class loader, the INTERNET
permission and the app's storage all exist. `android.sh test` starts the same node fixture ctest
uses, `adb reverse`s its ports on the same numbers (so the certificates' `127.0.0.1` still matches),
pushes `servers.json`, `ca.der` and the prelude into the app's storage, runs one row per launch and
treats a skip as a failure. `net-image` is not in the list: it uploads its download to a texture and
`GlSurfaceSdl` has no offscreen path, so this binary has no drawable. Both deviations are in
deferred-work.md.

**What the app process does that `SDL_main` normally would.** The rows are called from a plain
activity through JNI, so three things the host gets for free had to be done in the entry point, and
none of them touches the shipping runtime: `SDL_SetMainReady()` (under `SDL_MAIN_HANDLED`, so the
header does not redefine this file's `main`), without which `SDL_InitSubSystem(SDL_INIT_EVENTS)` in
`HermesHost.cpp` refuses outright; `SDL.setupJNI()` / `SDL.initialize()` / `SDL.setContext()` from
Java, which `SDLActivity.onCreate` does and without which SDL calls a null `jclass` and ART aborts
the process; and `warmIntl()`, the same throwaway-runtime warm-up `runtime/android/jni/HostMain.cpp`
runs, because the prelude itself calls `toLocaleUpperCase` and Hermes' Intl is Java behind fbjni's
class cache. The entry point also pipes its own stdout and stderr into logcat — a row reports through
`fprintf(stderr)` (TestSupport.h) and an app's stderr otherwise goes nowhere, since
`setprop log.redirect-stdio` is refused on the user builds every emulator image is.

**The last two matrix rows needed something to assert against.** "No JavaVM" cannot happen by itself
on a device, where `JNI_OnLoad` always runs, so the harness withholds it: `NetTests` sets
`SCREENKIT_NET_TESTS_NO_BACKEND` before `System.loadLibrary`, and the test library's `JNI_OnLoad`
then declines to call `setJavaVm`/`prepareAndroidNetwork`. Nothing in the backend knows the row
exists — it is one caller not calling. "No JNI global ref leaked" has nothing to look at either: a
reference the backend forgets to delete fails nothing and logs nothing, the Java object and its
socket simply never go. So `NetworkAndroid.h` exposes `net::liveTransportCount()`, one per live
connection, and the row asserts it is zero after `close()`, after the destructor, and after each
teardown. Both rows are Android-only (`SCREENKIT_ANDROID_NET_ROWS`) and absent from the macOS
binary, which is why that suite is still 163 rows and not 165.

**Staging the "no callback after close()" race.** Written the obvious way the assertion was vacuous:
`close()` runs on the I/O queue, so a delivery posted before it is ordinary and proves nothing, and
deleting the `closed` guard did not fail the row. The row now holds the queue busy for 400 ms first,
so the reader posts a chunk *behind* the closing job — the fixture sends one every 20 ms — and that
delivery is dequeued after `close()` has returned. With the guard removed it reaches the listener;
with it, it is dropped.

**Verified on the device.** All 18 rows pass on `screenkit-tv`, none skipped, and the three new
assertions were sabotage-checked one at a time (see Verification).

**Matrix Test Audit, 2026-09-18 -- PASSED.** Every row is covered by a row that ran and passed on
`screenkit-tv`, verified by re-running `sh tools/android/android.sh test` in this session rather than
from the implementation's report: 18/18, none skipped. Row by row -- existing rows: the 15 `net-*`
plus `net-cookie-public-suffix`; `net-image`: Apple only, by the human's amendment, and it passes in
the macOS 163; no JavaVM: `net-no-javavm`; bad certificates: `net-https`; backpressure:
`net-flow-control`; shutdown mid-call and the JNI global-ref claim: `net-shutdown-mid-call`, which
counts live refs through `net::liveTransportCount()`.

The audit failed twice before it passed, which is the useful part of the record. First nothing ran at
all: the disk was too small to boot the emulator, and 16/16 rows failed on
`SDL_InitSubSystem(SDL_INIT_EVENTS)` because the rows run from a plain Activity that never goes
through `SDL_main`. Then two rows had no test at all. Both are now covered and each was
sabotage-checked -- withholding the VM fails `net-no-javavm`, dropping the `fetch_sub` in
`closeShared` fails `net-shutdown-mid-call` with one leaked reference per connection, and removing the
`closed` guard in the data callback fails it only once the race is actually staged.

## Spec Change Log

- 2026-09-18 -- the two remaining matrix rows covered: `net-no-javavm` and `net-shutdown-mid-call`,
  Android-only, both sabotage-checked on the device. `NetworkAndroid.h` gained
  `net::liveTransportCount()` so the "no JNI global ref leaked" claim has something to assert
  against; the harness withholds the JavaVM for the first row without the backend knowing. 18/18 on
  `screenkit-tv`, macOS still 163/163, Linux still builds the Unavailable backend.

- 2026-09-18 -- implemented and verified on `screenkit-tv`: 16/16 rows pass, none skipped. Two
  departures from the task list, both forced and both in deferred-work.md: the device rows run inside
  the debug APK rather than from a pushed binary (a binary run from `adb shell` has no JavaVM, so
  every row would skip and the matrix counts a skip as a failure), and `net-image` is Apple-only (it
  uploads to a texture and `GlSurfaceSdl` has no offscreen path) -- the human amended the matrix to
  15 `net-*` rows plus `net-cookie-public-suffix`. The frozen block is otherwise untouched. The device
  run earned its keep: it caught a real backend bug no compile check could (a custom
  `X509TrustManager` disables host-name verification on Android; `net-https` accepted a certificate
  issued for another host).

- 2026-09-18 -- resumed from `draft` after M10 landed, as the frozen sequencing decision required.
  Investigation found the frozen Approach's premise wrong: `NetService` is not a seam (no virtuals),
  so OkHttp had nothing to sit behind. The human renegotiated the frozen block: the backend now
  implements `Connection` and OkHttp is dropped, restoring deferred-work.md:305. Verification moved to
  the emulator over `adb reverse`. The earlier `macos-okhttp` preset and its desktop-JVM verification
  are gone -- the frozen block already forbade desktop-JVM verification. Toolchain facts corrected:
  NDK, cmdline-tools and the AVD are installed. KEEP: the Code Map's seam facts and the class-loader
  finding -- both were mis-stated in the first pass and are what the implementation depends on.
  Renamed from `spec-android-okhttp-network.md`, since the spec now forbids OkHttp;
  `spec-m10-android-host.md:16,57` still refers to it by the old name and as "the OkHttp
  backend". That spec is `done` and its frozen block is left alone.

## Review Triage Log

Review pass 1, 2026-09-18. Three layers: blind-hunter (14), edge-case-hunter (14), verification-gap
(4 + 3 other). One row per finding; groups named in brackets.

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| 1 | [G1] The shipping host's `JNI_OnLoad` is never executed by any row | **high** | Verified: `NetTests.load()` loads `screenkit-net-tests`, not `screenkit`; the only two `JNI_OnLoad`s are `HostMain.cpp:545` and `RuntimeTests.cpp:6949`, and the rows reach only the second. Deleting the wiring from `HostMain.cpp` leaves 18/18 green. |
| 2 | [G2] `net_row` polls logcat with no binding to the launch it made | **high** | `android.sh:266` swallows a failed `logcat -c` with `|| true`, and the matched line carries no nonce, pid or `-T` window. A clear that does not take returns the previous run's `rc=`. |
| 3 | [G3] `write()` adds to `written` before the JNI call and swallows both failure paths | **high** | Verified at `NetworkAndroid.cpp`: `s_->written += bytes->size();` precedes `NewByteArray`/`CallVoidMethod`; both failures `return` silently, so `bytesWritten()` over-reports and the protocol layer stalls with no error. |
| 4 | [G3] `setReceiving` caches state before the call can fail | **medium** | Same function: `s_->receiving = receiving;` runs ahead of the guards, and the dedupe early-out then drops the retry, leaving the Java reader parked. |
| 5 | [G3] `releaseReader` parks the reader permanently on a JNI failure | **medium** | A lost `Transport.delivered()` is exactly the stalled connection its own comment warns about; the path logs at Warn and returns. |
| 6 | [G4] `sync()` blocks forever once `stopping_` is set | **high** | Verified: `schedule()` returns without queuing when `stopping_`; `sync()` then waits unconditionally on `finished`. |
| 7 | [G4] `~ThreadIoQueue` discards pending work | **high** | `run()` exits with `work_` non-empty, so a queued `closeShared`/`fail`/`delivered` never runs; libdispatch drains, so this diverges from Apple for a queue `bindings/Net.cpp` reuses. |
| 8 | [G4] A throwing job escapes `run()` and terminates the process | **medium** | No try/catch around the job call; `std::terminate` with `mutex_` held. |
| 9 | [G5] `GetMethodID` failure leaves a pending exception across five more JNI calls | **high** | `prepareAndroidNetwork` resolves six ids then checks for null once; a `NoSuchMethodError` makes the next JNI call illegal, which CheckJNI turns into an abort. |
| 10 | [G5] A failure after `NewGlobalRef` keeps both class refs, and a retry overwrites them | **high** | The early-out tests only `gReady`, so a second call re-runs `FindClass`/`NewGlobalRef` and leaks the first pair. |
| 11 | [G5] `prepareAndroidNetwork` is documented idempotent but is unsynchronised | **medium** | `NetworkAndroid.h` promises idempotence; the body is a bare `gReady.load()` then unsynchronised writes to seven globals, `gVm` included. |
| 12 | [G6] `closeShared` leaks the global ref when `ScopedEnv` fails | **medium** | Verified: `if (!env) return;` sits after `s->transport = nullptr`, so `DeleteGlobalRef` and `fetch_sub` never run and `liveTransportCount()` stays elevated. Narrow (needs `AttachCurrentThread` to fail). |
| 13 | [G7] `NET_ROWS` hand-copies the CTest list and drifts silently | **medium** | Nothing compares `android.sh:47` with `SCREENKIT_NET_TEST_CASES`; the Android branch of `tests/CMakeLists.txt` returns before that list exists. A new row runs on Apple only, with a green device run. |
| 14 | [G8] Every debug APK ships an exported `dev.screenkit.net.NetTests` | **medium** | Verified: `android:exported="true"` in `src/nettests/AndroidManifest.xml`; AGP merges the debug source set regardless of `-Pscreenkit.netTests`, which gates only the native target. `done()` ends in `System.exit(0)`, so any app that can send an intent can kill the process. |
| 15 | [G9] No timeout bounds the TLS handshake or any read | **medium** | `connectTimeoutMs` reaches only `plain.connect`; `startHandshake()` and every `read` run with no `setSoTimeout`, so a peer that connects then stalls parks the reader forever. Apple's 30 s covers establishment including TLS. |
| 16 | [G10] `isIpLiteral` disagrees with the canonical `Url.cpp` | **medium** | `Url.cpp:86` requires exactly three dots; the Java copy accepts any digits-and-dots, so `1.2.3` and `1234` silently lose SNI. |
| 17 | [G11] `SNIHostName` throws on hosts with an underscore or trailing dot | **medium** | Unguarded `new SNIHostName(host)` inside `handshake()`; an `IllegalArgumentException` aborts a connection that would otherwise work. |
| 18 | [G12] ALPN is offered on Apple but never on Android | **medium** | `NetworkFramework.mm:247` adds `http/1.1`; `Transport.handshake()` sets no `setApplicationProtocols`, so a server with a strict ALPN callback fails on Android and succeeds on Apple. Needs an API 29 guard against `minSdk 24`. |
| 19 | [G13] A null from `makeAnchors` falls back to the platform trust store in silence | **medium** | Configured test anchors would be dropped with no error; the connection then succeeds against a different trust root than asked for. |
| 20 | [G14] The writer drops a chunk when `output` is null after dequeue | **medium** | The chunk is already removed from `pending`; nothing re-queues it and no failure reaches the listener. |
| 21 | [G15] `AndroidShared` holds a `shared_ptr` to the queue that owns its pending jobs | **medium** | `NwShared` stores the raw `dispatch_queue_t` precisely to avoid this; here a job still in `work_` keeps the queue and its thread alive, and dropping the last reference from `~ThreadIoQueue` re-enters it. |
| 22 | [G16] Raw sockets bypass the platform cleartext-traffic policy | **medium** | `java.net.Socket` is not subject to `NetworkSecurityPolicy`, and the manifest declares neither `usesCleartextTraffic` nor a network-security-config, so `http://` works under `targetSdk 35` whatever an embedder configures. |
| 23 | [G17] `postAfter` can overflow its microsecond cast | **low** | A huge `delayMs` sorts before `now()` and fires at once. Unlikely, but the fix is a one-line clamp. |
| 24 | [G18] `android.sh logs` filters a tag the suite never uses, and `fixture_stop` removes every reverse forward | **low** | Tag filters are exact and the rows log under `ScreenKitNetTests`; `adb reverse --remove-all` tears down forwards this script never created. |
| 25 | [G19] `Transport.close()` blocks the shared I/O queue | **medium**, deferred | Closing an `SSLSocket` can block on close_notify to a stalled peer, stalling every other connection on that runtime's single queue. Real, but the fix is a thread hop rather than a direct correction. |
| 26 | [G20] Nothing keeps `Transport`/`NetTests` from R8 | **medium**, deferred | No keep rules and no `@Keep`; `GetMethodID`/`RegisterNatives` bind by name. No release buildType exists yet, so nothing is broken today. |
| 27 | [G21] No row for a VM present while `FindClass` fails, and `liveTransportCount()` is asserted in only two rows | **low**, deferred | `net-no-javavm` covers only the no-VM path; the class-loader failure the function exists for is untested. |
| 28 | [G22] `armeabi-v7a` is shipped and executed by nothing | **low**, deferred | The AVD is `arm64-v8a`; the 32-bit slice of every `jlong`/`jsize`/`size_t` conversion runs nowhere. |

No `intent_gap` or `bad_spec` entries: every root cause is inside the implementation, and none needs the
frozen block or the spec reopened. Findings 1-24 route to **patch**, 25-28 to **defer**.


## Design Notes

TLS must come from Java. The NDK ships no public TLS library, and the Never list forbids bundling
one, so `SSLSocket` with the platform trust store is the only route -- which is why plain sockets go
through Java too rather than splitting the transport across two languages.

Why the seam is this small: `Connection` carries bytes. Everything a browser-visible network API
needs above that -- status parsing, chunked bodies, redirects, cookies, six-slot pooling, the
`flowWindow`, gzip -- is portable C++ that Apple already exercises. A backend that gets
`start`/`write`/`setReceiving`/`close` and the five listener callbacks right inherits all of it.

## Verification

**Commands:**
- `sh tools/android/android.sh test` -- **Run on `screenkit-tv`: 18/18 PASS, none skipped.**
  `net-http-get`, `net-https`, `net-request-body`, `net-streaming-request`, `net-streaming-response`,
  `net-http-error-status`, `net-redirect`, `net-encoded-body`, `net-abort-timeout`,
  `net-flow-control`, `net-unreachable`, `net-cookies`, `net-websocket`, `net-eventsource`,
  `net-shutdown-idle`, `net-cookie-public-suffix` -- the same transcripts as Network.framework --
  plus `net-no-javavm` and `net-shutdown-mid-call`, the two matrix rows that exist only here.
  (`net-image` is Apple-only, by the amended matrix.)
- **Sabotage-checked**, one at a time, each rebuilt and re-run on the device:
  - the harness stops withholding the JavaVM (`NO_JAVAVM_ROW` renamed) → `net-no-javavm` fails
    `expected: !screenkit::net::networkAvailable()`. The row cannot pass with the backend wired up.
  - `gLiveTransports.fetch_sub(1)` removed from `closeShared` → `net-shutdown-mid-call` fails five
    times: `got 1, want 0` after `close()` and after the destructor, then `got 3`, `got 5`, `got 7`
    accumulating across the three teardowns — one leaked reference per connection, exactly the bug.
  - the `if (s->closed) return;` guard removed from the data callback →
    `recorder.afterClose.load() == 0 (got 1, want 0)` and `recorder.data.load() == before
    (got 2, want 1)`: a delivery queued behind the close reaches the listener after `close()` returned.
- `ctest --test-dir runtime/build/macos --output-on-failure` -- 163/163, Network.framework untouched.
  **Run, before and after: 163/163 both times.**
- `sh tools/batocera/pi.sh build` -- linux-arm64 still builds the Unavailable backend. **Run: the
  regenerated `build.ninja` still compiles `NetworkUnavailable.cpp` and the host links.**
- `sh tools/android/android.sh build && sh tools/android/android.sh run blits-example-app` -- the APK
  builds and an app runs with the backend in place. **`build` run (arm64-v8a + armeabi-v7a, with and
  without `-Pscreenkit.netTests`); `run` needs the emulator, so NOT RUN.**

**Also run, in place of the device:**
- `clang++ --target=aarch64-linux-android24 -std=c++17 -fsyntax-only -Wall -Wextra` on
  `NetworkAndroid.cpp` -- clean.
- `javac -bootclasspath android.jar` on `Transport.java`, then `javap -s`: every descriptor matches
  the JNI signature the backend registers, one by one.
- `llvm-nm -D` on both APK libraries: `JNI_OnLoad` is exported from `libscreenkit.so` and from
  `libscreenkit-net-tests.so`, beside the activity's and the test runner's own entry points.
- `aapt2 dump xmltree` on the built APK: `android.permission.INTERNET` is in the merged manifest, and
  the debug-only `dev.screenkit.net.NetTests` activity with it.

**Manual checks (if no CLI):**
- `adb shell dumpsys package dev.screenkit.host | grep INTERNET` -- the permission is granted.
