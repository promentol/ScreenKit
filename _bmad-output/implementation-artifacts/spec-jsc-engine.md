---
title: 'JavaScriptCore as a second JS engine, behind the same JSI'
type: 'feature'
created: '2026-09-18'
status: 'draft'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/runtime/README.md', '{project-root}/tools/prebuilts/README.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** The runtime is welded to Hermes. `Runtime::create` builds a `HermesRuntime`, the package format carries Hermes bytecode and gates on `hermesBytecodeVersion`, and the prelude is compiled by `hermesc`. There is no way to run ScreenKit on another engine — to get JIT where the OS allows it, to get real `Intl` on Apple platforms (Hermes has none there), or to check that the runtime really only depends on JSI.

**Approach:** Add JavaScriptCore as a build-time alternative. Everything above JSI stays as it is: the same `Runtime`, `JsExecutor`, event loop, bindings, object model and DOM shim. Hermes remains the default; `-DSCREENKIT_JS_ENGINE=javascriptcore` selects JSC.

## Boundaries & Constraints

**Always:**
- **One JSI.** Both engines compile against the `jsi/` headers the Hermes prebuilt already ships (byte-identical to React Native 0.88's), and a check fails the build if the two ever diverge. JSC gets `jsi.cpp` and `JSCRuntime.cpp` compiled from pinned, vendored sources; Hermes keeps exporting its own JSI.
- **An engine seam, not a fork.** `HermesHost`'s engine-specific parts (`HermesHost.cpp:120-144`, the `HermesRuntime` member) move behind an interface the two engines implement. The thread, work queue, event loop, pause/resume, executor and every binding stay shared and unchanged.
- **JSI features the JSC backend must carry:** host functions, `PropNameID`, `NativeState`, `HostObject`, `ArrayBuffer`/`MutableBuffer` zero-copy, `prepareJavaScript`/`evaluatePreparedJavaScript`, `JSError`, `getPropertyNames`, and `queueMicrotask`/`drainMicrotasks`.
- **Microtasks:** JSC runs Promise jobs on its own internal queue and drains them when the JS stack empties; `drainMicrotasks` drains only what the host queued. The event loop's contract — a checkpoint after every task, before the next — is kept, and a test pins the observable ordering (a promise resolved by a native completion settles before the next task runs) on both engines.
- **Packages:** a JSC build runs the same `.skpkg`, subject to the decision on Open Question 2. Whatever a runtime cannot run, it refuses at the gate with a message naming the engine and what the package carries — never by crashing inside the engine.
- **Verification:** the macOS suite passes on both engines. Rows that are about Hermes bytecode (`valid-bytecode`, `version-mismatch`, `corrupt-bytecode`, `truncated-header`, `bytecode-version-pin`, `hermes-builtins`) are skipped, not failed, on a JSC build, and every other row passes unchanged.
- **Default:** Hermes. No host, package, CLI default or existing verification changes behaviour unless the engine is selected explicitly.

**Never:**
- Building JavaScriptCore itself from source (Apple's system framework and the published Android AAR only).
- A second DOM shim, or engine-specific branches in bindings above the JSI seam.
- Dropping Hermes bytecode: precompiled packages stay the Hermes path's advantage.
- Edits to vendored sources by hand: they come from a pinned tarball through a vendor script, as `third_party/gl` does.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Same suite, other engine | macOS build with `SCREENKIT_JS_ENGINE=javascriptcore` | every non-bytecode row passes; the bytecode rows skip (77) | a failure names the row and the engine |
| Source package | a package whose entry is `.js` | runs on both engines | N/A |
| Hermes bytecode on JSC | a package carrying `app.hbc` | the gate refuses it, naming the engine and the entry | no crash inside the engine |
| Intl | `new Intl.NumberFormat('de-DE').format(1234.5)` on macOS | JSC gives `1.234,5`; Hermes on Apple has no Intl and the shim's fallback answers | N/A |
| Microtask order | a native completion resolves a promise, then a task is queued | the promise's `then` runs before the next task on both engines | N/A |
| Engine identity | `screenkit.engine` (or the host's startup log) | names the engine and its version | N/A |
| Unhandled rejection | a promise rejects with no handler | Hermes reports it through `HermesInternal`; JSC has no such hook, so nothing is reported and the difference is documented | N/A |

</frozen-after-approval>

## Open Questions

1. **Which platforms does this spec cover?** JSC is a system framework on macOS and tvOS (no prebuilt to add, no JIT on tvOS for third-party apps), an AAR on Android (`io.github.react-native-community:jsc-android:2026004.0.1`, BSD-2-Clause, 26.9 MB, ~10 MB per ABI, Intl and ICU inside), and on Linux only the distro's WebKitGTK JSC (version varies per distro).
   - **A. macOS only.** The smallest proof: the seam, the vendored sources, both engines' suites on one platform.
   - **B. macOS and tvOS.** Adds the Apple TV app, where JSC brings real `Intl` and loses precompiled startup.
   - **C. macOS, tvOS and Android.** Adds the AAR, prefab wiring and about 20 MB to the APK.
   - **D. All four,** with Linux on `libjavascriptcoregtk-4.1` as a tier-2, distro-dependent path.
2. **What does a `.skpkg` carry when the engine may differ?** Today it is `app.hbc` plus `manifest.hermesBytecodeVersion`, and JSC has no portable bytecode.
   - **A. Engine-tagged packages.** The manifest names the engine; `screenkit bundle --engine javascriptcore` writes `app.js` instead. Smallest packages; a package runs on one engine, and the other refuses it at the gate.
   - **B. Universal packages.** Every package carries both `app.hbc` and `app.js`, and each runtime picks what it can run. Any package runs anywhere ("the same `.skpkg` on every target", Architecture.md), at roughly double the entry size.
   - **C. Source-only when JSC is in play,** i.e. a package built for JSC carries source and Hermes runs that source too (slower start, no bytecode gate).

## Code Map

**The seam (all of Hermes' engine coupling)**
- `runtime/core/src/hermes/HermesHost.cpp:120-123` (`gcheapsize_t` clamp, `GCConfig::Builder().withMaxHeapSize()`), `:139-143` (`RuntimeConfig::Builder().withGCConfig().withMicrotaskQueue(true).withES6BlockScoping(true)`), `:144` `makeHermesRuntime`, `:158` error text. `HermesHost.h:10,146`: the `HermesRuntime` member — used only as `jsi::Runtime&`, nothing calls a Hermes-only method on it.
- `runtime/core/src/hermes/RuntimeImpl.cpp:191-198`: `Runtime::create` and `hermesBytecodeVersion()` — the only factory; every host and test goes through it (`host/Host.cpp:511-515,645-648`, `apple/HostMain.mm:201-204`, `android/jni/HostMain.cpp:393-396`, `tests/RuntimeTests.cpp:51-53,79`).
- Generic and unchanged: the SDL thread and handshake (`HermesHost.cpp:31-104,110-254`), pause/resume/tick/idle (`:255-371`), `HermesExecutor` (`:373-399`).
- `runtime/core/include/screenkit/Runtime.h:59-79` `RuntimeConfig` (no engine field; `maxHeapBytes` is documented as Hermes' GC ceiling), `:103` `create`, `:183` `hermesBytecodeVersion`.

**Bytecode and the package format**
- `runtime/core/src/hermes/BytecodeLoader.cpp:24` HBC magic, `:35-38` version, `:42-61,82-116` `mmap`, `:118-169` validation through `IHermesRootAPI`, `:80` `supportedBytecodeVersion`, `:124-132` the source fallback that already exists (`source-fallback`, `RuntimeTests.cpp:198-215`).
- Consumers: the gate `host/Host.cpp:258,316,391-397,445,535,686`; hosts' `--bytecode-version` (`apple/HostMain.mm:236,318-319`, `linux/HostMain.cpp:67,70-71`, `android/jni/HostMain.cpp:439`).
- Producers: `packages/@screenkit/cli/src/hermesc.js:22,25,31-46,86-98`, `cli/src/bundle.js:104,123,134-156` (writes `hermesBytecodeVersion`), `cli/src/graph.js:38-45` (uses `hermesc -dump-ast` as the parser — a JSC-only toolchain would need another), `vite-plugin/src/index.js:58-59` with `hermes-builtins.json`, `runtime/CMakeLists.txt:302-323` (`dom-shim.hbc`), `apple/CMakeLists.txt:80-91` (`hello.hbc`), `tools/prebuilts/fetch.mjs:663-676,734-737` (`--hermesc`).

**JSI surface the backend must satisfy** (all `jsi::`, no Hermes types)
- Host functions ×21 (`bindings/Timers.cpp:108-118`), `PropNameID::forAscii` ×29, `NativeState` (`jsi/JSIUtils.h:69-73`, `jsi/EventEmitter.cpp:29-47`, `jsi/SharedObject.cpp:19-32,180-201`), `HostObject` (`jsi/LazyObject.h:25`, `gfx/VendoredWebGL.cpp:79`), `MutableBuffer`/`ArrayBuffer` (`bindings/HostIO.cpp:77,154,200-209`, `Text.cpp:23,200`, `Net.cpp:30-41`, `WebGL.cpp:151-167`), `prepareJavaScript` (`RuntimeImpl.cpp:110-111,119-120`), microtasks (`loop/EventLoop.cpp:140,169`, contract at `EventLoop.h:88-108`).
- Not used anywhere in core: `WeakObject`, `Symbol`, `BigInt`, `instrumentation()` (tests only, `RuntimeTests.cpp:6065`).
- `bindings/Console.cpp:114-134` reads `HermesInternal.enablePromiseRejectionTracker` and returns silently when absent (`Console.h:25-26`) — on JSC that is simply off.

**Vendoring precedent**
- `tools/vendor/expo-gl.sh`, `tools/vendor/expo-gl.rules`, `tools/vendor/verify.sh`, `runtime/third_party/gl/VENDOR.md`, and `runtime/CMakeLists.txt:232-268` (globbed sources, warnings silenced by name, never hand-edited).

**Pinned upstream artifacts** (fetched and verified during planning)
- `@react-native-community/javascriptcore@0.2.0` (MIT, tarball 18,975 B, sha256 `6f7b908353661b664fdc448c417d05fe536aefac2b4f49b41de82823c8bc0a9d`): `common/JSCRuntime.{cpp,h}`, 1,603 lines, the RN-core file without RN-internal coupling, using only `<JavaScriptCore/JavaScript.h>`.
- `react-native@0.88.0-rc.1` (MIT, tarball 4,173,916 B, sha256 `e9f4757eb36bd8600d71e8b93eb78d449fe06ab76c6b43ac96679796e74d6fec`): `ReactCommon/jsi/jsi/jsi.cpp` (1,111 lines) and the headers that proved byte-identical to the Hermes destroot's.
- Android, if in scope: `io.github.react-native-community:jsc-android:2026004.0.1` (BSD-2-Clause, 28,172,306 B, sha256 `349eea406904de65f68b03c3141592171415c9d11eb78ec6a9d8fd04e81daf82`), prefab module `jsc`, minSdk 24, Intl and ICU included, `WeakRef` present but no `FinalizationRegistry`.

**Tests and docs**
- Hermes-specific rows: `RuntimeTests.cpp:183,198,217-228,237-258,275-308,296-308,315-331,580-587,3537-3565,6065`; registry `:6288,6294,6302,6368`; `tests/CMakeLists.txt:3-8,18,31-73,85-92,115,121,130,212`.
- Docs: `Architecture.md` §8 L465-499 (plus L38,50,71,95-96,142,269,289-315,332-341,543,637-644,736-743), `runtime/README.md` L134-150 ("Validate before evaluating") and L152-162 ("Hermes is prebuilt, never compiled"), `runtime/js/README.md:27`, `tools/prebuilts/README.md` L114-190.

## Tasks & Acceptance

**Execution:**
- [ ] `runtime/core/src/engine/` (new): a `JsEngine` seam — create the `jsi::Runtime` for a `RuntimeConfig`, name and version the engine, and answer what a package must carry. `HermesHost` keeps the thread and loop and asks the engine for its runtime; the Hermes engine holds today's `makeHermesRuntime` configuration and bytecode validation.
- [ ] `runtime/core/src/engine/JscEngine.cpp` (new): the JSC engine over the vendored `JSCRuntime`, including its microtask behaviour and an engine name and version for the startup log.
- [ ] `tools/vendor/jsc-runtime.sh`, rules and `runtime/third_party/jsc/VENDOR.md` (new), `tools/prebuilts/manifest.json`: vendor `JSCRuntime.{cpp,h}` and `jsi.cpp` from the pinned tarballs above; add a check that the vendored `jsi/` headers still match the Hermes destroot's.
- [ ] `runtime/CMakeLists.txt`, `runtime/cmake/JscEngine.cmake` (new): `SCREENKIT_JS_ENGINE` (`hermes` default | `javascriptcore`), which selects the engine sources, the JSI provider (Hermes' export vs the vendored `jsi.cpp`) and JSC itself (Apple's framework; the AAR's prefab on Android if in scope).
- [ ] `runtime/core/src/hermes/BytecodeLoader.{h,cpp}`, `host/Host.cpp`, hosts' `--bytecode-version`, `core/include/screenkit/Runtime.h`: make the gate engine-neutral — `Runtime::hermesBytecodeVersion()` becomes the engine's own answer, and the package check follows the decision on Open Question 2.
- [ ] `packages/@screenkit/cli`, `packages/@screenkit/vite-plugin`: bundle for the chosen engine per that decision, including what the manifest records.
- [ ] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`: skip the bytecode rows on a JSC build; add rows for engine identity, microtask ordering and `Intl` presence per engine.
- [ ] Docs: `Architecture.md` §8, `runtime/README.md`, `tools/prebuilts/README.md`, `deferred-work.md` (what JSC does not bring: no precompiled bundle, no `FinalizationRegistry` on the Android AAR, no unhandled-rejection hook, Symbol limits in `JSCRuntime`).

**Acceptance Criteria:**
- Given the default macOS build, when the suite runs, then it passes exactly as today (160/160) with Hermes.
- Given a macOS build with `-DSCREENKIT_JS_ENGINE=javascriptcore`, when the suite runs, then every non-bytecode row passes and the bytecode rows report skipped.
- Given a POC package (`poc/pixi-hello`), when it runs on the JSC build, then it renders the same scene as on Hermes.

## Implementation Notes

## Spec Change Log

## Review Triage Log

## Verification

**Commands:**
- `(cd runtime && cmake --preset macos && cmake --build build/macos && ctest --test-dir build/macos)`: 160/160 on Hermes.
- `(cd runtime && cmake --preset macos-jsc && cmake --build build/macos-jsc && ctest --test-dir build/macos-jsc)`: non-bytecode rows pass, bytecode rows skipped.
- `runtime/build/macos-jsc/screenkit-host --window poc/pixi-hello/app.skpkg`: the scene renders.
- `sh tools/vendor/verify.sh`: the vendored sources match the pinned tarballs.
