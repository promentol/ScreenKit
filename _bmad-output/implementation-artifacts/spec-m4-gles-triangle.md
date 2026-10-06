---
title: 'M4: GLES entry points and a triangle from JS'
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

**Problem:** JS can log and schedule work but cannot draw. `Architecture.md` M4 wants GLES entry
points reachable from JS; nothing in `runtime/` links ANGLE or owns a drawable.

**Approach:** Give the runtime a GL surface — SDL3 owns the window, ANGLE owns the context, a
`CAMetalLayer` is the seam — and expose the **raw GLES entry points** needed to draw. Exit: a
triangle drawn from JavaScript, animated through `requestAnimationFrame`.

**Scope, as narrowed by the request:** GLES entry points only. No `WebGLRenderingContext` object
model, no `WebGLShader`/`WebGLBuffer` wrapper objects — JS sees the integer handles GL itself uses.

**Decisions taken at planning:**
- **Platforms: macOS and the tvOS simulator**, matching M3. Both have ANGLE prebuilts already.
- **60fps is measured, not gated.** Log frame time once a second so the number is visible and a
  regression is noticeable; no CI threshold, because simulator frame timing is too noisy to gate on
  without producing a flaky test that gets disabled.

## Boundaries & Constraints

**Always:**
- ANGLE is consumed prebuilt from `tools/prebuilts`, linked statically.
- GL calls happen only on the JS thread, inside the existing executor discipline.
- The frame comes from `SDL_AppIterate` → `tickFrame` → `requestAnimationFrame`, which M3 already
  built. Do not add a second frame source.
- Entry points are generated from a `.def` table, not hand-written one by one.
- Typed-array uploads are zero-copy: read the `ArrayBuffer` behind a `Float32Array` directly.

**Never:**
- No `WebGLRenderingContext`, no WebGL validation layer, no object wrappers — a separate spec.
- No textures, framebuffers, or WebGL2 surface. A triangle needs none of them.
- Do not try to make SDL create the GL context: SDL's UIKit backend has no EGL path and our ANGLE
  is a static archive, so `SDL_GL_CreateContext` cannot reach ANGLE on tvOS.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Context creation | tvOS sim + macOS | EGL display/context over ANGLE Metal; `GL_RENDERER` names ANGLE Metal | Failure is an error, never a silent software fallback |
| Triangle | Bundle compiles shaders, uploads verts, draws | Coloured triangle on screen, stable across frames | N/A |
| Shader compile error | Deliberately broken GLSL | `glGetShaderInfoLog` text reaches JS | Draw skipped; host survives |
| Link error | Mismatched varyings | `glGetProgramInfoLog` reaches JS | As above |
| Typed array upload | `Float32Array` to `glBufferData` | Bytes arrive without a copy | Non-typed-array argument throws a JS `TypeError` |
| GL error surfaced | Invalid enum to a GL call | `glGetError()` returns it to JS | Never asserts or aborts |
| Frame loop | rAF draws each frame | Frames advance continuously, `eglSwapBuffers` per frame | A JS throw in the rAF callback is logged, loop continues |
| Teardown | Runtime shuts down with GL alive | Context and surface destroyed on the JS thread | No leak under ASan |

</frozen-after-approval>

## Code Map

**Reuse — this is largely an integration job, not new research:**
- `poc/angle-tvos/src/main.mm` — **the working reference.** It already creates an ANGLE display with
  `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`, picks a config, makes a window surface over a
  `CAMetalLayer`, and draws. Port that, do not re-derive it.
- `poc/angle-tvos/CMakeLists.txt` — link order that works: `libEGL`, `libGLES`, `libANGLE` last,
  plus `Metal`, `QuartzCore`, `IOSurface`, `Foundation`.
- `poc/angle-tvos/src/angle_stubs.mm` — Godot's archives omit 8 symbols (`astcenc_*`,
  `angle::GetCurrentSystemTime`, `angle::SetCurrentThreadName`). The same stubs will be needed.
- `runtime/cmake/HermesPrebuilt.cmake` / `Sdl3Prebuilt.cmake` — copy the shape for an
  `AnglePrebuilt.cmake`; the `angle` dep and its tvOS retagging already exist in the manifest.
- `runtime/core/src/bindings/Timers.cpp` — the established binding pattern
  (`createFromHostFunction`, weak-ref capture, argument coercion).
- `runtime/core/src/loop/EventLoop.h` — `tickFrame` and rAF already exist; the draw hangs off them.

**Verified integration facts — do not re-investigate:**
- **SDL3 cannot provide the context.** `src/video/uikit/` contains zero `SDL_EGL` references — the
  backend is EAGL and Metal only — and `SDL_EGL` loads `libEGL.dylib` via `SDL_LoadObject`, while our
  ANGLE prebuilts are static `.a`. Both reasons are independently fatal.
- **The seam is Metal.** `SDL_Metal_CreateView(SDL_Window*)` returns an `SDL_MetalView`, and
  `SDL_Metal_GetLayer(view)` returns its `CAMetalLayer`. That pointer is the
  `EGLNativeWindowType` for `eglCreateWindowSurface`.
- ANGLE's Metal backend on tvOS tops out at **ES 3.0**; 3.1/3.2 fail with `EGL_BAD_MATCH`
  (`Architecture.md` §9). Request ES 3.0 and do not probe higher.
- `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` is in `EGL/eglext_angle.h`, which must be included
  explicitly — it is not in `eglext.h`.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/cmake/AnglePrebuilt.cmake` -- resolve the pinned ANGLE from the manifest, fetch on a
      cold cache, expose an `angle::angle` target with the correct link order.
- [x] `runtime/core/src/gfx/GlSurface.{h,cpp}` -- own EGL display/config/context/surface over a
      supplied native layer; request ES 3.0; `swap()`; destroy on the JS thread.
- [x] `runtime/core/src/gfx/angle_stubs.mm` -- the 8 symbols Godot's archives omit.
- [x] `runtime/core/src/bindings/gl.def` -- X-macro table of the entry points a triangle needs:
      clear/viewport, shader compile + info log, program link + info log, buffers, vertex attribs,
      uniforms, `drawArrays`, `getError`.
- [x] `runtime/core/src/bindings/WebGL.cpp` -- expand `gl.def` into host functions on a `gl` global;
      coerce numbers and typed arrays; zero-copy `ArrayBuffer` access for `bufferData`.
- [x] `runtime/apple/HostMain.mm` -- create the Metal view from the SDL window, build the
      `GlSurface`, install `gl`, and swap after each rAF-driven frame.
- [x] `runtime/core/src/gfx/FrameStats.{h,cpp}` -- rolling frame time; log once a second at
      `LogLevel::Log` so 60fps is observable rather than assumed. No gate.
- [x] `runtime/tests/fixtures/triangle.js` -- compile shaders, upload a vertex buffer, draw a
      triangle, animate it via `requestAnimationFrame`.
- [x] `runtime/tests/` -- rows for each matrix scenario that can run headless (shader error, link
      error, typed-array coercion, GL error passthrough, teardown).

**Acceptance Criteria:**
- Given `triangle.js` compiled to `.hbc`, when the host runs it on the tvOS simulator, then a
  triangle is visible in a screenshot and `GL_RENDERER` names ANGLE's Metal backend.
- Given the same bundle, when it runs for 120 frames, then rAF callbacks keep arriving and no GL
  error is reported.
- Given the triangle running for a few seconds, when the log is read, then a frame-time line appears
  roughly once a second reporting the achieved rate.
- Given a bundle whose fragment shader has a syntax error, when it runs, then the JS receives the
  compile log and the host stays alive.
- Given `runtime/tests` under ASan and UBSan, when the suite completes, then it passes clean.

## Implementation Notes

**Four things the plan did not anticipate, each of which changed the shape of the code.**

**1. The archives' holes differ per slice.** `poc/angle-tvos/src/angle_stubs.mm` was written
against the iOS archive, where all eight symbols are missing. The macOS archive *defines*
`angle::GetCurrentSystemTime` and `angle::SetCurrentThreadName` (`nm` shows them as `T`, not `U`),
so porting the stubs verbatim is a duplicate-symbol link error on macOS. `AnglePrebuilt.cmake`
therefore runs `nm` over the archive that was actually fetched and defines
`SCREENKIT_ANGLE_STUB_PLATFORM_UTILS` / `SCREENKIT_ANGLE_STUB_ASTCENC` only when the definition is
genuinely absent. Probing beats a platform `if()`, because the holes are the publisher's and can
move with the pin.

**2. The wrapper is deduced, not spelled.** A `.def` row that encoded each argument's type would be
a table where the argument order can be got wrong silently. Instead `GL_FN(jsName, glFunction)`
deduces the whole wrapper from the C function's own type via `R (*)(A...)`, so a plain row is one
line and cannot disagree with GL. `GL_ADAPTED` names the ~20 entry points whose signature a JS
value cannot drive -- strings, out-parameters, buffer pointers -- and `static_assert` in the
coercion forces anything with a pointer argument into that half rather than compiling into
nonsense.

**3. Number coercion is ECMAScript's, written out.** `static_cast<GLuint>(-1.0)` and
`static_cast<GLuint>(1e30)` are both undefined behaviour, and this suite runs under UBSan. So
`toUint32` / `toInt32` / `toFloat` implement ToUint32/ToInt32 semantics with `fmod` before any
cast. This is the difference between a suite that passes under UBSan and one that passes only
because nobody passed a negative handle.

**4. The present needed a seam that did not exist.** `tickFrame` is asynchronous -- it sets a flag
and wakes the JS thread -- so a swap posted from `SDL_AppIterate` runs *before* that frame's rAF
callbacks, presenting the previous frame forever. `EventLoop` gained a frame-finished hook, exposed
as `Runtime::setFrameFinishedCallback`, which runs on the JS thread after the frame's callbacks and
their microtask checkpoint. It is the trailing edge of the existing clock, not a second one.

**Teardown on the JS thread falls out of ownership rather than a rule.** The only owners of the
`GlSurface` are the `gl` host functions and the frame hook. `EventLoop::shutdown()` clears the hook
and `runtime_.reset()` destroys the host functions, both on the JS thread, so the EGL objects are
released there without anybody having to remember it. `startGraphics` deliberately drops its own
reference the moment the hook is installed.

**Two additions beyond the task list, both load-bearing:**
- `gl.readPixels` (RGBA/UNSIGNED_BYTE only, bounds-checked against the destination view). It is
  what turns "a triangle is visible" from a screenshot a human has to look at into a headless
  assertion: `gl-triangle` reads the centre pixel (inside the triangle at every rotation, because
  it spins about the origin) *and* a corner (outside it), so a full-screen fill fails the row.
- `screenkit::reclaimRuntimeEvent` was declared in `Runtime.h` but never defined, and
  `SDL_AppEvent` never called it. SDL's main-callback loop drains the whole shared event queue,
  work events included, so tasks could be taken from the JS thread and dropped. Defined in
  `WorkQueue.cpp` and called from both hosts.

**Also changed:** the tvOS app now prefers `triangle.hbc` over `timers.hbc`, and
`run-tvos-simulator.sh` keys on the bundle's own `GL_RENDERER` line plus a frame-time line -- the
first only reachable through `gl.getString`, the second only produced if frames kept arriving --
then saves `build/tvos-simulator/triangle.png` while the app is still drawing. `screenkit-host` on
macOS gained an opt-in `--window` mode; the default headless path is unchanged, so the existing
host test rows still hold.

## Design Notes

**Why not `SDL_GL_CreateContext`.** It is the obvious first thing to try and it cannot work here.
SDL's UIKit video backend has no EGL implementation at all, and even on a backend that did, SDL
reaches EGL by `dlopen`ing `libEGL.dylib` — our ANGLE is a static archive with no dylib to open.
Creating the EGL objects directly against SDL's Metal layer sidesteps both problems and keeps SDL
doing what it is good at: the window, the lifecycle and the frame tick.

**One draw path, driven by the loop we already have.** M3 made `SDL_AppIterate` call `tickFrame`,
which services `requestAnimationFrame`. The GL work therefore belongs *inside* the rAF callback, with
`eglSwapBuffers` after the loop has serviced the frame. Adding a separate render thread or a second
timer would give two clocks racing over one context.

## Verification

**Commands:**
- `cmake -S runtime -B runtime/build/macos -G Ninja && cmake --build runtime/build/macos` -- builds
  with ANGLE linked, zero third-party sources compiled.
- `ctest --test-dir runtime/build/macos --output-on-failure` -- all rows pass.
- `runtime/scripts/build-tvos-simulator.sh && runtime/scripts/run-tvos-simulator.sh` -- the bundle's
  `GL_RENDERER` line reaches the platform log.
- `xcrun simctl io booted screenshot` -- the triangle is visible on the Apple TV simulator.

**Results (2026-09-16, macOS 26.6 / Apple M3 Pro / tvOS 26.5 simulator):**
- macOS build: clean, no third-party source compiled; `stubs: SCREENKIT_ANGLE_STUB_ASTCENC` only.
- `ctest`: **27/27 pass**, including the eight new `gl-*` rows.
- `-DSCREENKIT_SANITIZE=address,undefined`: **27/27 pass**, clean.
- tvOS simulator: `GL_RENDERER = ANGLE (Apple, ANGLE Metal Renderer: Apple tvOS simulator GPU...)`
  logged *by the bundle*; `frame time 16.67 ms mean, 17.64 ms worst over 60 frames (60.0 fps)`.
  Screenshot at 3840x2160 shows the rotating gradient triangle.
- macOS `--window`: `120.0 fps` on a 120 Hz panel -- the swap is vsync-bound, not the loop.
- `gl-triangle` headless: 120 rAF frames, 0 GL errors, centre pixel lit and corner pixel clear.

**Not covered.** macOS ASan has no LeakSanitizer, so "no leak under ASan" is a
use-after-free/UB result here, not a leak result. tvOS *device* remains unproven (unsigned, no
hardware), as it was after M3.
