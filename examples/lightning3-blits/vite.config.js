import { defineConfig } from 'vite'
import blits from '@lightningjs/blits/vite'

// `blits` is an array of plugins, not one: injectDevConfig, blitsFileConverter,
// reactivityGuard, preCompiler and msdfGenerator. The precompiler turns each
// component's template string into render instructions at build time instead of
// parsing it on device, and msdfGenerator produces the signed-distance-field
// atlases Lightning uses for text -- both are what Architecture.md M5/M6 assume.
export default defineConfig({
  plugins: [blits],
  server: { port: 5174 },
})
