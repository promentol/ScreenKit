import { defineConfig } from 'vite'
import blits from '@lightningjs/blits/vite'
import screenkit from '@screenkit/vite-plugin'

// The ScreenKit build: the same app, plus @screenkit/vite-plugin -- a legacy
// (SystemJS) build and the Lightning/Blits fixes, written to dist-screenkit/.
// `npm run build:screenkit` then runs `screenkit bundle`, which packs that into
// app.skpkg/. The browser build (vite.config.js) is untouched.
//
// screenkit() goes first: its Lightning fixes are `enforce: 'pre'` transforms
// that must see Blits' and the renderer's source before Blits' own plugins do.
export default defineConfig({
  plugins: [screenkit(), blits],
})
