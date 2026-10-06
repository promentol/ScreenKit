# Embedded Linux experiments

What we learned taking ScreenKit to a **Raspberry Pi 3 Model B+ running Batocera 42**: the device, the
Linux port it needed, what broke and how it was fixed, and how fast unmodified PixiJS, Phaser and
Lightning apps run there. Measured 2026-09-17.

The runnable pieces are in [`tools/batocera/`](tools/batocera/README.md); this file is the record of
why they look the way they do.

## Summary

- **It works.** The same `.skpkg` packages the Mac runs — `pixi-hello`, `phaser-hello`,
  `lightning3-blits`, `pixi-stress`, `phaser-stress` — run on the Pi, fullscreen on the TV, launched
  from EmulationStation's Ports menu. MSDF text (Lightning) and canvas text (Pixi, Phaser) both render.
- **The Pi 3 is a WebGL1 device.** Mesa's `vc4` driver offers OpenGL ES 2.0 and nothing newer, with
  no instancing. Engines had to be given a WebGL1 that looks like a browser's: real extension
  objects, no WebGL2 constants, `getContext('webgl2')` answering null, and an emulated
  `ANGLE_instanced_arrays` so Phaser starts.
- **Hermes on Linux had to be built by us.** Nobody publishes a Linux Hermes runtime. We build it in
  Docker from the same commit as the Apple framework, with Intl through a bundled ICU, because web
  engines assume `Intl` exists.
- **Batocera 42 ships SDL3** (3.2.18) and SDL3_ttf (3.2.2). The host links the system copies; nothing
  of SDL is bundled.
- **The limit is the CPU, not the GPU.** Hermes interprets bytecode (no JIT) on four Cortex-A53
  cores. Rendering at 640x480 instead of 1280x720 barely changes the stress results. A Pi 3 holds
  ~125 bouncing sprites, ~325 particles or ~100 bullets at 54 fps in Pixi, roughly 1/20–1/40 of a Mac.
  A scrolling tilemap platformer and per-frame canvas text do not hold 54 fps at all.
- **A fixed 640x480 screen is ScreenKit's job, not the compositor's.** The app draws into an
  offscreen framebuffer that each present scales onto the TV, centred and letterboxed. Asking
  Wayland for a 640x480 mode put the picture in the corner.

---

## 1. The device

| | Measured |
|---|---|
| Board | Raspberry Pi 3 Model B+ Rev 1.3 |
| CPU | 4× Cortex-A53, aarch64 |
| RAM | 908 MB visible to Linux |
| OS | Batocera 42 (2025/10/08), kernel 6.12.25-v8, aarch64 userland |
| libc / libstdc++ | glibc 2.40, `GLIBCXX_3.4.32` |
| Display server | Wayland, labwc compositor (`WAYLAND_DISPLAY=wayland-1`, `XDG_RUNTIME_DIR=/var/run`) |
| SDL | SDL 3.2.18 (wayland video driver), SDL3_ttf 3.2.2 — in `/usr/lib` |
| ICU | 73 (system) |
| Fonts | DejaVu in `/usr/share/fonts` |
| GPU driver | Mesa 25.1.9, `vc4` — "V3D 2.1", **OpenGL ES 2.0** (an ES 3.x context is refused) |
| Screen tools | `grim` is installed (screenshots of the real output) |
| Display | 1280x720 at 60 Hz on the test TV |

GL limits and extensions, read from a GLES 2 context with a small EGL probe program:

| | |
|---|---|
| `MAX_TEXTURE_SIZE` | 2048 |
| `MAX_VERTEX_ATTRIBS` | 8 |
| Texture units | 16, vertex shaders included |
| Fragment `highp` | yes (precision 23) |
| Present | `OES_vertex_array_object`, `OES_element_index_uint`, `EXT_blend_minmax`, `OES_depth_texture`, `OES_packed_depth_stencil`, `EXT_draw_buffers`, `EXT_frag_depth`, `KHR_parallel_shader_compile` |
| **Absent** | any instanced arrays extension, `OES_standard_derivatives`, Vulkan, ES 3.x |

**What this means against `Architecture.md`:**
- The Linux graphics chain there starts at ANGLE→Vulkan. A Pi 3 has neither Vulkan nor ES 3, so
  the host skips ANGLE and uses Mesa's GLES directly through SDL.
- Risk 6 ("Batocera may not ship SDL3") does not apply to Batocera 42.

---

## 2. How ScreenKit runs there

### The Linux host

The host that ran only on Apple platforms was split so Linux shares it:

- **`runtime/host/Host.{h,cpp}`** holds everything platform-neutral: starting graphics, evaluating
  the DOM shim, resolving and running a package, and the SDL window loop (`runWindowed`) with its
  fullscreen, resizable, fixed-size and "Guide quits" options. A platform supplies a small
  `WindowPlatform` (window flags, a hook before the window exists, attach/detach of the native
  drawable).
- **`runtime/apple/HostMain.mm`** keeps only the Metal drawable, the tvOS SDL callbacks and the macOS
  `main`.
- **`runtime/linux/HostMain.cpp`** is `screenkit-host [--window|--fullscreen] [--size WxH] <package>`
  (plus `--bytecode-version`).
  - It finds `dom-shim.hbc` beside the executable (via `/proc/self/exe`).
  - It keeps storage in `$XDG_DATA_HOME/screenkit` or `~/.local/share/screenkit`.
  - Its runpath is `$ORIGIN/../lib:$ORIGIN`, so `libhermesvm.so` sits in `lib/` beside `bin/`.
- **`runtime/core/src/gfx/GlSurfaceSdl.cpp`** is the Linux GL surface.
  - It asks SDL for an ES 2.0 profile (EGL hands back the newest compatible context, so an ES 3 GPU
    still gets ES 3), RGB8 with no alpha, depth 24 and stencil 8.
  - It creates the context with `SDL_GL_CreateContext` and turns on vsync.
- **`runtime/cmake/LinuxSystem.cmake`** finds SDL3, SDL3_ttf, GLESv2 and EGL, either through
  pkg-config or from a sysroot (`SCREENKIT_SYSROOT`).
- System fonts come from `SystemFontsLinux.cpp` (§5).
- There is no network backend on Linux.

The built host is **1.8 MB** (release, arm64). A push of the host, Hermes with ICU, and five apps is
**~49 MB**.

### Building for the Pi

The host is cross-built on the Mac in an **arm64 Debian 12 container**
(`tools/batocera/Dockerfile`, GCC 12). Under Docker Desktop on Apple silicon that runs natively, not
emulated.

It links against **the Pi's own SDL libraries**, gathered into `tools/batocera/sysroot/` by
`pi.sh sysroot`:
- `libSDL3.so.0` and `libSDL3_ttf.so.0` are copied off the Pi with `scp`.
- The headers come from the **exact source releases the Pi runs** (SDL 3.2.18, SDL_ttf 3.2.2), pinned
  by sha256. Headers from another version would compile against an ABI the Pi does not have.
- These libraries depend on Wayland, DRM, FreeType and so on, none of which are in the sysroot. The
  link passes `--unresolved-symbols=ignore-in-shared-libs`, and they resolve on the Pi at load time.

Two build-time details:
- GLES and EGL come from the container's Mesa dev packages. The symbols are the Khronos ABI, so they
  bind to the Pi's Mesa.
- The DOM shim is compiled to bytecode by the Linux Hermes' own `hermesc`, in the same container.

### Deploying and running: `tools/batocera/pi.sh`

Plain `sh` functions over ssh (`PI=batocera.local`, root password `linux` by default; an ssh key
makes it non-interactive):

| Command | Does |
|---|---|
| `sysroot` | the Pi's SDL libraries + matching pinned headers |
| `build` | `screenkit-host` + `dom-shim.hbc` for linux-arm64, in Docker (fetches the Hermes prebuilt) |
| `apps [app…]` | `npm run build:screenkit` in each `poc/<app>` |
| `push` | rsync to `/userdata/roms/ports/.data/screenkit`, one `screenkit-<app>.sh` Ports launcher per app, then refreshes EmulationStation through its local API (`http://127.0.0.1:1234/reloadgames`) |
| `run <app> [secs]` | runs on the TV from an ssh session (`XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-1 SDL_NOMOUSE=1`), prints the log |
| `shot <app> [secs]` | the same with `SCREENKIT_CAPTURE`, copies the frame back to `out/<app>.png` |
| `bench <app>` | runs a stress test until its `result {…}` line, saves `out/<app>.log` and `out/<app>.json` |
| `logs [app]` | what the Ports launchers logged |
| `all` | build + apps + push |

**Layout.**
- EmulationStation lists the `.sh` files in `ports/`.
- The runtime and packages live in `.data/`, which EmulationStation hides.
- Each launcher logs to `.data/screenkit/logs/<app>.log`.
- On Linux the host quits on the gamepad's **Guide** button, the usual way back to the menu for a
  port.

---

## 3. Hermes on Linux

React Native publishes Hermes runtimes for Apple (the `hermes-engine` tarball) and Android (the
`hermes-android` AAR), and a Linux **`hermesc`** only. `facebook/hermes` GitHub releases stop at
v0.13.0 and contain executables, not a library. So Linux is the one target where the "never build
third-party code" rule is kept by **building once ourselves and pinning the result**.
The rule covers contributors, not us.

**Recipe:** `tools/prebuilts/recipes/hermes-linux/`.
- It builds tag `hermes-v260318099.0.2`, commit `4188b63f`, the same Hermes as the Apple framework and
  the pinned `hermesc`.
- The build runs in `debian:bookworm-slim`, pinned by digest, one container per target.
- Its CMake flags follow React Native's Apple release build: `MinSizeRel`, debugger off, Intl on.

| | linux-arm64 | linux-x86_64 |
|---|---|---|
| Archive | `hermes-linux-arm64-260318099.0.2.tar.gz`, 16.6 MB | `hermes-linux-x86_64-260318099.0.2.tar.gz`, 16.7 MB |
| sha256 | `5bed90e9…a095c1` | `bbeded7c…d8e4c` |
| Build time on the Mac | several minutes (native arm64) | ~15–20 minutes (emulated) |
| Requires | glibc ≥ 2.34, `GLIBCXX_3.4.30` | same |

Each archive holds:
- `lib/libhermesvm.so`, which exports the JSI implementation, so an embedder links it alone;
- ICU 72 `.so` files beside it;
- a native `bin/hermesc`;
- headers in the Apple `destroot/include` layout;
- `BUILDINFO.json`.

**Before packaging, a smoke test runs.** It:
- loads bytecode compiled by the *pinned host* `hermesc`, which checks bytecode-version parity with the
  Apple side;
- runs the same program from source;
- checks that the archive's own `hermesc` writes bytecode the library accepts;
- does all of this with the system ICU moved aside, so the bundled ICU is what loads.

`fetch.mjs` looks for the archive in three places and checks each against the manifest's sha256:
1. `hermes.linux.url`, null until the archives are hosted;
2. `$SCREENKIT_PREBUILTS_MIRROR`;
3. `tools/prebuilts/dist/`.

### Findings

- **Web engines need `Intl`.** The first Linux Hermes was built without Intl, and Pixi and Blits died
  at startup with "Property 'Intl' doesn't exist": Pixi reads `Intl?.Segmenter`, which throws when
  `Intl` itself is undeclared. Intl on Linux means ICU.
- **ICU has to travel with Hermes.**
  - The system ICU's soname differs by distribution (72 on Debian 12, 73 on Batocera 42).
  - Hermes cannot link a static ICU into a shared library. `HERMES_USE_STATIC_ICU` gave undefined
    references, and a cached CMake value kept it on until it was set `OFF` explicitly.
  - So Debian's ICU 72 `.so` files ship in `lib/`, with `patchelf --set-rpath '$ORIGIN'`.
  - ICU versions its symbols (`ucol_open_72`), so a different ICU loaded in the same process does not
    collide.
  - ICU is about 34 MB uncompressed, most of it data.
- **Hermes does not put ICU on `libhermesvm.so`'s link line** on Linux; only its executables get it.
  The recipe adds `-Wl,--no-as-needed -licui18n -licuuc -licudata` to the shared linker flags.
- **Hermes' Linux ICU backend is unfinished.**
  - `Intl.Collator` and `Intl.DateTimeFormat` are real.
  - `Intl.NumberFormat` and `String#toLocaleUpperCase`/`toLocaleLowerCase` are placeholders:
    the case mappings return the literal strings `"uppered"`/`"lowered"`
    (`PlatformIntlICU.cpp`), and a formatted number comes back in C's `%f` (`"1234.500000"`).
  - The DOM shim checks for the placeholders and only then installs plain versions: en-US digits and
    grouping, root-locale case mapping. Apple and Android Hermes are left alone.
  - The smoke test asserts only what is real.
- **`WeakRef` exists only with a microtask queue.** The first smoke test saw `WeakRef` undefined
  because it created the runtime with default config. It now uses ScreenKit's own config
  (`withMicrotaskQueue(true)`, `withES6BlockScoping(true)`).
- **No 32-bit ARM build.** Batocera on a Pi 3 is aarch64, so arm64 is enough for it. 32-bit Raspberry
  Pi OS would need `armhf`, and Docker Desktop here has no `linux/arm/v7` emulation.
- Build-machine gotcha: pulling the amd64 base image failed with "docker-credential-desktop not
  found" until `/Applications/Docker.app/Contents/Resources/bin` was on `PATH`.

---

## 4. WebGL1 on a GLES 2 GPU

expo-gl's WebGL implementation, vendored under ScreenKit, had only ever run on ANGLE with ES 3. On the
Pi each engine found a different gap. All fixes go through the vendoring rules
(`tools/vendor/expo-gl.rules` + `tools/vendor/seam/`) or the DOM shim; the vendored source is not
hand-edited.

| Symptom on the Pi | Cause | Fix |
|---|---|---|
| `std::atomic_uint` undeclared compiling expo-gl | libstdc++ does not pull in `<atomic>` transitively the way libc++ does | a rules patch adds `#include <atomic>` |
| No extensions reported at all | upstream enumerates with `glGetStringi`, which is GLES 3 only | seam `readSupportedExtensions`: on ES 2, parse `glGetString(GL_EXTENSIONS)` and map GLES names to WebGL names (`EXT/NV_instanced_arrays` → `ANGLE_instanced_arrays`, `OES_depth_texture` → `WEBGL_depth_texture`, `EXT_draw_buffers` → `WEBGL_draw_buffers`, s3tc/etc1/astc) |
| Engines saw extension names but got no working objects | WebGL1 extensions are objects with their own methods and constants | the shim builds real extension objects whose methods forward to the WebGL2 natives underneath |
| Phaser: "ANGLE_instanced_arrays extension not supported" | Phaser 4 requires the extension at startup even though its sprite batches never draw instanced; the Pi has none | the shim supplies an emulated `ANGLE_instanced_arrays`: an instanced draw repeats a plain draw per instance; a draw with a per-instance attribute (a divisor) is skipped and logged once |
| Pixi: "undefined is not a function" in `_createProgramData` | WebGL2-only constants (e.g. `ACTIVE_UNIFORM_BLOCKS`) were visible on the WebGL1 context, so Pixi took its WebGL2 path | the shim removes the 256 WebGL2-only constant names (from the Khronos IDL) from `WebGLRenderingContext`'s prototype and constructor |
| An ES 2 context still offered as `webgl2` | — | `getContext('webgl2')` returns null unless the context really is WebGL2, so engines fall back as they would in a browser |
| `SCREENKIT_CAPTURE`: "glReadPixels failed" | a GL error latched by the app was read back as the capture's own | frame capture drains pending GL errors before reading |

What remains:
- **Real instancing is unavailable.** Phaser's `SpriteGPULayer` needs per-instance attributes and will
  not draw.
- **three.js requires WebGL2**, so `examples/threejs-cube` is not deployed to the Pi.
- The stress assets use power-of-two textures of 256 px or less, well inside the 2048 limit.

---

## 5. Canvas text and fonts

Lightning draws text from MSDF atlases. Pixi's `Text` and Phaser's `Text` draw into a 2D canvas and
upload it, so the Pi work needed a canvas text path. It is deliberately a **minimal shim**: enough for
engines' text, not a 2D engine.

- **Native: `__screenkit.text`** (`core/src/bindings/Text.cpp`, `core/src/text/FontLibrary.cpp`).
  - Built on SDL3_ttf, which carries FreeType and HarfBuzz.
  - It adds fonts from bytes, lists and loads system faces, and returns font metrics, text measures,
    and a coverage mask per string. The mask can be bold, italic, unkerned, or outlined with a
    miter, round or bevel join.
  - Font instances are cached per size/style/outline; hinting is light-subpixel.
- **JS: the DOM shim** does the rest:
  - parses the CSS `font` shorthand;
  - resolves a family through `document.fonts`, then system faces, then `sans-serif`, picking the
    closest face by style and then by weight;
  - implements `measureText` with Chromium's baseline rules;
  - composites the native mask into the canvas pixels for `fillText`/`strokeText`, with a source-over
    fast path;
  - makes `FontFace` really load `url()`, `local()` and bytes.
- **Linux system fonts** (`SystemFontsLinux.cpp`):
  - It scans `/usr/share/fonts` and related directories with SDL_ttf.
  - A face's name is its file path, so JavaScript never names an arbitrary file to read.
  - `sans-serif`, `serif` and `monospace` map to DejaVu Sans, Serif and Sans Mono. Unknown families
    such as "Helvetica" or "Arial" fall back to `sans-serif`.

Findings:
- `TTF_GetFontAscent` includes twice the outline width, so outlined text sat 4 px low until the glyph
  origin subtracted it.
- The first family match on Batocera picked **DejaVu Sans ExtraLight** for `sans-serif`. Matching
  now picks the nearest weight to 400 (or 700 for bold).
- **Canvas text is expensive on the Pi.** One Pixi `Text` changing every frame runs at ~21 fps, and
  one Phaser `Text` at ~35 fps (640x480). The first stress runs scored **0 everywhere** because the
  status line was redrawn every 500 ms, and the harness measured that redraw as load. The status now
  redraws only between phases.

---

## 6. A fixed 640x480 screen

Old games and small UIs want a fixed low resolution on any TV. `--size 640x480` gives the app a
640x480 canvas whatever the display is.

**What did not work: a display mode.**
- Wayland clients cannot set modes, so SDL emulates a 640x480 fullscreen mode.
- labwc placed the 640x480 surface in the **top-left corner** of the 1280x720 output, unscaled.
- Where a smaller fullscreen surface goes is the compositor's decision, so ScreenKit cannot rely on it.

**What works: scale it ourselves** (`GlSurfaceSdl.cpp`).

1. With a fixed size, the surface creates an **offscreen target**: an RGBA texture, a depth24/stencil8
   renderbuffer, and a framebuffer. The app renders into it.
2. That framebuffer becomes expo-gl's **default framebuffer**, so WebGL's "bind null" means the
   offscreen target, and `canvas.width/height` and `window.innerWidth/innerHeight` report 640x480.
3. `swap()` switches to a **second GL context that shares the texture**. That context:
   - clears the window to black;
   - sets the viewport to the largest centred rectangle with the target's aspect ratio;
   - draws the texture with a GLSL ES 1.00 quad;
   - swaps, then switches back.
4. A separate present context leaves every piece of the app's GL state — program, buffers, vertex
   attributes, blend — untouched. GLES 2 has no `glBlitFramebuffer`, so a draw is needed anyway.
5. Frame capture reads the app's framebuffer, so captures are 640x480 too.

On macOS `--window --size WxH` is simply a window of that size.

App-side findings at 640x480:
- **`pixi-hello` and `phaser-hello` captions were cut off**: their layouts were in pixels for 720p.
  They now lay out on a 720-unit-tall design scaled to the screen height (a scaled Pixi container; a
  Phaser camera zoom from origin 0,0).
- **Lightning (Blits) showed only its background**, on the Mac too. Blits' built-in
  `SCREEN_RESOLUTIONS` covers only 720, 1080 and 2160 lines; every other height falls through to a
  ratio of 1, so a 1920x1080 stage is drawn 1:1 into whatever drawable it was given and the screen
  shows its top-left corner. `examples/lightning3-blits` first fixed this in its own source, with
  `pixelRatio: Math.min(innerWidth / 1920, innerHeight / 1080)`.

  That is now `screenkit-blits-fits-the-drawable` in `@screenkit/vite-plugin`
  (`src/lightning.js`), which rewrites the fallback in Blits' own
  `src/engines/L3/launch.js`, so an unmodified app off npm gets it too -- the test for that is
  `examples/blits-example-app`, built and run without a line changed in its `src/`. An author's own
  `pixelRatio` or `screenResolution` still wins, and on 720/1080/2160 Blits' table answers first
  with the same number. On 4:3 the 16:9 stage fits the width and leaves black below, because
  Lightning has no vertical stage offset.

Verified on the Pi with `pi.sh shot` (the app's frame) and `grim` (the real output):
`pixi-hello`, `phaser-hello` and `lightning3-blits` are centred with black bars on the 1280x720 TV.

---

## 7. Performance

### Method

`examples/pixi-stress` and `examples/phaser-stress` share one harness (`src/stress.js`) and build each scene
the way that engine's games would. Seven phases, each a kind of game:

| Phase | Scene | Load |
|---|---|---|
| sprites | bunnymark: bouncing, spinning, tinted sprites | sprites |
| platformer | 400-column scrolling tilemap, two parallax layers, running player (Pixi: culled chunk containers; Phaser: a Tilemap layer) | enemies with gravity and tile collisions |
| shooter | five turrets spraying bullet patterns, additive | live bullets |
| particles | bursts that fall and fade, additive (Pixi: `ParticleContainer`; Phaser: its emitter) | live particles |
| puzzle | match-3 board, every gem bobbing, swaps (Phaser: a tween per gem) | gems |
| vector | units with health bars, redrawn with Graphics every frame | shapes |
| text | canvas Text changing every frame | texts |

How a phase runs:
- **Target:** 90% of the display rate, capped at 60 × 0.9 = **54 fps**.
- **Growth:** a phase starts light and grows ~30% per measurement window while the target holds (the
  puzzle board grows its side by ×1.15). The result is the largest load that held.
- **Descent:** a starting load that fails is **halved in a fresh scene** until one holds. So a slow
  device still gets a number, and **0 means even the smallest load of that scene misses the target**.
- **Scenes** are built when their phase begins, so one phase's objects never weigh on another.
- **Capping:** a phase still holding when it reaches its maximum load or the time limit is listed in
  `capped`: its real limit is higher.
- **Output:** every window logs `stress(<engine>): …`, and the run ends with one `result {json}` line.

### Raspberry Pi 3 B+ results

Loads that held 54 fps, WebGL1, 60 Hz TV:

| Engine | Size | sprites | platformer | shooter | particles | puzzle | vector | text |
|---|---|---|---|---|---|---|---|---|
| PixiJS 8 | 640x480 | 125 | 0 | 100 | 325 | 144 | 4 | 0 |
| PixiJS 8 | 1280x720 | 125 | 0 | 100 | 325 | 144 | 5 | 0 |
| Phaser 4 | 640x480 | 62 | 0 | 16 | 62 | 36 | 3 | 0 |
| Phaser 4 | 1280x720 | 62 | 0 | 0 | 62 | 36 | 2 | 0 |

> **Mostly explained, on 2026-09-18.** The same benchmark fell to sprites 40, shooter 0-1, puzzle 25,
> vector 1-2 -- about a third of the budget. It was not the frame pacing: the same binary with
> `SCREENKIT_FRAME_PACING=0`, the old loop exactly, gave the same lower numbers. Removing expo-gl's
> batch queue (below) put sprites, particles, puzzle and vector back where this row has them.
> **Shooter did not fully recover** -- 4-6 against the 16 here -- so a residual remains unexplained.
> A gamepad is now paired to the Pi and `InputRouter::tick` polls it every frame, which is the first
> thing to rule out for that.

Frame rates behind the zeros and the small numbers:

| | 640x480 | 1280x720 |
|---|---|---|
| Pixi platformer, 1 enemy | 8 fps | ~8 fps |
| Phaser platformer, 1 enemy | 31.4 fps | 17.5 fps |
| Phaser shooter | 12 bullets 58.6 fps · 16 held 55.6 · 21 → 52 | ~46 fps with the turrets alone |
| Pixi text, 1 text | 21 fps | — |
| Phaser text, 1 text | 35 fps | — |
| Pixi vector | 6 shapes 46 fps · 12 → 29 · 25 → 15 | — |
| Phaser vector | 3 shapes 56 fps · 6 → 34 · 12 → 19 · 25 → 10 | — |

For comparison, **Pixi on the development Mac** (120 Hz display, same 54 fps target):

| sprites | platformer | shooter | particles | puzzle | vector | text |
|---|---|---|---|---|---|---|
| 3454 | 1932 | 2336 | 7588 | 1225 (capped) | 161 | 7 |

On sprites, shooter, particles and vector the Pi holds roughly **1/20 to 1/40** of what the Mac does
(the Mac's puzzle stopped at the time limit, so that ratio is not meaningful).

### Hermes' JIT: measured on the Pi

Hermes has a JIT for arm64, and the prebuilt we ship is built without it. Built with it and asked
for it at runtime, the same Phaser stress test on the same Pi, both at 640x480 and a 54 fps target:

| Phase | Interpreter | JIT | |
|---|---|---|---|
| sprites | 62 @ 56 fps | 62 @ 59.2 fps | same load, more headroom |
| platformer | 0 | 0 | unchanged |
| shooter | 33 @ 54.5 fps | 33 @ 54.2 fps | unchanged |
| particles | 62 @ 61.4 fps | **125** @ 55.8 fps | **2x** |
| puzzle | 36 @ 56.3 fps | **49** @ 55.1 fps | **+36%** |
| vector | 3 @ 56.2 fps | 3 @ 60.9 fps | same load, more headroom |
| text | 0 | 0 | unchanged |

The phases that move are the ones spending their time in JavaScript. The ones that do not are
bottlenecked elsewhere: the platformer walks a tilemap through Phaser's objects, and one changing
canvas text already costs more than a frame (~35 fps) in native text rendering.

**Turning it on.** Two switches, and both are needed:

- **The build.** `HERMESVM_ALLOW_JIT` passed to CMake only decides whether Hermes builds its JIT
  *unit tests*; `include/hermes/VM/JIT/Config.h` reads it as a **preprocessor macro**, and an
  undefined macro is 0. Passing it to `cmake` alone yields a library with no JIT in it and no
  warning. It has to reach the compiler:
  `HERMES_JIT=2 tools/prebuilts/recipes/hermes-linux/build.sh linux-arm64`, which writes a separate
  `hermes-jit-linux-arm64-<version>.tar.gz` and adds `-DHERMESVM_ALLOW_JIT=2` to `CMAKE_CXX_FLAGS`.
- **The runtime.** `RuntimeConfig`'s `EnableJIT` is false even in a JIT build. The host reads
  `SCREENKIT_HERMES_JIT` (`1`, or `force` to compile every function rather than waiting for the
  32-call threshold); `VARIANT=jit sh tools/batocera/pi.sh push` deploys a parallel installation
  whose launchers set it.

**What the JIT does and does not speed up**, measured while getting there:

- **Bytecode only.** The same workload evaluated as *source* through JSI showed no gain at all
  (1.0x): Hermes compiles source lazily and does not JIT lazily-compiled functions. As bytecode from
  `hermesc -O` -- what a `.skpkg` ships -- it was 3.6x in the same process. A dev build that
  evaluates source gains nothing.
- **Shape decides it.** An arithmetic loop is about 20% *slower* JIT'ed; the interpreter is well
  tuned and the JIT emits helper calls for those operations. Object and method code -- a game's
  shape -- is where the gain is.
- **Called functions only.** Hermes compiles a function when it is called and has no on-stack
  replacement, so one call wrapped around a long loop is never compiled however hot it gets.

Not measured: JIT memory on the device (Hermes' default ceiling is 32 MB of code on a 908 MB Pi),
cold-start cost, and every other app.

### The loop ran at 800 Hz: pacing `requestAnimationFrame`

Found while asking why the Blits example app felt slow at 1080p. It was not slow in any of the
places that get blamed first.

The windowed loop (`runtime/host/Host.cpp`) used to tick like this:

```c++
state.runtime->tickFrame(...);              // services rAF + microtasks, then presents
if (!state.runtime->idle()) SDL_Delay(1);   // a fixed millisecond, not the display interval
```

A Lightning app re-registers `requestAnimationFrame` every frame, so the runtime is never idle, so
the loop ticked every ~1.25 ms -- **~800 times a second against a 60 Hz panel**. Every tick runs the
frame's rAF callbacks, and for Blits one of those is a whole scene-graph update. The app was doing
that work thirteen times over for each frame the screen could show.

**vsync did not pace it**, which is the part worth remembering: `swap()` does block on the refresh,
but `presentFrame` only swaps a frame that actually painted, and a UI holding still paints rarely.
So on exactly the frames where the spinning was pure waste, there was nothing to block on. The
`screenkit.gl` frame log reported ~800 fps and 1.3 ms means, which reads like good news and is not:
it counts loop turns, not paints.

The fix is a frame deadline from the display's own refresh rate, with the loop sleeping in the event
queue until the frame is due -- and, load-bearing, **only for frames that painted nothing**. A frame
that painted was already paced by its swap, so holding it to a deadline as well would pace it twice.
`SCREENKIT_FRAME_PACING=0` turns the deadline off, which is how the two are measured against each
other on one binary.

Blits example app, 1080p, Pi 3 B+, 20 s of a screen holding still:

| | ticks/s | CPU (of one core) | worst frame |
|---|---|---|---|
| Unpaced, interpreter | ~800 | 29% | 39 ms |
| Unpaced, JIT | ~800 | 21% | 32 ms |
| **Paced, interpreter** | **60** | **16%** | 42 ms |

The JIT row is the same app on the unpaced loop, for scale: pacing saved more than the JIT did, and
the two are independent.

**The hitch is not in the GL layer.** Instrumenting the DOM shim to time every native `gl` call: at
its busiest the whole GL surface accounted for **42 ms out of 1000**, and 0-1 ms/s once the screen
settled -- while the worst frame in that same second was 49 ms. The remaining ~30 ms is JavaScript,
which is why the JIT moves it and pacing does not.

### What the once-a-second hitch is

Pacing leaves the Blits example app at a clean 60 fps with one stall a second, which is what the app
actually feels like. Timing every `requestAnimationFrame` callback from the shim, on the Pi at 1080p:

| | rAF total per second | worst single callback |
|---|---|---|
| A second with a focus change | 41-54 ms | **23-44 ms** |
| A second without one | 2-13 ms | 1 ms |

So it is **one callback**, not a gradual load: every other frame in the second costs about a
millisecond, and the app has ~16 ms to spend. The example app moves its focus roughly every 1.4 s,
and each of those frames overruns the budget and drops one frame. Ruled out by measurement, not by
argument:

- **Not GL, not canvas text, not texture upload.** The same probe timed `texImage2D`,
  `texSubImage2D`, `compileShader`, `linkProgram`, `fillText`, `measureText`, `drawImage` and
  `getImageData`: "nothing else over 1 ms" through every hitch. Only the first second after launch
  shows a texture upload worth naming (10 ms).
- **Not the demo's own debug logging.** Blits' example app runs at `debugLevel: 1` and prints a
  multi-line focus chain on every change, which is a good suspect and is not the answer: rebuilt with
  `debugLevel: 0` the worst callback does not improve.
- **It is interpreted JavaScript.** The same callback costs ~4 ms on a Mac, an ~8x gap that matches
  the interpreter against JIT-compiled JS everywhere else in this document, and the Hermes JIT moves
  it (39 ms -> 32 ms).

**A measurement trap worth writing down:** the first numbers for this were ~38 ms, taken with the
host's stdout piped over ssh. Run again with output to a file on the Pi -- which is how the Ports
launcher runs it -- the same callback is ~24 ms. About 14 ms was the log pipe, not the app. Time an
app the way it actually runs.

Which function inside that callback is still open, and **Hermes' sampling profiler could not be made
to answer it**. The profiler is compiled in -- `HERMESVM_SAMPLING_PROFILER_AVAILABLE` is 1 on every
non-Emscripten platform, and the API throws when it is not -- and enabling it needs two calls, not
one: `enableSamplingProfiler()` globally, and `registerForProfiling()` on the runtime *and the thread*
to be sampled, or the global dump writes an empty file because no runtime is registered. With both
in place the dump deadlocks teardown: `dumpTraceryTraceGlobal` takes the global profiler lock and
then the per-runtime one while a live sampling thread holds them the other way round. Disabling the
sampler first avoids that inversion and the hang moves elsewhere -- shutdown took 16 s against 10 s
without it, and no profile was produced either way. So the knob was taken out again rather than
shipped: one that hangs an app on exit and writes nothing is worse than none.

Attributing that callback therefore still needs a profiler, and the options are a Hermes built for
it (`tools/prebuilts/recipes/hermes-linux`, where the JIT variant already comes from), or
instrumenting the app's own JS.

### Typed arrays across the JSI boundary

Audited at the same time, since "it must be the copies" is the obvious guess. Measured with a
wrapped `gl` object, counts being hardware-independent:

| | busiest second | screen holding still |
|---|---|---|
| GL calls | 27,170/s | 831/s |
| Bytes handed to them | 2.77 MB/s | 56 KB/s |
| Time inside `gl` | 42 ms/s | 0-1 ms/s |

- **Reads out of JS are zero-copy.** `bytesOf` (`gfx/VendoredWebGL.cpp`) takes a pointer straight
  into the `ArrayBuffer`; nothing is marshalled element by element.
- **But not through internal slots, and JSI's TypedArray API does not change that.** The pinned JSI
  has `Object::isTypedArray` and `TypedArray::buffer/byteOffset/byteLength`, which look like the
  browser's slot access and are not: `jsi.h` says "the 'byteLength' *property* of", and means it.
  Measured against a `Float32Array` with `byteLength` shadowed to 4 and `byteOffset` to 999999,
  `view.byteLength()` answers 4 -- so they are the same property reads spelled differently, and
  trusting them uploaded 4 bytes from an offset outside the buffer. Blink reads the real slots and
  ignores the shadowing; we cannot, so `bytesOf` range-checks what it reads and refuses instead.
  Chrome uploads the real 32 bytes where this throws a TypeError: a deliberate divergence, and the
  safe answer available to a binding that cannot see slots (test: `gl-typed-array-shadowed`).
- **`isTypedArray` itself is a real type check**, though -- a plain object carrying `buffer`,
  `byteOffset` and `byteLength` is rejected by it. So it answers first in `isArrayBufferView`, and
  every typed array WebGL is handed in anger is settled natively; only a `DataView` still costs a
  call back into JS to `ArrayBuffer.isView`.
- **Each data-carrying call then copies once**, into a `std::vector`, because the call is *deferred*
  into a batch and replayed later -- by which time JS could have written to the buffer again.
- **That deferral was dead weight here, and is now gone.** It is expo-gl's Android design, where GL
  lives on a separate `GLSurfaceView` thread. In ScreenKit `presentFrame` runs on the JS thread, the
  same one that queued the ops, so the copy and the `std::function` per call guarded a race that
  cannot happen. `addToNextBatch` and friends are now templates that call the op, so nothing is
  heap-allocated at the call site either -- keeping the `std::function` signature would have run the
  op immediately and still allocated to build the argument. Six `patch` rows in
  `tools/vendor/expo-gl.rules`; the three payload copies in the seam went with them.
- **It is worth a third of the Pi's budget on a GL-heavy app**, which is not what the table above
  predicts, and the gap is a lesson in its own right. Phaser stress at 640x480, batched against
  de-batched on the same device the same afternoon:

  | | sprites | shooter | particles | puzzle | vector |
  |---|---|---|---|---|---|
  | Batched (3 runs) | 40 | 0-1 | 62 | 25 | 1-2 |
  | De-batched (2 runs) | **62** | 4-6 | 62 | **36** | 2-3 |

  **The "42 ms/s, not worth it" reading was wrong, and wrong in an instructive way:** that number was
  measured on a Mac with the instrumented shim. The *counts* it produced are hardware-independent,
  and the timings are not -- a malloc/free pair and an indirect call per GL call cost far more on a
  Cortex-A53 with slow memory than on an M-series Mac, and the stress test drives many more calls
  per frame than the mostly-idle Blits app the 42 ms/s came from. Time the boundary on the device
  that has the problem.
- It also buys a runtime with no queue and no backlog mutex, and a GL error that lands on the call
  that caused it rather than at the end of the frame.
- The one allocation left on that path is `toVector()` in `uniform*v` (`SKWebGLMethodsDraw.cpp`),
  which `unpackArg` produces before the helper moves it into the call. Removing it means a span type
  through the argument transform; the uniform calls are well below the top of the call counts, so it
  has not been done.

### Conclusions

- **The Pi is CPU-bound.** A quarter of the pixels leaves Pixi's numbers unchanged and moves Phaser
  by a handful. Hermes has no JIT, and every sprite's update, transform and batch is JavaScript
  running on a Cortex-A53.
- **Where fewer pixels helps, it is because there is less JavaScript to run**, not less fill.
  Phaser's tilemap walks only the visible tiles, so its platformer went from 17.5 to 31.4 fps at
  640x480. That is still short of 54.
- **Pixi runs about 2× Phaser** on sprites, and 4–6× on shooter, particles and puzzle. Phaser's
  per-object overhead (game objects, per-gem tweens, its emitter) is heavier per item.
- **Per-frame work in JS decides what is playable on a Pi 3:**
  - dozens to a hundred or so moving sprites, a few hundred particles, a small puzzle board — yes;
  - a scrolling tilemap platformer in Pixi as written, per-frame `Graphics` redraws, or changing
    canvas text every frame — no.
  - Text that changes rarely (scores, menus) is fine; the cost is only paid on change.
- The Phaser shooter's cost with zero bullets at 1280x720 (~46 fps) is not explained yet.
- Neither the Pixi platformer's cost nor canvas text has been profiled on the Pi.

---

## 8. Problems hit, in order

| Problem | Fix |
|---|---|
| Linux Hermes: `WeakRef` undefined in the smoke test | create the runtime with a microtask queue, as ScreenKit does |
| amd64 image pull: "docker-credential-desktop not found" | Docker Desktop's `bin` on `PATH` |
| Pixi and Blits: "Intl doesn't exist" | rebuild Hermes with Intl |
| Static ICU: undefined references; a cached CMake value kept it static | `HERMES_USE_STATIC_ICU=OFF` explicitly; bundle ICU `.so` files with an `$ORIGIN` runpath |
| `libhermesvm.so` not linked to ICU | shared linker flags name ICU with `--no-as-needed` |
| `Intl.NumberFormat`, `toLocale*Case` return placeholders | shim fallbacks; smoke test asserts only what is real |
| `std::atomic_uint` undeclared (libstdc++) | rules patch `#include <atomic>` |
| No WebGL extensions on ES 2 | seam extension reader over `glGetString` |
| Phaser: no `ANGLE_instanced_arrays` | emulated extension in the shim |
| Pixi: WebGL2 path on a WebGL1 context | remove WebGL2-only constants from WebGL1 |
| `glReadPixels failed` in frame capture | drain latched GL errors first |
| `sans-serif` → DejaVu ExtraLight | nearest-weight face matching |
| Outlined text 4 px low | subtract the outline from SDL_ttf's ascent |
| Stress results all 0 on the Pi | halve a failing start load in a fresh scene; redraw the status only between phases |
| 640x480 mode drawn in the screen's corner (labwc) | offscreen framebuffer + shared present context that letterboxes |
| Hello demos cut off at 640x480 | lay out on a 720-unit design scaled to the screen |
| Lightning blank at 640x480 | fit `pixelRatio` to both axes of the drawable, from the vite plugin |

---

## 9. Not verified

- **Gamepad on the device.** Navigation with a physical controller and the Guide button quitting to
  EmulationStation are implemented, but our runs went through ssh (`pi.sh run/shot/bench`), not a
  controller.
- **Audio** on the Pi was not tested.
- **The test suite on Linux.** `runtime/tests` builds on macOS only (147/147 there); on the Pi,
  verification was the apps themselves, frame captures and screenshots.
- **The x86_64 Hermes archive** passed its smoke test in its container but has not run a ScreenKit
  host.
- **Any other Linux device**: a Pi 4/5 (V3D, GLES 3.1, which would allow WebGL2), 32-bit OSes, X11,
  KMSDRM without a compositor.

## 10. Open questions and next steps

- **Where the Pi's frame time goes.** Profile Hermes on the device before optimising: the Pixi
  platformer at 8 fps with one enemy, per-frame `Graphics`, the Phaser shooter's base cost.
- **Cheaper canvas text.** Compositing happens pixel by pixel in JavaScript. Blending the mask
  natively, or caching rendered runs, would make changing text affordable.
- **Tilemaps.** Rendering static tile chunks once into textures, whether the engine or the app does
  it, is the likely difference between 8 and 54 fps for a platformer.
- **Host the Hermes Linux archives** so `fetch.mjs` gets them without a local build (`hermes.linux.url`
  is null today).
- **`armhf` Hermes** for 32-bit Raspberry Pi OS, which needs an arm/v7 build host or emulator.
- **A network backend on Linux.** `fetch` to the network does not work there yet.
- **Run the runtime tests on Linux**, in the Docker image and on the Pi.
- **Shared runtime (`Architecture.md` M11).** Today each deployment carries its own `screenkit-host`
  and `libhermesvm.so`. SDL3 already comes from the system.
- **Real instancing** is impossible on this GPU. Engines that insist on it (Phaser's `SpriteGPULayer`)
  stay unsupported on a Pi 3.

---

## 11. SpiderMonkey behind JSI (2026-09-19)

`poc/engine-bench` measured SpiderMonkey's JIT running Phaser-shaped JavaScript 5-10x faster than
the Hermes interpreter on this Pi, so the runtime gained a second engine: JSI implemented over
SpiderMonkey 128 ESR (`runtime/core/src/spidermonkey/`, `SCREENKIT_ENGINE=spidermonkey`, Linux
only). Everything above `core/src/engine/Engine.h` is JSI, so the GL, timers, net, text and media
run on it unchanged; the engine, the compiler and the reasons are in `tools/spidermonkey/README.md`.

**Verified:**
- JSI's own conformance suite (the one Hermes and JSC run): 47 of 50 pass; the 3 left out keep two
  runtimes alive on one thread, which SpiderMonkey does not allow (refused with an error). Plus 4
  stencil tests. `sh runtime/tests/jsi/run.sh`.
- On the Pi, from precompiled stencils: `phaser-hello` (image, bitmap font, canvas text),
  `phaser-stress`, `lightning3-blits` and the unmodified `blits-example-app` run and render
  (`ENGINE=spidermonkey sh tools/batocera/pi.sh ...`).
- The Hermes builds are unchanged in behaviour: macOS 186/186 tests, the Linux host builds.

**Stencils** -- SpiderMonkey's bytecode, compiled ahead by `screenkit-smc` for exactly the engine
build the Pi loads and run in place (pinned bytecode). `phaser-hello` from launch to "entry ready":

| | ready | RSS |
|---|---:|---:|
| SpiderMonkey, source | 4.5 s (7.8 s cold) | 75-77 MB |
| SpiderMonkey, lazy stencil | 3.6-4.4 s | 78-79 MB |
| SpiderMonkey, eager stencil | 2.2-2.6 s | 81-85 MB |
| Hermes interpreter, bytecode | 2.6-3.3 s | 55 MB |

With an eager stencil, SpiderMonkey had the app up in 2.2-2.6 s: engine 0.4 s, window and GL 0.5 s,
DOM shim 0.37 s (0.59 s as source), Phaser's bundle and boot 0.95 s.

**Not yet measured validly: frame rates.** A first `phaser-stress` pass on SpiderMonkey gave sprites
250 and shooter 65 (Hermes: 62 and 16-33). The full comparison run that followed came out far lower,
and the reason was the Pi, not the engine: `vcgencmd get_throttled` read **0x50005 -- under-voltage,
throttled now and since boot -- with the ARM at 600 MHz** instead of 1.4 GHz, which
`scaling_cur_freq` does not show. Every Pi number above, and the engine bench's, carries that caveat
until they are re-measured on a sound supply with `get_throttled` read before and after.

## Where the details live

- [`tools/batocera/README.md`](tools/batocera/README.md) — `pi.sh`, the fixed screen, Pi facts, results
- [`tools/prebuilts/README.md`](tools/prebuilts/README.md) — "Hermes on Linux: our own prebuilt" and its gaps
- [`runtime/README.md`](runtime/README.md) — the Linux target, `--size`
- [`runtime/js/README.md`](runtime/js/README.md) — the DOM shim's canvas text
- [`examples/pixi-stress/README.md`](examples/pixi-stress/README.md), [`examples/phaser-stress/README.md`](examples/phaser-stress/README.md) — the stress phases and options
- [`Architecture.md`](Architecture.md) — §7 text paths, §8 the Linux gap
