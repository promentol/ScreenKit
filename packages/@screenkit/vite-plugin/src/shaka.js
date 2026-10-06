// `shaka-player` -> @screenkit/shaka, for every build this plugin is in.
//
// Real Shaka Player cannot run on ScreenKit: it feeds segments to MediaSource
// Extensions, and the runtime has none. @screenkit/shaka is a player whose API
// mirrors Shaka's over the runtime's platform players, so an app's Shaka code --
// the Blits example's PlayerManager.js, `import shaka from 'shaka-player'` --
// builds and runs unmodified. Every import of the package or one of its subpaths
// (`shaka-player/dist/shaka-player.compiled.js`, `.../shaka-player.ui.js`)
// resolves to @screenkit/shaka's entry; a stylesheet subpath (the UI's
// controls.css) resolves to an empty module, since there is no DOM to style.
// @screenkit/shaka has no `shaka.ui`: code that uses it fails at the call.
//
// @screenkit/shaka is found, in order: from the app (it may depend on it
// directly, "also importable directly"), from this plugin's own location, then
// as this plugin's sibling in the monorepo (packages/@screenkit/shaka) -- this
// package is linked with `file:`, so its own location has no node_modules.

import { existsSync } from 'node:fs'
import { createRequire } from 'node:module'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'

/** `shaka-player` and every subpath of it. */
export const SHAKA_IMPORT = /^shaka-player(\/.*)?$/

const EMPTY_STYLESHEET = '\0screenkit:shaka-player-stylesheet'

const SIBLING = fileURLToPath(new URL('../../shaka/src/index.js', import.meta.url))

/** Where @screenkit/shaka's entry is, or null: the app's copy first. */
export function resolveScreenkitShaka(roots, sibling = SIBLING) {
  for (const root of roots) {
    try {
      return createRequire(join(root, 'package.json')).resolve('@screenkit/shaka')
    } catch {
      // not installed in this root
    }
  }
  try {
    return createRequire(import.meta.url).resolve('@screenkit/shaka')
  } catch {
    // not beside this plugin either
  }
  return sibling !== null && existsSync(sibling) ? sibling : null
}

/**
 * @param {() => string[]} roots  where to look for the app's own @screenkit/shaka
 * @param {(roots: string[]) => string | null} [find]  how (tests pass their own)
 */
export function shakaOnScreenKit(roots, find = resolveScreenkitShaka) {
  let entry
  return {
    name: 'screenkit:shaka-player',
    enforce: 'pre',
    // Never pre-bundle the real package in dev: its imports are this plugin's.
    config() {
      return { optimizeDeps: { exclude: ['shaka-player'] } }
    },
    resolveId(source) {
      if (!SHAKA_IMPORT.test(source)) return null
      if (/\.css$/.test(source)) return EMPTY_STYLESHEET
      if (entry === undefined) entry = find(roots())
      if (entry === null) {
        const message =
          `@screenkit/vite-plugin: "${source}" resolves to @screenkit/shaka on ScreenKit (real Shaka needs ` +
          'MediaSource Extensions, which the runtime has none of), but @screenkit/shaka is not installed: ' +
          'npm install -D @screenkit/shaka'
        if (typeof this?.error === 'function') this.error(message)
        throw new Error(message)
      }
      return entry
    },
    load(id) {
      return id === EMPTY_STYLESHEET ? 'export default ""' : null
    },
  }
}
