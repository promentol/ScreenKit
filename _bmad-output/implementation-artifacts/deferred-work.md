# Deferred work

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: JSI object model — SharedObject, SharedRef, EventEmitter, NativeModule, LazyObject, JSIUtils.
  resolved: 2026-09-17 -- tested, mutation-checked. object-model-gc: a native object JS drops is destroyed exactly once by the collector, one JS still holds is not, a released-then-collected one is not destroyed again, and teardown destroys a survivor once (Hades finalizes concurrently, so the row collects until the registry reaches the expected count rather than trusting one collectGarbage). object-model-release: release() destroys at once and every later use is a JS error naming the cause; SharedRef names and releases its reference. object-model-listener-isolation: three listeners, the middle throws, the others run, emit does not throw, one error line per throw. object-model-lazy-module: a registered module is not built until touched, and built once. Breaking ~SharedObjectState's release, or rethrowing in callIsolated, fails the rows. Also under leaks(1): leaks-object-model-gc.
  evidence: Split from the runtime spec at the 1600-token scope gate. Independently shippable — the
    object model is only needed once native modules exist, and it merges on top of a working runtime
    without changing it. Design already researched: NativeState + id registry rather than HostObject;
    HostObject reserved for LazyObject alone (mirrors expo-modules-core/common/cpp).

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Event loop — timers, microtask checkpoint, rAF hook, and the freeze/thaw primitive behind
    the Architecture §5.1 `Paused` state.
  resolved: 2026-09-17 -- shipped in M3 (timers-ordering, timers-cancel, timers-freeze). Two real bugs found while closing the rest of this list: setInterval(fn, 0) fired once, because SDL reads a timer callback's 0 as stop -- repeats now use HTML's 4 ms nesting clamp (timers-cancel asserts five ticks); and HermesHost::idle() read busy/queue under the host lock and the loop's timers after it, so a timer claimed in between read idle mid-callback (timers-cancel failed 1 run in 20) -- the loop is read first now, 60/60 clean.
  evidence: Split from the runtime spec at the 1600-token scope gate. Independently shippable on top
    of the Hermes host. Carries the one piece of genuinely original design in this area: React Native
    has no freeze equivalent (BufferedRuntimeExecutor is a one-shot startup gate; iOS RCTTiming
    throttles backgrounded timers onto a coalesced NSTimer rather than stopping them), so the gate in
    task selection plus handle-stable timer re-arming must be designed, not copied.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Hermes runtime for embedded Linux — build from the pinned commit and publish through
    tools/prebuilts, with a CI bytecode-parity assertion.
  evidence: Deferred by decision at planning. React Native publishes no Linux Hermes runtime
    (verified: Maven has only hermes-android and hermes-ios; facebook/hermes releases stop at v0.13.0
    and ship executables only). Linux/Batocera is M11, and nothing in the current spec depends on it.
    This is the one target that will break the "never build Hermes" rule.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Pin ANGLE's headers to a ref instead of cloning google/angle default-branch HEAD.
  resolved: 2026-09-16 -- manifest angle.headers.ref pins aaebda1c5a40c15340d7a01a935a7c5398443272, the head of chromium/7578 and the commit the libraries report in GL_VERSION. The drift was real: the cached checkout was 22 headers away, including gl3.h, gl2ext.h and eglext_angle.h. fetch.mjs fetches by commit and reuses the cache only when HEAD matches and include/ is clean; CMake refetches on a stale stamp; gl-context asserts headers and libraries share a commit.
  evidence: `fetchAngleHeaders()` runs `git clone --depth 1 --sparse` with no ref, so headers float
    while the .a files are pinned to chromium/7578 — contradicting the "checksum-verified against the
    manifest" claim in tools/prebuilts/README.md. Real, but pre-existing: written before this spec, so
    not caused by this change. Fix: add `angle.headers.ref` to the manifest and check it out.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Assert console output is retrievable from os_log, not just from the stderr mirror.
  resolved: 2026-09-16 -- console-os-log (runtime/tests/OsLogTests.mm) installs the real sink, logs through a runtime and reads its own entries back with OSLogStore's current-process scope -- no entitlement, no `log show`, ~2s. Checks the exact text, the category and the level. Dropping %{public} fails it with `[log] <private>`; typing error as default fails it too.
  evidence: PlatformLog.mm writes each line to both os_log and stderr; `host-runs-hello` matches the
    stderr copy, so dropping `%{public}` from the os_log format leaves the suite green while
    `log show` prints `<private>`. That makes the "appears in the platform log" acceptance criterion
    verifiable only by hand. Closing it means scraping `log show` or linking OSLogStore inside ctest —
    slow and timing-sensitive; wants CI first.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Add CI and a root CMakePresets.json.
  resolved: 2026-09-17 -- presets done, CI not. runtime/CMakePresets.json (beside the project it configures) holds macos, macos-asan and tvos-simulator configure/build presets, test presets and macos/macos-asan workflow presets; build-macos.sh and build-tvos-simulator.sh use them instead of re-spelling flags, runtime/README.md documents `cmake --workflow --preset macos`. CI is re-filed below: there is no VCS remote to run it against.
  evidence: No .github/ exists, yet tools/prebuilts/manifest.json and README claim CI asserts
    bytecode-version parity, and the documented ASan configuration is never run. Architecture.md §11
    says the Ninja rule and per-platform build trees "carry into CMakePresets.json"; instead the build
    scripts re-spell the flags inline and no presets file exists. Outside this spec's intent.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Make the prebuilts cache safe against concurrent fetches and a damaged tree.
  resolved: 2026-09-17 -- real on both halves. Deleting hermesc with its stamp intact made every run say "cached" and then fail "missing after unpack" without repairing. Concurrent fetches all downloaded and unpacked into the same directory through the same temporary archive path (8 simultaneous runs did not fail, but nothing prevented it). fetch.mjs now trusts a stamp only while the files a build reads exist (per entry: the framework binary and headers, hermesc, the three ANGLE archives), fetches each entry under <entry>.lock (mkdir; a lock whose pid is gone is taken over) into a staging directory renamed into place only when complete, and cleans staging left by a crash. Verified against a private SCREENKIT_PREBUILTS: a damaged entry is refetched; four concurrent fetches download once, three wait and report cached; a lock with a dead pid is taken over; the real cache still reports every macOS and tvOS-simulator entry cached.
  evidence: Unverified (maybe-false). `fetchArchive` rmSync+unpacks without a lock, so a CMake
    auto-fetch racing a build script could delete a tree the other is unpacking; and `cached()` trusts
    the `.verified` stamp without probing that the framework still exists. Settled by running two
    fetches simultaneously and by deleting a slice while leaving the stamp.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Confirm whether swapping the log sink can race a sink still executing on the JS thread.
  resolved: 2026-09-17 -- real. log-sink-swap swaps sinks ~40k times while the JS thread logs flat out, freeing each old sink's object as the swap returns; under ASan the old Log.cpp hit heap-use-after-free in 4 of 6 runs (a call still inside the old sink). setLogSink now waits for calls into the sink it replaced -- only those, so a busy logger cannot starve it -- and Log.h says it must not be called from inside a sink. 10/10 clean under ASan. Building the row also exposed the setInterval(0) bug above.
  evidence: Unverified (maybe-false). setLogSink replaces a std::function that another thread may be
    inside; ~LogCapture restores it without joining the JS thread. Not reproduced under ASan across
    the suite. Settled by a stress test that swaps sinks while the JS thread logs continuously.

- source_spec: `_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md`
  summary: Run the bundle off the tvOS main thread so a slow bundle cannot trip the launch watchdog.
  resolved: 2026-09-17 -- SDL_AppInit starts evaluateBundle on a helper thread and returns; SDL_AppIterate joins it when it signals done and turns a failed evaluation into SDL_APP_FAILURE; SDL_AppQuit shuts the runtime down (which cancels an evaluation in flight) before joining. run-tvos-simulator.sh package mode passes with the Blits example app (ready line, 60 fps).
  evidence: HostMain.mm calls runBundle synchronously inside didFinishLaunchingWithOptions, blocking
    before the first frame. Real on device, but tvOS hardware is already unproven and out of scope
    here. Fix: dispatch_async onto the main queue so launch returns first.

- source_spec: `_bmad-output/implementation-artifacts/spec-m4-gles-triangle.md`
  summary: Verify the "no leak under ASan" acceptance criterion on a platform that actually has
    LeakSanitizer.
  resolved: 2026-09-17 -- leaks(1) rows in the plain macOS build (Apple's ASan has no LeakSanitizer and leaks cannot read an ASan process): leaks-gl-teardown, leaks-teardown-race, leaks-object-model-gc, leaks-dom-texture-chain, leaks-net-shutdown-idle, leaks-net-websocket, via runtime/tests/leaks-row.sh with MallocStackLogging. Allowed, by allocation site and nothing else: ANGLE's per-thread EGL bookkeeping (egl::Thread, egl::Display::InitTLS, 96 bytes per GL thread; re-filed below). A deliberate leak in a row fails it. The first report also showed ~GlSurface running on Hermes' background GC thread (hermes::SerialExecutor): the GL context was released by a HostObject destructor after the heap went away, i.e. torn down off the thread it was current on. Host teardown now releases each runtime's contexts on the JS thread (releaseVendoredWebGL), ~GlSurface warns when destroyed off its creating thread, and gl-teardown fails on that warning (it does with the release removed).
  evidence: `-DSCREENKIT_SANITIZE=address,undefined` passes 27/27 on macOS, but LeakSanitizer is not
    available in Apple's ASan runtime (`detect_leaks` is a no-op on Darwin), so that run proves the
    absence of use-after-free and undefined behaviour, not the absence of leaks. The `gl-teardown`
    row deliberately leaves buffers, a VAO and a pending rAF callback live at shutdown and would be
    the row to watch. Closing it means running the suite on Linux ASan — which needs the M11 Hermes
    build — or wiring a Darwin-native leak check (`leaks --atExit`) into ctest.

- source_spec: `_bmad-output/implementation-artifacts/spec-m4-gles-triangle.md`
  summary: Decide what a window resize does to the EGL window surface.
  resolved: 2026-09-16 -- no surface recreation needed -- measured. ANGLE's WindowSurfaceMtl follows its CAMetalLayer, lazily: it compares bounds x contentsScale with the drawable size when it takes the next drawable. gl-drawing-buffer-resize resizes a real window and reads back a clear from a corner outside the old size.
  evidence: Unverified (maybe-false). `GlSurface::width()/height()` query EGL live rather than
    caching, and `triangle.js` sets the viewport from them every frame, so a resize *should* follow
    the CAMetalLayer. Nothing tests it: tvOS never resizes and the macOS `--window` path was only
    run at its initial size. If ANGLE's Metal window surface does not track `drawableSize`, the fix
    is to recreate the surface on `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`. Settled by dragging the
    macOS window and watching for a stretched or clipped triangle.

- source_spec: `_bmad-output/implementation-artifacts/spec-m4-gles-triangle.md`
  summary: Second `GlSurface` in one process shares nothing and terminates the display out from
    under any sibling.
  resolved: 2026-09-17 -- the display refcount already existed (retainDisplay/releaseDisplay; the last surface terminates). Added GlSurface::Desc::shareWith -> eglCreateContext(share): gl-shared-context makes a texture on one runtime's JS thread, reads it back through a framebuffer on a second runtime's thread, destroys the first surface and reads it again; without shareWith the framebuffer is incomplete. presentFrame now flushes only its own runtime's contexts -- it flushed every runtime's, on the wrong thread.
  evidence: `~GlSurface` calls `eglTerminate`, which is right while the runtime owns exactly one
    display per process and is what lets `gl-teardown` prove the display is reusable. It becomes
    wrong at M9, where one iframe is one instance with its own `EGLContext` and the contexts must
    share textures and buffers (`Architecture.md` §2). That spec needs a process-wide display owner
    with refcounting plus `eglCreateContext(shareWith)`, not a per-surface `eglTerminate`.

- source_spec: `_bmad-output/implementation-artifacts/spec-expo-gl-vendor.md`
  summary: Vendored bufferData is laxer than the WebGL spec about non-typed-array data.
  resolved: 2026-09-16 -- the premise was half wrong. Measured in Chromium, bufferData never throws TypeError for bad data: WebIDL sends it to the size overload ([1,2,3] -> 0 bytes, '16' -> 16, true -> 1), and null/undefined raise INVALID_VALUE. bufferSubData is the one that throws TypeError. Both bodies are now `patch` rows (a new rules kind that must match exactly once) routed to seam functions, which also implement WebGL2 srcOffset/length -- upstream silently ignored them. gl-buffer-data pins 45 cases against Chromium's results.
  evidence: Verified by test `gl-typed-array`. Passing an array or a duck-typed object raises a plain
    Error rather than the TypeError WebGL requires, and passing a string or null is accepted silently
    with no throw at all. A bundle written against the web may branch on `e instanceof TypeError`, or
    may rely on bad input being rejected. The test now pins the shipped behaviour so an upstream
    change is visible. Fixing it means a validation layer in front of the vendored entry points --
    which is the WebGLRenderingContext gating work Architecture.md 9 already calls for.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — window / self / location / performance.
  resolved: 2026-09-16 -- window/self/location/performance shipped (dom-environment).
  evidence: Split from "build all those shims" at the single-goal gate. Static analysis of the
    Lightning bundle shows `window.devicePixelRatio`, `window.removeEventListener("resize")`,
    `self.location`, `location.hash`, `performance.now`. Small and self-contained; nothing else
    depends on it.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — image loading: Image / HTMLImageElement, ImageBitmap, createImageBitmap, ImageData.
  resolved: 2026-09-16 -- Image/HTMLImageElement/ImageBitmap/createImageBitmap/ImageData shipped; texture chain proven by pixel readback (dom-texture-chain, dom-image-element).
  evidence: Split at the single-goal gate. Needed for textures. `stb_image.h` already came across with
    the vendored expo-gl, so the decode path exists; this is the JS-facing half. Runtime trace shows
    ImageBitmap.width/height touched even by a hello-world.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — networking: fetch and XMLHttpRequest.
  resolved: 2026-09-16 -- fetch/XMLHttpRequest shipped over confined local asset reads; no network by design (dom-fetch, dom-texture-chain).
  evidence: Split at the single-goal gate. Static analysis found both; the runtime trace never fired
    them because a static hello-world loads nothing. A real app needs them for fonts and assets.
    XMLHttpRequest's proven surface is 10 members (open("GET"), responseType, onload, readyState…).

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — encoding/binary: Blob, URL.createObjectURL/revokeObjectURL, URLSearchParams, atob.
  resolved: 2026-09-16 -- Blob/URL/webkitURL/URLSearchParams/atob/btoa shipped (dom-encoding).
  evidence: Split at the single-goal gate. `URL.createObjectURL` is proven at runtime. Note the trap
    found while building the tracer: Web IDL validates internal slots, so a Blob shim must be a real
    object these APIs accept, not a stand-in.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — events and observers: Event, MutationObserver, ResizeObserver.
  resolved: 2026-09-16 -- Event/CustomEvent and the three observers shipped (dom-events).
  evidence: Split at the single-goal gate. ResizeObserver matters on a TV only when the surface
    changes; MutationObserver is constructed twice in the bundle. Low urgency, low risk.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — OffscreenCanvas and CanvasRenderingContext2D.
  resolved: 2026-09-16 -- RESOLVED BY DECISION: deliberately absent. Lightning feature-detects OffscreenCanvas and a null 2d context and takes its WebGL-only path; asserted by dom-identity-and-absence.
  evidence: Split at the single-goal gate. The runtime trace proves 13 CanvasRenderingContext2D
    members and 6 on OffscreenCanvas — drawImage, getImageData, putImageData, globalCompositeOperation.
    This is a second rasteriser, not a thin shim: Lightning uses 2D canvas for text atlases. Likely the
    largest item after Worker.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — Worker.
  resolved: 2026-09-16 -- RESOLVED BY DECISION: deliberately absent. Lightning uses Worker only for its imageWorkerManager and checks `!!self.Worker`; the main-thread loader path is used instead. Asserted by dom-identity-and-absence.
  evidence: Split at the single-goal gate. The runtime trace shows `Worker.onmessage` fired 12 times in
    six seconds, so Lightning genuinely uses a worker thread. We have no worker story at all: it needs a
    second Hermes runtime, a message channel and structured-clone-ish transfer. Biggest item in the set
    and the one most likely to need its own architecture decision.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: DOM shim — FontFace and document.fonts.
  resolved: 2026-09-16 -- FontFace and document.fonts shipped; text is MSDF from build-time atlases, so no glyph is rasterised here (dom-fonts).
  evidence: Split at the single-goal gate. Lightning probes `document.fonts` and constructs FontFace.
    Couples to the MSDF path in M6, so it may be better solved there than as a DOM shim.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: `gl.drawingBufferWidth`/`Height` are captured once and do not track a surface resize.
  resolved: 2026-09-16 -- they are getter-only accessors on both context prototypes, as in Chromium (dom-drawing-buffer-shape), and window.innerWidth/innerHeight are live [Replaceable] accessors. A live eglQuerySurface was not enough: ANGLE's Metal surface adopts a new layer size only at its next drawable, so a 320x200 window resized to 480x300 still reported 320x200 after a presented frame. GlSurface::width()/height() now report the layer's pixel size for window surfaces. gl-drawing-buffer-resize proves it with a real SDL window.
  evidence: The vendored expo-gl sets them at install time from the GL viewport
    (`runtime/third_party/gl/SKWebGLRenderer.cpp`), while `GlSurface::width()` is live via
    `eglQuerySurface`. The shim reads the vendored values on every access, so `canvas.width` and
    `gl.drawingBufferWidth` always agree with each other -- the spec's invariant holds -- but both go
    stale together after a resize. The fix belongs in the vendored seam, since reading GlSurface from
    JS would need the native binding Architecture 2 says to avoid.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: `gl.canvas` is not set, though Lightning reads it.
  resolved: 2026-09-17 -- stale: the shim has set gl.canvas to the backed canvas since M6 (dom-shim.js getContext). Now asserted in dom-gl-handoff: ctx.canvas === the canvas and its width matches.
  evidence: The runtime trace records `WebGLRenderingContext.canvas` being read. The shim deliberately
    does not set it, because this spec's Boundaries forbid changing the existing `gl` global and the
    enumerated scope omits it. One line when a spec asks for it; worth settling before M6.

- source_spec: `_bmad-output/implementation-artifacts/spec-dom-shim-canvas.md`
  summary: tvOS proves the prelude loads but not the canvas handoff end to end.
  resolved: 2026-09-17 -- stale: since M5's package mode the tvOS simulator runs the Blits example app, which reaches GL only through document.createElement('canvas').getContext, and run-tvos-simulator.sh waits for its ready line and frame times (re-run 2026-09-17 after this batch: pass, Portal screenshot rendered).
  evidence: The bundled tvOS app still runs `triangle.hbc`, which reaches `gl` directly. Switching the
    default to `dom-canvas.hbc` would break the M4 tvOS gate, since that fixture draws nothing and
    schedules no frames. Needs a selectable bundle path through SDL_MAIN_USE_CALLBACKS.

- source_spec: `_bmad-output/implementation-artifacts/deferred-work.md` (drawingBufferWidth fix)
  summary: A resize changes every size JS can read, but nothing dispatches `resize`.
  resolved: 2026-09-17 -- ViewportEvents (core/include/screenkit/Viewport.h) turns SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED into one coalesced JS task; the shim's __screenkitResize fires `resize` at window, as a stepper with a microtask checkpoint per listener, only when the readable size changed. Both hosts wire it in. gl-drawing-buffer-resize resizes a real window, feeds SDL's own events through it, and checks listener order, isTrusted, innerWidth and canvas.width at dispatch, and that an unchanged size fires nothing.
  evidence: The sizes are live now (gl-drawing-buffer-resize), yet the shim's window accepts
    listeners and never fires them, and the host ignores SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED.
    Lightning sizes its stage at launch, and the DOM trace shows it handling `resize`, so it will
    not adapt to a resized window until the event reaches it. Only the macOS dev window resizes
    today; a tvOS output-resolution change would be the device case. Needs a native -> JS signal.

- source_spec: `_bmad-output/implementation-artifacts/deferred-work.md` (drawingBufferWidth fix)
  summary: The first frame after a resize can render into a drawable taken before it.
  resolved: 2026-09-17 -- measured first: frame 1 after the resize read 0,0,0,0 in the new corner. The resize task now presents and lets go of a drawable taken before the resize (releaseStaleDrawables: flush, eglSwapBuffers, only when the layer is not the size of the last present; ANGLE's Metal surface presents nothing when it holds no drawable) before `resize` reaches JS, so the answering paint takes a drawable of the new size. gl-drawing-buffer-resize now checks frame 1, not frame 2.
  evidence: ANGLE adopts a new layer size only when it takes its next drawable, so a drawable
    already held when the window resizes is presented at the old size while JS reports the new
    one. One frame, and only if a drawable was held across the resize; gl-drawing-buffer-resize
    therefore checks the drawable on the second frame, not the first. Low impact on a TV, where
    the surface does not resize in normal use.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-vite-plugin-skpkg-pipeline.md`
  summary: A package whose entry module throws or fails to import still exits 0 headless and shows a blank screen on tvOS.
  resolved: 2026-09-17 -- the packed wrapper's catch logs "screenkit bundle: entry \"<entry>\" failed: <error>" and calls `__screenkit.reportFailure`, a new HostIO binding recorded per runtime (`Runtime::failure()`, thread-safe, first report wins); a resolved entry logs "screenkit bundle: entry \"<entry>\" ready". Headless and `--window` exit 65 at their next loop iteration once it is set, idle or not (and `--window` still exits 65 when a quit arrives in the same iteration); tvOS SDL_AppIterate returns SDL_APP_FAILURE, which SDL's UIKit main turns into process status 1. runtime/VERSION is 2, so an older runtime refuses a package that needs the binding. Rows: report-failure (binding), host-refuses-entry-throws, host-refuses-entry-rejects (an interval keeps the app busy; must exit within 5 s -- it takes ~0.25 s), host-window-refuses-entry-throws, host-window-refuses-entry-rejects, and cli-bundle's "Entry rejects" row on the real host. The fixture packages are packed by the CLI's own packScript. run-tvos-simulator.sh package mode waits for the ready line and checks the app exited on its own after a reported failure.
  evidence: The packed wrapper ends in `System.import(entry).catch(console.error)`, carried byte-for-byte from the pre-M5 packer; a scratch package with a throwing `execute` logged `entry failed` and exited 0, unlike `throws.hbc` (exit 65). Needs a host-level signal for an unhandled entry rejection so `screenkit-host app.skpkg` is a usable CI assertion.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-vite-plugin-skpkg-pipeline.md`
  summary: Legacy transpile/polyfill targets follow browserslist (`defaults, not IE 11`), not what the pinned Hermes lacks.
  resolved: 2026-09-17 -- measured, and real. The pinned Hermes lacks 24 ES2015-ES2023 built-ins (Annex B included): Array.prototype.toSorted, %TypedArray%.prototype.toReversed/toSorted/with, the 13 Annex B String HTML methods, RegExp.prototype.compile, Symbol.species, Symbol.unscopables, Promise.prototype[@@toStringTag], SharedArrayBuffer, Atomics, and Function.prototype.toString source text. None would have been polyfilled: every browser in the targets has them. @screenkit/vite-plugin now adds the 17 that core-js can fill through additionalLegacyPolyfills, from packages/@screenkit/vite-plugin/src/hermes-builtins.json, which records why the other 7 cannot be. The hermes-builtins row probes the engine as bytecode (runtime/tests/fixtures/hermes-builtins.js) and fails unless the missing set is exactly that list.
  evidence: Unverified (maybe-false). Settled by listing ES built-ins missing from Hermes 260318099.0.2 and checking whether core-js for those targets polyfills them; if any is missing, medium -- code passes hermesc and fails at runtime.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-vite-plugin-skpkg-pipeline.md`
  summary: The tvOS embedded-package path (SCREENKIT_APP_PKG, app.skpkg preferred over fixtures, gate in SDL_AppInit) has no automated check.
  resolved: 2026-09-17 -- run-tvos-simulator.sh has a package mode, chosen when the app embeds app.skpkg/manifest.json: it waits for the host's "<...>screenkit-host.app/app.skpkg: runtimeVersion" line (logged only after the gate passed, and only for the embedded package) and "frame time", fails at once on a reported failure or a gate refusal, and fails naming the package line if it never appears. Triangle mode is unchanged for an app without a package.
  evidence: runtime/scripts/run-tvos-simulator.sh requires triangle.hbc and waits for the triangle's log line, so it cannot run a package; M5 verified tvOS by simulator screenshots only. A package mode for the script would catch the lookup silently falling back to triangle.hbc.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-vite-plugin-skpkg-pipeline.md`
  summary: The Lightning build fixes silently skip when Blits or the renderer moves the file they match.
  resolved: 2026-09-17 -- each fix is built per screenkit() call and records, in its transform, whether its package (@lightningjs/blits/ or @lightningjs/renderer/) was in the build and whether its target module was transformed; buildEnd fails the build naming the fix and the expected path when the package was seen and the target was not. Inert for an app without Lightning, and silent when the build already failed. vite-plugin tests cover fire, not-fire, earlier-failure and non-Lightning for all three fixes.
  evidence: webglTextOnly and hermesBitExactQuadCopy match exact module ids; they fail loudly when the matched code changes, but a renamed or moved file is never matched and the app then crashes on getContext('2d') or draws wrong colours on device. Needs a check that a Lightning build saw the target module.

- source_spec: none (Blits example app fixes, 2026-09-16)
  summary: createImageBitmap(source, sx, sy, sw, sh, options) ignores the crop rectangle and returns the whole image.
  resolved: 2026-09-17 -- crops as Chromium does: long arguments, negative width/height extend left/up, outside the source is transparent black, zero width or height rejects with RangeError. A cropped bitmap is pixels decoded from any source (asset Blob, <img>, asset-backed ImageBitmap, ImageData, bytes). dom-image-bitmap-crop reads each case back from the GPU through a framebuffer off a 4x2 fixture where every pixel is its own colour.
  evidence: runtime/js/dom-shim.js reads the options from the right argument but builds the bitmap from the full file, because the vendored texImage2D decodes from `localUri`. Lightning calls the cropped form when a texture has sx/sy/sw/sh, so a sprite frame would upload as the whole sheet. Not yet hit: the example app's Sprites page is only reachable with input.

- source_spec: none (SDL remote input, 2026-09-17)
  summary: On tvOS, with any keyboard attached, the Siri Remote does nothing and Escape/Menu never arrives.
  resolved: 2026-09-17 -- implemented, not verified end to end. runtime/apple/RemotePresses.mm wraps SDL_uikitview's pressesBegan/Ended/Cancelled (SDL's run first, unchanged): while SDL_HasKeyboard(), a press whose UIPress.key is nil is pushed as the SDL key event SDL's own table gives it (Menu -> Escape -> back). Builds and runs on the simulator; the forwarded presses need a manual pass with a keyboard attached (automation cannot drive the remote or the simulator's keyboard without taking over the desktop) -- tracked with the Siri Remote verification entry below.
  evidence: SDL's SDL_uikitview.m forwards UIPresses only `if (!SDL_HasKeyboard())`, and a GCKeyboard (a Bluetooth keyboard on a device, or the simulator's Connect Hardware Keyboard) makes that true; in the simulator Escape then reached neither path (SCREENKIT_LOG_INPUT showed nothing). Delete/Backspace still goes back. Fixing it means handling UIPressTypeMenu outside SDL or patching SDL.

- source_spec: none (SDL remote input, 2026-09-17)
  summary: The Siri Remote's UIPress path and the Android/Linux input paths are unverified end to end.
  partly resolved: 2026-09-17 -- Android on the screenkit-tv emulator (Android TV 14, arm64): `adb shell input keyevent` DPAD_RIGHT/DPAD_DOWN move Blits' focus, DPAD_CENTER opens the page, BACK returns to the portal, and BACK at the root finishes the activity (spec-m10-android-host.md; screenshots in tools/android/out/). Still unverified: a physical Android TV or Fire TV remote, the Siri Remote, and a gamepad on the Pi.
  evidence: InputRouter maps SDL's scancodes/gamepad events and is unit-tested, but the simulator's Apple TV Remote window could not be driven from automation (no accessibility elements, no screen-capture permission), and Android and Linux have no host yet. Needs a manual pass with the remote window or a device, and a check when M10/M11 hosts land.

- source_spec: none (SDL remote input, 2026-09-17)
  summary: The tvOS Menu button at the app's root does not exit to the Home screen.
  partly resolved: 2026-09-18 -- the JS-visible way out exists, and it is the app's own call, not a guess about Back: `window.close()` calls a host-defined `__screenkitClose` when the host defines one, and the Android host finishes the activity on it (runtime/js/dom-shim.js, runtime/android/jni/HostMain.cpp; dom-window-close covers it). An earlier heuristic -- exit when no listener called preventDefault() and location did not change -- was rejected in review: it exits for an overlay closed without preventDefault(), for a page that takes Back on keyup, and for a gamepad's B, which maps to GoBack too. The tvOS host does not define the hook yet, and forwarding the press to UIKit so Menu leaves the app is still to do there.
  evidence: SDL never passes presses to super (unless text input is active), so Menu is always consumed as back. Apple's HIG and App Review expect Menu at the top level to leave the app; needs a JS-visible "back not handled" signal and a host that then forwards the press.

- source_spec: none (SDL remote input, 2026-09-17)
  summary: A key press queued just before a runtime pauses is delivered on resume.
  resolved: 2026-09-17 -- JsExecutor::pauseEpoch (EventLoop counts pauses); InputRouter records it when queuing and the task drops itself if a pause came since. input-event-loop holds the JS thread busy, queues a key behind it, pauses, releases and resumes: the key never arrives.
  evidence: InputRouter drops input while `Runtime::paused()` is true, but a task already queued sits behind the freeze gate and runs on resume. One task wide; closing it needs the pause to discard queued input tasks.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: Run M3-M6 on real Apple TV hardware.
  evidence: M5 and M6 close on the tvOS simulator by decision (2026-09-17): no Apple TV is attached. Every Apple milestone row still lists real hardware as remaining. Needs a device, a signing identity for a device build (the simulator app is ad-hoc signed), and run-tvos-simulator.sh's package-mode checks repeated against device logs (`log collect` or Console), plus the Siri Remote pass already deferred above.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: MutationObserver never fires, though the element tree now mutates.
  resolved: 2026-09-17 -- MutationObserver and MutationRecord per the DOM: records queued from insert/remove/move/replaceChild (one record) and attribute changes (style included), childList/attributes/subtree/attributeOldValue/attributeFilter, delivery from one microtask in observer creation order, a throwing callback reported without stopping the rest, transient registrations for removed nodes until delivery, observe() replacing options, takeRecords, disconnect, and the DOM's TypeErrors. characterData is accepted and never fires (no text nodes). dom-mutation-observer.
  evidence: runtime/js/dom-shim.js keeps the M2 observer that registers and never fires; before M6 that was truthful because nothing mutated a tree. Now appendChild, attributes and style do. No measured bundle depends on a record (the module-preload polyfill observes <head> for added <link>s it does not need here), so it is not built; a library that waits on a mutation record would wait forever. Closing it means queueing MutationRecords from the tree operations and delivering them as a microtask, as the DOM specifies.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: A package built now calls `__screenkit.reportFailure`, which an older runtime lacks, but runtime/VERSION was not bumped.
  resolved: 2026-09-17 -- bumped: runtime/VERSION is 2 (Architecture.md 1 ties a new native capability to a new runtime version). Packages `screenkit bundle` writes now carry runtimeVersion 2, which a runtime-1 host refuses at the manifest gate; runtime-1 packages still run on a runtime-2 host.
  evidence: The packed wrapper calls the binding unconditionally, and manifests still say runtimeVersion 1. On a runtime built before this change the rejected-entry path throws a TypeError inside its catch instead of exiting 65 -- the failure is still logged, but the host exits 0 as before. Every runtime in use today is built from this tree, so nothing breaks; the question is whether a new binding the packer depends on should bump runtime/VERSION (Architecture.md 1 ties native capability to runtime versions). A decision, not a fix.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: On macOS, `screenkit-host --window` closes the Blits example app a few seconds after launch, exit 0.
  resolved: 2026-09-17 -- a windowed host runs until closed. macOS --window no longer breaks on idle() -- it sleeps in SDL_WaitEventTimeout (16 ms) while idle instead of spinning -- and tvOS SDL_AppIterate no longer returns SUCCESS on idle (verified: the app still running at 60 fps 25 s after launch). host-window-runs-package now requires the host to still be running 3 s after its line (hello.skpkg schedules nothing) and to exit 0 on SIGTERM.
  evidence: Measured, and not caused by the M6 shim: with the pre-M6 dom-shim.hbc swapped in the example app exited 0 after 4, 5 and 16 s; with the new one after 2, 3 and 8 s. runWindowed breaks its loop as soon as `runtime->idle()`, and the Portal goes idle once nothing animates (the macOS loop runs uncapped, ~770 fps). The tvOS simulator ran the same package for minutes at 60 fps and navigated. A dev window that closes while an app waits for input is a bug for M7's dev loop; the windowed loop probably wants to keep running while the window is open, not exit on idle.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: No automated check proves the Hermes polyfills from hermes-builtins.json actually land in a real legacy polyfills bundle and fill the gaps on the engine.
  resolved: 2026-09-17 -- packages/@screenkit/cli/test/polyfills.test.js (in the cli-bundle ctest row): builds the hermes-builtins probe as an app with the real Vite, plugin-legacy and screenkit() from poc/blits-example-app, packs it with screenkit bundle, runs it on screenkit-host --window, and requires the missing set to equal exactly the entries hermes-builtins.json marks unpolyfillable. Pointing Array.prototype.toSorted at the wrong core-js module fails it.
  evidence: plugin.test.js checks only the options passed to a fake plugin-legacy, and the hermes-builtins row probes a bare engine by name. A wrong core-js module name in the JSON, or a plugin-legacy release that drops additionalLegacyPolyfills with renderModernChunks:false, passes both while the built-in stays missing on device. It was verified once by hand (lightning3-blits polyfills bundle re-probed on Hermes). Closing it needs Vite, plugin-legacy and core-js in a test path, for example a ctest row that runs hermes-builtins.hbc after a polyfills bundle built from HERMES_POLYFILLS.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: Event handler properties (`document.onkeydown`, `window.onresize`, `el.onclick`) are never invoked by dispatch.
  resolved: 2026-09-17 -- event handler IDL attributes per HTML: accessors on HTMLElement, Document, window and the network targets; a handler is a listener placed at first assignment, replaced in place, removed by null, run in target/bubble only, false cancels, non-objects are null; no ontouch* (touch detection). fireEvent is gone: XHR, WebSocket, EventSource, AbortSignal and <img> use the same mechanism. dom-event-handlers, including document.onkeydown from a host key and a bare `onhashchange = f`.
  evidence: Pre-existing: neither the pre-M6 eventTarget nor the new dispatcher in runtime/js/dom-shim.js reads `on<type>` properties, so a TV app that handles keys with `document.onkeydown = fn` receives nothing. Blits uses addEventListener, so no measured bundle is affected. Closing it means invoking the handler property as part of the target/bubble passes, as the HTML spec defines for event handler IDL attributes.

- source_spec: `_bmad-output/implementation-artifacts/spec-m5-m6-finish-pipeline-and-element-tree.md`
  summary: Windowed host rows skip on exit 70, which also covers a runtime or DOM-shim start failure, not only a missing window server.
  resolved: 2026-09-17 -- a failed SDL_Init(VIDEO) in runWindowed is now exit 71 (no window server, like a failed window or GL context); host-row.sh --window and host-window-row.sh skip only on 71, so a runtime or DOM-prelude start failure (70) fails the row.
  evidence: host-window-row.sh:51 (pre-existing) and now host-row.sh --window turn exit 70 (kRuntimeFailed) into a CTest skip. A regression in the windowed prelude path would show as skipped rows rather than failures; non-window dom-* rows still fail on a broken dom-shim.hbc. Tightening it means skipping only on 71, or on the specific window/GL failure message.

- source_spec: `_bmad-output/implementation-artifacts/spec-m7-module-runner-hmr-go-client.md`
  summary: screenkit-go finds dev servers by Bonjour and shows an on-screen picker driven by the remote (decided 2026-09-17).
  evidence: Split from the M7 spec at the token-count gate (~3,800 tokens). Decided design: `@screenkit/vite-plugin` advertises `_screenkit._tcp` (name, port, TXT with app name and Vite version) while `vite dev` listens; the client browses and resolves it through `dns_sd.h` on Apple, behind an interface for later Android/Linux backends; the picker is a Blits launcher app (`apps/screenkit-go/launcher/`) packaged with `screenkit bundle` and embedded in screenkit-go, listing servers via a dev-only `__screenkit.devServers` binding and starting a session with the same runtime restart the dev loop uses; Info.plist needs `NSBonjourServices` `_screenkit._tcp` and `NSLocalNetworkUsageDescription`. Until then the dev loop launches with the URL given directly (`--dev <url>` on macOS, `SCREENKIT_DEV_SERVER` on tvOS).

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: The M7 dev loop (module runner, cached restart with one module re-sent, `screenkit-go` launched by URL) resumes from its draft once runtime networking lands.
  evidence: Replanned at M7's CHECKPOINT 1 (2026-09-17): the human asked for SDL3_net to give the JS runtime full WebSocket, fetch and HTTP APIs, planned first as their own spec. The draft `spec-m7-module-runner-hmr-go-client.md` keeps its decisions (restart with one module re-sent, verification on the tvOS simulator over the Mac's LAN IP) and a change-log note on what to drop when it resumes.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: Android and Linux have no network backend: every request, WebSocket and EventSource fails with `unsupported` there.
  resolved: 2026-09-18 -- all three, in three passes on the same day. First (spec-android-network-backend.md) at the `Connection` seam; then (spec-platform-http-clients.md) the seam moved to `NetService` and the whole portable protocol layer was deleted, so Android's client is OkHttp over JNI (`NetServiceAndroid.cpp` + `dev/screenkit/net/HttpClient.java`) and Apple's is NSURLSession; then (spec-linux-http-client.md) **Linux got cpp-httplib** (`NetServiceLinux.cpp` over the vendored `runtime/third_party/httplib/`, one MIT header pinned by sha256) with TLS from the image's own OpenSSL and its own CA bundle, `LinuxCookieJar` for cookies because there is no platform store to point at, and real backpressure -- cpp-httplib is synchronous, so the reader callback simply not returning stops the socket being drained. **What Linux does not get is HTTP/2**, and its WebSocket has one divergence; both are filed separately below. The superseded description follows. `NetworkAndroid.cpp` implemented the `Connection` seam over `dev.screenkit.net.Transport` -- `java.net.Socket`, or `javax.net.ssl.SSLSocket` against the platform trust store with `setEndpointIdentificationAlgorithm("HTTPS")` for host-name verification -- and nothing above the seam changed. `RuntimeConfig::testTlsAnchors` is honoured only when non-empty, through a custom `X509TrustManager` that adds the anchors to the system store rather than replacing it. `JNI_OnLoad` in runtime/android/jni/HostMain.cpp captures the JavaVM, resolves the class where the app's loader is reachable and `RegisterNatives` its callbacks; without a VM `networkAvailable()` is false and every request fails `unsupported`, as before. `android.permission.INTERNET` is in the manifest.
  evidence: The HTTP/1.1, RFC 6455, cookie and binding code in runtime/core/src/net/ is portable C++ over the `Connection`/`IoQueue` seam (Connection.h); the one implementation is Network.framework (NetworkFramework.mm, Apple). Non-Apple builds compile NetworkUnavailable.cpp. Each needs the OS's own stack with its host -- Android (M10): a JNI-backed connection over the platform's sockets and TLS (javax.net.ssl / Conscrypt, the system trust store); Linux/Batocera (M11): the distribution's TLS (OpenSSL/GnuTLS as shipped by the image) and its CA bundle. No networking or TLS library is to be bundled on Apple; whether Linux may link a system one is that milestone's decision.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: Networking features absent by decision -- HTTP/2, proxies, WebSocket permessage-deflate, WritableStream/TransformStream (pipeTo/pipeThrough), byte streams and BYOB readers, service workers, document.cookie, synchronous network XHR.
  resolved: 2026-09-17 -- RESOLVED BY DECISION: absent and documented (runtime/js/README.md "Networking"); dom-identity-and-absence asserts the stream and cookie absences, WebSocket fails a handshake that negotiates any extension, sync XHR over the network throws NetworkError (dom-fetch). **Amended 2026-09-18 (spec-platform-http-clients.md): HTTP/2 and proxies are no longer absent and no longer ours -- they are whatever NSURLSession and OkHttp do. permessage-deflate is still never offered, but a server that answers with an unrequested extension is refused by OkHttp and ignored by NSURLSession, so that assertion is Android-only now.**
  evidence: The spec's Never list. Every connection is HTTP/1.1 through Network.framework; the runtime applies no proxy configuration of its own (whatever Network.framework does with a system-wide proxy for raw TCP was not measured). Revisit per feature when an app needs one -- HTTP/2 first if many parallel image requests to one origin prove slow.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: Network bodies have no backpressure, and there is no per-origin connection limit.
  reopened: 2026-09-18 on Apple only (spec-platform-http-clients.md). See the entry below: with the portable stack gone, `Connection::setReceiving` -- the thing that made this a guarantee -- went with it, and NSURLSession offers nothing that replaces it. The per-origin limit and the JS-side window survive on every platform, and the server-side stall survives on Android (OkHttp is pull-based) and on Linux (spec-linux-http-client.md: cpp-httplib is synchronous, so the reader callback blocking on the flow window is the socket not being drained -- `net-flow-control` asserts the same under-24-MiB stall there as on Android). **On Apple, and so on tvOS, an unread body can still finish downloading into memory: exactly the failure this entry was closed against.** The rest of the 2026-09-17 note stands. 2026-09-17 -- both. Connection::setReceiving (Network.framework: the next nw_connection_receive is not asked for); a request's flowWindow stops reading once that many delivered bytes are unacknowledged and resumes on __screenkit.net.acknowledge; fetch uses a 1 MiB window with a byte-size stream strategy and acknowledges from pull. ConnectionPool keeps six slots per origin with a FIFO queue; a request holds one from before it takes or opens a connection until its response ends, and an abort while waiting frees it. net-flow-control: a 64 MiB body nobody reads stalls the server under 24 MiB, then reads in full; ten concurrent requests peak at exactly six; eight aborted waiters leave the origin usable. Without the window the body finishes unread; with sixty slots the peak is ten. Found on the way, under ASan: sendNow/receiveNext took the shared connection state by reference, and a block captures a C++ reference as a reference -- a completion after the connection was destroyed read freed memory; both take it by value now.
  evidence: net::HttpTask hands every received chunk to JS as its own task whether or not the ReadableStream is read (the response stream is push-fed, highWaterMark 0), so an unread large download sits in memory; and a new connection opens whenever no idle one exists for the origin, where browsers cap at six. Neither shows in the example app (a handful of images). Closing the first means pausing nw_connection_receive while the stream's queue is above its high-water mark; the second, a per-origin wait queue in ConnectionPool.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: `redirect: 'manual'` resolves with the real 3xx response, not a browser's opaque status-0 one.
  resolved: 2026-09-17 -- changed to browser parity: a redirect status under redirect:'manual' resolves with type 'opaqueredirect', status 0, no headers, null body and the answering URL, and the request is abandoned there. net-redirect pins it; runtime/js/README.md says so.
  evidence: Decided in implementation: there is no origin for opacity to protect (the document is screenkit://), and a TV client that asks for manual redirects wants the Location -- Node's and Deno's fetch behave the same. net-redirect pins it (`manual 302 /data.json`). A spec that needs browser parity changes the JS in networkFetch, not native.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: The cookie jar has no public-suffix list, enforces no SameSite, and on tvOS every app the host runs shares one jar.
  resolved: 2026-09-18 -- by deletion (spec-platform-http-clients.md). There is no jar: cookies are the platform store's, `NSHTTPCookieStorage` on Apple and `android.webkit.CookieManager` on Android, and the public-suffix rule, the eviction policy and SameSite are each whatever that store does. Chromium's list comes free on Android; Apple's store has none. `net-cookie-public-suffix` is retired with it. What is still open moves with M9: a per-package store, which neither platform gives for free. The superseded note follows. 2026-09-17 -- partly addressed, still open. The public suffix list's default rule is applied: a Domain attribute with no dot (com, test, localhost) is refused unless it is the host itself, which stores the cookie host-only (RFC 6265 5.3 step 5; net-cookie-public-suffix). Still open, and all of it M9's: the full list for multi-label suffixes (co.uk, github.io), a per-package jar -- which needs M9's definition of what identifies a package or instance -- and SameSite, which stays stored-only: the document's screenkit:// origin is never same-site with an http one, so enforcing Chrome's Lax default would stop sending a TV app's own session cookies.
  evidence: CookieJar honours any Domain attribute the request host domain-matches (a server at a.example.co.uk could set a cookie for co.uk); SameSite is stored only, because there is no site to compare against; and HostMain.mm's storage directory is per host bundle id, not per embedded package. Fine for one app per host; M9's iframe instances and a launcher running third-party packages need a per-package directory and, if hostile content is in scope, a PSL.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: The ASan suite runs with detect_container_overflow=0.
  evidence: Measured: once runtime/core/src/net/Url.cpp instantiated vector<string>::push_back(const string&), every GL row failed under ASan with a container-overflow inside ANGLE's egl::DisplayExtensions::getStrings -- the uninstrumented prebuilt ANGLE's call resolved to our instrumented instantiation, which read elements ANGLE's inlined code wrote without annotations. That is ASan's documented false positive for mixed instrumentation, and it would recur whenever any of our files instantiates a template ANGLE uses. runtime/tests/CMakeLists.txt turns only that check off for sanitizer builds; heap-buffer-overflow, use-after-free and UBSan still halt. Closing it needs an ASan-instrumented ANGLE (our own ANGLE CI build, Architecture.md 10.2).

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: The DNS-failure check in net-unreachable resolves a reserved `.invalid` name through the system resolver.
  resolved: 2026-09-17 -- replaced rather than captured: the row now resolves a name with a 64-octet label, which cannot be encoded in a DNS message (RFC 1035 2.3.4) -- mDNSResponder refuses it as BadParam in ~5 ms and no query can be sent -- and it still classifies as dns.
  evidence: Unverified (maybe-false) that the query never leaves the machine. RFC 6761 reserves `.invalid` and says resolver libraries should answer NXDOMAIN locally; mDNSResponder answered in ~0.1 s here, but whether it forwarded the query upstream was not observed (no packet capture without root). Every other row is loopback-only. Settled by a packet capture during `ctest -R net-unreachable`; if it leaves the host, replace the check with a Network.framework resolver override or drop it to a unit test of the error classification.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: A downloaded image is decoded synchronously on the JS thread.
  resolved: 2026-09-17 -- __screenkit.net.decodeImage(bytes, callback) decodes with the same stb_image (now core/src/bindings/ImageDecode, shared with the synchronous binding; stb 2.30 keeps its failure reason thread-local) on a serial queue of its own, and delivers one image/error event through the binding's event channel, holding the loop busy meanwhile. <img> downloads, Blob-of-bytes createImageBitmap and crops use it. dom-image-decode-async: a 0 ms timer set after createImageBitmap on a 2048x2048 in-memory PNG runs before the bitmap arrives (with the synchronous path it runs after).
  evidence: `__screenkit.decodeImage` runs stb_image inside createImageBitmap / the <img> load, like the package path's decode at texImage2D. A large photo costs a frame. Moving the decode onto the I/O queue (decode when the body ends, hand JS RGBA) would remove it without changing the JS surface.

- source_spec: `_bmad-output/implementation-artifacts/spec-runtime-networking.md`
  summary: No test checks that each host run mode passes a storage directory to the runtime, so cookie persistence could silently break in one of them.
  reopened: 2026-09-18 (spec-platform-http-clients.md) -- both rows are retired. They could only observe the storage directory through the `cookies.txt` the jar wrote there, and the jar is gone: cookies live in the platform store and nothing in the network layer writes to that directory any more. `RuntimeConfig::storageDirectory` is still plumbed from every host and is now unverified by any row until something else uses it. Superseded note: 2026-09-17 -- for both macOS run modes: host-storage-directory and host-window-storage-directory run screenkit-host under a private home (CFFIXED_USER_HOME), store a persistent cookie in one launch, check the jar file under Application Support/dev.screenkit.host, and read the cookie back in a second launch.
  evidence: runtime/apple/HostMain.mm sets RuntimeConfig::storageDirectory in runBundle, runWindowed and tvOS SDL_AppInit; net-cookies passes its own directory through the test harness, and no host row involves the network or cookies. A host-level row needs the fixture server and an isolated HOME (macOS) or a simulator run that can reach the fixture (tvOS) -- a natural fit for the M7 dev-loop verification on the simulator.

- source_spec: `_bmad-output/implementation-artifacts/deferred-work.md` (CMakePresets and CI entry)
  summary: No CI runs the suites.
  evidence: The presets half of the original entry is done (runtime/CMakePresets.json; `cmake --workflow --preset macos` and `macos-asan` are the whole of what a job would run, plus `run-tvos-simulator.sh` after `build-tvos-simulator.sh`). The project has no VCS remote, so there is nothing for a workflow to run against; tools/prebuilts/README.md's bytecode-parity claim and the ASan and leaks rows stay manual until there is.

- source_spec: `_bmad-output/implementation-artifacts/deferred-work.md` (Darwin leak check)
  summary: ANGLE keeps 96 bytes of per-thread EGL state for every thread that ever called EGL, and never frees it on Apple.
  evidence: Measured with leaks(1): an egl::Thread (egl::GetCurrentThread) and a display TLS slot (egl::Display::InitTLS), 48 bytes each, per JS thread that created a GL surface, still allocated after the thread exits -- ANGLE's Apple build keeps them in thread-local storage with no destructor, and eglReleaseThread does not free them. leaks-row.sh allows exactly these allocation sites. Harmless for one runtime per process; at M9, where every iframe instance is a thread with a GL context, a launcher that opens and closes games leaks 96 bytes per launch. Closing it needs ANGLE (a TLS destructor) or instance threads that are pooled rather than exited.

- source_spec: none (PixiJS and Phaser hello worlds, 2026-09-17)
  summary: The software 2D context draws no paths, patterns or shadows, fills a gradient in its first colour, does not antialias shapes, and blends only the common composite modes.
  partly resolved: 2026-09-17 -- text is drawn. fillText/strokeText/measureText rasterise through SDL3_ttf (prebuilt, like SDL3) via __screenkit.text; fonts come from document.fonts (FontFace really loads url()/local()/bytes) or installed families through CoreText. Text is drawn upright at the transform's scale, maxWidth shrinks rather than condenses, and there is no letterSpacing. Verified with Pixi Text, Phaser Text and a Lightning web font (dom-canvas-text, frame captures). The vite-plugin's webglTextOnly and webFontsAsMsdf transforms and screenkit bundle's font-atlas refusal are gone. tvOS is build-verified only: which installed families the generic names resolve to there is not measured.
  evidence: Decided scope (the user chose "Software 2D subset"): enough for Phaser to boot and for engines that use a 2D canvas as a scratchpad beside WebGL (dom-canvas-2d). beginPath/arc/fill/stroke/clip, createLinearGradient/createPattern, fillText/strokeText and overlay/color-dodge/... exist, draw nothing (or blend as source-over) and warn once; measureText answers width 0. An app that renders UI with Canvas2D paths or text -- Phaser's Text, Phaser Graphics.generateTexture, Pixi's Text -- draws nothing there. Closing it is a path rasteriser (scanline with coverage) and, for text, a font rasteriser, which Architecture.md 7 keeps out on purpose.

- source_spec: none (PixiJS and Phaser hello worlds, 2026-09-17)
  summary: `gl.getSupportedExtensions()` lists the GLES/ANGLE extension names, not WebGL's.
  evidence: The vendored expo-gl passes GL_EXTENSIONS through: ANGLE_*, OES_*, EXT_*, CHROMIUM_lose_context -- where a browser lists WEBGL_lose_context, EXT_texture_filter_anisotropic, WEBGL_compressed_texture_* and so on, each with a JS extension object of constants. Pixi and Phaser booted regardless (their WebGL2 paths need no extension), but an app that feature-detects by WebGL extension name finds nothing or the wrong thing, and getExtension returns an empty object where a browser returns the extension's constants. Closing it is a mapping from WebGL extension names to the GLES ones that implement them, in the seam.

- source_spec: none (PixiJS and Phaser hello worlds, 2026-09-17)
  summary: PixiJS needs `Assets.setPreferences({ preferWorkers: false })`, because it creates a Web Worker without checking one exists.
  evidence: Workers are absent by design (runtime/js/README.md). Pixi's loadTextures calls `new Worker(...)` whenever preferWorkers is true (the default), so an unmodified Pixi app's first texture load throws ReferenceError; poc/pixi-hello sets the preference, which browsers accept too. A Worker-shaped stub would hang instead (nothing ever answers). Closing it for unmodified apps is either a vite-plugin build fix that defaults the preference, like the Lightning fixes, or real Workers.


- source_spec: `_bmad-output/implementation-artifacts/spec-web-gamepad-api-sdl3.md`
  summary: A gamepad-held key released by SDL_EVENT_GAMEPAD_REMOVED while the runtime is paused never reaches the page, so the key stays down after resume.
  evidence: InputRouter's REMOVED branch releases held_ through deliver(), which drops events while paused (and via pauseEpoch after queueing), then erases held_ anyway; pre-existing, found while reviewing the Gamepad API claim release, which had the same shape.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: The Android host has not run on a Fire TV (or any physical Android TV) device.
  evidence: Verified only on the screenkit-tv emulator (system-images;android-34;android-tv;arm64-v8a, emulator 37.1, host GPU through gfxstream: "Android Emulator OpenGL ES Translator (Apple M3 Pro)", ES 3.0). Unmeasured on hardware: the armeabi-v7a build at all (Fire TV sticks run a 32-bit userland; this Mac's emulator cannot run an arm32 image), cold start against Architecture.md's 300 ms budget, frame rates, the /system/fonts scan (SystemFontsAndroid opens every .ttf/.otf there with SDL_ttf on the first canvas text that names a system family), Fire OS's own fonts and GL drivers, and a real remote's key codes. Needs an adb-reachable Fire TV stick: `sh tools/android/android.sh install/push/run/shot` work over any adb device.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: The ES 2 fallback on Android (no ES 3 config, or an ES 3 context refused) has not run on a GLES 2-only device.
  evidence: HostMain.cpp's openWindowAndGraphics asks for ES 3 and retries the window and surface with ES 2; the WebGL1 path itself is the one verified on the Pi. The emulator cannot stand in: with `-feature -GLESDynamicVersion` (ES 2 only) an API 34 image does not boot -- SurfaceFlinger's Skia RenderEngine aborts in AutoBackendTexture::makeImage -- and an ES 2 request on an ES 3 device gets ES 3 back from EGL. Needs an ES 2-only device or an older emulator image that boots on ES 2.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: No ANGLE for Android, so no ANGLE→Vulkan / ANGLE→GLES rungs, no backend probe cache and no config override.
  evidence: Architecture.md 9's Android chain starts at ANGLE→Vulkan; no linkable ANGLE exists for Android (tools/prebuilts/README.md), so M10 draws on the device's GLES through SDL's EGL context. Driver normalisation on Fire OS is the case for building ANGLE for Android from source (android_arm64 / android_arm GN targets) and adding it as a prebuilt; Architecture.md 14 item 9 (force each rung) waits on it.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: No x86_64 (or x86) Android build.
  evidence: runtime/android/app/build.gradle's abiFilters are arm64-v8a and armeabi-v7a, the ABIs TV devices ship, and runtime/CMakeLists.txt maps only those to prebuilt targets. All three AARs carry x86 and x86_64, and react-android's libjsi.so exists for them, so adding android-x86_64 is manifest entries (hermes.android.jsi.extract, targets) and an abiFilter -- wanted for x86_64 emulators on Intel hosts and ChromeOS, not for TVs.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: Android's Intl depends on warming Hermes's Java classes on SDL's main thread.
  evidence: hermes-android implements Intl, localeCompare, normalize and toLocale*Case in Java (com.facebook.hermes.intl, through fbjni), and fbjni caches each class on first use. The JS thread is an SDL thread attached to the VM, whose FindClass sees only the system class loader, so a first use there throws. HostMain.cpp's warmIntl touches every entry point once in a throwaway runtime on SDL's Java-started main thread before the app's runtime exists; a probe on the emulator then got real results on the JS thread ('i'.toLocaleUpperCase('tr-TR') is 'İ', NumberFormat '1,234.5'). A Hermes release that adds a Java-backed entry point the warm-up does not touch would throw a RangeError from JS. The robust fix is fbjni's own `facebook::jni::ThreadScope::WithClassLoader`, which runs a block on a native thread with the app's class loader in place: the JS thread could be started inside one (or each Intl call routed through one), which needs a hook where the JS thread begins rather than anything from Hermes.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: reclaimRuntimeEvent can deadlock when SDL dispatches an app event while another thread is queueing runtime work.
  evidence: Found while moving Android's background handling out of SDL_AppEvent. SDL_main_callbacks.c dispatches app events (WILL/DID_ENTER_BACKGROUND/FOREGROUND, LOW_MEMORY, TERMINATING) from an event watcher, i.e. with SDL_event_watchers.lock held, and first drains the queue into SDL_AppEvent -- where reclaimRuntimeEvent takes WorkQueue::mutex_. A thread queueing work (the SDL timer thread firing a JS timer, a decode or network completion) holds HermesHost::mutex_ and WorkQueue::mutex_ across SDL_PushEvent, which needs the watcher lock. If both happen at once on the same queue, each waits on the other. The window is microseconds, so it has not been seen, but every backgrounding on Android and tvOS opens it. Closing it means WorkQueue::push not holding its lock across SDL_PushEvent (the close()/drain race it guards needs another answer), or reclaim not locking the queue.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: A task queued while the app was off screen could make a GL call before the rebind task runs.
  evidence: Returning queues `GlSurface::resume()` and then thaws the runtime, so the rebind is the first work the JS thread does among tasks queued by the gate -- timers, frames and input are all held by the pause. What is not proven is that nothing else is already queued ahead of it: a network or image-decode completion posted while paused, whose JS callback draws synchronously, would run with no drawable bound. Settling it means a test that queues such a completion during a suspend and asserts the order (a runtime-level test with a fake surface, since the emulator cannot be driven into that race reliably), or making resume() run ahead of queued work rather than behind it.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: The /system/fonts scan's cost on a device is unmeasured.
  evidence: SystemFontsAndroid opens every .ttf/.otf in /system/fonts with SDL_ttf on the first canvas text that names a system family -- about 100 files on the emulator, more on Fire OS with its own faces. It was never timed, on the emulator or anywhere: cold start is the budget it eats into (Architecture.md 2, 300 ms). Needs a measurement on a Fire TV stick, and, if it is slow, a cheaper first pass (parse the name table directly, or read /system/etc/fonts.xml, which maps families to files without opening any).

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: No release build: signing, shrinker keep rules, versionCode and lint are all debug-only today.
  evidence: The APK is `assembleDebug`, signed with the debug key, unshrunk, versionCode 1, and `lint { abortOnError = false }`. A release build needs a signing config, and R8 keep rules for what is reached by name rather than by call: org.libsdl.app (SDLActivity and its inner classes are named from the manifest and from JNI), com.facebook.hermes.intl (hermesvm finds those classes with FindClass), fbjni's ThreadScopeSupport and HybridData, and dev.screenkit.host.ScreenKitActivity's nativeLeaveScreen / nativeReturnToScreen (the JNI names). Also a versionCode scheme -- the extraction stamp is versionCode plus install time, which is fine either way -- and lint turned back on.

- source_spec: `_bmad-output/implementation-artifacts/spec-m10-android-host.md`
  summary: Three Android paths are checked by hand on the emulator and by nothing automated.
  evidence: (1) Background and return -- pause, drawable release, rebind, the state and last frame surviving; driven three times by hand with `adb shell input keyevent HOME`. (2) Bundle extraction and re-extraction -- the stamp, the staging rename, the sync, and a reinstall with a different bundled app; observed in logcat. (3) fetch.mjs's Hermes-Android cache stamp -- the `hasAll` check that decides a cached AAR set is complete, including libjsi.so from react-android; exercised only by repeated runs on this machine. The first two want an instrumented test on a device or emulator (androidTest, driving the lifecycle through ActivityScenario), the third a unit test over a temporary cache directory.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: A custom `X509TrustManager` on Android silently turns host-name verification off, whatever `setEndpointIdentificationAlgorithm` says.
  resolved: 2026-09-18 -- `Transport.AddedAnchors` extends `X509ExtendedTrustManager` and forwards the `Socket` and `SSLEngine` overloads.
  evidence: Found by `net-https` on the emulator, which resolved the wrong-host certificate with status 200 where Apple gives `TypeError tls`. Conscrypt performs the host-name check inside `X509ExtendedTrustManager.checkServerTrusted(chain, authType, socket)`; a manager that implements only the plain two-argument `X509TrustManager` is called through that form instead, and no host name is checked at all. The socket's `endpointIdentificationAlgorithm` is set and looks right in the code either way, which is what makes the mistake quiet -- the chain still verifies, so only a certificate for another name shows it. It applies only to the test-anchor path (production uses the platform's own manager, which is extended), but it is the classic Android TLS pitfall and worth a note for any later code that wraps a trust manager.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: `net-image` does not run on Android, and the device rows run inside the app rather than as a pushed binary.
  evidence: Both follow from the same two facts. `GlSurfaceSdl` has no offscreen path -- it needs an SDL window -- so a test binary has no drawable, and `net-image` is the one net row that uploads its download to a texture; it is left out of `NET_ROWS` in `tools/android/android.sh`, so the decode-and-upload path is covered on Apple only. And a binary run from `adb shell` has no JavaVM, so `FindClass` could never reach `dev.screenkit.net.Transport` and every row would skip for "no network backend" -- which the spec's matrix counts as a failure. The rows therefore load `libscreenkit-net-tests.so` inside the debug APK (`dev.screenkit.net.NetTests`, `app/src/debug/`), where the VM, the class loader, the INTERNET permission and the app's storage all exist. Closing either one means giving `GlSurface` an EGL pbuffer path on Android (which would also let `net-image` run), or starting an ART VM from a native binary with the Transport class dexed onto its classpath.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: Android's TCP keepalive is the OS default; the idle, interval and probe counts Apple sets have no Java equivalent.
  resolved: 2026-09-19 -- by deletion. `Transport` and Apple's `NetworkFramework.mm`, whose 30 s / 5 s / 4 probe timings this compared against, are both gone (spec-platform-http-clients.md): keepalive is now whatever NSURLSession and OkHttp do, as it is in a browser, and there is no hand-set socket left on either platform to give timings to.
  evidence: `NetworkFramework.mm` probes after 30 s of silence, every 5 s, four times, so a peer that vanishes without a FIN or RST cannot hold an open WebSocket or EventSource -- and `idle()` -- forever (deferred-work's BH3b). `java.net.Socket` exposes only `setKeepAlive(true)`, which `Transport.connect` sets; the timings are then whatever `net.ipv4.tcp_keepalive_*` says on the device, typically two hours. Matching Apple needs `setsockopt(TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT)` on the socket's file descriptor, which from Java means `android.system.Os` on the `FileDescriptor` behind the socket -- reachable for a plain socket, awkward for an `SSLSocket` wrapping one.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: Transport.close() blocks the runtime's single I/O queue while an SSLSocket sends close_notify.
  resolved: 2026-09-19 -- by deletion. `Transport` is gone; an abort is now `Call.cancel()` / `WebSocket.cancel()`, which OkHttp documents as safe from any thread and which returns at once.
  evidence: Review pass 1, finding 25. closeShared calls Transport.close() synchronously on the serial queue; a stalled peer therefore stalls every other connection on that runtime, including the six-slot pool. Apple's nw_connection_cancel is asynchronous. Not a correctness bug, and the fix is a thread hop rather than a direct correction, so it was not patched blind.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: Nothing keeps dev.screenkit.net.Transport from R8, so a minified build would lose the network backend.
  resolved: 2026-09-19 -- superseded by the HttpClient entry below, which is resolved.
  evidence: Review pass 1, finding 26. GetMethodID and RegisterNatives bind by name and signature, and there are no keep rules, no proguardFiles and no @Keep anywhere under runtime/android/app/src. Harmless today because no release buildType exists (see the 'No release build' entry above); it becomes a silent total failure the moment one does.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: No row covers a JavaVM that is present while FindClass('dev/screenkit/net/Transport') fails.
  evidence: Review pass 1, finding 27. net-no-javavm covers only the no-VM path. The class-loader failure is the one prepareAndroidNetwork exists for, and is the shape a stripped or repackaged build would hit. Stageable with a second env var that makes the test library resolve the class from a detached native thread. Also: liveTransportCount() is asserted only in the two Android-only rows and could be asserted in the streaming rows for free.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: armeabi-v7a is built and shipped for Fire TV but is executed by nothing.
  evidence: Review pass 1, finding 28. The AVD is system-images;android-34;android-tv;arm64-v8a, so the 32-bit slice of NetworkAndroid.cpp -- every jlong/jsize/size_t conversion in makeAnchors, write and transportData -- is compiled for the Fire TV target and never run.

- source_spec: `_bmad-output/implementation-artifacts/spec-android-network-backend.md`
  summary: Android offers no ALPN below API 29, where Apple always sends http/1.1.
  resolved: 2026-09-19 -- by replacement. This was `Transport`'s `SSLParameters.setApplicationProtocols`; the client is OkHttp now, and OkHttp 4.12 negotiates ALPN on API 24-28 itself: `okhttp3.internal.platform.android.AndroidSocketAdapter` calls Conscrypt's `OpenSSLSocketImpl.setAlpnProtocols` by reflection (checked in the pinned jar). Not *executed* below API 29 here -- the one AVD is API 34.
  evidence: Review pass 1 residual, reported by the implementation. SSLParameters.setApplicationProtocols is API 29 and minSdk is 24, so on API 24-28 a server with a strict ALPN callback fails on Android and succeeds on Apple. Closing it needs a TLS stack we do not bundle.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: On Apple a response body nobody reads can still download in full -- `flowWindow` holds the page back but not the server. This is the failure mode backpressure was added to prevent (see the entry resolved 2026-09-17 above), and it is now absent on macOS and tvOS.
  resolved: 2026-09-19 -- the delegate was the problem, not NSURLSession. Measured with standalone NSURLSession probes against a 64 MiB body, the task suspended at 1 MiB: `[task suspend]` is honoured whenever the delegate is caught up and **ignored while callbacks are queued behind a slow one** -- a delegate spending 20 ms per callback let all 64 MiB through, at loopback speed and at a paced 128 MB/s alike, re-suspending on every late callback made no difference, and suspending in `didReceiveResponse` held every time. A delegate that only counts, suspends and hands the NSData on held the server at about 3 MiB in every case, even with whatever consumed it taking 100 ms a chunk. So every sink call now goes through a second serial queue (`AppleCore::delivery`, NetServiceApple.mm) and `didReceiveData` does nothing that costs; `net-flow-control` asserts the unpaced stall (under 24 MiB of 64 MiB) on all three clients, `kStallsUnreadBodies` is gone, and the row passed 6/6 in `runtime/build/macos-asan` -- the build that used to read the whole body. Sabotage: the copy moved back onto the delegate fails the ASan row (one run in two).
  evidence: Measured both ways, against a server that writes as fast as the socket takes it. In `runtime/build/macos` (RelWithDebInfo) the 64 MiB `/firehose` body stalls the server under 1.2 MiB, 4/4 runs, and `[task suspend]` visibly holds it. In `runtime/build/macos-asan` the server writes all 67,108,864 bytes and finishes, 4/4 runs, although the task is suspended after the first ~4 MiB -- CFNetwork's read-ahead is unbounded relative to the delegate and, in a build that slow, the first `didReceiveData` already carries 3.6 MiB. So it is not a debug-build artefact but a guarantee NSURLSession does not make: a slow or busy page on a fast link can have a large response buffered in CFNetwork whatever the window says. Nothing inside `didReceiveData` closes it -- the only lever would be to start the task suspended, and then nothing would ever acknowledge. OkHttp has no such problem: it is pull-based and the reader loop simply stops. What *is* true of both, and is now asserted on both in both builds, is the paced case: against a server offering at most 128 KiB per 10 ms, a 16 MiB body nobody reads is still short of the end after 1.5 s (3.2-6.9 MiB through in the release build, 6.0-9.6 MiB under ASan), and deleting `[task suspend]` finishes it -- sabotage-checked in both builds. So `net-flow-control` asserts the window everywhere and only the *unpaced* stall per platform. Closing the gap itself needs either a much cheaper delivery path (an `onData` that does not copy) or an Apple client whose read-ahead is bounded -- `NSURLSessionResponseBecomeStream` would give one, at the cost of the HTTP framing this change exists to stop owning.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: Neither vendor client can say `decode`, and they disagree about truncated and mis-framed bodies -- NSURLSession accepts a chunked body whose terminating chunk never arrives; OkHttp accepts two disagreeing Content-Length lines.
  resolved: 2026-09-19 -- partly. Android now decodes `gzip` and `deflate` itself (it asks for them, which turns OkHttp's transparent gzip off, and uses okio's `GzipSource`/`InflaterSource`), so a `deflate` body -- zlib or raw -- is decoded, `Content-Encoding` stays on the response and an empty gzip body is empty, as on Apple; and it refuses two disagreeing `Content-Length` lines (`protocol`) instead of reading the first. Linux decodes in its own code too (below). **Still open, on Apple only:** NSURLSession accepts a chunked body whose terminating chunk never arrives, and a gzip stream cut short inside one, as complete, and exposes nothing -- no error, no metric -- that would let the client tell. Android and Linux refuse both. `net-encoded-body` keeps all three transcripts.
  evidence: `net-encoded-body` now carries both transcripts, measured on macOS and on `screenkit-tv`. Apple: `/chunked-truncated` and `/gzip-cut` resolve with a short body, `/gzip-truncated` and `/length-truncated` fail `network`, `/conflicting-length` fails `protocol`. Android: every truncation fails (`network` or `protocol`) and `/conflicting-length` resolves with the first length's five bytes. The portable stack knew what it was framing and could answer `decode` for all of them; neither client exposes that distinction. A page that needs certainty about a body's completeness has to check `Content-Length` itself. Also recorded there: OkHttp offers `Accept-Encoding: gzip` only, so a server that answers `deflate` is handed through undecoded on Android and decoded on Apple, and OkHttp strips `Content-Encoding` and `Content-Length` off what it did decode while NSURLSession leaves them.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: CFNetwork buffers a `text/plain` response body whole before delivering a byte of it, so `response.body` cannot stream one on Apple.
  resolved: 2026-09-19 -- it was content sniffing, and it holds 512 bytes, not the whole body: a `text/plain` head and its first bytes wait until 512 have arrived or the body ends (measured: 200-byte chunks 300 ms apart arrived as one 600-byte lump). `X-Content-Type-Options: nosniff` from the server stops it, and so does CFNetwork's own per-request switch, which is what WebKit sets for fetch: the client now sets `_kCFURLConnectionPropertyShouldSniff` to NO through the public `+[NSURLProtocol setProperty:forKey:inRequest:]`. The key is undocumented -- see the new entry below -- and `net-streaming-response` now reads a `text/plain` body line by line on all three clients, so a later OS that stops honouring it fails the row (sabotage-checked). The fixture's `/slow` takes a `type`.
  evidence: Measured with a standalone NSURLSession probe against a Node server writing three chunks 300 ms apart: with `content-type: text/plain` the head and all 24 bytes arrive together at 0.91 s; with `application/octet-stream`, `text/event-stream`, `application/json`, `text/html` or `text/csv` the head arrives at 0.006 s and each chunk as it is written. Adding a charset does not help. The fixture's `/slow` route was changed to `application/octet-stream` so `net-streaming-response` and `net-abort-timeout` measure streaming rather than this quirk; a real app streaming a `text/plain` endpoint on Apple will see the whole body at once.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: On Apple, cookies do not survive a runtime restart, and the `Cookie:` header's order is the platform store's.
  evidence: Deliberate (the cookie decision in the spec, and `net-cookies`, which now asserts an empty jar after a restart). Each runtime gets its own `NSHTTPCookieStorage` -- the instance an ephemeral `NSURLSessionConfiguration` carries -- rather than `sharedHTTPCookieStorage`, because the shared one is process-wide and disk-backed: the test suite would write 127.0.0.1 cookies into the developer's own cookie file and rows would depend on each other's leftovers. The cost is persistence, which the cookie decision already gave up with `cookies.txt`. Android keeps the device-wide WebView store and does persist. Closing it means a per-app container identifier (`sharedCookieStorageForGroupContainerIdentifier:`) once there is one, which is also what M9's per-package jar needs.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: A request body with no Content-Type is sent with an empty `Content-Type:` header on Apple, because CFNetwork otherwise invents `application/x-www-form-urlencoded`.
  resolved: 2026-09-19 -- narrowed to POST. Measured: CFNetwork invents `application/x-www-form-urlencoded` only for a POST (upload task, `HTTPBody` or streamed alike; `fromFile:` invents `application/octet-stream`), and a PUT with the same body goes out with no Content-Type at all. So the empty header is now set for a POST only, and `net-request-body` asserts that a typeless PUT carries none on all three clients. Still open for a POST: no body mechanism NSURLSession offers sends none.
  evidence: Measured: `POST` with an `NSData` body and no Content-Type reaches the server as `application/x-www-form-urlencoded`; setting the header to `""` reaches it as `''` and CFNetwork adds nothing. A browser sends no header at all for an `ArrayBuffer` body, and a wrong type is worse than an empty one for a server that switches on it -- but an empty `Content-Type` is not what RFC 9110 has in mind either, and a server that parses the value strictly rather than testing it for emptiness could reject it. OkHttp needs none of this (`RequestBody.contentType()` null adds nothing).

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: `net-image` is still Apple-only, so the Android client's decode-and-upload path has never run on a device.
  resolved: no -- carried over from spec-android-network-backend.md, unchanged by the client swap. `GlSurfaceSdl` has no offscreen path, so the device test binary has no drawable and `net-image` is left out of `NET_ROWS`.
  evidence: Everything else did run: `sh tools/android/android.sh test` is 18/18 on `screenkit-tv`, twice, none skipped, against OkHttp. Closing this one needs an EGL pbuffer path for `GlSurface` on Android.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: `net-shutdown-mid-call`'s "no callback after the seam is told to stop" no longer stages the race; it relies on the response being in flight.
  resolved: 2026-09-19 -- staged again, on every client. `net-seam-contract` holds the delivering thread inside a sink call while the server streams a chunk every 5 ms, calls `shutdown` meanwhile, and asserts that it returns only once the held call has and that nothing arrives afterwards. No queue needs exposing: the sink *is* the hook. Sabotage on macOS: without the delivery-queue drain in `shutdown`, it returns with the call still running and the row fails.
  evidence: The old row held the I/O queue busy for 400 ms so a delivery was queued *behind* the close, which made the assertion non-vacuous (removing the `closed` guard failed the row). `NetService` exposes no queue, so the rewritten part 1 starts a 200-chunk stream, aborts mid-body and asserts nothing arrives in the next 600 ms. That still exercises a real crossing -- the server sends a chunk every 20 ms and OkHttp's reader is on its own thread -- but it is timing, not staging, and a regression could pass. Staging it again needs the seam to expose its queue, or a test-only hook.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: OkHttp's WebSocket has no per-message send completion, so `bufferedAmount` is drained by polling `queueSize()`.
  evidence: `HttpClient.Socket2.drainSent` compares `queueSize()` against the payload sizes it queued and reports the ones that have gone, re-scheduling itself every 5 ms while any remain. NSURLSession has a real completion handler per message and reports exactly. The poll costs one scheduled task per 5 ms only while a send is outstanding, but it makes `bufferedAmount` on Android approximate to within 5 ms and the byte counts approximate to within OkHttp's framing overhead, which `queueSize()` includes and this does not subtract.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: Nothing keeps `dev.screenkit.net.HttpClient` or OkHttp from R8, and the old Transport keep-rule gap moves with it.
  resolved: 2026-09-19 -- `runtime/android/app/proguard-rules.pro`, used by the release build type and, with `-Pscreenkit.minify`, by the debug one: `HttpClient` whole, every native method by name, and the two prebuilt AARs that ship no rules of their own -- hermes-android, whose Intl and Unicode helpers libhermesvm calls through fbjni by name, and fbjni itself. SDL3's AAR and OkHttp's jar carry their own. `MINIFY=1 sh tools/android/android.sh test` runs every net row and the host row against an R8-processed APK.
  evidence: Carried over from spec-android-network-backend.md's finding 26, which named `Transport`. `GetMethodID` and `RegisterNatives` bind by name and signature, there are still no keep rules, no `proguardFiles` and no `@Keep` under `runtime/android/app/src`, and OkHttp itself ships consumer rules that only cover its own reflection. Harmless while no release buildType exists; a silent total failure the moment one does.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: Two WebSocket handshake refusals the RFC 6455 client made are no longer made by either vendor client, and `net-websocket` no longer asserts them.
  resolved: 2026-09-19 -- all three clients refuse both, and one premise here was wrong: **NSURLSession and OkHttp both offer `permessage-deflate`** on every socket (seen on the wire), so a server that answers with it has negotiated it. NSURLSession already refused an unoffered subprotocol and an unknown extension, and now reports the negotiated one as `WebSocket.extensions` (from the 101 on the task). OkHttp opened both; `HttpClient` now fails the connection in `onOpen` -- no `open` reaches the page -- and passes the negotiated extension through. cpp-httplib offers none and checks neither, so the Linux client refuses any extension and an unoffered subprotocol. `net-websocket` asserts `/ws-bad-protocol` and a new `/ws-unknown-extension` refused everywhere, and `/ws-extension` (permessage-deflate) negotiated on Apple and Android and refused on Linux.
  evidence: Measured on both platforms. `/ws-bad-protocol` -- the server answers with a subprotocol the client never offered -- is refused by NSURLSession and ignored by OkHttp, which opens the socket and left the row hanging on `screenkit-tv` waiting for a close. `/ws-extension` -- an unrequested `Sec-WebSocket-Extensions` -- is ignored by NSURLSession, which opened the socket and hung the row on macOS; OkHttp's behaviour was not measured once the case left the list. Only `/ws-bad-accept` is still asserted, and both clients refuse it. Both fixture routes are kept so the assertion can come back per platform. Neither client offers permessage-deflate, so the practical exposure is a server that volunteers an extension the client then ignores and does not implement -- the frames would not decode, which fails the connection anyway.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: `android.webkit.CookieManager` returns HttpOnly cookies to `getCookie`, so the "HttpOnly is never visible to JS" invariant is kept by a name list this client maintains, not by the store.
  resolved: 2026-09-19 -- the JS view is an allow-list now. `ScreenKitCookieJar` records every cookie this runtime stored *without* HttpOnly by RFC 6265's identity (name, domain, path, host-only) and value, and shows a pair the store returns only when such a cookie has that name and value and applies to the URL; anything else -- HttpOnly, or a cookie whose flags this runtime never saw, such as one left by an earlier run -- stays hidden. It can hide too much, never too little. `setCookie` from JS refuses to replace a known HttpOnly cookie and any same-named cookie the JS view cannot see. `net-cookies` passes unchanged on the emulator.
  evidence: Measured on `screenkit-tv`: before the filter, `net-cookies`' "JS view hides HttpOnly" line came back with `http=4` in it -- the cookie the fixture set `HttpOnly`. The store hands back a bare `name=value` string with no attributes, so there is nothing to filter on; `HttpClient.ScreenKitCookieJar` therefore remembers the *names* it has seen arrive with the flag and leaves them out of `cookiesFor`/`setCookie`. Keyed by name, it can only ever hide too much, never too little -- but a cookie another app's WebView set HttpOnly before this runtime started is not in the list and would be visible. Apple's `NSHTTPCookie.isHTTPOnly` needs none of this. Closing it properly needs an Android store that reports the flag, which `CookieManager` does not.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: `net-cookies` on Android depends on `android.webkit.CookieManager` not flushing to disk, which is not a documented guarantee.
  resolved: 2026-09-19 -- `NetTests` empties the store (`removeAllCookies`, then `flush`) before every row, and starts the row from the callback so nothing overtakes the clearing.
  evidence: The row's first half asserts an exact `Cookie:` header, and it is stable across `android.sh test` runs only because nothing calls `CookieManager.flush()` and the store therefore dies with the process -- so `expiring=12`, set at the end of one run, is absent at the start of the next. If Chromium ever flushes on its own the row starts seeing the previous run's cookies and fails. The second half was already made order-independent (it asserts membership, not the header). Closing it means clearing the store at the start of the row, which would need `CookieManager.removeAllCookies` reachable from the harness.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: The fixture's `/status/204` no longer sends a Content-Length, so nothing tests a null-body status whose headers claim one.
  resolved: 2026-09-19 -- closed, not a defect. A 204 with a non-zero Content-Length is invalid HTTP (RFC 9110 8.6); OkHttp refusing it and NSURLSession tolerating it are both reasonable, and asserting either would be asserting one vendor's leniency. What matters -- a 204 has no body and the connection is usable after it -- is asserted by `net-http-get`.
  evidence: It used to send `content-length: 15` with no body, which is invalid HTTP (RFC 9110 8.6) and which OkHttp refuses outright -- `ProtocolException: HTTP 204 had non-zero Content-Length: 15` -- while NSURLSession tolerates it. The row's real subject, that `response.body` is null for a 204 and the connection is usable afterwards, is kept; the tolerance of a malformed head is gone, and it was never a promise worth making.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: `RuntimeConfig::storageDirectory` is gone: nothing read it once cookies moved to the platform stores.
  evidence: Removed with its plumbing -- the field, `NetConfig::storageDirectory`, `runBundle`'s parameter, `WindowOptions::storageDirectory` and the `storageDirectory()` helper in all three hosts. It survived the cookie-jar deletion as a field every host computed and no implementation read, which is worse than absent: it reads as load-bearing. Whatever needs per-launch state next -- an HTTP cache, a per-package cookie container, M11's Linux client -- brings it back with a consumer and a row that can see it.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: On Linux a WebSocket close the *peer* sent arrives as 1006 with no reason, and cannot be told from a dropped connection.
  resolved: 2026-09-19 -- by a patch to the vendored header, the first. `tools/vendor/httplib.rules` keeps the Close frame's payload in `WebSocket::read` and exposes it as `WebSocketClient::peer_close_payload()`; `tools/vendor/httplib.sh` applies it on import, each row must match exactly once, and the patched header is pinned by its own sha256 beside upstream's. The client reports the peer's code and reason as the other two do (1005 for an empty frame), and `net-websocket`'s `server close` line is one line for all three again. Upstream master still discards the payload.
  evidence: `WebSocket::read`'s `case Opcode::Close` echoes the frame and returns `Fail` **after discarding the payload**, which is where the close code and reason are, and no accessor exposes them. A transport failure returns `Fail` too, so the two are indistinguishable from outside. `settleSocket` therefore reports 1006 with no reason for any close this side did not send -- the conservative direction, and the same answer `net-websocket`'s `dropped` line already expects -- and that row carries the one divergent line per platform. Closing it means a patch upstream (surface the Close payload) or driving the frame loop here, which would mean reaching past `WebSocket`'s private `strm_`.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: Linux is HTTP/1.1 only -- HTTP/2 came back with the vendor clients and did not come back here.
  evidence: cpp-httplib speaks HTTP/1.1. Apple and Android get HTTP/2 from NSURLSession and OkHttp, which is one of the reasons the seam moved to `NetService` at all, and the Linux client does not. The practical cost is the one the deleted entry above named: many parallel requests to one origin are limited to six connections with no multiplexing. Not measured on a Pi 3, where the CPU running JavaScript is the limit long before the connection count is.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: A raw-deflate `Content-Encoding: deflate` body fails on Linux instead of being decoded.
  resolved: 2026-09-19 -- the client decodes bodies itself (`BodyDecoder`, NetServiceLinux.cpp, over the zlib the runtime already links): gzip, and `deflate` zlib-wrapped or raw, told apart by the first two bytes as browsers do; an empty body is empty; a compressed stream that ends before its framing does is a failure, not a short body. `set_decompress(false)`, and `CPPHTTPLIB_ZLIB_SUPPORT` is no longer defined. `/deflate-raw` decodes on all three clients.
  evidence: `net-encoded-body`'s `/deflate-raw` line. RFC 9110 defines `deflate` as the zlib format and cpp-httplib decodes it through zlib's automatic header detection (`inflateInit2(32 + MAX_WBITS)`), which reads zlib and gzip but not a raw deflate stream; the body then fails the read and the page gets a `TypeError` with cause `network`. Browsers, and NSURLSession, accept both. Android sends `Accept-Encoding: gzip` only and hands such a body through undecoded, so all three differ. Failing is the safe direction -- the page never gets compressed bytes it thinks are plain -- but a real server doing this is unreachable on Batocera. Closing it means decoding the body in `NetServiceLinux.cpp` rather than in the library, which is the protocol code this project stopped owning.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: OpenSSL's headers on Linux come from the build container, not from a pinned source release.
  evidence: `tools/batocera/pi.sh sysroot` takes SDL3's headers from the exact source release the device runs, pinned by sha256, and copies the device's libraries beside them. OpenSSL cannot work that way: `opensslconf.h` and `configuration.h` are generated by its own Configure script and are in no tarball, so the headers come from `libssl-dev` in the pinned Debian 12 build image while the libraries still come off the device -- the same arrangement GLES and EGL already use. It is safe in one direction only: OpenSSL 3.x keeps its ABI, so Debian's 3.0 headers against a newer 3.x on the image are fine, and `pi.sh sysroot` refuses an image whose soname is not `libssl.so.3` rather than letting a 1.1.1 mismatch reach the linker. Closing it means either running OpenSSL's Configure in the container for the device's exact version, or a pinned prebuilt of its generated headers.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: A Linux worker thread blocked in `getaddrinfo` cannot be interrupted, so an abort or a shutdown waits for the resolver.
  resolved: 2026-09-19 -- names are resolved by the client on a thread whose answer can be abandoned (`startResolving`), and handed to cpp-httplib with `set_hostname_addr_map`, which changes the connection target and nothing else (SNI, `Host` and the certificate name are still the host). The addresses are tried in the resolver's order while the connect itself fails, as cpp-httplib's own loop did. A cancel abandons the wait. See also the connect entry below, found on the way.
  evidence: Every other blocking point in `NetServiceLinux.cpp` is unwound by `httplib::Client::stop()` (which shuts the socket down under the client's own mutex) or by a condition variable, and `shutdown` joins each worker after waking it. Name resolution is the exception: `getaddrinfo` has no cancellation, so tearing a runtime down while a request is resolving an unreachable name blocks the I/O queue for as long as the resolver takes -- typically a few seconds, and bounded only by the resolver's own timeout. The other two clients have the same class of problem inside their own resolvers. Closing it means resolving asynchronously (`getaddrinfo_a`, or a resolver thread whose result is abandoned), which is name resolution this runtime would then own.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: A body with no `Content-Type` still goes out with an empty `Content-Type:` header on Apple.
  summary_correction: Apple only -- Android was already correct.
  evidence: A browser sends no such header at all. Android already did the right thing (`UploadBody.contentType()` returns null, so OkHttp writes none), and the Linux client now matches it by putting every request body behind a `ContentProvider` rather than in `Request::body`, which is the only way past cpp-httplib writing `text/plain` into a typeless body (`ClientImpl::write_request`). **Apple cannot follow without a regression**: CFNetwork invents `application/x-www-form-urlencoded` for any request that has a body and no type of its own, and an empty header value is the documented way to stop it (`NetServiceApple.mm`, the `hasContentType` branch) -- removing it would send a *wrong* type instead of none. Closing it needs a body mechanism that is not `NSMutableURLRequest.HTTPBody`. Nothing saw this for either platform until now: `net-request-body`'s existing lines print the header through `Array.join`, which renders an absent header and an empty one identically, so the row now asks about its presence directly.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: `shutdown` on Linux still joins its workers on the calling thread, so a request stuck in `getaddrinfo` delays it.
  resolved: 2026-09-19 -- the joins stay, and are now prompt: every blocking point a worker can be in has a way out another thread can take -- a lookup is abandoned, a connect, a TLS handshake or a read is ended by shutting the socket down under cpp-httplib, and the waits are woken.
  evidence: Reduced, not closed. `abortRequest` and the retirement job no longer join at all -- they hand the thread to `reap`, which only ever joins one whose body has already returned -- so nothing blocking runs on the serial I/O queue any more. `shutdown` is different: `NetService.h` promises that nothing is delivered after it returns, which means the joins have to happen somewhere, and they now happen on the caller's thread after the queue work is done rather than inside it. A worker inside `getaddrinfo` (which has no cancellation) therefore still delays teardown by up to the resolver's own timeout, on the thread that asked for it. Apple and Android have the same shape of problem inside their own resolvers. Closing it needs asynchronous resolution.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: An abort or a shutdown on Linux waited out a TCP connect to an address that never answers -- up to 30 s -- and did it on the I/O queue, stalling every network event in the runtime.
  resolved: 2026-09-19 -- found while fixing the `getaddrinfo` entry, not reported by any review. cpp-httplib holds its socket mutex through a whole connect and TLS handshake (`ClientImpl::send_`), and `stop()` takes that mutex, so `stop()` cannot interrupt either -- it *waits* for them -- and `abortRequest` called it from the serial I/O queue. The descriptor is local to the connect until it finishes, so nothing else could reach it either. The client now keeps a duplicate of every socket cpp-httplib opens, taken in `set_socket_options` (a duplicate, so it can never be closed under the client and handed to another socket, as a cached descriptor number could), and shuts that down *before* calling `stop()`: the connect fails at once and the mutex is free. The same duplicate ends a WebSocket's connect, handshake and read.
  evidence: `ClientImpl::send_` at httplib.h:14864 takes `socket_mutex_` and calls `ensure_socket_connection` under it; `ClientImpl::stop()` takes the same mutex. Linux's `shutdown(2)` on a socket in SYN_SENT disconnects it and wakes the poller (`inet_shutdown`).

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: Apple's `text/plain` streaming rests on an undocumented CFNetwork request property.
  evidence: `_kCFURLConnectionPropertyShouldSniff`, set to NO through the documented `+[NSURLProtocol setProperty:forKey:inRequest:]` -- no private symbol or selector is called, but the key's meaning is not public, and WebKit is its only documented-by-example user. If an OS release stops honouring it the cost is the old behaviour (a `text/plain` head and its first 512 bytes held back), not a failure, and `net-streaming-response` fails so it is noticed. If App Review ever objects to the string, deleting the one line is the whole rollback.

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: Duplicate request and socket ids were accepted on Apple and Android, stranding the call that held the id.
  resolved: 2026-09-19 -- every client refuses an id already in use (the new call fails with `url`), as Linux already did; `NetService.h` states the contract and `net-seam-contract` asserts it on all three, including that an abort of the id still reaches the first call (the server sees its connection close). Apple used to overwrite the map entry, Android the id-to-handle map, and the first call then ran on out of reach of any abort. The binding never reuses an id, so this guards the seam, not a live path.

- source_spec: `_bmad-output/implementation-artifacts/spec-linux-http-client.md`
  summary: Which of the Linux review round's fixes have a row now, and which cannot have one.
  evidence: Now asserted -- the `Max-Age` clamp (a cookie with `Max-Age=2^63-1` lives, `Max-Age=-1` deletes it) and the 4096-byte name-plus-value cap in `net-cookie-rules`; distinct TLS refusal reasons (untrusted, expired and wrong host each say something different, and the wrong host says so) in `net-system-ca`; duplicate-id refusal in `net-seam-contract`. Not stageable from outside, so reviewed rather than tested -- the redirect that drains a streamed body (cpp-httplib writes the whole request before it reads the response, so a 303 is only ever seen after the body has ended and a row would pass without the fix), the destructor's detach of a live worker (reachable only if the I/O queue dies first), retiring by pointer rather than id (needs a late retirement to race a reused id, which the binding never produces), the null-store fallback to the image's roots (`SSL_CERT_FILE` makes OpenSSL's own defaults find the same bundle, so no row can tell them apart), and the one fewer chunk copy (a performance change).

- source_spec: `_bmad-output/implementation-artifacts/spec-platform-http-clients.md`
  summary: The Unavailable client was compiled by nothing and run by nothing.
  resolved: 2026-09-19 -- `net-unavailable` (runtime/tests/NetUnavailableTests.cpp), its own executable on the macOS build because it replaces the platform's client: a request fails `unsupported` once, off the caller's thread; a socket errors and closes 1006; cookies are empty and unsettable; `shutdown` is idempotent and nothing is delivered after it.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: Sampling video in WebGL -- `gl.texImage2D(..., videoElement)` through `CVMetalTextureCache` (Apple), `SurfaceTexture` (Android) and a decoder frame through EGL (Linux), throwing for a protected element.
  evidence: Deferred by the spec ("No `gl.texImage2D(..., video)` in this spec"). Video is direct play on every platform: a plane the platform composites beneath the canvas, never through the runtime's GL. Architecture.md 4 records the path as deferred; the vendored `texImage2D` still takes no video source.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: Real FairPlay decryption -- a CKC from a real key server decrypting a FairPlay stream on a device.
  evidence: The spec verifies FairPlay up to the key server only. On macOS the fixture's certificate is not Apple-issued, so `makeStreamingContentKeyRequestDataForApp` refuses it (NSOSStatusErrorDomain -42650) and no SPC is made; `media-licence` asserts that path (certificate fetched through the networking engine, then 6007) and `media-licence-exchange` asserts the SPC request's shape through the filters to the licence URL and the answer back to the key system, against a scripted key system. The tvOS simulator has no FairPlay at all. Needs an Apple-issued FPS certificate, a key server and a protected stream.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: Linux video: variants, ClearKey and cue text under libvlc 3, and hardware decoding beyond the Raspberry Pi 3.
  evidence: Accepted divergences of the libvlc decision (human, 2026-09-19): libvlc 3 exposes no variant list (only the playing stream is listed; ABR limits apply at load, and stepping down reopens at the current position), no CENC keys (ClearKey is 6001), and no cue text (VLC draws captions into the picture; `cuechange` never fires). Hardware decoding is ScreenKit's VLC decoder module over the image's `libavcodec.so.58` V4L2 decoders, verified on a Pi 3 only; a Pi 4 or 5 reaching 1080p60, stateless V4L2 (HEVC on a Pi 4/5) and any other board are recorded unverified until run. Revisit with VLC 4, whose libvlc exposes programs and more of the player.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: No automated row asserts that a video plane actually reached the compositor on Apple or Android.
  evidence: Review triage 26. The rows register a VideoHost only on Linux (`ensureMediaWindow`), so `SKPlaneHost applyRect`, the z-order restack and the metal-layer opacity switch never run on Apple, and `VideoLayer.videoCreated()` returns early in the Android harness. The geometry is now covered (`media-unavailable` checks `planeInWindow`/`fitContain`) and Linux counts presented pictures under `SCREENKIT_MEDIA_TRACE` -- which is what caught the window-transparency defect this spec records -- but "the picture is on the screen" is still a manual screenshot on Apple and Android. What would settle it: a windowed row that runs a package with a `<video>` and waits on a log line from the plane path, plus a presented-count assertion per platform.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: A `<video>` whose `src` is a package asset is never loaded by any row.
  evidence: Review triage 27. `screenkit:` URLs resolve through `resolveAssetPath` in `bindings/HostIO.cpp` and are refused for anything outside the package, but every `v.src` and `p.load` in the rows is an `http://` fixture URL, so both the asset path and its confinement check can be removed with the suite green. Offline video is the case a TV app most often needs.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: The Linux ABR ceiling and the drop-rate step-down are exercised by nothing.
  evidence: Review triage 28. The fixture ladder tops out at 640x360 while the probed ceiling on a Pi 3 is 1280x720, so no rung is ever refused, and `media-hardware` plays a progressive MP4, which `checkDrops` skips. Both behaviours did run in the field during this review -- `dropped 9 of 607 frames over 10 s at 720p: the ceiling steps down to 480p`, on the Blits page -- but a rung above the ceiling and a driven drop rate would pin them.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: The three media `announce()` warnings are asserted by nothing.
  evidence: Review triage 29. `media-above-canvas` (a z-index that would lift video over the canvas, a decision the spec names), `media-audio` and `media-srcObject` each warn once, and no row checks that they do -- the repo already counts announcements elsewhere with `LogCapture` (`RuntimeTests.cpp:1372`).

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: `videoRobustness` and `audioRobustness` are configurable and enforced nowhere.
  evidence: Review triage. Parsed in `bindings/Media.cpp`, carried in `DrmConfig`, forwarded by `@screenkit/shaka` with a unit test that they round-trip, and read by no player: an app asking for `HW_SECURE_DECODE` gets unprotected playback and no indication. Either apply them (Android's `MediaDrm` security level, Apple's `AVContentKeySession` has no equivalent) or refuse a robustness the platform cannot promise.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: Two overlapping video planes stack by z-index on Apple and by creation order on Android and Linux.
  evidence: Review triage. `setPlane`'s `order` argument is the element's CSS z-index; `MediaPlayerApple.mm` sorts its containers by it and re-sorts on change, while `VideoPlayer.java`'s `placePlane` always adds at index 0 and the Linux subsurface ignores it. Recorded in `runtime/js/README.md`'s divergence table rather than fixed, because one video over another is not a case any target needs yet.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: One libvlc instance is shared by every runtime in the process, and its log is keyed by URL.
  evidence: Review triage. `acquireInstance` holds a function-local `static weak_ptr`, so a second runtime inherits the first's VLC argv and plugin path and cannot configure its own, and the instance outlives the runtime that made it. Benign today (nothing runs two runtimes with media at once), but the spec says one instance per runtime. Separately, `VlcLog` is keyed by URL, so two players loading the same URL erase and read each other's recorded HTTP status and manifest probe.

- source_spec: `_bmad-output/implementation-artifacts/spec-video-player.md`
  summary: The 60 s hardware-decode measurement only runs under `SCREENKIT_MEDIA_SOAK=1`.
  evidence: Review triage. The acceptance criterion is 60 s of 1080p30 with under 1% dropped; `media-hardware` plays one 12 s pass by default so the suite stays quick, and `pi.sh` does not set the variable, so the 60 s figure in the spec came from a manual run. A scheduled soak, or a device run that sets it, would keep the number honest.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: The `Suspended` lifecycle state and the memory-pressure LRU escalation.
  evidence: Deferred by decision in the spec's frozen intent, and recorded in `Architecture.md` §5.1 and §12. Both need a memory-pressure signal per platform -- Apple's `didReceiveMemoryWarning`, Android's `onTrimMemory`, nothing at all on Linux -- and an audit of which GL objects an instance can genuinely rebuild after they are dropped. Until then `Architecture.md`'s risk 10 stands unresolved: on a tvOS device, retained `Paused` instances plus their last-frame textures count against Apple's per-app limit, and a launcher that embeds many games has to terminate the ones it is not showing. `suspend` and `memorywarning` are not dispatched at `window`, which `dom-identity-and-absence` pins.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: An instance's layer texture is made once, at the size the element had when it loaded.
  evidence: `createInstance` takes `width`/`height` from `planeFor` at load time and `GlSurface::createShared` allocates the framebuffer once. A later CSS change moves and scales the layer -- the composite pass takes the rect every frame -- so the game stays where the page puts it, but its resolution does not follow: an `<iframe>` grown from 320x180 to fullscreen is a 320x180 frame scaled up until something reloads it. Closing it means resizing the child's framebuffer on the child's own thread, which is a task, a fence and a frame the host must not composite mid-resize. Recorded in `runtime/js/README.md`'s known limits; no target needs it yet, because a launcher's tiles are a fixed size.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: The `iframe-*` rows run on macOS only; the device suites cannot run them.
  evidence: Every row needs a drawable -- an instance draws into a texture in the page's own share group, and the composite is asserted as pixels read back out of the launcher's framebuffer -- and `screenkit-net-tests`, the library `android.sh test` and `pi.sh test` drive, has no window. SDL's backends offer no offscreen surface either (`core/src/gfx/GlSurfaceSdl.cpp` refuses `nativeLayer == nullptr`), so a pbuffer is not a way round it. `IFRAME_ROWS` is present and empty in both scripts with that reason written down. What would close it: an offscreen path for the SDL backend (an FBO on a hidden 1x1 window), which would also give the Linux and Android suites the `gl-*` and `dom-*` rows they skip today.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: The launch/remove loop asserts nothing survives, not that RSS is flat.
  evidence: `Architecture.md` §14.7 asks for RSS watched across a launch/remove loop, because leaking one instance per launch stays invisible until roughly the tenth game. `iframe-remove` runs the loop ten times and asserts that no instance is registered afterwards, that the launcher can go idle again, and that no GL surface was destroyed off its owning thread, and it is clean under ASan and would be under `leaks(1)`. The number itself is still missing: the suite has no platform RSS reading, and `leaks-iframe-remove` was not added because the row takes ~10 s and `leaks(1)` multiplies that.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: The four-target screenshot check has run on macOS and the tvOS simulator only.
  evidence: The spec's acceptance criterion is a launcher page embedding a game on macOS, the tvOS simulator, the Android emulator and the Pi, with a screenshot of each showing the game inside the iframe's rect and the launcher's UI around it. The composite is asserted by pixel readback in `iframe-embed`, and the tvOS-simulator and Android and Linux hosts are wired up and build, but no device run has happened -- and on the SDL backends the composite path (`enableCompositing`, `createShared`, the fence) has never executed at all. It is the highest-value remaining check, because the SDL half of the GL work is the half nothing has exercised.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: Three process-wide singletons a second instance still shares.
  evidence: The spec's Code Map lists five process globals an instance contends for. Two were fixed because they were bugs the moment a second runtime existed -- `EventEmitter`'s log tag (an unsynchronised cross-thread write) and `HostIO`'s asset root (one app could have read the other's package) -- and three were left, deliberately, because sharing them is benign today and fixing them is a platform change rather than a keying change: `media::VideoHost` (one window's video planes, and there is one window), `GamepadRegistry::shared()` (the pads are the box's, and the claim is already documented as process-wide in `runtime/js/README.md`), and the Linux libvlc instance (already filed above). What would force each: a second window; an instance that should see different pads from its launcher; and a second runtime with media on Linux.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: No row runs the shipping host's `<iframe>` wiring -- its per-frame instance tick, its focus targets and `terminateInstances`.
  evidence: Review triage 23. The `iframe-*` rows build their own harness (`IframeHost`, `tickTree`), so deleting the `for (instance : liveInstances(...)) instance->tickFrame(...)` loop in `runtime/host/Host.cpp` leaves every row green while embedded games get zero frames in the shipped binary. What would close it: a `launcher.skpkg` fixture in `make-fixtures.mjs` whose entry embeds `iframe-child.skpkg`, and one `host-window-row.sh` row beside `host-window-runs-package`.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: Layer z-order and layer opacity are unpinned by any test.
  partly resolved: 2026-09-20 (spec-m6-composited-canvases.md) -- the compositor's alpha uniform now has a test behind it: `canvas-composite` places a canvas at `opacity: 0.5` over the page's blue and reads the blend (`128,0,128`), and setting that canvas to `opacity: 1` fails the row. Z-order across layer kinds is pinned by `canvas-over-iframe` (a HUD canvas at `z-index: 2` over an instance at `1`, all three regions read). What is still open is the original case: two *instance* layers of distinguishable colours overlapping, and an instance embedded at `opacity: 0.5` -- `LayerList::paintOrder`'s comparator and the alpha path are shared, so this is now a narrower gap than it was.
  evidence: Review triage 24 and 25. `iframe-two`'s two children paint the same colour, so inverting `LayerList::paintOrder`'s comparator changes nothing it reads; and no row sets `opacity`, so the eighth `setPlane` argument and `CompositePass`'s alpha blend can stop working silently. Both are one row's worth of work: give the two children distinguishable colours (the child already answers messages) and read the overlap pixel, and embed one child at `opacity: 0.5` over the launcher's blue.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: Reusing one `<iframe>` element -- a second `src`, or removing and re-appending it -- is unverified.
  evidence: Review triage 26. Every row creates a fresh element and writes `src` once, so the stale-serial filter in `bindings/Instance.cpp` and `unload`'s serial bump are unpinned: a launcher that switches games through one element could get the previous game's `load` after the new one already failed, and nothing would fail. One row reusing an element across a good package, a refused one and a good one again covers it.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: Fence support is detected by a non-null `GetProcAddress` result alone.
  evidence: Review triage 30. `syncApi()` has no version or extension gate, and both `eglGetProcAddress` and `SDL_GL_GetProcAddress` may return a valid-looking pointer for an entry point the current context does not support -- which is exactly the ES 2.0 VideoCore IV and Android NDK case the code's own comment names. Producer ordering is safe either way (`insertFenceOrFlush` flushes in both branches), so the cost is a bogus `GLsync` handed to `waitSync`/`deleteSync`. Settle it on a Pi 3 by logging what the three lookups return; gate on `GL_ES_VERSION_3_0` if they are non-null.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: A detached but still-referenced `<iframe>` keeps its runtime from ever going idle.
  evidence: Review triage 27. `create` takes the work hold and only `destroy` releases it, so an element that has been removed -- its instance terminated -- still holds the loop non-idle until the element is collected. Harmless in a windowed host (a 16 ms idle wait), but the headless path waits on `idle()` and would sit out its 30 s bound.

- source_spec: `_bmad-output/implementation-artifacts/spec-m9-iframe-instances.md`
  summary: The iframe test harness feeds `tickFrame` timestamps that go backwards.
  evidence: Review triage 35. `tickTree(host, frames)` keeps a function-local `static` clock while `pumpTree` and `iframeFocus` keep their own locals starting at 0, and every row interleaves them. No fixture reads its rAF argument today, so nothing observes it; a child that integrated on the timestamp would see negative deltas. One clock on `IframeHost` fixes it.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: A canvas that already took the page's frame cannot be promoted to a layer when CSS places
    it afterwards.
  evidence: Which of the two a canvas gets is decided when it asks for its context: a canvas the CSS
    subset has already placed, resized or hidden becomes a layer, and one it has not takes the frame.
    A page that calls `getContext` first and sets `style.cssText` second therefore keeps the frame,
    and the declaration is stored, reads back and warns once (`dom-canvas-style`). Promotion after the
    fact cannot swap the context object the page is already holding, so it would mean redirecting that
    context's default framebuffer to a layer FBO of its own and making `gl.drawingBufferWidth` report
    the layer rather than the window -- which the shim also reads as the *drawable* size for
    percentages, viewport units, `<body>` and `window.screen`, so that source has to be separated
    from `gl` first. Real but not reached by any app measured: Lightning sets sizes that match the
    drawable, so nothing it writes diverges.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: A 2D canvas is not composited.
  evidence: Deferred by decision in the spec. `getContext('2d')` stays what it is -- CPU pixels the
    page uploads with `texImage2D` -- so an overlay drawn in 2D is still the page's to upload. Giving
    it a layer means uploading its buffer to a texture every frame it changed, which is the work the
    page is doing anyway and with a dirty-rect answer it does not have. Nothing measured asks for it.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: A second WebGL canvas inside an `<iframe>` instance gets no context.
  evidence: An instance draws into a layer surface of its own, and `GlSurface::enableCompositing`
    refuses one -- its frame is published straight out of `colorTexture_` and there is no present
    context to build a `CompositePass` in, nor an FBO the present context could bind (framebuffers
    are not shared across a share group). So `__screenkit.canvas.create` answers null there and
    `getContext` returns null with the reason, once, which is the web's own "getContext may return
    null". Closing it means giving a layer surface a present context with its own FBO wrapping
    `colorTexture_`, composited before the fence -- about the same amount of GL as the host path.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: The four-target screenshot check for a game canvas under a HUD canvas has run on macOS
    only (`canvas-evidence/`).
  evidence: The spec's acceptance criterion is a two-canvas page on macOS, the tvOS simulator, the
    Android emulator and the Pi, with a screenshot of each showing both canvases in z-order and the
    HUD's transparent pixels showing the game through. The composite, the z-order and the blend are
    asserted by pixel readback in `canvas-composite` and `canvas-over-iframe`, and on macOS by a
    screenshot from the shipping windowed host (`canvas-evidence/macos-hud-over-game.png`), and the
    tvOS-simulator build is green, but no other target has run it -- and on the SDL backends
    (`GlSurfaceSdl.cpp`) the canvas-layer path has never executed at all. It is the same gap the
    `<iframe>` composite already has and for the same reason: the SDL half of this GL work is the
    half nothing has exercised.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: Nothing measures what a canvas switch costs.
  evidence: `gl::ContextGet` gained an id comparison on every vendored GL call and a `makeCurrent`
    where a page alternates between canvases, and the claim that this is cheap is reasoned rather
    than measured: `gl-frame-stats` reports frame time but no row draws on two canvases per frame at
    a rate anything could regress against. What would close it: a bench that alternates draws between
    two canvases and compares against the same draw count on one.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: Two rows flake under `ctest -j6`, neither of them about graphics.
  evidence: Observed while verifying this spec, once each across five full-suite runs and never
    standalone (8/8 and 5/5 passes each): `promise-rejection-logged` missed its
    "Unhandled promise rejection" line, and `iframe-child-fails` timed out in `pumpTree`. Both wait on
    wall clock under load -- the first on `pumpUntilIdle` racing Hermes' rejection reporting, the
    second on `pumpTree`'s 20 s bound while five other rows have the machine -- and neither touches
    the canvas, the compositor or the GL registry this change altered. Pre-existing; recorded so the
    next person who sees one does not go looking in the wrong place.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: A SpiderMonkey build gets no `__screenkit.canvas` at all, so every second canvas takes the
    "the binding is missing" refusal.
  evidence: Review triage, 2026-09-20. `installCanvas` is called only from `HermesHost::threadMain`,
    and `runtime/CMakeLists.txt` hard-fails a SpiderMonkey configure with tests on, so none of the
    canvas code is compiled or run on that engine. The port is experimental and not a shipping
    target; its own files predate this change.

- source_spec: `_bmad-output/implementation-artifacts/spec-m6-composited-canvases.md`
  summary: `gl.getParameter` answers numbers where WebGL specifies booleans -- COLOR_WRITEMASK as well
    as BLEND.
  evidence: Review triage, 2026-09-20. The vendored parameter path pushes the raw GLints, so
    `getParameter(COLOR_WRITEMASK)` reads `1 1 1 0` rather than `true,true,true,false`. Pre-existing,
    already recorded for BLEND in `canvas-isolation`, and surfaced again while widening
    `canvas-placed`. A conformance fix belongs with the vendored renderer (an `expo-gl.rules` patch
    row plus a re-run), not with the canvas change.

