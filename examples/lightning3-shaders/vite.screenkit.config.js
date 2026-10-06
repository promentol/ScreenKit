import { defineConfig } from 'vite'
import blits from '@lightningjs/blits/vite'
import screenkit from '@screenkit/vite-plugin'

// The ScreenKit build: the same app plus @screenkit/vite-plugin, packaged into
// app.skpkg/ by `npm run build:screenkit`. See
// examples/lightning3-blits/vite.screenkit.config.js for why screenkit() goes first.
export default defineConfig({
  plugins: [screenkit(), blits],
})
