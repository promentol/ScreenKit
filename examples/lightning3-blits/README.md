# Lightning 3 + Blits — hello world

The reference app for **M6** (*"Unmodified Lightning 3 renders a real UI with text"*). It runs in a
browser today; the point is to have a real, unmodified Lightning app on hand before the runtime is
asked to host one.

```sh
npm install
npm run dev      # http://localhost:5174
npm run build    # -> dist/
```

Verified: builds clean, and renders "Hello World" plus a colour-cycling accent bar through
Lightning's WebGL renderer (screenshotted in headless Chromium).

## What it is

| | |
|---|---|
| `@lightningjs/blits` | 2.9.0 — the Lightning 3 app framework |
| `@lightningjs/renderer` | 3.3.1 — pulled in by Blits, does the actual WebGL work |
| `src/App.js` | one `Blits.Component` with a template string, reactive state, a `ready()` hook |
| `src/index.js` | `Blits.Launch(App, 'app', { w: 1920, h: 1080 })` |

Design resolution is 1920x1080 and Lightning scales it to the canvas, so the same numbers hold on a
4K panel.

## Why the Vite config matters to this project

`@lightningjs/blits/vite` is **an array of plugins**, not one. Two of them are exactly what
`Architecture.md` M5 and M6 assume exist:

- **`preCompiler`** turns each component's template string into render instructions at build time
  instead of parsing it on the device. This is the kind of work `Architecture.md` M5's Vite plugin has
  to preserve — a bundler that discards it moves that cost back onto a TV CPU.
- **`msdfGenerator`** produces the signed-distance-field font atlases Lightning uses for text. M6
  names MSDF directly. Note it **scans `public/` at build time and fails the build if that directory
  does not exist** — an empty `public/fonts/` is enough, and this repo keeps one for that reason.

## Numbers worth keeping

A production build of this hello world is **330 KB raw / 93 KB gzipped** of JavaScript. That is
Lightning plus Blits plus a three-node template — the framework floor, before any app code.

It is not the M2 budget (that measures the **DOM layer** against 500 KB of *bytecode*, a different
artifact), but it is the closest real datapoint we have, and it says the framework itself is not
small. Worth re-measuring as `.hbc` when the runtime can host it.

## Running on ScreenKit

Runs unmodified on the tvOS simulator: MSDF text, the colour cycle, 60 fps, no errors in the log.
Verified by simulator screenshots whose sampled pixels change between frames.

```sh
npm run build:screenkit   # vite build (vite.screenkit.config.js) -> dist-screenkit/, then screenkit bundle -> app.skpkg/
SCREENKIT_APP_PKG=app.skpkg ../../runtime/scripts/build-tvos-simulator.sh
xcrun simctl install booted ../../runtime/build/tvos-simulator/screenkit-host.app
xcrun simctl launch booted dev.screenkit.host
```

On macOS: `../../runtime/build/macos/screenkit-host --window app.skpkg`.

`vite.screenkit.config.js` adds `@screenkit/vite-plugin` (`packages/@screenkit/vite-plugin`) to the
same app: a `@vitejs/plugin-legacy` build plus the Lightning fixes below, written to
`dist-screenkit/`. `screenkit bundle` (`packages/@screenkit/cli`) checks the chunk graph, packs the
SystemJS chunks into one script, compiles it with the pinned `hermesc` and writes `app.skpkg/`:
`manifest.json`, `app.hbc` and `fonts/`. The host checks the manifest's bytecode and runtime versions
before it runs anything, and the package directory is the app's asset root, so `/fonts/...` resolves
inside it. Both packages come from this repo as `file:` dependencies (`npm install` links them).

What it took. Each item is either the runtime behaving like a browser or a build step working around
the engine, and each has a test row in `runtime/tests`:

| Symptom | Cause | Fix |
|---|---|---|
| Boots, 60 fps, black screen | expo-gl queues non-blocking GL calls; nothing ran them before the swap | `gfx::presentFrame` flushes the queue at frame end (`dom-present-frame`) |
| Black between redraws | The host swapped every frame; Lightning draws only when dirty | Swap only frames that painted the default framebuffer (same row) |
| First state change: `undefined is not a function` | Hermes ships with ES6 block scoping off, so `for (let ...)` closures share one binding | `withES6BlockScoping` + `hermesc -Xes6-block-scoping` (`block-scoping`) |
| A quad's colour turns `(5,5,202)` after it changes | Hermes canonicalises NaN bit patterns; Lightning copies packed colours via `Float32Array` | Build transform to `TypedArray.set` (`float-bit-patterns`) |

The build transform lives in `packages/@screenkit/vite-plugin/src/lightning.js`, so every app
that uses the plugin gets it, and it fails the build if the code it rewrites changes shape. It is worth sending to Lightning upstream: `set(subarray())`
is also one call instead of twenty.

`App.js` uses `:color` (reactive) and hex colours. Blits parses `hsl()` but rejects it at runtime, in
browsers too.
