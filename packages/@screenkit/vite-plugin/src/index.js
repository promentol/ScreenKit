// @screenkit/vite-plugin -- the "vite build" half of the M5 pipeline.
//
//   // vite.screenkit.config.js
//   import screenkit from '@screenkit/vite-plugin'
//   export default defineConfig({ plugins: [screenkit(), ...yourPlugins] })
//
//   vite build --config vite.screenkit.config.js   ->  dist-screenkit/
//   screenkit bundle                               ->  app.skpkg/
//
// It is additive: the app's own vite.config.js, and so its browser build, is
// left exactly as it was. A ScreenKit build is a second config that adds this.
//
// What it adds:
//
//   - @vitejs/plugin-legacy, output-only. Legacy transpiles with Babel down to
//     `targets` and injects core-js polyfills for what those targets lack, which
//     is conservative ground for Hermes. Those targets are browsers, though, so
//     the ES built-ins Hermes itself lacks are added to the polyfills bundle
//     explicitly (hermes-builtins.json). It emits SystemJS (`System.register`)
//     chunks plus a polyfills bundle carrying the SystemJS loader; `screenkit
//     bundle` resolves that chunk graph at build time and packs it into one
//     script. renderModernChunks: false -- the modern ESM output is what the
//     browser build already produces, and Hermes cannot load it.
//
//   - The Lightning 3 fixes in lightning.js, fresh for each build, failing the
//     build if its target module is gone.
//
//   - The output shape `screenkit bundle` reads, forced over whatever the merged
//     config says -- a ScreenKit config often merges the app's own, and that one
//     was written for a browser:
//       build.outDir  'dist-screenkit' (or screenkit({ outDir })), so the browser
//                     build's dist/ is never written to;
//       build.emptyOutDir true, so no stale chunk from an earlier build is packed;
//       base '/', because the package root is the asset root on device -- a
//                     subpath base names files the package does not have.
//     A different value in the config is overridden with a note, not silently.
//
//   - `shaka-player` resolved to @screenkit/shaka (shaka.js): Shaka's API over
//     the runtime's platform players, since real Shaka needs MediaSource
//     Extensions and the runtime has none. Shaka code builds unmodified.
//
//   - A check that plugin-legacy is added once. A second copy (an app config's
//     own legacy() preset) writes a second output shape into the same build.
//
// plugin-legacy is resolved from the app, never bundled or depended on here: it
// has to be the copy that matches the app's Vite (7 or 8), and this package is
// linked in with `file:`, so its own location has no node_modules to look in.
// "The app" is, in order: screenkit({ root }), the package npm is running a
// script for (npm_package_json), then the working directory.

import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { dirname, join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

import { lightningOnScreenKit } from './lightning.js'
import { shakaOnScreenKit } from './shaka.js'

export { BLITS_CONTAIN_FIT, blitsFitsTheDrawable, hermesBitExactQuadCopy, lightningOnScreenKit } from './lightning.js'
export { resolveScreenkitShaka, SHAKA_IMPORT, shakaOnScreenKit } from './shaka.js'

export const DEFAULT_OUT_DIR = 'dist-screenkit'

/** The ES built-ins the pinned Hermes lacks, and the core-js module for each that has one. */
export const HERMES_BUILTINS = JSON.parse(readFileSync(new URL('./hermes-builtins.json', import.meta.url), 'utf8'))

// plugin-legacy resolves these from its own core-js dependency.
export const HERMES_POLYFILLS = Object.freeze(
  HERMES_BUILTINS.missing.filter((gap) => gap.polyfill).map((gap) => `core-js/modules/${gap.polyfill}.js`),
)

// Fixed, not options: `screenkit bundle` reads exactly this output shape -- one
// legacy entry, SystemJS chunks, the polyfills bundle that defines `System`.
const LEGACY_OPTIONS = Object.freeze({
  targets: ['defaults', 'not IE 11'],
  renderModernChunks: false,
  polyfills: true,
  additionalLegacyPolyfills: HERMES_POLYFILLS,
})

/**
 * @param {{ root?: string, outDir?: string }} [options]
 *   root   -- where to resolve @vitejs/plugin-legacy from; see the header.
 *   outDir -- the build output, if not dist-screenkit. `screenkit bundle` then
 *             needs the same directory: `screenkit bundle <outDir>`.
 */
export default function screenkit(options = {}) {
  const outDir = options.outDir ?? DEFAULT_OUT_DIR
  // Where the app's own @screenkit/shaka would be: the same roots legacy is
  // resolved from, and the root Vite settles on.
  let appRoot
  const shakaRoots = () => legacyRoots(options.root ?? appRoot)
  return [
    ...lightningOnScreenKit(),
    { ...shakaOnScreenKit(shakaRoots), configResolved(resolved) { appRoot = resolved.root } },
    {
      name: 'screenkit:build-defaults',
      config(config) {
        const forced = { base: '/', build: { outDir, emptyOutDir: true } }
        for (const note of overrides(config, forced)) console.warn(`[screenkit] ${note}`)
        return forced
      },
      configResolved(resolved) {
        const copies = resolved.plugins.filter((p) => p?.name === LEGACY_CONFIG_PLUGIN).length
        if (copies > 1) {
          throw new Error(
            `@screenkit/vite-plugin: @vitejs/plugin-legacy is in this build ${copies} times. screenkit() ` +
              'adds it with the options `screenkit bundle` reads; remove the other legacy() -- often a ' +
              'browser preset in the app config this ScreenKit config merges.',
          )
        }
      },
    },
    // Vite flattens a promise inside `plugins`, which is what lets the app's own
    // copy be imported lazily from a synchronous factory.
    loadLegacy(legacyRoots(options.root)).then((legacy) =>
      legacy({ ...LEGACY_OPTIONS, additionalLegacyPolyfills: [...HERMES_POLYFILLS] }),
    ),
  ]
}

// plugin-legacy registers several plugins; this one exists once per legacy().
const LEGACY_CONFIG_PLUGIN = 'vite:legacy-config'

/** What the merged config set that `forced` replaces -- one note per value. */
function overrides(config, forced) {
  const notes = []
  if (config.base !== undefined && config.base !== forced.base) {
    notes.push(`base ${JSON.stringify(config.base)} replaced by "/": a .skpkg is served from its own root`)
  }
  const outDir = config.build?.outDir
  if (outDir !== undefined && outDir !== forced.build.outDir) {
    notes.push(
      `build.outDir ${JSON.stringify(outDir)} replaced by ${JSON.stringify(forced.build.outDir)}: ` +
        'pass screenkit({ outDir }) to choose it',
    )
  }
  if (config.build?.emptyOutDir === false) {
    notes.push('build.emptyOutDir false replaced by true: a stale chunk would be packed')
  }
  return notes
}

/** Where to look for the app's plugin-legacy, most specific first, without repeats. */
export function legacyRoots(root, env = process.env, cwd = process.cwd()) {
  const roots = []
  if (root !== undefined) roots.push(resolve(root))
  if (env.npm_package_json) roots.push(dirname(resolve(env.npm_package_json)))
  roots.push(resolve(cwd))
  return [...new Set(roots)]
}

async function loadLegacy(roots) {
  for (const root of roots) {
    let entry
    try {
      entry = createRequire(join(root, 'package.json')).resolve('@vitejs/plugin-legacy')
    } catch {
      continue
    }
    const mod = await import(pathToFileURL(entry).href)
    return mod.default ?? mod
  }
  throw new Error(
    `@screenkit/vite-plugin: @vitejs/plugin-legacy is not installed in ${roots.join(', or ')}. ` +
      'A ScreenKit build is a legacy (SystemJS) build, and the plugin must come from the app ' +
      'so it matches its Vite: npm install -D @vitejs/plugin-legacy terser',
  )
}
