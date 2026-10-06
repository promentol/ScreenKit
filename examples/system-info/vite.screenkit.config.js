import { defineConfig, mergeConfig } from 'vite'
import screenkit from '@screenkit/vite-plugin'

import appConfig from './vite.config.js'

// The ScreenKit build: the same app plus @screenkit/vite-plugin -- a legacy
// (SystemJS) build written to dist-screenkit/. `npm run build:screenkit` then
// runs `screenkit bundle`, which packs it into app.skpkg/.
export default defineConfig(mergeConfig(appConfig, { plugins: [screenkit()] }))
