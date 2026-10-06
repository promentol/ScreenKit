# three.js cube

An ordinary three.js app -- a lit cube spinning on a fullscreen canvas -- built with Vite and run on
ScreenKit unmodified. `src/main.js` is plain three.js (0.186): `WebGLRenderer`, `PerspectiveCamera`,
`MeshStandardMaterial`, an ambient and a directional light, `setAnimationLoop`, a `resize` handler.

```sh
npm install
npm run dev               # the browser build, http://localhost:5175
npm run build:screenkit   # vite build (vite.screenkit.config.js) -> dist-screenkit/, screenkit bundle -> app.skpkg/
```

Run it on macOS:

```sh
../../runtime/build/macos/screenkit-host --window app.skpkg
```

Run it on the tvOS simulator (this embeds it in place of whatever package the app carried before):

```sh
SCREENKIT_APP_PKG=examples/threejs-cube/app.skpkg runtime/scripts/build-tvos-simulator.sh   # from the repo root
runtime/scripts/run-tvos-simulator.sh
```

Verified 2026-09-17: on macOS the log is clean and frames run at the display's 120 Hz; on the tvOS
simulator the package gate, the ready line and ~51 fps at 4K (the simulator's GPU, not a device's),
with screenshots of the cube at different rotations. The package is 867 KB of bytecode, most of it
three.js.

## What running it took

three.js is the first WebGL app here that is not Lightning, and it found four gaps in the runtime --
fixed there, not worked around here:

- **`document.createElementNS`** was absent; `WebGLRenderer` makes its canvas with it. The HTML
  namespace now gives the same backed canvas `createElement` does (`dom-canvas-create`).
- **The drawable had no depth buffer**, while `getContextAttributes()` said `depth: true`. One convex
  cube hides that (back faces are culled); overlapping geometry would draw in submission order. The
  surface is now D24/S8 (`gl-context` draws a far quad after a near one).
- **`pixelStorei`** logged "doesn't support this parameter" for `PACK_ALIGNMENT` and
  `UNPACK_COLORSPACE_CONVERSION_WEBGL`, which the renderer resets on startup. Every WebGL1/2 parameter
  is handled now (`gl-pixel-store`).
- **`renderer.setSize(innerWidth, innerHeight)`** warned that a canvas size write was ignored, though
  it wrote the size the canvas already has. Same-size writes are silent now (`dom-dimension-write`).
