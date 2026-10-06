# Phaser hello world

An ordinary Phaser 4 game -- a scene with a logo spinning and pulsing above "Hello, Phaser!" on a
fullscreen canvas -- built with Vite and run on ScreenKit. `src/main.js` is plain Phaser:
`Phaser.Game` with `type: WEBGL` and `Scale.RESIZE`, a scene with `preload`/`create`, `load.image`,
`load.bitmapFont`, tweens.

```sh
npm install
npm run dev               # the browser build, http://localhost:5177
npm run build:screenkit   # vite build (vite.screenkit.config.js) -> dist-screenkit/, screenkit bundle -> app.skpkg/
../../runtime/build/macos/screenkit-host --window app.skpkg
```

Verified 2026-09-17 on macOS: clean log, and a frame captured with `SCREENKIT_CAPTURE` shows the logo
and the text.

**The title is a bitmap font**, `public/assets/lato-black-plain.xml` + `.png` (Lato Black, SIL Open Font
License); the caption below it is Phaser's `Text`, rasterised on the 2D canvas in Courier. Phaser's
BitmapText has no distance-field support, so this is a plain alpha atlas derived from the MSDF one
(`node ../scripts/make-hello-assets.cjs public/assets`); it is sharpest at its native 64 px.

## What running it took

Phaser leans on the browser more than Pixi does, and each of these was a runtime gap, fixed in the
runtime:

- **A 2D context at import.** `CanvasFeatures` draws into a 2D canvas (`fillRect`, `getImageData`,
  `drawImage` with `multiply`) the moment Phaser is imported, with no null check. The runtime now has a
  software Canvas2D subset -- rectangles, images, pixel data, transforms, compositing; no paths or
  text.
- **`data:` URLs.** Phaser's default textures are base64 PNGs loaded through `Image`.
- **`DOMContentLoaded`.** Phaser boots on it (or on `document.readyState`), and neither existed: the
  game was created and never started. The document is now `'loading'` while the app runs, then
  `'interactive'` with `DOMContentLoaded` and `'complete'` with `load`.
- **`window.screen`** (its scale manager listens on `screen.orientation`).
- **Numeric WebGL arguments.** Phaser passes a one-element array to `uniform1i`; WebIDL converts it
  with ToNumber, and the vendored bindings threw. They convert as a browser does now.
- **`<body>` had no size.** `Scale.RESIZE` sizes the game from `document.body.getBoundingClientRect()`,
  which was 0x0 without layout, so the game rendered at 0x0 -- a black screen. `<html>` and `<body>`
  fill the screen now.
- The probe-canvas, `DOMParser` and WebGL-class fixes the Pixi sample needed (`../pixi-hello`).
