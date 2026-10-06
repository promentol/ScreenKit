---
title: 'Composited canvases: every `<canvas>` its own GL context and its own layer'
type: 'feature'
created: '2026-09-20'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
followup_review_recommended: true
deferred:
  - summary: >-
      A SpiderMonkey build gets no `__screenkit.canvas` at all, so every second canvas
      takes the "the binding is missing" refusal.
    evidence: |-
      `installCanvas` is called only from `HermesHost::threadMain`, and
      `runtime/CMakeLists.txt` hard-fails a SpiderMonkey configure with tests on,
      so none of the canvas code is compiled or run on that engine. The port's own
      files predate this change (2026-09-19) and it is not a shipping target yet.
    location: >-
      runtime/core/src/hermes/HermesHost.cpp (installCanvas) vs runtime/core/src/spidermonkey/
    severity: low
  - summary: >-
      SpiderMonkey runtime init leaks its per-thread flag when a post-JS_NewContext step
      fails, and SCREENKIT_SM_NURSERY_KB is parsed without validation.
    evidence: |-
      Both are pre-existing in files this change does not touch; raised only because the
      baseline snapshot predated them and they rode along in the reviewed diff.
    location: >-
      runtime/core/src/spidermonkey/SpiderMonkeyRuntime.cpp:706; SpiderMonkeyEngine.cpp:58
    severity: low
  - summary: >-
      Frame-or-layer is decided when a canvas asks for its context, so the same elements and
      styles in a different call order give a different result.
    evidence: |-
      A canvas that takes the frame and is placed by CSS afterwards keeps the frame and warns
      once. Promotion would mean redirecting the page's own context to a layer framebuffer and
      separating "the drawable's size" from `gl.drawingBufferWidth`, which the shim reads for
      percentages, `<body>` and `window.screen`. Already recorded in deferred-work.md.
    location: >-
      runtime/js/dom-shim.js (getContext, placedCanvas)
    severity: low
  - summary: >-
      The two-canvas screenshot acceptance check ran on macOS only, and the SDL backends'
      canvas-layer path has never executed anywhere.
    evidence: |-
      No Android emulator or device in this session, and the Pi cross-build is blocked by the
      Docker engine failing to start. `canvas-composite` and `canvas-over-iframe` stand in by
      reading the same pixels out of the page's own framebuffer on macOS. The `<iframe>`-after-
      canvas context fix found in this review is exactly the class of defect only those targets
      can show.
    location: >-
      runtime/core/src/gfx/GlSurfaceSdl.cpp
    severity: medium
  - summary: >-
      `gl.getParameter` answers numbers where WebGL specifies booleans, for COLOR_WRITEMASK
      as well as BLEND.
    evidence: |-
      The vendored parameter path pushes the raw GLints, so `getParameter(COLOR_WRITEMASK)`
      reads `1 1 1 0` rather than `true,true,true,false`. Pre-existing and already recorded
      for BLEND in `canvas-isolation`; surfaced again while widening `canvas-placed`. A
      spec-conformance fix belongs with the vendored renderer, not with this change.
    location: >-
      runtime/third_party/gl/SKWebGLMethods.cpp (getParameter)
    severity: low
context:
  - '{project-root}/Architecture.md'
  - '{project-root}/runtime/js/README.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** A page gets one GL canvas. The second to ask for a context **throws**, naming a limit that no longer exists: "a second backed canvas needs the compositor, which does not exist yet" (`dom-shim.js`). It does exist -- M9 built it for `<iframe>` instances -- so a page still cannot put a HUD over a game, or a launcher's UI beside a preview, without an iframe and a whole second runtime to carry it.

**Approach:** Every canvas that takes a WebGL context gets its own real GL context in the host's share group and its own FBO-backed layer, composited at the element's CSS rect by the same `LayerList` an instance's layer goes through. The pieces are in place: the registry is keyed by context id and already holds several per runtime, `drawingBufferWidth/Height` are already per-context, and a same-thread producer needs no fence.

**Decisions (human, 2026-09-20):**
- **A real GL context per canvas**, not one context with a framebuffer each. Each canvas keeps its own GL state -- bound program, textures, blend -- as WebGL says it should, so two engines on two canvases cannot disturb each other. The cost is making a context current when the page moves between canvases, which is a comparison per GL call and a switch only where the page actually alternates.
- **A canvas with no CSS rect of its own is still the whole drawable**, exactly as today: a single-canvas page -- every app that exists now -- keeps the frame, the size behaviour and the present path it has. A canvas the CSS subset gives a position or a size to becomes a layer at that rect instead of warning that CSS moves no canvas.
- **A size write is honoured for a canvas that has its own layer**, and still ignored for the one that is the drawable. `canvas.width = 640` on a placed canvas gives it a 640-wide drawing buffer; on the fullscreen canvas it is accepted and changes nothing, with the warning it has today, because Lightning writes a size and then trusts `gl.drawingBufferWidth`.
- **WebGL canvases only.** A 2D canvas stays what it is -- CPU pixels and a texture source -- so an overlay drawn in 2D is still uploaded by the page. Compositing 2D canvases is recorded as deferred.
- **Layers are one list.** A canvas layer and an instance layer sort together, by `z-index` then insertion, so a page may put its HUD canvas over the game it embeds. Video planes stay beneath every layer, as `MediaPlayer.h` already requires.

## Boundaries & Constraints

**Always:**
- The first canvas of a page that has only one keeps today's contract to the letter: `canvas.width` reads `gl.drawingBufferWidth`, size writes are ignored with the same warning, the default framebuffer stays 0 and the present path is the one it has now. A page with one canvas must not pay for this feature.
- One real GL context per canvas, made current lazily in `gl::ContextGet` -- the seam our own `VendoredWebGL.cpp` implements and every vendored method already calls. Nothing in `runtime/third_party/gl/` is hand-edited; a change there is a rule in `tools/vendor/expo-gl.rules` and a re-run.
- Every canvas's GL work happens on the runtime's own JS thread. No fence: a same-thread producer publishes with `LayerSource::produced(texture, nullptr)`, which the compositor already accepts.
- Each canvas has its own paint flag. "Present only a frame that painted" stays true per layer, so an idle canvas keeps its last image and costs nothing.
- The CSS subset that already computes a plane rect (`planeFor`) is what places a canvas. No second geometry path.
- A canvas that loses its context or is removed releases its layer and its GL objects with it, and the page keeps running.

**Never:**
- A shared GL context between canvases, or any state that leaks from one canvas to another.
- Resizing the drawable, or letting a canvas layer change the window.
- 2D canvases drawing through GL; they stay CPU pixels and a texture source.
- Reviving the "one fullscreen drawable" rule anywhere it has been removed -- the divergence notes in `runtime/js/README.md` and `Architecture.md` are part of this change.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| One canvas, as today | a page with a single `<canvas>` and no CSS rect | the whole drawable, framebuffer 0, no compositor, the present path unchanged | N/A |
| A second canvas | `document.createElement('canvas').getContext('webgl')` | a second context and a layer; both canvases draw independently | N/A |
| State isolation | canvas A binds a program and a texture, canvas B binds its own | each keeps what it bound; neither sees the other's state | N/A |
| Placed by CSS | `style.cssText = 'position:absolute; left:10%; top:10%; width:50%; height:50%'` | the canvas composites at that rect, and `canvas.width` is that rect's pixels | N/A |
| Z-order with an iframe | a HUD canvas at `z-index: 2` over an instance at `1` | the canvas draws over the instance | N/A |
| Z-order with video | any canvas layer over a `<video>` plane | the plane stays beneath; the existing warning still fires for a canvas sent below it | N/A |
| An idle canvas | canvas A paints this frame, canvas B does not | B keeps its last image; the frame still presents | N/A |
| Removal | `canvas.remove()` and dropped | its layer goes, its GL objects go, the page keeps painting | N/A |
| Context loss on teardown | the runtime shuts down with three canvases | every context and layer is released, in the owning thread, before shutdown returns | N/A |
| Probe canvases | an engine's `isWebGLSupported` makes a throwaway canvas, asks, drops it | it gets a real context; dropping it frees it, and no layer is left behind | N/A |
| Too many | a page that asks for more contexts than the driver will give | the failing `getContext` returns null, as the web says, and names the limit once | `getContext` answers null rather than throwing |
| Zero-sized canvas | a canvas whose CSS rect computes to zero | no layer is composited; nothing is allocated for it | N/A |
| Size write, placed canvas | `canvas.width = 640` on a canvas with its own layer | its drawing buffer becomes 640 wide and `gl.drawingBufferWidth` agrees | N/A |
| Size write, the drawable's canvas | `canvas.width = 640` on the one fullscreen canvas | accepted, ignored, warned once -- exactly as today | N/A |
| A 2D canvas | `getContext('2d')` on a second canvas | CPU pixels and a texture source, as now; no layer | N/A |

</frozen-after-approval>

## Code Map

**The seam that makes a context per canvas cheap.** Every vendored GL method resolves its context through `CTX()` -> `method::getContext` -> `gl::ContextGet(id)` (`runtime/third_party/gl/SKWebGLMethodsMacros.h:24-27`, declared `SKGLContextSeam.h:44`), and `ContextGet` is implemented in our own `runtime/core/src/gfx/VendoredWebGL.cpp:711`. Making the right GL context current belongs there: an id comparison per call, a `makeCurrent` only when the page moves between canvases. Nothing in `runtime/third_party/gl/` is touched.

- `runtime/core/src/gfx/VendoredWebGL.cpp` -- `Registry` (`:51-59`) keyed by context id, `Entry{runtime, SKGLContext, GlSurface}` (`:38-47`); `installVendoredWebGL` (`:794`) allocates the id (`:817`), sets `context->defaultFramebuffer` from the surface (`:830`), **overwrites the `gl` global** (`:831-846`) and **overwrites `__screenkitGLReaper`** (`:855-858`) -- both are why it may only be called once today; `surfaceFor` (`:859-866`) returns the first entry for a runtime and is ambiguous with two (its caller is `bindings/Instance.cpp:516`); `presentFrame` (`:877-912`) already flushes every context of the runtime and gates on `painted || layersDirty()`; `refreshDefaultFramebuffer` (`:868`), `releaseStaleDrawables` (`:915`) and `releaseVendoredWebGL` (`:930`) already iterate per runtime; `installDrawingBufferSize` (`:753`) is the pattern for a per-context property from outside, and `drawingBufferWidth/Height` (`:722-751`) already read the entry's own surface.
- `runtime/core/src/gfx/GlSurface.{h,cpp}`, `GlSurfaceSdl.cpp`, `GlSurfaceLayers.cpp`, `GlSurfaceTarget.h` -- all hand-written (nothing under `core/src/gfx` is vendored). `Desc::shareWith` (`GlSurface.h:68`) and `createShared` are what an instance's layer surface is made with, on the creating thread, `adopt()`ed on the using one; `defaultFramebuffer()` (`:172`); `enableCompositing` and `setLayers` are how the host's frame became a texture; `~GlSurface` now touches the thread's binding only when one of its own contexts is bound.
- `runtime/core/src/compositor/Compositor.{h,cpp}` -- `LayerSource::produced(texture, fence)` with a null fence for a same-thread producer (`Compositor.h:51-53`), `Layer{rect, opacity, order, visible, sequence}` (`:76-86`), `LayerList::set/remove` keyed by `uint64_t` keeping insertion order (`:94-115`), `paintOrder()` skipping sources with no texture (`Compositor.cpp:178-194`), `CompositePass::draw` (`:250-292`). The list is owned by the instance binding today (`bindings/Instance.cpp:148`, installed at `:528`) and has to be shared with the canvas path rather than duplicated.
- `runtime/js/dom-shim.js` -- `canvasStates` (`:98`), `defineBackedSize` (`:104-132`, the size-write refusal at `:120-123` and its message at `:88-94`), `drawableWidth/Height` (`:40-55`), `backedCanvas` (`:2127`), `getContext` (`:2130-2205`: the 2D refusal `:2137`, the probe hand-off `:2174-2176`, the claim `:2180-2194`, **the throw `:2198-2205`**), `makeCanvas` (`:2210-2231`), the 2D path `make2dContext`/`resizeContext2d` (`:2391-2412`), the CSS-subset canvas refusal `divergence` (`:1699-1710`, with `:1521-1529` and `:1640`), `planeFor`/`updatePlane`/`planeChanged` (`:7384-7470`, including the video-above-canvas warning at `:7448-7457` that needs a per-canvas answer), and the paint tracker wired to `global.gl` alone (`:9306-9331`).
- `runtime/core/src/bindings/Instance.cpp` -- `setPlane`'s rect wire format (`:654-685`, zero-area forces `visible` false at `:673`) and the id space of `LayerList`; a canvas layer reuses the shape and must not collide with instance ids.
- Tests and docs that pin the old rule: `runtime/tests/RuntimeTests.cpp:1404-1439` (`dom-second-canvas`, asserting the throw's wording), `runtime/tests/fixtures/dom-canvas.js:76-77`, `RuntimeTests.cpp:1853-1865` (canvas size == drawable), `:9370-9388` (`iframe-none-costs`: framebuffer 0 and no dirty layers for a page with no iframe -- a one-canvas page must still pass it); `runtime/js/README.md:86-101`, `:195-205`, `:884-894`; `Architecture.md:143`, `:153-156`, `:849`.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/core/src/gfx/VendoredWebGL.{h,cpp}` -- more than one context per runtime: `installVendoredWebGL` returns a context handle instead of claiming the `gl` global, one reaper per context, `surfaceFor` answers for a named context (its instance caller wants the page's frame surface), and `ContextGet` makes the requested context current when it is not already -- the one place a canvas switch costs anything.
- [x] `runtime/core/src/gfx/GlSurface.{h,cpp}`, `GlSurfaceSdl.cpp` -- a layer surface made and adopted on the same thread, for a canvas that is not the frame; its size follows the canvas rather than the window.
- [x] `runtime/core/src/compositor/` + `runtime/core/src/bindings/` -- one `LayerList` shared by canvas layers and instance layers, with an id space that cannot collide, and a canvas layer published with a null fence.
- [x] `runtime/js/dom-shim.js` -- `backedCanvas` becomes a per-canvas record; `getContext` hands every canvas its own context; `defineBackedSize` keeps the drawable's size for a canvas with no rect of its own and takes the layer's otherwise; the CSS subset places a canvas instead of warning; each canvas has its own paint flag; `getBoundingClientRect` reports the placed rect.
- [x] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`, `runtime/tests/fixtures/dom-canvas.js` -- `canvas-*` rows covering the matrix, including the pixels of two canvases composited in z-order and the state-isolation case; `dom-second-canvas` becomes the row that proves a second canvas works; `iframe-none-costs`'s one-canvas assertions stay green.
- [x] `runtime/js/README.md`, `Architecture.md` (3.1, 12's M6 row), `_bmad-output/implementation-artifacts/deferred-work.md` -- the one-canvas rule replaced by what is true now, and whatever this spec defers.

**Acceptance Criteria:**
- Given a page with a game canvas and a HUD canvas above it, when it runs on macOS, the tvOS simulator, the Android emulator and the Pi, then both are on screen in z-order with the HUD's transparent pixels showing the game through (a screenshot on each).
- Given two canvases, when one binds a program and a texture and the other binds its own, then neither sees the other's state -- asserted by reading it back, not by drawing.
- Given a page with exactly one canvas and no CSS rect, when it presents, then the default framebuffer is 0, no compositor is built and the frame cost is what it was before this change.
- Given a canvas removed from the document and collected, when the next frames present, then its layer is gone and its GL objects are freed -- RSS flat across a make/remove loop.
- Given an engine's feature probe that makes a canvas, asks for a context and drops it, when it then makes the canvas it draws on, then that canvas gets a context and no layer is left behind.

## Implementation Notes

**What each task became.**

- `gfx/VendoredWebGL.{h,cpp}`: `installVendoredWebGL` returns a `GlContextId` and takes `asGlobal`,
  so the page's frame still claims `gl` and a canvas layer does not. The registry gained a
  per-runtime `RuntimeState{primary, layers}` beside the per-context map, a `thread_local`
  "which context is current", `makeContextCurrent`, `layersFor`, `enableCompositing`,
  `takeContextObject`, `releaseContext` and `contextCount`. `ContextGet` compares the id it is asked
  for against the current one and calls `GlSurface::makeCurrent` only when they differ -- the one
  place a page pays for having several canvases. One reaper per context lives in
  `__screenkitGLReapers[id]` rather than a single `__screenkitGLReaper` the next install would
  overwrite. `surfaceFor(runtime)` answers for the primary rather than for whichever entry the hash
  map happened to yield first; `surfaceFor(runtime, id)` is the named form.
- `presentFrame` publishes each canvas layer that painted (`__screenkitLayerPainted[id]`, read
  outside the registry lock because reading it runs JS), then makes the page's own context current
  before the page's frame is drawn and presented. `releaseStaleDrawables` now touches the primary
  alone -- a canvas layer has no drawable to let go of.
- `gfx/GlSurface.{h,cpp}`, `GlSurfaceSdl.cpp`, `GlSurfaceLayers.cpp`: `createShared`/`adopt` already
  made exactly the surface a canvas layer needs, so what was added is `resizeLayer` (shared, in
  `GlSurfaceLayers.cpp`: the framebuffer and texture names survive, so WebGL's `null` binding and the
  compositor's texture do too) and `setLayerSource(source, sameThread)` -- a canvas produces on the
  thread that composites it, so it publishes with `glFlush` and a **null fence**, as the spec
  requires.
- `compositor/Compositor.h` gained `kCanvasLayerBase` (`1 << 62`): the instance binding counts its
  ids from 1 and a canvas layer is keyed by its GL context id, so the two spaces cannot meet. The one
  `LayerList` now lives in the GL registry (`gfx::layersFor`) and both bindings take it from there;
  `bindings/Instance.cpp` no longer makes its own and goes through `gfx::enableCompositing`.
- `bindings/Canvas.{h,cpp}` is new: `__screenkit.canvas` with `create`/`setSize`/`setPlane`/`release`,
  a `CanvasHandle : jsi::NativeState` on the element so the collector releases the context, and the
  same rect wire format `setPlane` has for an instance. Installed for every runtime in
  `HermesHost::threadMain` and shut down just before `releaseVendoredWebGL`.
- `js/dom-shim.js`: `canvasStates` entries carry the context, its id, the plane and whether a script
  has written a size; `getContext` decides frame-or-layer once, when it is asked; `defineBackedSize`
  reads and writes the layer's own drawing buffer for a placed canvas and keeps the drawable's for
  the frame; the CSS subset places a canvas instead of warning (the frame's canvas still warns, and
  says how to get a layer); `getBoundingClientRect` reports the placed rect; `trackPaint` became a
  function called once per context; and the video-above-canvas warning asks every canvas in the
  document rather than the one that holds the drawable.

**Two things the plan did not foresee.**

1. *The vendored snapshot had to go.* `createWebGLRenderer` set `drawingBufferWidth`/`Height` as data
   properties. Since M4 they are getter-only accessors on the prototypes (live, because the surface's
   size is), and upstream's own `delete` of the data properties hid the conflict while there was one
   context. With two, the accessors are already installed when the second context is built and the
   data write throws out of the middle of `createWebGLRenderer`. Fixed where the spec says to fix the
   vendored tree: a `patch` row in `tools/vendor/expo-gl.rules` and a re-run (`(void)viewport;`), with
   the now-pointless `deleteProperty` removed from `installDrawingBufferSize`.
2. *`__SKGLContexts[id]` is a root.* Upstream parks every context object on the global for the life of
   the runtime, which makes "dropping the canvas frees it" impossible -- the context holds
   `gl.canvas`, which holds the element. Nothing reads that map (every vendored method resolves
   `this.contextId` through `ContextGet`), so `takeContextObject` unparks a canvas layer's context and
   hands it to the shim, leaving the canvas, its context and its layer one collectable group. The
   page's own `gl` keeps its slot. `canvas-remove` fails without this.

**Where it stops.** Which of the two a canvas gets is decided when it asks for its context, so a
canvas that took the frame and is placed *afterwards* keeps the frame and says so once. Promotion
would mean redirecting the page's own context to a layer framebuffer and separating "the drawable's
size" from `gl.drawingBufferWidth`, which the shim reads for percentages, `<body>` and
`window.screen`. Recorded in `deferred-work.md`, with compositing a 2D canvas, a second canvas inside
an `<iframe>` instance, the three unrun targets and the missing switch-cost measurement.

**Matrix audit.** Every row is covered by a row that ran and passed. Three had no test and were
added: `canvas-over-video` (z-order with video -- a scripted player, because the geometry is what is
under test; it proves `aboveACanvas` now walks every canvas that is a layer, not just the drawable's),
`canvas-2d-no-layer` (a 2D canvas: pixels and a texture source, no context, no layer), and
`canvas-refused` (too many: `create` answering null, `getContext` returning null rather than throwing,
and the reason said once however many canvases ask). The rest map to `canvas-isolation`,
`canvas-placed` (which also carries both size-write rows and the zero-sized rect), `canvas-composite`,
`canvas-idle`, `canvas-remove`, `canvas-teardown`, `canvas-over-iframe`, `dom-second-canvas` (a second
canvas, and the probe) and `iframe-none-costs` (one canvas, as today: no compositor, framebuffer 0).

Two rows are honoured in spirit rather than to the letter, and the tests say which:

- *Zero-sized canvas* -- "nothing is allocated for it" is not what happens. A rect that computes to
  zero still gets a context and a 1x90 framebuffer (`Math.max(1, ...)`, because a framebuffer of no
  size is not complete), but `planeFor` marks it invisible and the compositor skips it, so nothing is
  composited and nothing is drawn. Allocating nothing would mean deferring the context until the rect
  grows, which is a promotion path this spec does not have.
- *Probe canvases* -- "dropping it frees it" is not how it works, and the difference matters. A probe
  run **before** any canvas has taken the frame is handed the page's own context, so it allocates
  nothing to free at all: `dom-second-canvas` asserts `contextCount() == 1` across it, which is the
  case every engine actually hits (Pixi's `isWebGLSupported` and Phaser's `Features.webGL` both probe
  at startup). A probe run **after** the frame is taken gets a context of its own, like any other
  second canvas, and gives it back when the collector takes the element -- which is also what a
  browser does with a detached canvas. The review flagged the first sentence of this note as
  overstated; it was.

One thing the audit turned up that is not a canvas matter: in a host with no graphics bootstrap at
all, `getContext('webgl')` throws rather than returning null. That is the answer it had before this
feature and the right one -- a misconfigured host is a developer error, where a driver that will not
give one more context is a condition the web says to answer with null. `canvas-refused` now pins both.

**What the review changed.** Fourteen patches, and two of them are the kind only a second pass finds:

- *The thread-local said "the page" while the driver said "nothing".* `GlSurface::adopt` binds its own
  context directly -- it has to, it is below the registry -- so a canvas whose `adopt` failed left the
  driver unbound while `gfx`'s bookkeeping still named the page, and the restoring
  `makeContextCurrent` believed itself and skipped the bind. `forgetCurrentContext()` is the seam that
  lets a caller say "I no longer know", and every post-`createShared` refusal path says it.
- *A second `<iframe>` after a canvas had drawn would have failed on Android and the Pi, and only
  there.* `Instance.cpp` restored the page's context before building the compositor, but that is
  inside `if (!b->compositing)` -- so the *second* embed ran with whatever a canvas had left current,
  and SDL shares only with the current context (`SDL_GL_SHARE_WITH_CURRENT_CONTEXT`), with
  `createShared` refusing outright when it is not the parent's. macOS shares explicitly and cannot
  show it. The restore now runs for every embed. This is the clearest evidence for the deferred item
  that says the SDL canvas path has never executed.

The rest: the released id is dropped from `__screenkitLayerPainted` too; the reaper clears the
thread-local before destroying a context; a layer's paint flag is cleared only once its frame is
really going out; `resizeLayer` saves the depth and stencil write masks as well (a page holding
`glDepthMask(GL_FALSE)` was getting the uncleared depth buffer the clear exists to prevent) and puts
the old buffer back when a resize fails; `enableCompositing` clears the page's new frame texture on
both backends, **and restores every piece of state it touched** -- the first cut of that fix clobbered
the page's own clear colour, which `canvas-isolation` caught within the hour; `idArg` and the plane's
`order` are range-checked rather than cast (undefined behaviour on a `1e300` from page script); the
8192 cap says so once; a refused canvas remembers it, instead of re-running the native allocation on
every `getContext` from a render loop; `setAttribute('width')` reflects into the property, as on the
web; the nested refusal says what it means for a canvas rather than passing the `<iframe>` wording
through; and `__screenkit.canvas.release(id)` is gone -- it had no caller and page script could have
used it to pull a context out from under a live handle.

Seven rows were strengthened to close the verification gaps: a layer's plane now has to follow CSS
that changes *after* `getContext` ran (every row until now set its CSS first), the page going idle
while a canvas keeps painting is exercised through the prelude's `paintPage` knob, `canvas-placed`
asserts the framebuffer binding, colour mask and scissor survive a resize rather than only the three
pieces of state that happened to be at their defaults, a half-transparent canvas pins the
compositor's alpha uniform, an `<iframe>`'s layer has to follow a *removed* declaration,
`dom-second-canvas` pins the probe contract that a deleted assertion had left open, and
`canvas-remove` runs five create/drop cycles instead of one.

## Spec Change Log

## Review Triage Log

### 2026-09-20 — Review pass
- verdicts: 48 findings — high 0, medium 6, low 28, false 5, maybe-false 0 (plus 9 rows routed defer/reject on scope); the last two were found while patching, by the strengthened tests
- findings:
  - `[medium]` `[patch]` `create`'s failure paths leave the thread with no GL context current — confirmed: `GlSurface::adopt` binds with `eglMakeCurrent`/`SDL_GL_MakeCurrent` itself and never tells the registry, so after a failed adopt `~GlSurface` unbinds while `t_currentContext` still names the page, and the restoring `makeContextCurrent` short-circuits on its own bookkeeping. Fixed by adding `gfx::forgetCurrentContext()` and calling it on all three post-`createShared` refusal paths.
  - `[low]` `[patch]` `releaseContext` deletes the released id from `__SKGLContexts` and `__screenkitGLReapers` but not `__screenkitLayerPainted` — confirmed as a key leak; the reviewer's "walks them all every frame" half is wrong, since `publishCanvasLayers` iterates registry ids and only looks the flag up. Fixed by adding the third map to the delete loop.
  - `[low]` `[patch]` the `ContextReaper` backstop erases a context without clearing `t_currentContext` — confirmed; ids never repeat so the window is narrow, but `makeContextCurrent(thatId)` would answer true for a destroyed context. Fixed with a one-line clear before the erase.
  - `[low]` `[patch]` `resizeLayer` saves the colour mask and scissor but not the depth/stencil write masks, so a page holding `glDepthMask(GL_FALSE)` gets the uncleared depth buffer the clear exists to prevent — confirmed by reading the save list. Fixed: both masks saved, forced open for the clear, restored (back face included).
  - `[medium]` `[patch]` a failing `resizeLayer` leaves the surface describing a texture it no longer has — confirmed: `makeFrameTarget` reallocates in place and the size fields are only updated after it returns. Fixed by re-running it at the old size on failure and saying so in the error when even that fails.
  - `[low]` `[patch]` `__screenkit.canvas.release(id)` has no caller — confirmed by grep across `runtime/js`; it is page-reachable and could pull a context out from under a live handle. Fixed by deleting it; the element's native state remains the only release path, and `Canvas.h` now says why.
  - `[low]` `[patch]` a refused canvas is retried on every `getContext` — confirmed: `state.gl` and `state.kind` stay null, so an engine asking from its render loop re-runs the native allocation every frame while the message is said once. Fixed with `state.refused`.
  - `[low]` `[reject]` `state.connected` is written for canvases and never read — the claimed cost does not follow: `updateCanvasPlane` early-returns on an unchanged plane, so the extra scheduling is a no-op microtask, and removing the field is cosmetic.
  - `[low]` `[patch]` the 1..8192 drawing-buffer clamp is silent, against the shim's own "a gap says so, once" rule — confirmed. Fixed: `sizeArg` now warns once when it caps.
  - `[low]` `[reject]` one `state.sized` flag for both dimensions, so writing `canvas.width` alone stops `canvas.height` following the rect — real but narrow, and per-dimension tracking is more than a direct correction; the README documents the rect-following default.
  - `[low]` `[patch]` the probe-canvas contract changed and the assertion that guarded it was deleted — the behaviour matches a browser (a detached canvas gets a real context), but the contract was left unpinned. Routed to the test layer, which re-pins it in `dom-second-canvas`.
  - `[low]` `[patch]` a second canvas inside an `<iframe>` is refused with a message about iframes — confirmed: the nesting refusal is passed through verbatim. Fixed by adding `GlSurface::isLayer()` and saying what it means for a canvas.
  - `[low]` `[defer]` ~1,750 lines of an unrelated SpiderMonkey JSI port ride along in the diff, and `installCanvas` is wired only from `HermesHost` — the files are dated 2026-09-19, before this change; they are in the diff only because the baseline snapshot predates them. Not this story's problem.
  - `[medium]` `[patch]` (verification gap, pre-verified) a canvas layer's plane is never re-checked after its CSS changes by any test: every canvas row sets CSS before `getContext`. Routed to the test layer.
  - `[medium]` `[patch]` (verification gap, pre-verified) the page's own frame going idle while a canvas layer keeps painting is unexercised — the prelude's `paintPage` knob is never set false. Routed to the test layer.
  - `[low]` `[patch]` (verification gap, pre-verified) `<iframe>` layers now follow a removed style declaration and no test observes it. Routed to the test layer.
  - `[medium]` `[patch]` (verification gap, pre-verified) `resizeLayer`'s state restore is only half asserted — three of the five saved pieces of state are restored into values they already had. Routed to the test layer.
  - `[low]` `[patch]` (verification gap, pre-verified) a canvas layer's opacity is asserted nowhere, so the compositor's alpha uniform has no test behind it. Routed to the test layer.
  - `[low]` `[patch]` `release(id)` unreachable — same root cause as the row above; shares its fix.
  - `[low]` `[reject]` `create` enables compositing before it tries to make the layer, so a refusal leaves the page on the compositing present path — real, but it needs a driver failure to reach and undoing it needs a `disableCompositing` this spec does not have.
  - `[low]` `[reject]` `canvas.width = 0` on a placed canvas reads back 1 — a framebuffer of no size is not complete; recorded as a deviation in the Implementation Notes rather than fixed.
  - `[medium]` `[patch]` `create`'s failure paths leave nothing current (edge-case layer, same defect as the first row) — shares its fix.
  - `[low]` `[reject]` `release` when `primaryContext` is already 0 leaves nothing current — teardown-only, and `makeContextCurrent(0)` returns false so the next `ContextGet` binds for real.
  - `[low]` `[patch]` a finite id beyond `unsigned` range is undefined behaviour on the cast — confirmed, and `__screenkit` is page-reachable. Fixed with an upper bound in `idArg`.
  - `[low]` `[patch]` a `setPlane` order outside `int` range is the same undefined behaviour — fixed by clamping rather than casting.
  - `[false]` `[reject]` `takeContextObject` answering undefined would leak the context — `installVendoredWebGL` parks the object immediately before, in the same call, so `__SKGLContexts[id]` exists by construction.
  - `[medium]` `[patch]` a second `<iframe>` embedded while a canvas layer's context is current fails on SDL — confirmed twice over: `createShared` refuses outright unless the parent's context is current, and SDL shares only with the current context. macOS cannot show it, which is exactly why it survived. Fixed by restoring the page's context before every embed, not only the first.
  - `[medium]` `[patch]` a failing `resizeLayer` leaves a size mismatch (edge-case layer, same defect) — shares its fix.
  - `[low]` `[patch]` the reaper leaves a destroyed id in `t_currentContext` (edge-case layer, same defect) — shares its fix.
  - `[low]` `[patch]` the paint flag is cleared before `swap()` is known to have run, so a frame whose context could not be made current is dropped for good — confirmed. Fixed by clearing after the publish.
  - `[low]` `[patch]` compositing is enabled without clearing the page's new frame texture, so a present before the page's next paint shows undefined pixels — confirmed by comparison with `adopt`, which clears for exactly this reason. Fixed on both backends.
  - `[low]` `[reject]` `releaseStaleDrawables` skips a runtime whose primary is 0 — only a partly torn-down runtime has no primary, and it has no drawable left to release.
  - `[low]` `[reject]` a canvas that lost the frame keeps handing the frame context out — it hands back the same object it was legitimately given, and the frame is the drawable either way; guarding it adds a branch for a case no engine reaches.
  - `[medium]` `[patch]` `setAttribute('width'|'height')` on a layer canvas does nothing — confirmed: `attributeChanged` routes width/height for `<video>` and `<iframe>` but not for a canvas, and on the web the content attribute reflects into the IDL one. Newly load-bearing now that a layer canvas honours size writes. Fixed by mirroring to the property.
  - `[low]` `[defer]` SpiderMonkey runtime teardown leaks a thread flag on a failed init — pre-existing, in a file this change does not touch.
  - `[low]` `[defer]` `SCREENKIT_SM_NURSERY_KB` is parsed without validation — same, pre-existing.
  - `[low]` `[patch]` the deleted `domSecondCanvas` probe assertion left the contract unpinned (edge-case layer, same as the blind layer's row) — shares its fix.
  - `[false]` `[reject]` `InstanceBinding::layers` lost its initialiser and could be null — `installInstances` assigns it from `gfx::layersFor` at line 404 and every use is in a member function that can only run afterwards.
  - `[low]` `[reject]` the note's claim that a probe "allocates nothing to free" holds only while the frame is untaken — correct, and the Implementation Notes have been corrected to say so; as a defect it is none, since a browser also gives a detached canvas a real context.
  - `[low]` `[patch]` the "RSS flat across a make/remove loop" claim is not measured anywhere — `canvas-remove` makes and drops one canvas. Routed to the test layer as a five-cycle loop.
  - `[false]` `[reject]` the intent's "instead of the single fullscreen canvas" is not implemented — the frozen decision reads "A canvas with no CSS rect of its own is still the whole drawable, exactly as today", so the diff implements the approved reading, not an overlooked one.
  - `[low]` `[defer]` frame-or-layer is decided at `getContext` time, so the same page in a different call order gets a different answer — already recorded in `deferred-work.md` as the promotion path this spec does not have.
  - `[low]` `[patch]` the size clamp is undocumented (intent layer, same as the blind layer's row) — shares its fix.
  - `[low]` `[reject]` `canvasLayerCount` is incremented and never decremented — the counter gates cheap early-outs only, and decrementing needs a release signal the shim is never given (the context goes with the element, on the collector's thread).
  - `[false]` `[reject]` a zero-area rect allocates a context and a 1px framebuffer against the spec's "nothing is allocated" row — verified as deliberate and already recorded as a deviation in the Implementation Notes; the compositor skips the layer.
  - `[medium]` `[patch]` (found while patching, by the test layer) `enableCompositing` clobbers the page's own clear colour and viewport — the clear this pass added for the undefined-frame-texture finding above ran on a context the page is already drawing with, and unlike `resizeLayer` it saved nothing. `canvas-isolation` caught it: the page's `COLOR_CLEAR_VALUE` went `0 0 1 1` -> `0 0 0 0` the moment the first canvas took a layer. Fixed by snapshotting and restoring viewport, clear colour, colour mask, both write masks and the scissor enable on both backends, exactly as `resizeLayer` does.
  - `[low]` `[defer]` `gl.getParameter(COLOR_WRITEMASK)` answers numbers where WebGL specifies booleans — found while widening `canvas-placed`; the same deviation `canvas-isolation` already records for `BLEND`. Pre-existing in the vendored parameter path, not caused by this change.
  - `[low]` `[defer]` the spec's own acceptance criterion — a two-canvas screenshot on tvOS, Android and the Pi — is met on macOS alone, and the SDL canvas-layer path has never executed. Already deferred; the Android emulator and the Pi cross-build are both unavailable in this session.

## Design Notes

**Why the switch is cheap.** `ContextGet` already runs on every vendored GL call to find the context. It gains a comparison against the id that is current and, when they differ, one `makeCurrent`. A page with one canvas never switches; a page with two switches where it alternates, which is once per canvas per frame for any engine that draws its canvas in one pass.

**Why the first canvas stays the frame.** Every app that exists draws into one canvas the size of the screen, reads `gl.drawingBufferWidth` for it and never positions it. Making that canvas a layer would change its size semantics, add a composite pass and a texture to every page that has no use for one. A canvas becomes a layer only when the page asks for one by placing it.

## Auto Run Result

**What was built.** Every `<canvas>` a page places with CSS now gets a real GL context of its own in
the page's share group and its own compositor layer at its rect, sorted through the same `LayerList`
an `<iframe>` instance's layer goes through. The first canvas a page asks for -- the one CSS has not
placed -- is still the whole drawable, framebuffer 0, no compositor: a page with one canvas pays
nothing for any of this. A placed canvas honours `canvas.width`; the drawable's canvas still does not.
2D canvases stay CPU pixels and a texture source, with no layer.

**Files changed**

| File | What changed |
|---|---|
| `runtime/core/src/bindings/Canvas.{h,cpp}` | new: `__screenkit.canvas` (`create`/`setSize`/`setPlane`), a `CanvasHandle` native state that releases the context when the element is collected |
| `runtime/core/src/gfx/VendoredWebGL.{h,cpp}` | per-runtime primary + layer list, a thread-local current context, `makeContextCurrent`/`forgetCurrentContext`, `takeContextObject`, `releaseContext`, per-layer publish in `presentFrame` |
| `runtime/core/src/gfx/GlSurface.{h,cpp}`, `GlSurfaceSdl.cpp`, `GlSurfaceLayers.cpp` | `resizeLayer`, `setLayerSource(source, sameThread)`, `isLayer()`, and a state-preserving clear of the page's new frame texture |
| `runtime/core/src/compositor/Compositor.h` | `kCanvasLayerBase` so canvas and instance layer ids cannot collide |
| `runtime/core/src/bindings/Instance.cpp` | one shared layer list; the page's context restored before **every** embed |
| `runtime/core/src/hermes/HermesHost.{h,cpp}` | installs and shuts down the canvas binding |
| `runtime/js/dom-shim.js` | per-canvas state, frame-or-layer at `getContext`, CSS places a canvas, per-context paint tracking, `setAttribute` sizing, remembered refusals |
| `runtime/tests/RuntimeTests.cpp`, `CMakeLists.txt` | ten `canvas-*` rows, seven existing rows strengthened |
| `tools/vendor/expo-gl.rules` (+ generated `third_party/gl`) | drops upstream's `drawingBufferWidth` data-property snapshot, which broke the second context |
| `runtime/js/README.md` | the canvas section: layers, sizes, lifetime, the 8192 cap |

**Review findings.** 48 findings from four layers. 14 patched, 7 deferred, 12 rejected, 5 disproved;
the rest were duplicates of a patched entry and share its fix. Patched by verdict: **6 medium, 8 low**
— no highs. Rejected, with reasons: `state.connected` is dead but its claimed cost is a no-op
microtask; per-dimension `sized` tracking is more than a direct correction; `create` leaving
compositing on after a refusal would need a `disableCompositing` this spec does not have;
`canvas.width = 0` reading back 1 is the recorded zero-size deviation; `release` with no primary and
`releaseStaleDrawables` with no primary are both teardown-only; a canvas that lost the frame handing
its context back is browser behaviour; a detached probe getting a real context likewise;
`canvasLayerCount` never decrementing gates cheap early-outs only. Disproved: `takeContextObject`
cannot answer undefined where it is called; `InstanceBinding::layers` is assigned before any use; the
intent's "instead of the single fullscreen canvas" is the frozen decision's approved reading, not an
oversight; the zero-area allocation is deliberate and documented.

**Follow-up review recommended: true.** Not for patch volume — for one named risk. Three of the six
medium patches (`Instance.cpp`'s per-embed context restore, and the frame-texture clear plus its state
restore in `GlSurfaceSdl.cpp`) live in code that has never executed: the SDL backends' canvas-layer
path has no coverage on Android or the Pi, because neither an emulator nor the Pi cross-build was
available. The `<iframe>`-after-canvas defect is proof that this class of bug exists and that macOS
cannot show it — it was found by reading SDL's sharing rules, not by a test. A pass on real Android
and Pi hardware is what would retire this.

**Verification performed**

- `ctest --test-dir runtime/build/macos` — **210/210 pass**, before and after the review's patches.
- `ctest --test-dir runtime/build/macos-asan -R "canvas-|dom-|gl-|iframe-"` — **91/91 pass**, clean,
  before and after.
- `cmake --build runtime/build/tvos-simulator` — builds and signs, re-run after the patches.
- `./gradlew :app:assembleRelease` — builds `libscreenkit.so` for arm64-v8a and armeabi-v7a. A build,
  not a run: no device or emulator.
- `sh tools/vendor/expo-gl.sh --check` — exit 0, the vendored tree is rule-generated, re-checked after
  the patches.
- Manual: the two-canvas page on the shipping macOS host, screenshot in `canvas-evidence/`.

**Residual risks**

1. The SDL canvas-layer path is unexecuted (see the follow-up risk above, and `deferred:`).
2. A canvas that takes the frame cannot later become a layer; placing it afterwards warns once.
3. No measurement of what a context switch actually costs per frame on a weak GPU. The design argues
   it is one `makeCurrent` per canvas per frame; nothing has timed it.
4. A second canvas inside an `<iframe>` instance is refused — one level of nesting, by design, and now
   with a message that says so in canvas terms.

## Verification

**Commands:**
- `cmake --build runtime/build/macos && ctest --test-dir runtime/build/macos` -- expected: every row passes, `canvas-*` included. **Run: 210/210 pass**, and again 210/210 after the review's patches (200 before, plus ten `canvas-*` rows: the seven the implementation added and the three the matrix audit found missing; seven existing rows were also strengthened during review). One earlier run had `net-cookies` fail and pass again alone and in the next full run -- a load-sensitive row, unrelated to this change.
- `cmake --build runtime/build/macos-asan && ctest --test-dir runtime/build/macos-asan -R "canvas-|dom-|gl-|iframe-"` -- expected: pass, no ASan reports. **Run: 91/91 pass, clean** (88 before, plus the three rows the matrix audit added), and 91/91 again after the review's patches.
- `cmake --build runtime/build/tvos-simulator` -- expected: builds. **Run: builds and signs**, and is current with the change (a re-run after the audit's test rows had nothing to do: the tests are not part of that target).
- `sh tools/android/android.sh test` and `MINIFY=1 sh tools/android/android.sh test` -- expected: every row passes. **Not run** (no device or emulator in this session); the SDL backend's canvas-layer path is therefore unexecuted -- `deferred-work.md`. What was run is the build: `./gradlew :app:assembleRelease` produces `app-release-unsigned.apk` with `libscreenkit.so` for arm64-v8a and armeabi-v7a, so the change compiles and links for Android -- it is the execution that is missing, not the compile.
- `sh tools/batocera/pi.sh test` -- expected: every row passes on the device. **Not run**, same reason.

**Manual checks:**
- The two-canvas page on each of the four targets: both canvases on screen, in z-order, the upper one's transparent pixels showing the lower through (a screenshot on each). **Run on macOS** with the shipping windowed host -- `_bmad-output/implementation-artifacts/canvas-evidence/` has the page, the command and the screenshot: the HUD's left half orange at its 10%/10% 50%x50% rect, its right half showing the game's blue through, and a drawing buffer of the rect's pixels rather than the screen's. **Not run on the tvOS simulator, the Android emulator or the Pi.** What stands in for those is `canvas-composite`, which reads those exact pixels out of the page's own framebuffer on macOS (two layers in `z-index` order, the upper one's transparent half showing the lower through, the page's frame under both), and `canvas-over-iframe`, which does the same for a HUD canvas over an `<iframe>` instance. Filed in `deferred-work.md`.
