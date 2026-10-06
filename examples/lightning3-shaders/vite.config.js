import { defineConfig } from 'vite'
import blits from '@lightningjs/blits/vite'

// The browser build. `npm run dev` is the reference rendering: whatever this
// shows in Chrome is what the ScreenKit build must show on a TV.
export default defineConfig({
  plugins: [blits],
  server: { port: 5175 },
})
