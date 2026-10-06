# PixiJS hello world

An ordinary PixiJS 8 app -- a logo spinning and pulsing above "Hello, PixiJS!" on a fullscreen canvas
-- built with Vite and run on ScreenKit. `src/main.js` is plain Pixi: `Application` with
`resizeTo: window`, `Assets.load`, a `Sprite`, `BitmapText`, the ticker.

```sh
npm install
npm run dev               # the browser build, http://localhost:5176
npm run build:screenkit   # vite build (vite.screenkit.config.js) -> dist-screenkit/, screenkit bundle -> app.skpkg/
../../runtime/build/macos/screenkit-host --window app.skpkg
```

Verified 2026-09-17 on macOS: clean log, and a frame captured with `SCREENKIT_CAPTURE` shows the logo
and crisp MSDF text.

## Two things specific to a TV runtime

- **The title is an MSDF bitmap font**, `public/assets/lato-black-msdf.xml` + `.png` (Lato Black, SIL
  Open Font License), drawn through WebGL; the caption below it is Pixi's `Text`, rasterised on the 2D
  canvas in an installed font.
  The font is BMFont **XML**, because Pixi's text-format `.fnt` parser skips the `distanceField` line
  (its line pattern is lowercase-only) and would draw the atlas raw. Regenerate the assets with
  `node ../scripts/make-hello-assets.cjs public/assets`.
- **`Assets.setPreferences({ preferWorkers: false })`**: Pixi decodes textures in a Web Worker by
  default, and ScreenKit has no Workers. Browsers are unaffected.

## What running it took

Pixi was the second non-Lightning engine here, and it found runtime gaps -- fixed in the runtime, not
worked around in this app:

- **Throwaway probe canvases.** `isWebGLSupported()` asks a scratch canvas for a context and drops it;
  the renderer's own canvas then asked the one GL surface and was refused. A canvas that holds the
  surface but is not in the page now gives it up to the next one that asks.
- **`WebGL2RenderingContext` inherited from `WebGLRenderingContext`** (upstream expo-gl), so
  `gl instanceof WebGLRenderingContext` was true and Pixi took WebGL2 for WebGL1. The two interfaces
  stand alone now, as in a browser.
- **`<video>` had no `canPlayType`**; Pixi probes `video/mp4` while it boots. Media elements exist now
  and answer `''`.
- **No `DOMParser`**, which BMFont XML needs: an XML `DOMParser` now builds an `XMLDocument`.
- **No 2D context**: a software subset exists now (see `runtime/js/README.md`).
