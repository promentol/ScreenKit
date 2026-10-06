import Blits from '@lightningjs/blits'
import App from './App.js'

// 1920x1080 is the design resolution; Lightning scales it to the canvas, so the
// same numbers in App.js hold on a 4K panel.
Blits.Launch(App, 'app', {
  w: 1920,
  h: 1080,
  // Blits picks its scale from the screen height only for 720, 1080 and 2160 and draws
  // unscaled otherwise, so a 640x480 screen would show the top-left corner of the stage.
  // Fit the 1920x1080 design to whatever screen there is instead.
  pixelRatio: Math.min(window.innerWidth / 1920, window.innerHeight / 1080),
  debugLevel: 1,
  renderMode: 'webgl',
  // 'lato' is MSDF: the Blits Vite plugin turns the .ttf into
  // Lato-Regular.msdf.{json,png} at build time and a shader samples the atlas.
  // 'lato-web' is the same file as a web font, which Lightning rasterises
  // through a 2D canvas on the device.
  fonts: [
    { family: 'lato', type: 'msdf', file: '/fonts/Lato-Regular.ttf' },
    { family: 'lato-web', type: 'web', file: '/fonts/Lato-Regular.ttf' },
  ],
  defaultFont: 'lato',
})
