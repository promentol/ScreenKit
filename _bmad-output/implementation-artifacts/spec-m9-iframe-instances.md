---
title: 'One `<iframe>` = one Instance: a second app in its own runtime, composited, with lifecycle, postMessage and sandbox'
type: 'feature'
created: '2026-09-20'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/Architecture.md'
  - '{project-root}/runtime/js/README.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** `document.createElement('iframe')` makes an inert element. There is no second browsing context, no compositor (a second GL canvas throws, `dom-shim.js:2174`), no `postMessage`, and no way for a launcher to embed a game -- the one thing the multi-app model exists for. `Architecture.md` §5 defines it; nothing implements it.

**Approach:** One iframe = one **Instance** = a `Runtime` on its own thread with its own GL context in the host's share group, drawing into an FBO-backed texture that the host composites at the element's CSS rect. The pieces already exist separately: two runtimes with shared contexts are tested (`gl-shared-context`), the freeze gate is what `Paused` means, and `planeFor` already computes the layer rect. What this adds is the compositor, the element, the channel and the capability gate.

**Decisions (human, 2026-09-20):**
- **All four parts in one spec** -- instances and the compositor, lifecycle and `focus()`, `postMessage`, and `sandbox` -- rather than shipping the embed-and-render slice first.
- **`src` names a local package only** -- a path confined to the parent's package, which the asset root already enforces. A launcher ships the games it embeds; nothing is downloaded, so there is no cache, no integrity check to write and no store. An `http(s)` src is refused with a documented error, and fetching one stays M12's job.
- **Three lifecycle states: `Running`, `Paused`, `Terminated`.** They fall out of the freeze gate, which already stops rAF, timers, input and the media players. `Suspended` -- dropping regenerable GL resources while keeping the heap -- and the memory-pressure LRU escalation are deferred: they need a memory-pressure signal per platform (Apple's warning, Android's `onTrimMemory`, nothing on Linux) and an audit of which GL objects can be rebuilt. `Architecture.md` §5.1's table is amended to say so.
- **`postMessage` carries a structured-clone subset**: primitives, plain objects and arrays, and `ArrayBuffer` copied by value; anything else throws `DataCloneError`. The two runtimes share no values, so every message is copied natively -- there is no transfer and no `MessagePort` in this spec.
- **One level of nesting.** An iframe inside an instance is refused with a clear error, and focus is parent-or-child. `Architecture.md` §5 allows a tree; that this build does not is a recorded divergence.

## Boundaries & Constraints

**Always:**
- An Instance is a real `Runtime`: `HermesHost` unchanged as its body, its own `EventLoop`, `WorkQueue` event type and bindings. `Paused` is `Runtime::pause()`, which already stops timers, rAF, input and the media players. Teardown order (`HermesHost.cpp:213-236`) and the gate-at-selection (`:174`) are not to be touched.
- **GL stays on the owning JS thread.** The child draws into its own FBO in its own context; the composite pass runs at the *host* runtime's frame end (`presentFrame`, `VendoredWebGL.cpp:859`), on the host's JS thread. Nothing draws on the main thread.
- Ordering across contexts is explicit: a fence where the platform has one, a flush where it does not. Never `glFinish` in the frame path.
- Per-instance, not per-process: the asset root (`HostIO.cpp:61`), the emitter tag (`EventEmitter.cpp:24`, today an unsynchronised cross-thread write), the painted flag (`dom-shim.js:8696`), and the input target (`Input.h:125`).
- A `<video>` plane stays beneath every canvas layer, as `MediaPlayer.h:51` already requires; an instance's layer slots into that order rather than competing with it.
- `postMessage` is task-to-task and asynchronous -- no `invokeSync` (`Runtime.h:30-33`), no shared JSI values, no JSON bridge between the two runtimes.
- `sandbox` is a `RuntimeConfig` capability, enforced where the binding is installed (as `testTlsAnchors` is): nothing reachable from the child's JS can widen it.
- A page with no iframe costs what it cost before: no composite pass, no second context, no extra present.

**Never:**
- Presenting this as isolation. `Architecture.md` §5.2 stands: JS is isolated, memory is not, a crash or OOM takes every instance with it, and `sandbox` gates capabilities only. The docs must say so where the API is described.
- Navigation inside an instance (no `location` assignment, no history, no back/forward) beyond the fragment-only rule the shim already has.
- A second GL canvas *within* one instance -- that still throws.
- Reviving `document.domain`, origins, or any same-origin check: there are no origins here.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Embed and paint | `iframe.src = <package>`, appended | the instance loads, runs and paints at the element's CSS rect; `load` fires on the element | N/A |
| Focus switch | `child.focus()` | the outgoing context is paused before the incoming one resumes; input goes to the focused instance only; `document.activeElement` is the iframe | N/A |
| Paused instance | a context that lost focus | zero rAF and zero timer callbacks; its last frame keeps showing; `pause` fires on its `window` | N/A |
| Resume | `parent.focus()` back | `resume` on the child's window, rAF resumes within a frame, no burst of stale timers | N/A |
| Removal | `iframe.remove()` | the instance is terminated: thread joined, heap and GL resources freed, layer gone | N/A |
| Message out and back | `child.contentWindow.postMessage(v)` / `parent.postMessage(v)` | a `message` event on the other side with a copy of `v` and a `source` that can reply | a value the clone cannot carry throws `DataCloneError` |
| Sandbox refusal | `sandbox` without a token | the gated API in the child reports unavailable in its documented shape, and says why once | N/A |
| Bad `src` | missing package, wrong `format`, newer `runtimeVersion`, bad bytecode | `error` on the element, no instance left behind, the parent keeps running | the existing package gates (`Host.cpp:414-442`) decide |
| `src` off the package | an `http(s)` URL, or a path escaping the parent's package | `error` on the element, naming what is allowed; no instance is made | the asset-root confinement decides, as it does for `<video>` |
| Nested iframe | an `<iframe>` inside an instance | `error` on that element; the instance keeps running | refused, not silently inert |
| Child fails at runtime | the child's entry rejects | `error` on the element; the parent is untouched | the child's `FailureReport` is reported, not the parent's |
| Two instances | two iframes, both appended | both load; only the focused one runs; both layers composite in z-order | N/A |
| Runtime shutdown | the host shuts down mid-play | every instance is terminated before the host returns; nothing is delivered afterwards | N/A |
| No compositor | a platform where the composite path fails to start | `src` fails with a documented error and the parent keeps painting | N/A |

</frozen-after-approval>

## Code Map

**Instances already work.** `gl-shared-context` (`runtime/tests/RuntimeTests.cpp:3163-3233`, registered `CMakeLists.txt:230`) creates two `Runtime`s on two JS threads with two `GlSurface`s joined by `Desc::shareWith`, writes a texture in one and reads it in the other, and survives the first's destruction. Extend it rather than start over.

- `runtime/core/include/screenkit/Runtime.h` -- `Runtime::create` :97, `evaluateBundle` :110, `executor()` :118, `pause/resume/paused` :125-127, `tickFrame` :134, `setFrameFinishedCallback` :149, `idle()` :156, `shutdown()` :173. `RuntimeConfig` :59-73 carries `name`, `maxHeapBytes`, `testTlsAnchors` -- where `sandbox` and the parent handle belong. No `invokeSync` :30-33.
- `runtime/core/src/hermes/HermesHost.cpp` -- JS thread :80, `threadMain` :115-236 (every binding installed :115-137), freeze gate at task selection :171-181, `pause/resume` :278-288, `setPaused` :265-276 (calls `setMediaRuntimePaused` from the calling thread), `idle` :304-313, teardown order :213-236, `stop()` idempotent :324-351. `loop/WorkQueue.h:24-26` -- one SDL event type per instance, already keyed correctly; `reclaimRuntimeEvent` (`Runtime.h:192`) already routes by type.
- `runtime/host/Host.cpp` -- `WindowState` :611-620 holds exactly one runtime/router/viewport; the frame loop :824-886 with the single `tickFrame` :876 and the `idle()`/`painted` pacing :833/:842; `startGraphics` :87-172 (surface made on the JS thread, present hung off `setFrameFinishedCallback` :141-157) is already per-runtime; `openPackage` :384-503 and its gates :414-442; `resolveLaunch` :525; prelude → asset root → bundle ordering :775-790 (load-bearing, :174-184); `setVideoHost` :680-694. tvOS equivalent `runtime/apple/HostMain.mm:249-285`.
- `runtime/core/src/gfx/GlSurface.h` -- `Desc::nativeLayer` :59, `shareWith` :68 ("what one instance compositing another's output needs"), `fixedWidth` :75; `defaultFramebuffer()` :148, `colorTexture_` :176 (private -- the compositor needs it); thread rule :34-42.
  - **Apple** (`GlSurface.cpp`): share groups work :167-177, display refcounted :33-56 -- but `swap()` :236 is a bare `eglSwapBuffers`, so **there is no offscreen FBO and no present quad on Apple at all**: that path has to be added.
  - **SDL** (`GlSurfaceSdl.cpp`): `shareWith` is refused :228-231 though `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` is already used :125-127; no offscreen surface :224-227; `createFixedTarget` :88-163 is the FBO + program + quad to generalise from one letterboxed frame to N layer rects; `swap()` :335-372.
  - No fence or sync anywhere in the tree (`Architecture.md:80-82` plans `EGL_KHR_fence_sync`).
- `runtime/core/src/gfx/VendoredWebGL.cpp` -- `presentFrame` :859-888 is the single frame boundary and already keys contexts by runtime pointer :863; `releaseStaleDrawables` :892.
- `runtime/js/dom-shim.js` -- backed elements: `register` :439-449 (`backed`), `<video>` sets `backed` directly :6669 and keeps its own WeakMap, `defineBackedSize` :104-130 (canvas only; do not widen), the `localName === 'canvas'` style branch :1677 (an iframe wants the video treatment, not `divergence`). Layer rect: `styleOf` :1834, `state.layer` :1689, `layerOf` :7329, `zIndexOf` :7332, `precedes` :7336, **`planeFor` :7355-7409** (position/size/transform/visibility in drawable pixels -- reuse verbatim), `updatePlane` :7411, coalescing :7435-7448. Tree hooks: `detach` :562-582 and `insert` :584-597 (the `mediaElementCount` guard to copy), `mediaTreeChanged` :7454-7476 (microtask-deferred `isConnected` check -- exactly what removal→terminate needs), `setAttr` routing :972-975. Absent today and to be added: `postMessage`, `structuredClone`, `MessagePort`, `window.parent/top/frames`, `HTMLIFrameElement`, `element.focus()`, `sandbox`; `MessageEvent` exists :3468-3479, `dispatcher` (the stepper native uses) :254, `defineEventHandlers` :324-356. `document.activeElement` is pinned to `body` :8040 and key dispatch targets it :8373. `global.open` :3254 says "there is no second browsing context" -- that comment stops being true.
- `runtime/core/src/bindings/EventChannel.h` -- `ChannelTarget` :25-36, `EventChannel::post` :50-141 (one task per channel, 64 events per drain, microtask checkpoint between), `Poster` :144-156. `bindings/Media.cpp` is the binding to copy: `MediaBinding` :450-534, `create(target)` :649-672 (rejects a target that already has native state, `WeakObject`, `holdWork`, `setNativeState` as the GC hook), `deliver` :480-505 (stale serials dropped), `destroy`/`shutdown` :511-533. `bindings/Media.h:28-68` is the header-comment convention for the new binding's JS surface.
- `runtime/core/src/bindings/HostIO.cpp` -- `confined()` :32-59 (double `realpath`, keep), `rootFor()` :61-66 **one root per process** and `setAssetRoot` write-once :119-138: both must become per-runtime. `runtime/js/README.md:715-716` already records this as M9's job.
- `runtime/core/include/screenkit/Input.h` -- `InputRouter` :80 holds one `shared_ptr<Runtime>` :125; `deliver` (`input/Input.cpp:489-533`) drops events while paused :499 and stamps `pauseEpoch` :507. A focus switch must release held keys (`held_` `Input.h:132`) the way a disconnect does (`Input.cpp:316`). `Viewport.h:30-40` has the same one-runtime shape for `resize`.
- Process-globals a second instance contends for: `emitterTag()` (`jsi/EventEmitter.cpp:24-27`, written from each JS thread -- a real race today), the asset root, `media::VideoHost` (`media/VideoHost.cpp:14-38`), `GamepadRegistry::shared()` (`input/Gamepads.cpp:33`), the Linux libvlc instance (`media/MediaPlayerLinux.cpp:355-365`). Leave alone: EGL display refcount (`GlSurface.cpp:33-56`), `WorkQueue`'s `QueueTable`, `TimerRegistry`'s `SlotTable`, log sinks, `SDL_Init`/`SDL_Quit` (whole-process, `Host.cpp:737`/:895).
- Tests: `domRuntime` :1218, `domEval` :1228, `netTranscript` :4561 with `checkTranscript` :4585, the row table :8828-8940, DOM rows `tests/CMakeLists.txt:240-314`. Absence pins that must change: `domCoverage` :1682-1694 (the `names` list and its `"covered 33"` string) and `domIdentityAndAbsence` :1696-1736 (gains an iframe block). Lifecycle precedents to imitate: `mediaRuntimePause` :8270-8292, `mediaCollected` :8299-8328 (GC release proof), `mediaRemove` :8155-8200. Host rows: `host-window-row.sh` and `host-window-runs-package` (`CMakeLists.txt:613-615`), fixtures from `make-fixtures.mjs` `writePackage` :134-147. `dom-shim-names` (`CMakeLists.txt:646`) forbids duplicate top-level names in the shim.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/core/src/gfx/GlSurface.h`, `GlSurface.cpp`, `GlSurfaceSdl.cpp` -- an offscreen layer surface an instance draws into: expose the colour texture behind `defaultFramebuffer()`, add the FBO + present program to the Apple backend (the SDL backend has it at `createFixedTarget`), and lift the SDL `shareWith` refusal onto `SDL_GL_SHARE_WITH_CURRENT_CONTEXT`. Ordering between the producer and the compositor: a fence where the platform has one, a flush where it does not.
- [x] `runtime/core/src/compositor/` (new) -- the layer list and the composite pass: layers sorted by `order` then insertion, each a texture and a destination rect in drawable pixels, drawn at the host runtime's frame end. No layers means the present path is exactly what it was.
- [x] `runtime/core/src/instance/` (new) + `runtime/core/src/bindings/Instance.{h,cpp}` -- the Instance seam and its binding: `create(target)`, `load(id, {src, sandbox})`, `focus(id)`, `setPaused(id, bool)`, `post(id, message)`, `setPlane(id, ...)`, `destroy(id)`, `capabilities()`, and `onevent(target, type, payload)` for `load`/`message`/`error`/lifecycle. Shaped like `Media.cpp`: `ChannelTarget`, `WeakObject`, `holdWork`, native state as the GC hook, serial-stamped events. The header documents the whole JS surface.
- [x] `runtime/core/include/screenkit/Runtime.h`, `runtime/core/src/hermes/HermesHost.cpp` -- `RuntimeConfig` gains the sandbox capabilities and the parent link; the installers refuse a gated binding the way an unavailable platform does (`NetServiceUnavailable`, `MediaPlayerUnavailable`), never a half-installed one.
- [x] `runtime/core/src/jsi/EventEmitter.cpp`, `runtime/core/src/bindings/HostIO.cpp` -- make the emitter tag and the asset root per-runtime; keep `setAssetRoot` write-once per instance and `confined()` untouched.
- [x] `runtime/host/Host.{h,cpp}`, `runtime/apple/HostMain.mm` -- `WindowState` holds the instance tree rather than one runtime: tick every live instance, pace on the focused one, route `reclaimRuntimeEvent` per queue, and terminate every instance before the host returns.
- [x] `runtime/core/include/screenkit/Input.h`, `runtime/core/src/input/Input.cpp`, `runtime/core/src/viewport/Viewport.cpp` -- input and `resize` follow focus: a settable target, held keys released on a switch, and the pause-epoch drop rule unchanged.
- [x] `runtime/js/dom-shim.js` -- `HTMLIFrameElement` as a backed element (`src`, `sandbox` as a token list, `contentWindow`, `onload`/`onerror`, `focus()`); `window.postMessage`, `MessageEvent.source`, `window.parent`/`top`/`frames`/`length`; `document.activeElement` following focus; `window` events `pause` and `resume` through a native `__screenkitLifecycle` in the `__screenkitKey` family (`suspend` and `memorywarning` belong to the deferred fourth state); the plane rect from `planeFor`.
- [x] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`, `runtime/tests/make-fixtures.mjs` -- `iframe-*` rows covering the matrix, a child package fixture, the absence pins updated, and the lifecycle proofs below. `tools/android/android.sh` and `tools/batocera/pi.sh` row lists updated so the device suites run them.
- [x] `Architecture.md` §5 and §12, `runtime/js/README.md`, `runtime/README.md`, `_bmad-output/implementation-artifacts/deferred-work.md` -- what shipped, the divergences, and what the sandbox does and does not promise.

**Acceptance Criteria:**
- Given a launcher page with an `<iframe>` whose `src` is a game package, when the page loads on macOS, the tvOS simulator, the Android emulator and the Pi, then the game renders inside the iframe's rect with the launcher's own UI around it (a screenshot on each).
- Given a focused child, when `parent.focus()` is called, then within one frame the child fires zero rAF callbacks and zero timer callbacks while the parent resumes -- measured, not asserted by eye (`Architecture.md` §14.7).
- Given a launch/remove loop of ten instances, when it finishes, then RSS is flat to within a per-instance heap and no GL object is left allocated (`Architecture.md` §14.7).
- Given a message posted in either direction, when it arrives, then it is a copy -- mutating the sender's object afterwards does not change the receiver's -- and the `source` can reply.
- Given a sandboxed instance without a capability token, when its page uses that API, then it gets the documented unavailable shape rather than a crash, and the refusal is logged once.
- Given a page with no iframe, when frames are presented, then the present path and its cost are what they were before this change.

## Implementation Notes

**The package gate moved into core** (`runtime/core/src/bundle/Package.{h,cpp}`), out of
`runtime/host/Host.cpp`. There are two callers now -- the host gating the app it was asked to run,
and an instance gating the package a launcher's `src` names -- and the matrix says "the existing
package gates decide", which is only true if it is the same code. Every message is byte-identical, so
the `host-refuses-package-*` rows still assert them; the one log line that named the host now takes
the caller's tag. `host::resolveLaunch` is a three-line forwarder.

**The host's own frame had to move into a texture.** The spec's "the composite pass runs at the host
runtime's frame end" is not enough on its own: after a swap the window's back buffer is undefined, so
a frame in which a child painted and the page did not has nothing of the page left to re-present.
`GlSurface::enableCompositing()` therefore turns the host's frame into an offscreen texture -- the
first time an instance is made, and only then -- and the present draws that texture and then the
layers over it. That is also what "add the FBO + present program to the Apple backend" is for: the
Apple backend had neither, because it had never needed an offscreen frame. `presentFrame` asks
`layersDirty()` before deciding to present, and that is false for a surface with no layers, which is
what keeps a page without an iframe on exactly its old path.

**Where each thread is.** `createInstance` makes the shared GL context on the *launcher's* JS thread,
because SDL shares only with the context current on the calling thread and there is nowhere else it
could be made (`GlSurface::createShared` on both backends, `adopt()` on the instance's thread
afterwards). Everything after that -- the package gate, the prelude, the asset root, the entry -- runs
on a short-lived bootstrap thread, because each of those blocks its caller and the caller would
otherwise be the launcher's JS thread.

**`pause` and the freeze are one task**, on the instance's own thread, in that order. A task queued
after the gate closed waits behind it and arrives on resume, which is precisely the burst of stale
callbacks `Paused` exists to prevent. `resume` is the other way round. The same applies to a launcher
freezing *itself* on `focus(child)`, which is what `InstanceBinding::freezeSelf` is.

**`Paused` had to stop the page without stopping the window.** The spec puts the composite at the
host runtime's frame end, and §5 has the launcher pause when the game it embeds takes the remote --
which, taken together, would run the game and show nothing, because a frozen host serves no frame.
`EventLoop::setPresentWhilePaused` is the answer: a runtime that owns layers keeps serving the
frame-finished hook while frozen and nothing else -- no queued task, no microtask checkpoint, no rAF
callback -- and it is off by default, so an instance's own pause still burns no wakeup. `iframe-focus`
counts presents from inside the present itself, which is the one place a frozen page's compositing
can be observed from another thread; removing the one line that turns it on fails the row.

**One race needed a design, not a lock.** `window.parent.focus()` runs on the instance's thread and
has to thaw a launcher that is frozen -- so it cannot be a task. But the launcher's freeze is a task,
and the two can land in either order: a thaw that arrives first would be followed by a freeze with
nothing left to open it. `InstanceBinding::focusRequest` is an atomic the child writes before it
thaws and the freeze task reads before it closes the gate, so the freeze skips itself when focus has
already come back. `iframe-focus` drives both orders.

**Two process-wide values became per-runtime**, both latent bugs the moment a second runtime existed:
`EventEmitter`'s log tag (an unsynchronised cross-thread write, as the Code Map said) and `HostIO`'s
asset root (which would have let either app read the other's package). Both are keyed by the runtime
pointer, as the vendored GL registry keys its contexts, and erased at that runtime's teardown, so a
reused address cannot inherit a dead runtime's confinement.

**Fences are resolved at run time**, through `eglGetProcAddress` or `SDL_GL_GetProcAddress`, never
named at link time: `glFenceSync` is ES 3.0, a Raspberry Pi 3's VideoCore IV is ES 2.0, and the
Android NDK's `libGLESv2.so` exports the ES 2 set alone. Where the driver has no fence the producer
flushes instead, which is the weaker ordering the spec allows. `glFinish` is nowhere in the frame
path.

**`DOMTokenList` was generalised over its attribute** so `sandbox` could be a real token list rather
than a string that looks like one. The owner record is `{node, name}` instead of a node; `classList`
passes `'class'` and reads exactly as before.

**Loads are coalesced into a microtask.** Appending the element and setting its `src` both ask for a
load, and a page does them in either order in the same turn; without the coalescing the first
instance was created and immediately torn down for the second. The plane already worked this way.

**What is deliberately not here**, each recorded in `deferred-work.md`: the `Suspended` state and the
memory-pressure LRU (no per-platform pressure signal, and no audit of which GL objects are
regenerable); an instance's layer resolution following a CSS resize; the `iframe-*` rows on device
(the device test library has no drawable and SDL has no offscreen surface); the RSS number in the
launch/remove proof; and the four-target screenshot check, of which macOS and the tvOS simulator have
run.

**Found in the step-3 audit, after the implementation returned** (its session ended
mid-cleanup, so these are the verifier's, not its own):

- *A deep message overflowed the stack instead of being refused* (`iframe-message`
  under `macos-asan`). The clone caps nesting at a depth meant to be reached before
  any engine's own stack guard is, and at 128 that was not true where a frame is
  several times its usual size: Hermes raised `RangeError` first, so the page saw an
  engine overflow where the matrix promises `DataCloneError`. Both caps -- the shim's
  and the native encoder's -- are 64 now, still far deeper than any real message.
- *A child that failed after it loaded never told the element.* The matrix has a row
  for it; no row covered it, and nothing implemented it: `loaded` fires exactly once,
  and a packed entry that throws or rejects is caught by the wrapper and reported
  afterwards, so the element heard `load` and nothing else. `Instance::tickFrame` now
  makes the same check the host makes on its own app every frame (`appFailed`), and
  reports it once through a new `InstanceEvents::failed`, which the binding posts as
  `error`. `iframe-child-fails` covers both timings.
- *Two matrix rows had no test at all* -- the one above, and a runtime with no
  compositor -- so `iframe-child-fails` and `iframe-no-compositor` were added and both
  device row lists left as they were, for the reason the implementation recorded.

**The review round, and what it changed.** Nineteen findings were real and are
fixed in place (`## Review Triage Log`); three were refuted and one could not be
settled without a device. Two of them were the kind only a review finds: a
`focus()` on an element whose load was refused crashed the launcher outright,
and a window resize while a child held the remote reached nobody -- the one
context that owns the window and lays out every plane was the one context not
told. Three more were latent rather than visible: a bootstrap that timed out
wrote into a dead stack, a never-adopted child surface unbound the launcher's
own GL context, and a throwing `pause` handler skipped the freeze it was part
of.

Two tests were also added for behaviour that shipped untested -- `iframe-input`,
which presses a key before, during and after a focus switch (and fails if
`InputRouter::target()` stops following focus), and the refusing half of
`allow-media` -- and two assertions in `iframe-sandbox` were retargeted, having
been satisfied by a message that merely listed the token names they matched.

**Verified after the fixes:** macOS 200/200, macos-asan 193/193, the tvOS
simulator builds and runs, Android 39/39 plain and 39/39 minified, and the
launcher screenshot re-taken on macOS, the tvOS simulator and the Android
emulator against the patched build. The Pi is the one gap: its host is a Docker
cross-build and the Docker engine would not start on this machine, so neither
`pi.sh test` nor the Pi screenshot has run against this change.

## Spec Change Log

- 2026-09-20 -- implemented. `Architecture.md` §5.1's table is amended to record that `Suspended` and
  the memory-pressure LRU are deferred, §5 records the three divergences the frozen intent decided
  (local-package `src`, one level of nesting, the structured-clone subset), §12's M9 row records what
  is met and what is not, and risk 10 and verification item 7 are updated rather than left claiming
  M9 closed them.

## Review Triage Log

Three layers ran against the staged diff: a blind hunter (14 findings), an
edge-case hunter (21) and a verification-gap reviewer (7 gaps plus 3 notes).
Every claim was checked at its cited line before a verdict was written; the gap
layer's findings arrive verified by its own evidence rules. Three claims were
refuted and one could not be settled. Nothing routed to intent_gap or bad_spec,
so there was no loopback.

**Fixed (patch).**

| # | Finding | Verdict | Evidence, and what was done |
|---|---|---|---|
| 1 | `focus()` crashes on an element whose load was refused or unloaded | high | Confirmed and reachable from page JS (`f.onerror = () => f.focus()`, or remove-then-focus): `focus` dereferenced `entry->instance` where `setPaused` and `post` both guard it, and an entry outlives its instance by design. All three dereferences are guarded now, and focusing an empty entry leaves the remote where it was. |
| 2 | A window resize never reaches the launcher while a child holds focus | high | Confirmed: the resize went to the focused runtime, so the one context that owns the window, lays out every plane in drawable pixels and holds the stale drawable heard nothing -- and the event was consumed, not deferred. `releaseStaleDrawables` ran on the wrong runtime for the same reason. It goes to the window's own runtime now, which also removes the stranded-flag case below. |
| 3 | A never-adopted child surface unbinds the launcher's GL context | high | Confirmed: `~GlSurface` unconditionally unbound the current context and called `eglReleaseThread()` on the *shared* display (`SDL_GL_MakeCurrent(window, nullptr)` on SDL), and an instance whose bootstrap failed is destroyed on the launcher's own JS thread. Both backends now touch the binding only when one of their own contexts is the one bound. |
| 4 | A timed-out graphics bootstrap writes into a dead stack | high | Confirmed: `onJsThreadBlocking` captured a local `std::string&` into a task that outlives it when the wait times out behind a freeze gate. The cell is shared now. |
| 5 | A throw in the load microtask leaves the element blank and holding work | high | Confirmed: `api.create`/`api.load` throw for a refused target, a shut-down layer or an unknown id, and nothing caught it -- an unhandled rejection, no `error` event, and `holdWork` already taken. The microtask now fails the element the documented way. |
| 6 | A malformed percent escape in `src` throws out of the same microtask | medium | Confirmed: `frameSource` guarded `new URL` but then called `decodeURI`, which throws on `%`. The path is handed over as written for the native side to refuse by name. |
| 7 | A throwing lifecycle handler skips the freeze | medium | Confirmed: `dispatchLifecycleEvent` caught `jsi::JSError` and `jsi::JSIException` but not `std::exception`, and both callers run `pause()` in the same task -- so an escaping exception left the instance running while the launcher believed it frozen. |
| 8 | A page that closed its last iframe wakes every frame while paused | medium | Confirmed: `setPresentWhilePaused(true)` was cleared only by `shutdown()`, against "set only while a page has layers" in `runtime/README.md` and `EventLoop.h`. Cleared when the last entry goes. (The offscreen-texture present itself is one-way by design -- `swap()` keys off `presentContext_` -- so only the wake is undone.) |
| 9 | A resize during compositing clobbers the page's texture bindings | medium | Confirmed: `makeFrameTarget` runs inside `swap()` on the app's context and left `GL_TEXTURE_BINDING_2D` and `GL_RENDERBUFFER_BINDING` at 0, so a framework that caches its own binding state (three.js does) draws the next frame against texture 0. Both are saved and restored now. |
| 10 | A key held over a focus switch is never released | medium | Confirmed for gamepad-derived keys: the outgoing context is frozen before the router notices the switch, so every synthesised key-up was dropped at the gate and again by the pause epoch -- a game that took the remote mid-press resumed with the key down. Releases now wait for that context to run again, which is the answer `gamepadsClaimed` already gives a paused page. |
| 11 | A fence nobody waits on leaks per closed iframe | medium | Confirmed: a layer removed before the host sampled its last frame left its `GLsync` alive until the share group died. `discardFence` frees it on the host's thread, which is in the same share group. |
| 12 | `f.src = ''` leaves the old app running | medium | Confirmed: neither an emptied nor a removed `src` did anything, so the package kept its thread, its layer and its place in `liveInstances`. HTML processes the attribute on every change and navigates away; this unloads. |
| 13 | A zero-sized iframe gets a full-drawable texture | medium | Confirmed: `sizeProperty` treated a given `0` as "not given" and substituted the whole drawable, so an invisible element was quietly expensive. A size that is given and is zero now gets the smallest layer there is. |
| 14 | An unpaired `resume` | low | Confirmed and reachable in one turn (`f.focus(); window.focus();`): the freeze cancels itself but the thaw dispatched `resume` regardless. Only a context that was actually frozen hears it. |
| 15 | "Said once" sandbox warnings said on every load | low | Confirmed: `allow-storage` and unknown-token warnings were plain logs inside the token loop, and a launcher re-parses the attribute on every `src` write. They are once per process now, as the header promised. |
| 16 | `setPlane` declares seven parameters and reads eight | low | Confirmed: `Function.length` read 7 while the body reads `args[7]` and the header documents eight. The arity matches. |
| 17 | `Instance::id()` is not the id the binding uses | low | Confirmed (documentation only -- `id()` has no callers): the two counters part company on the first reload or refused load. The comment says what the value is instead of what it is not. |
| 18 | `allow-background-audio` / `-timers` gate nothing | low | Confirmed: parsed and reported, read by nothing, and the freeze gate is all-or-nothing. `runtime/js/README.md` already recorded it; `screenkit/Instance.h`'s flatter wording now says the same. |
| 19 | The milestone row undercounts its own tests | low | Confirmed: `Architecture.md` said eleven `iframe-*` rows where thirteen (now fourteen) are registered, and claimed the four-target screenshot was two. Both corrected. |

**Verification gaps, from the gap layer:**

| # | Gap | What was done |
|---|---|---|
| 20 | Input never follows focus in any test -- the milestone's headline behaviour, revertible to a one-liner with everything green | `iframe-input` added: the shipping `InputRouter`, wired to the same focus target the hosts wire, presses a key before the switch, one while the game holds the remote, and one after it hands it back. Proven to fail when `target()` is reverted to the launcher. |
| 21 | Two of `iframe-sandbox`'s three diagnostic assertions were tautologies | Confirmed: `has(Warn, "allow-network")` and `has(Warn, "allow-storage")` were both satisfied by the unknown-token warning's own list of tokens. They name the wording of the message they mean now. |
| 22 | The refusing side of `allow-media` was never exercised | `iframe-sandbox` now embeds a second child without the token and asserts the child has no media binding at all. |
| 23 | The shipping host's instance wiring has no row | Deferred: the rows drive their own harness, so `Host.cpp`'s tick, focus wiring and `terminateInstances` are covered only by the manual launcher check. Recorded. |
| 24 | `iframe-two` cannot observe a layer-order error | Deferred: both children paint the same colour, so inverting the comparator changes nothing the row reads. Recorded with the shape of the fix. |
| 25 | Layer opacity is never read back | Deferred: no row sets `opacity`, so the eighth `setPlane` argument and the blend can stop working silently. Recorded. |
| 26 | Reload and re-insertion of an `<iframe>` are unverified | Deferred: no row writes `src` twice or re-appends a removed element, so the stale-serial filter -- the thing that keeps a launcher reusing one element from getting the wrong game's `load` -- is unpinned. Recorded. |

**Rejected.**

| # | Finding | Verdict | Why |
|---|---|---|---|
| 27 | `unload()` never calls `releaseWork()` | false | The hold is taken by `create` and released by `destroy`; `unload` deliberately keeps the entry for a re-insert. What is true is narrower -- a detached but still-referenced element keeps `idle()` false -- and is recorded as deferred. |
| 28 | `swap()`'s early return leaves the wrong context current | false | The return is reached only when `eglMakeCurrent` fails, which leaves the previously current context untouched; `presentFrame` ignores the result and retries. The destructor half is also wrong in consequence: by then no context is current and both are destroyed. |
| 29 | `executor()` can return null on the paths that chain off it | false | `RuntimeImpl::executor_` is set in the constructor and never cleared, and `RuntimeImpl` is the only implementation. The interface that can return null is `RuntimeControl::executor()`, which its callers already guard. |
| 30 | Fence detection trusts a non-null `GetProcAddress` | maybe-false | The concern is legitimate -- there is no version or extension gate -- but the stated consequence does not follow: `insertFenceOrFlush` flushes in both branches, so producer ordering is never lost. What would settle it is a device with an ES 2.0 driver reporting non-null `glFenceSync`; recorded as deferred rather than guessed at. |
| 31 | `makeFrameTarget` failure leaves an incomplete framebuffer bound | low | Confirmed as written, but it needs an OOM or an over-limit size, the sizes are not updated so it retries and self-heals, and every frame logs. Left as is. |
| 32 | The host paces on the launcher rather than the focused instance | low | Confirmed as a statement of fact, but inert: `painted` is set by whichever frame reached the screen, and a launcher holding an iframe is never `idle()` anyway, so the wrong term never decides anything. |
| 33 | `afterTask`'s microtask checkpoint is gated while paused | low | Confirmed that a pause landing inside a task defers that task's microtasks until resume. That is what "no queued task, no microtask checkpoint" in the frozen state means; the alternative is running page code after the gate closed. |
| 34 | `IFRAME_ROWS=""` is a dead variable | low | Confirmed dead, and deliberately so: both scripts carry the reason in a comment beside it, and `deferred-work.md` records the same. A comment that a future contributor can act on is worth more than a variable that does nothing. |
| 35 | `tickTree`'s clock goes backwards between helpers | low | Confirmed: a function-local `static` in one helper and locals in two others feed rAF timestamps that jump back. No fixture reads its rAF argument, so nothing observes it today; recorded as deferred rather than reworked mid-review. |
| 36 | The `queued_` resize flag can strand for ever | medium | Confirmed as filed, and fixed by finding 2: the resize now goes to the host's own runtime, which outlives every instance, so there is no terminated target to strand it. |


## Design Notes

**Where the composite runs.** `GlSurface`'s contract is that every GL call happens on the thread that owns the context (`GlSurface.h:34-42`), and the main thread only pumps SDL. So the compositor is not a main-thread pass: it runs at the *host* runtime's frame end, where `presentFrame` already stands, sampling each child's colour texture through the share group. The child's own `swap()` becomes "finish into my FBO and signal", not a window present.

**Why the parent still paints while a child runs.** `Paused` stops rAF and timers, but the compositor keeps drawing the paused instance's last texture -- which is what makes a launcher's frozen tile free (`Architecture.md:298`). A paused instance is not an invisible one.

**Which `sandbox` tokens mean anything.** `Architecture.md` §5 names `allow-media`, `allow-storage`, `allow-background-audio` and `allow-background-timers`. Of those, `allow-storage` gates nothing today -- the runtime has no storage API at all -- so it is accepted and reserved rather than dropped, and says so once when used. `allow-network` is added to the set, because networking is the capability an embedded app most obviously needs withheld and `RuntimeConfig` already reaches `installNet`. `allow-background-audio` and `allow-background-timers` are what a `Paused` instance is allowed to keep running; the freeze gate is all-or-nothing today, so they are what makes it selective. An unknown token is ignored with a one-time warning, as a browser ignores one.

**The order of layers.** Video planes are already beneath everything (`MediaPlayer.h:51`), and the shim already warns when a `z-index` would lift one above the canvas. Instance layers take the same rule: they sort among themselves and among canvases by `order`, and no instance layer goes below a video plane.

## Verification

**Commands:**
- `cmake --build runtime/build/macos && ctest --test-dir runtime/build/macos` -- expected: every row passes, `iframe-*` included
- `cmake --build runtime/build/macos-asan && ctest --test-dir runtime/build/macos-asan -R "iframe-|dom-|gl-"` -- expected: pass, no ASan reports
- `cmake --build runtime/build/tvos-simulator` -- expected: builds
- `sh tools/android/android.sh test` and `MINIFY=1 sh tools/android/android.sh test` -- expected: every row passes
- `sh tools/batocera/pi.sh test` -- expected: every row passes on the device
- `cd packages/@screenkit/cli && npm test` -- expected: pass (a manifest change lands on both sides)

**Manual checks:**
- A launcher page embedding a game on each of the four targets: the game inside the iframe's rect, the launcher around it, a remote press switching focus, and the launcher's tile showing the game's last frame while it is paused (a screenshot on each).
