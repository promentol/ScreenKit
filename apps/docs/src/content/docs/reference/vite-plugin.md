---
title: '@screenkit/vite-plugin'
description: Options, what the plugin forces, and the errors it raises on purpose.
---

```js title="vite.screenkit.config.js"
import { defineConfig } from 'vite'
import screenkit from '@screenkit/vite-plugin'

export default defineConfig({
  plugins: [screenkit()],
})
```

The plugin is **additive**: your app's own `vite.config.js`, and so its browser build, is left
exactly as it was. A ScreenKit build is a second config.

## Options

```ts
screenkit({ root?: string, outDir?: string })
```

| Option | Default | What it does |
|---|---|---|
| `root` | the package npm is running a script for, else the working directory | Where `@vitejs/plugin-legacy` and `shaka-player` are resolved from, so they match your app's Vite (7 or 8) |
| `outDir` | `dist-screenkit` | The build output. `screenkit bundle` then needs the same directory: `screenkit bundle <outDir>` |

## What it forces

These are overridden over whatever a merged config says — a ScreenKit config often merges the app's
own, and that one was written for a browser. Each replacement is announced in the build log, never
silent.

| Setting | Forced to | Why |
|---|---|---|
| `build.outDir` | `dist-screenkit` | so the browser build's `dist/` is never written to |
| `build.emptyOutDir` | `true` | so no stale chunk from an earlier build is packed |
| `base` | `'/'` | a package is served from its own root; a subpath base names files the package does not have |

## What it adds

- **`@vitejs/plugin-legacy`, output only** — `renderModernChunks: false`, `polyfills: true`,
  targeting `defaults, not IE 11`. Legacy transpiles with Babel and emits SystemJS chunks plus a
  polyfills bundle carrying the loader, which is what `screenkit bundle` resolves at build time.
- **core-js polyfills for the ES built-ins the pinned Hermes lacks** — the ones browser targets never
  ask for.
- **Lightning 3 / Blits build-time fixes**, each matched to the exact code it rewrites.
- **`shaka-player` resolved to `@screenkit/shaka`** — Shaka's API over the platform players, since
  real Shaka needs Media Source Extensions. Shaka code builds unmodified.

## Errors it raises on purpose

- **`@vitejs/plugin-legacy` present twice**, as when a merged app config brings its own preset.
- **A Lightning fix whose target module changed shape**, or whose package is in the build while its
  target module never is (moved or renamed).

## Not done by the plugin

It does not rewrite `import.meta.url`, pin `build.target`, warn about meaningful markup in
`index.html`, or generate MSDF atlases. A Lightning / Blits app's atlases come from Blits' own
`msdfGenerator` plugin, which writes them into the build like any other asset;
`screenkit bundle` then copies them into the package.
