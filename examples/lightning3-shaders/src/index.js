import Blits from '@lightningjs/blits'
import App from './App.js'
import { Plasma } from './shaders/Plasma.js'

Blits.Launch(App, 'app', {
  w: 1920,
  h: 1080,
  debugLevel: 1,
  renderMode: 'webgl',
  fonts: [{ family: 'lato', type: 'msdf', file: '/fonts/Lato-Regular.ttf' }],
  defaultFont: 'lato',
  // Custom shaders are registered by name next to the built-in ones
  // (blits/src/engines/L3/shaderLoader.js), then used as `shader="{type: ...}"`.
  shaders: [{ name: 'plasma', type: Plasma }],
})
