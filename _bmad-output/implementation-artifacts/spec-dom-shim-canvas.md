---
title: 'DOM shim: document + canvas, wired to the real GL context'
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

**Problem:** The runtime has a real `WebGLRenderingContext` (M4) but no `document`, so Lightning
cannot start: it calls `document.createElement("canvas")`, sets `width`/`height`, then
`canvas.getContext("webgl")`. Without those three steps the GL context we already have is
unreachable from an unmodified app.

**Approach:** The smallest `document` + canvas shim that lets a real app reach the existing context.
Scope is taken from measured evidence, not from the DOM spec: `tools/dom-usage` static analysis plus
a runtime trace of the Lightning bundle in `poc/lightning3-blits`.

**Decisions taken at planning:**
- **The shim is JavaScript, evaluated as a prelude before the app bundle.** `Architecture.md` §2 puts
  the DOM layer in JS; only the pieces needing native access live in C++. `gl` is already a global, so
  the shim needs no new native surface.
- **One canvas is backed by the real context.** `GlSurface` owns one EGL surface, so the first
  canvas asked for a webgl context gets it. A second request must fail loudly, not silently share.
- **Every backed element is fullscreen, and `width`/`height` writes are ignored.** Canvas, and later
  video and iframe, fill the display. Reads always report the real drawable size; assignment is
  accepted syntactically and changes nothing. `Architecture.md` §3.1 describes CSS rects mapped onto
  compositor layers, but there is no compositor yet, so one fullscreen surface is the only truthful
  answer today — and reporting anything else would let `canvas.width` and `gl.drawingBufferWidth`
  disagree.

## Boundaries & Constraints

**Always:**
- Every shimmed member is one the evidence shows Lightning touching. No speculative DOM surface.
- `getContext("webgl"|"webgl2"|"experimental-webgl")` returns the existing `gl` object — the vendored
  `WebGLRenderingContext` — never a new or wrapped one.
- `canvas.width` / `canvas.height` always report the real drawable size from the GL surface. Writes
  are ignored, so a read after a write still returns the true size — the two can never diverge.
- Unsupported calls throw or return `null` with a clear message. A silent stub that returns
  `undefined` turns a missing shim into a crash somewhere unrelated.

**Never:**
- No `OffscreenCanvas`, no 2D context, no image loading, no `fetch`, no `Worker`, no observers, no
  `Blob`/`URL` — each is its own deferred spec.
- Do not modify the vendored tree under `runtime/third_party/gl/`.
- Do not change the existing `gl` global, `GlSurface`, or the M3 timer bindings.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Canvas creation | `document.createElement("canvas")` | An object with `width`, `height`, `getContext` | N/A |
| GL handoff | `canvas.getContext("webgl")` | Returns the **same** object as the `gl` global | N/A |
| WebGL2 request | `getContext("webgl2")` | Returns the same context (ANGLE gives ES 3.0) | N/A |
| Dimensions | read `canvas.width` | The GL surface's real pixel width, always | N/A |
| Dimension write | `canvas.width = 640` then read it | Still the real drawable width — the write is ignored | No throw; assignment is legal and inert |
| Second GL canvas | a second `getContext("webgl")` on a *different* canvas | Refused | Throws naming the single-surface limit |
| Unknown element | `document.createElement("link")` | An inert object that tolerates property set/get | Never throws — Lightning's loader does this |
| Root lookup | `document.getElementById("app")` | An element with `appendChild` and `style` | Returns `null` for any other id |
| Unsupported context | `getContext("2d")` | `null`, as the web spec allows | Logged once so the gap is visible |

</frozen-after-approval>

## Code Map

**Evidence — the scope of this spec is these two files, do not re-derive it:**
- `_bmad-output/implementation-artifacts/m2-lightning-dom-trace.json` — runtime-proven members.
  `HTMLCanvasElement`: `width`×11, `height`×11, `width =`, `height =`, `getContext("webgl")`.
  `Document`: `createElement("link","canvas")`, `getElementById("app")`. `HTMLElement`: `style`.
  `Node`: `appendChild`.
- `_bmad-output/implementation-artifacts/m2-lightning-dom-usage.json` — static superset.

**Reuse:**
- `runtime/core/src/gfx/VendoredWebGL.cpp` — `installVendoredWebGL` already binds the context to the
  `gl` global and to `__SKGLContexts[id]`. The shim reads `gl`; it does not create contexts.
- `runtime/core/src/gfx/GlSurface.h` — `width()` / `height()` are the real drawable size.
- `runtime/core/src/bindings/Timers.cpp` — the established pattern if any native helper is needed.
- `runtime/apple/HostMain.mm` — `startGraphics` installs GL on the JS thread; the prelude must be
  evaluated after that and before the app bundle.
- `poc/lightning3-blits/` — the app this is measured against.

**Verified facts:**
- Globals today: `console`, `setTimeout`/`clearTimeout`/`setInterval`/`clearInterval`,
  `queueMicrotask`, `requestAnimationFrame`/`cancelAnimationFrame`, and `gl`.
- Lightning requests `getContext("webgl")` — WebGL1 — via `e ? "webgl2" : "webgl"`, then falls back
  to `experimental-webgl`. All three spellings must resolve.
- ANGLE on Apple caps at ES 3.0 (`Architecture.md` §9), which is what WebGL2 requires, so one context
  satisfies both spellings.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/js/dom-shim.js` -- the prelude: `document` with `createElement`/`getElementById`, a
      canvas object whose `getContext` returns the existing `gl`, an inert element for everything
      else, and `appendChild`/`style` on the root.
- [x] `runtime/js/README.md` -- why this is JS rather than C++, and the rule that every member here
      must trace to a row in the evidence files.
- [x] `runtime/CMakeLists.txt`, `runtime/tests/CMakeLists.txt` -- compile `dom-shim.js` to `.hbc` with
      the pinned `hermesc`, as the fixtures already are, and make it available to host and tests.
- [x] `runtime/apple/HostMain.mm` -- evaluate the prelude after `startGraphics` and before the app
      bundle; a prelude failure is fatal and says so.
- [x] `runtime/tests/fixtures/dom-canvas.js` -- exercises every matrix row from JS.
- [x] `runtime/tests/RuntimeTests.cpp` -- one CTest row per matrix scenario.

**Acceptance Criteria:**
- Given the prelude is loaded, when JS runs `document.createElement("canvas").getContext("webgl")`,
  then it returns the same object as the `gl` global and `gl.getParameter(gl.RENDERER)` still names
  ANGLE's Metal backend.
- Given a canvas from the shim, when `width`/`height` are read before assignment, then they equal the
  GL surface's real pixel dimensions.
- Given a second canvas, when `getContext("webgl")` is called on it, then it throws naming the
  single-surface limitation rather than returning a broken context.
- Given `runtime/tests` under ASan and UBSan, when the suite completes, then it passes clean and the
  existing 28 rows still pass.

## Implementation Notes

- **Verified independently after hand-back:** `getContext("webgl")` returns the *same object* as the
  `gl` global (unwrapped `WebGL2RenderingContext`); all three spellings resolve on one canvas; a
  `640x480` write is ignored and `canvas.width` still agrees with `gl.drawingBufferWidth`; a second
  canvas throws naming the single-surface limit. 39/39 on macOS, clean under ASan+UBSan, tvOS gate
  exits 0 with the prelude loading before the bundle and M4's triangle still at 60.1 fps.
- **One gap found and fixed during verification.** The inert element implemented "tolerates property
  set/get" as *properties only*, so `link.setAttribute`, `link.getAttribute`,
  `link.addEventListener`, `document.head.appendChild` and `document.querySelectorAll` all threw —
  against the matrix row that says it must never throw, with the parenthetical "Lightning's loader
  does this". Those are method calls, and a bundler's module-preload polyfill makes them before any
  app code runs, so the real PoC bundle would have died at startup. Added as no-ops that store
  attributes and return empty matches, plus `document.head`/`body`, and pinned by a new
  `dom-loader-tolerance` row.

**The drawable size is `gl.drawingBufferWidth`, not a second reading of the surface.**
`canvas.width` is an accessor that reads `gl.drawingBufferWidth` on every access, so the
two are literally the same number and cannot drift. The vendored expo-gl sets that
property once, at `createWebGLRenderer`, from the GL viewport at install time
(`third_party/gl/SKWebGLRenderer.cpp:64`) -- which means neither number tracks a window
resize today. `GlSurface::width()` *is* live (`eglQuerySurface`), so the truthful-size
claim currently holds only until the window changes size. Reading `GlSurface` from JS
would need a native binding, which §2 says to avoid; the fix belongs in the vendored
seam, refreshing `drawingBufferWidth` on resize, and is left for the compositor spec.

**`getContext` is a per-canvas closure, not a shared method.** The single-surface rule is
about element identity, so it must not hinge on `this`: a destructured
`const { getContext } = canvas` still knows which canvas it came from.

**The prelude is bytecode, and it is fatal.** `runtime/CMakeLists.txt` now hard-fails
configure when `hermesc` is absent, matching what `tests/` and the tvOS app already did --
a host without its prelude is a broken artifact dressed up as a successful build. The
host locates `dom-shim.hbc` through `NSBundle`, which answers for both shapes it takes:
beside the executable for the macOS CLI, inside the `.app` on tvOS.

**The headless `screenkit-host <bundle>` path deliberately gets no prelude.** It has no
window and no GL context, so a canvas there could not be backed. `--window` is what loads
it, which is also what the spec's verification command uses.

**Deliberately left out: `gl.canvas`.** The trace records `WebGLRenderingContext.canvas`
being read, but the Code Map's enumerated scope does not list it and "do not change the
existing `gl` global" points the other way. A back-reference from the context to its
canvas is a one-line addition when a spec asks for it.

## Design Notes

**Why JS and not C++.** `Architecture.md` §2 puts the DOM layer in JS and keeps C++ for what needs
native access. Nothing here needs native access: `gl` is already a global and the drawable size is
reachable through it. Writing this in C++ would mean host functions for property getters that a JS
object does for free, and would make the next eight shims harder rather than easier.

**Fullscreen is the whole layout model, for now.** Lightning sets `canvas.width` and then trusts
`gl.drawingBufferWidth`. Letting the write stick would make those two disagree and render everything
at the wrong scale. Ignoring it removes the failure mode rather than documenting it: there is exactly
one surface, it fills the display, and both numbers come from the same place.

This is the same contract `video` and `iframe` will want, so the shim should express it as a property
of *backed elements* rather than something canvas does specially. When the compositor arrives and
§3.1's CSS layer rects become real, this is the single place that changes.

## Verification

**Commands:**
- `cmake --build runtime/build/macos && ctest --test-dir runtime/build/macos --output-on-failure` --
  all rows pass, including the existing 28.
- `runtime/build/macos/screenkit-host --window runtime/build/macos/fixtures/dom-canvas.hbc` -- prints
  the renderer string obtained *through* `document.createElement("canvas")`.
- `runtime/scripts/build-tvos-simulator.sh && runtime/scripts/run-tvos-simulator.sh` -- the same path
  works on tvOS.
