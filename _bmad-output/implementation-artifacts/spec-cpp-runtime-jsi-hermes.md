---
title: 'C++ runtime: Hermes host and bytecode loading'
type: 'feature'
created: '2026-09-16'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** ScreenKit cannot execute JavaScript. `Architecture.md` §2 specifies seven native
subsystems under `runtime/core/`; none exist. Everything from M3 onward is blocked on booting Hermes.

**Approach:** A Hermes host — one `jsi::Runtime` owned by one thread — plus bytecode loading and a
`console` binding to prove it. Hermes consumed prebuilt via `tools/prebuilts`.

**Decisions taken at planning:** platforms are **macOS + tvOS simulator** (both have first-class
prebuilt slices); **Linux is deferred** (RN publishes no Linux Hermes runtime; Linux is M11); the
**JSI object model and event loop are separate specs** (`deferred-work.md`) — `console` is installed
directly here, with no `SharedObject`, `EventEmitter`, or timers.

## Boundaries & Constraints

**Always:**
- Hermes consumed prebuilt, never compiled.
- One thread owns the `jsi::Runtime`; all access goes through its executor. No mutex around the
  runtime, no `jsi::Runtime&` crossing threads.
- Executor and context held `weak_ptr`, re-locked in the lambda, so work queued against a torn-down
  runtime is dropped.
- Version pin lives only in `tools/prebuilts/manifest.json`; CMake reads it via `string(JSON …)`.

**Never:**
- No React Native dependency: link `hermesvm` alone, do not vendor ReactCommon or compile `jsi.cpp`.
- No timers, microtask scheduling, or object model — deferred specs own those.
- No multi-instance support: one runtime, one thread. Leave the API open to a second; build one.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Valid bytecode | `.hbc` from the pinned `hermesc` | Evaluates; `console.log` hits the platform log | N/A |
| Source fallback | `.js` text buffer (dev path) | Evaluates, no bytecode caching | N/A |
| Version mismatch | `.hbc` from another Hermes | Refused **before** evaluation | Error names expected vs actual version; no crash |
| Corrupt bytecode | Truncated `.hbc` | Refused | `hermesBytecodeSanityCheck` failure surfaced |
| JS throws | Bundle throws at top level | Caught at the boundary | `jsi::JSError` message + JS stack logged; host survives |
| Teardown race | Work queued, runtime destroyed | Callback dropped | Weak ref fails to lock; no use-after-free |

</frozen-after-approval>

## Code Map

**Reuse:** `tools/prebuilts/fetch.mjs` (download → sha256 → unpack → `.verified`; **needs `.tar.gz`
support**, currently shells to `unzip` only) · `tools/prebuilts/manifest.json` (add a `hermes` block
beside `angle`) · `poc/angle-tvos/CMakeLists.txt` (`string(JSON …)` pin read, `file(GLOB)` +
`FATAL_ERROR` lookup) · `poc/angle-tvos/scripts/build-app.sh` (Ninja + `CMAKE_SYSTEM_NAME=tvOS`;
**never the Xcode generator**, `Architecture.md` §11) · `Architecture.md` §8, §10.1.

**Do not touch:** `tools/prebuilts/retag-macho.py` — Hermes ships real tvOS slices, unlike ANGLE.

**Verified artifact facts — do not re-investigate:**
- Tarball `hermes-ios-$V-hermes-ios-release.tar.gz` (`repo.reactnative.dev/maven2`), root `destroot/`.
  Framework is **`hermesvm`**, not `hermes`.
- Headers **only** under `destroot/include/` (`jsi/jsi.h`, `hermes/hermes.h`) — frameworks have no
  `Headers` dir, so that path must be on the header search path.
- **`hermesvm` exports the JSI implementation** (466 `facebook::jsi::*` symbols) — link it alone.
- `universal/hermesvm.xcframework` carries `tvos-arm64`, `tvos-arm64_x86_64-simulator`, `ios-arm64`,
  `ios-arm64_x86_64-simulator`, `-maccatalyst`, `xros-*` — **no macOS slice**. macOS is a separate
  fat framework at `destroot/Library/Frameworks/macosx/hermesvm.framework` (x86_64+arm64). Two
  shapes, both handled. *(Corrected during implementation; the original claim was wrong.)*
- Create: `facebook::hermes::makeHermesRuntime(const ::hermes::vm::RuntimeConfig&)`.
- Validate: `makeHermesRootAPI()` → `jsi::castInterface<IHermesRootAPI>` → `isHermesBytecode` /
  `hermesBytecodeSanityCheck` / `getBytecodeVersion`. The static `HermesRuntime::isHermesBytecode`
  **no longer exists**.
- Load: `prepareJavaScript(shared_ptr<const jsi::Buffer>, url)` → `evaluatePreparedJavaScript`.
  `jsi::FileBuffer` is in `jsi/jsilib.h`.
- `hermesc` ships in the **`hermes-compiler`** npm package, not the `react-native` tarball.

## Tasks & Acceptance

**Execution:**
- [x] `tools/prebuilts/manifest.json` -- add `hermes` block (version, tarball URL, sha256, slice per
      target) -- single source of truth for the pin.
- [x] `tools/prebuilts/fetch.mjs` -- add `.tar.gz` extraction and a `hermes-compiler` resolver.
- [x] `runtime/core/include/screenkit/Runtime.h` -- embedder API (create/evaluate/shutdown); the only
      header a platform shell includes.
- [x] `runtime/core/src/hermes/HermesHost.{h,cpp}` -- owns thread, runtime, heap limit, executor;
      drops queued work after teardown.
- [x] `runtime/core/src/hermes/BytecodeLoader.{h,cpp}` -- `jsi::Buffer` over an mmap'd file; validate
      via `IHermesRootAPI` **before** `prepareJavaScript`.
- [x] `runtime/core/src/bindings/Console.cpp` -- `console.log/warn/error` host functions.
- [x] `runtime/CMakeLists.txt`, `runtime/apple/` -- link `hermesvm`; build `screenkit-host` for macOS
      and the tvOS simulator.
- [x] `runtime/tests/` -- cover every I/O-matrix row, with version-mismatched and truncated `.hbc`
      fixtures.

**Acceptance Criteria:**
- Given a `.hbc` from the pinned `hermesc`, when `screenkit-host` runs it on macOS **and** the tvOS
  simulator, then its `console.log` output appears in the platform log on both.
- Given a fresh clone with an empty prebuilts cache, when the build runs, then Hermes is fetched,
  checksum-verified, and **no third-party source is compiled**.
- Given teardown with work still queued, when that work fires, then it is dropped and ASan reports no
  use-after-free.
- Given `runtime/tests` under ASan and UBSan, when the suite completes, then it passes clean.

## Implementation Notes

**Pin.** `260318099.0.2`, read from `facebook/react-native@main`
`sdks/hermes-engine/version.properties`. Engine:
`com.facebook.hermes:hermes-ios` on Maven Central (sha256
`1179456e…`, 24 MB). Compiler: npm `hermes-compiler@260318099.0.2` (sha256
`4fede970…`, 19 MB). Bytecode version **99**, recorded in the manifest and
asserted against the live engine by the `bytecode-version-pin` test, so the
manifest cannot silently lie about what the pinned engine accepts.

**One correction to the verified-artifact facts.** The spec said
`universal/hermesvm.xcframework` carries `macos-arm64_x86_64`. It does not — its
slices are `ios-arm64`, `ios-arm64_x86_64-simulator`,
`ios-arm64_x86_64-maccatalyst`, `tvos-arm64`, `tvos-arm64_x86_64-simulator`,
`xros-arm64`, `xros-arm64_x86_64-simulator`. macOS is a *separate* versioned
framework at `destroot/Library/Frameworks/macosx/hermesvm.framework` (x86_64 +
arm64 fat, minos 11.0). Everything else in that list held: framework is
`hermesvm`, headers only under `destroot/include/`, `hermesvm` exports JSI, tvOS
slices are first-class so `retag-macho.py` is never applied. The tarball also
ships `destroot/bin/hermesc`, but the npm `hermes-compiler` resolver was built as
specified — it is the only route that works on a Linux host.

**Cache shape.** Every Apple slice comes out of one archive, so the cache holds a
single unpacked `destroot` at `hermes/<version>/apple/` and a target key selects
which framework path inside it the build links. `manifest.json` owns that
mapping; `cmake/HermesPrebuilt.cmake` reads it with `string(JSON …)`.

**`fetch.mjs` became multi-dep.** `fetchArchive()` is shared; `.tar.gz` goes
through `tar` with an optional `--strip-components`, zip still through `unzip`.
A target one dep does not publish is skipped with its reason instead of failing
the run (Hermes has no Linux/Windows runtime, ANGLE has no Android one); `--dep`
makes it exact again. `--hermesc` prints the host compiler path on stdout with
all logging on stderr, so CMake and scripts can capture it. ANGLE's behaviour is
unchanged.

**Configure-time fetch.** `cmake -S runtime -B …` fetches Hermes into the cache
when it is missing rather than erroring with instructions, so the fresh-clone
acceptance criterion holds for the raw cmake command and not only for the wrapper
scripts. It still falls back to the instructive `FATAL_ERROR` when node is absent
or the fetch fails.

**Validation order matters.** `isHermesBytecode` answers yes/no;
`hermesBytecodeSanityCheck` says *why not*. Running the sanity check first is
what turns the truncated fixture into `"Buffer smaller than a bytecode file
header. Expected at least 128 bytes but got 64 bytes"` instead of a generic
refusal. Version is read out of the header (magic at [0,8), uint32 LE at [8,12))
before either, so the mismatch error names both sides.

**Teardown has three drop points**, all exercised by the `teardown-race` test:
`post()` refusing once `stopping_` is set; the JS-thread loop checking
`stopping_` under the same lock it pops under, so a queued task is never
dispatched after stop was seen; and the executor's `weak_ptr` failing to lock
inside the queued lambda. The host thread captures a raw `this`, not a
`shared_ptr` — capturing the `shared_ptr` would make the thread keep the host
alive forever, so the destructor that joins it could never run.

**Blocking evaluate, without `invokeSync`.** `Runtime::evaluateBundle/Source`
post a task and wait on a one-shot cell that is satisfied either by the work
running or by the host cancelling it on teardown, so a caller cannot be left
waiting on a thread that has gone. `JsExecutor` still has no sync entry point,
and calling evaluate from the JS thread returns an error rather than deadlocking.

**Platform log.** `screenkit::setLogSink` is process-wide with a stderr default;
`runtime/apple/PlatformLog.mm` installs an `os_log` sink (subsystem
`dev.screenkit`, `%{public}s` so nothing is redacted) that also writes stderr, so
`simctl launch --console-pty` and `log show` both see the same lines.

**Files added:** `runtime/CMakeLists.txt`, `runtime/README.md`,
`runtime/cmake/HermesPrebuilt.cmake`, `runtime/core/include/screenkit/{Runtime,Log}.h`,
`runtime/core/src/Log.cpp`, `runtime/core/src/bindings/Console.{h,cpp}`,
`runtime/core/src/hermes/{HermesHost,BytecodeLoader}.{h,cpp}`,
`runtime/core/src/hermes/RuntimeImpl.cpp`,
`runtime/apple/{CMakeLists.txt,HostMain.mm,PlatformLog.h,PlatformLog.mm,Info.plist.in}`,
`runtime/tests/{CMakeLists.txt,RuntimeTests.cpp,TestSupport.h,make-fixtures.mjs}`,
`runtime/tests/fixtures/{hello,throws}.js`, `runtime/scripts/*.sh`.
**Modified:** `tools/prebuilts/{manifest.json,fetch.mjs,README.md}`.
`retag-macho.py` untouched, as instructed.

**Left for a follow-up:** `Architecture.md` §10 still lists
`tools/prebuilts/hermes.json  # [todo]`. The pin lives in `manifest.json`
instead, per this spec's Boundaries; that line is now stale and wants a one-line
correction whenever Architecture.md is next edited.

## Spec Change Log

## Review Triage Log

Three layers, ~54 raw findings. No `intent_gap` or `bad_spec` — no loopback.

**Routed to patch**

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| 1 | `threadMain` dispatch loop runs `task.run` outside any try/catch | high | Verified `HermesHost.cpp`: the try only wraps runtime creation; the `for(;;)` dispatch is bare. A `jsi::JSError` from any `invokeAsync` callback escapes the thread fn → `std::terminate`. Reachable from ordinary embedder code. |
| 2 | `console.error(new Error(...))` logs `{}` | medium | Verified `Console.cpp format()`: every non-function object goes through `JSON.stringify`, which yields `"{}"` for Error. The most common diagnostic call loses message and stack. |
| 3 | Only `log/warn/error` exist; `console` replaces the global wholesale | medium | Verified `Console.cpp:61-64`. `console.info` → TypeError, aborting the bundle. Hermes ships no console of its own, and bundler output commonly calls info/debug. |
| 4 | `host-runs-hello` cannot fail on exit code; host exit contract unpinned | medium | Two layers; verification-gap proved it empirically with a probe test that echoes the pass-string then exits 65 and still passes. `PASS_REGULAR_EXPRESSION` makes CTest ignore process status. |
| 5 | `stop()` called on the JS thread skips the join, leaving `thread_` joinable | medium | Verified `HermesHost::stop()`: guard is `get_id() != this_thread::get_id()`. `~HermesHost` then destroys a joinable thread → abort. Reachable today: `shutdown()` from inside an `invokeAsync` callback. |
| 6 | `maxHeapBytes` cast to 32-bit `gcheapsize_t` with no clamp | medium | Verified `HermesHost.cpp:63`. ≥4 GiB silently truncates; the heap ceiling becomes meaningless rather than erroring. |
| 7 | `PlatformLog.mm` nil dictionary key on a non-UTF-8 `RuntimeConfig::name` | medium | `stringWithUTF8String:` returns nil for invalid UTF-8; `cache[nil]` raises `NSInvalidArgumentException`. Tag is embedder input. Also `Error→OS_LOG_TYPE_FAULT` overstates a routine `console.error`. |
| 8 | Shell-glob interpolation in the ANGLE retag step | medium | `fetch.mjs` builds `sh -c "ls ${outDir}/*.a"` from an unquoted path derived from `$SCREENKIT_PREBUILTS`. A space or glob char in the cache root misbehaves. `readdirSync` needs no shell. |
| 9 | Cache layout documented three ways, and contradicts itself | medium | `tools/prebuilts/README.md` says `<dep>/<version>/<target>/` (true for ANGLE, false for Hermes), contradicts itself 40 lines later, and the layout is recomputed independently in `HermesPrebuilt.cmake` — two sources of truth, which this spec's Boundaries forbid. |
| 10 | Four untested branches that hold documented contracts | medium | Pre-verified by the verification-gap layer, each with a demonstration that keeps all 10 tests green: sub-12-byte `.hbc` header path; shutdown-during-evaluate (the anti-deadlock `Task::cancel`); JS-thread reentrancy guard; tvOS launch asserts nothing. |
| 11 | UBSan diagnostics do not fail the suite | medium | No `UBSAN_OPTIONS=halt_on_error=1`, so UBSan can print and the test still exits 0 — meaning my own "ASan/UBSan clean" verification is weaker than it appears. |
| 12 | `SCREENKIT_TARGET` sends iOS/watchOS/visionOS into the `elseif(APPLE)` macOS branch | medium | Silently selects the macOS framework; fails obscurely at link. Trivial guard. |
| 13 | tvOS configure with empty `HERMESC` warns and ships a bundle-less app | medium | `apple/CMakeLists.txt` emits `message(WARNING)`; the app then only logs "no hello.hbc in the app bundle" while the build reports success. |
| 14 | Windows host selects the linux `hermesc` path | low | `HermesPrebuilt.cmake` branches on `CMAKE_HOST_APPLE` with an `else()` fallthrough, though the manifest declares `hosts.win32`. |
| 15 | `--list` truncates every gap explanation at the first period | low | Renders the Hermes Linux reason as "...releases stop at v0." — cut inside "v0.13.0". `--list` is the advertised way to discover gaps. |
| 16 | No ARC for the `.mm` sources | low | Nothing sets `-fobjc-arc`, so both files compile MRR; `HostMain.mm` leaks its window and view controller. Fix is one compile option, not added complexity. |
| 17 | Assorted doc/robustness nits | low | `make-fixtures.mjs` header says "two of four" while emitting seven; `runtime/README.md` claims teardown-race covers all three drop points (the inner re-lock is unreachable by construction); bare `COMMAND node`; `run-tvos-simulator.sh` device detection dies under `set -e`; missing `@autoreleasepool` in the log sink; fetch has no timeout. |

**Rejected**

| Finding | Verdict | Refutation |
|---|---|---|
| `invokeAsync` never sets `Task::cancel` | low | The cancel channel exists for blocking callers, and the only blocking caller (`RuntimeImpl::run`) does set it. `invokeAsync` returns void and never promised a drop signal. The proposed fix adds public API surface, so a low finding is rejected on that basis. |
| `run()` bypasses `JsExecutor` | false | It posts to the same host queue under the same invariant — no `jsi::Runtime&` crosses a thread. Not a second unguarded path. |
| `castInterface<IHermesRootAPI>` unchecked for null | false | The version is pinned in the manifest; the pinned `hermesvm` exports the interface. No shown path reaches a null. |
| `ResultCell::wait` has no timeout | low | A bundle that never returns is the app's defect; adding a timeout means new config surface and a way to abandon a running VM. |
| `evaluateSource` does not reject HBC bytes | low | Requires the embedder to hand bytecode to the source entrypoint; the fix is a guard on a path never shown reachable. |
| mmap SIGBUS if the file is truncated after mapping | low | Fix is to stop mmapping — a design change, not a correction. |
| `kMagic` hardcoded; `--bytecode-version` flag unused | low | Speculative future-change risk and unused surface; neither is a present defect. |

**Deferred**

| Finding | Verdict | Why |
|---|---|---|
| ANGLE headers cloned from default-branch HEAD, unpinned | medium | Real, and it contradicts the manifest-is-the-pin claim — but pre-existing, written before this spec. Not caused by this change. |
| os_log output never read back; `host-runs-hello` observes the stderr mirror only | medium | Pre-verified disposition. Dropping `%{public}` leaves the suite green while `log show` prints `<private>`. Closing it means scraping `log show` in ctest — slow and timing-sensitive; wants CI. |
| No CI, no `CMakePresets.json` | medium | `Architecture.md` §11 calls for presets and the manifest claims CI asserts bytecode parity; neither exists. Outside this spec's intent. |
| Concurrent-fetch race; `.verified` stamp trusts a damaged cache | maybe-false | Would need two simultaneous fetches to demonstrate. Settled by adding a lock file and an existence probe. |
| Log-sink swap race between `setLogSink` and `~LogCapture` | maybe-false | Not reproduced under ASan across the full suite. Would be settled by a stress test swapping sinks while the JS thread logs. |
| `runBundle` blocks the main thread before first frame on tvOS | low | Real watchdog risk on device, but tvOS device is already unproven and out of scope here. |


## Design Notes

**Thread model.** React Native (`RuntimeExecutor` over `MessageQueueThread::runOnQueue`) and Expo
(`BridgelessJSCallInvoker`) converged independently on one executor owning the runtime. Adopt it now,
even for a single instance — retrofitting thread discipline is far harder.

```cpp
// invokeSync deliberately absent: sync native->JS is an error. Expo's
// BridgelessJSCallInvoker::invokeSync throws outright; match that.
class JsExecutor {
 public:
  virtual void invokeAsync(std::function<void(jsi::Runtime&)>&&) = 0;
  virtual ~JsExecutor() = default;
};
```

**Validate before evaluating.** `evaluateJavaScript` accepts bytecode directly, so skipping the check
is tempting. A mismatched or truncated `.hbc` then faults inside the VM instead of raising a catchable
error — the difference between two acceptance criteria passing and a crash.

## Verification

**Commands:**
- `node tools/prebuilts/fetch.mjs --list` -- expected: a `hermes` entry beside `angle`.
- `cmake -S runtime -B runtime/build/macos -G Ninja && cmake --build runtime/build/macos` --
  expected: links `hermesvm`, zero third-party sources compiled. On a cold cache configure fetches
  and checksum-verifies Hermes first.
- `runtime/build/macos/screenkit-host runtime/build/macos/fixtures/hello.hbc` -- expected: prints the
  bundle's `console.log` output. (Fixtures are compiled by the pinned `hermesc` into the *build*
  tree, not committed to `runtime/tests/fixtures/`, which holds only the `.js` sources.)
- `ctest --test-dir runtime/build/macos --output-on-failure` -- expected: all I/O-matrix cases pass.
- `cmake -S runtime -B runtime/build/macos-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DSCREENKIT_SANITIZE=address,undefined && ctest --test-dir runtime/build/macos-asan` --
  expected: clean, in particular `teardown-race`.
- `runtime/scripts/build-tvos-simulator.sh && runtime/scripts/run-tvos-simulator.sh` -- expected:
  the bundle's `console.log` reaches os_log; read it with
  `xcrun simctl spawn booted log show --predicate 'subsystem == "dev.screenkit"' --last 2m`.
