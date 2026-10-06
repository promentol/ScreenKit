---
title: Quickstart
description: Add a second Vite config, build it, pack it into a .skpkg, and run it.
sidebar:
  order: 2
---

A ScreenKit build is a **second Vite config**, never a change to your first one. Your browser build
and its `dist/` are left exactly as they were.

## 1. Add the plugin

```js title="vite.screenkit.config.js"
import { defineConfig } from 'vite'
import screenkit from '@screenkit/vite-plugin'

export default defineConfig({
  plugins: [screenkit()],
})
```

If your app already has a config worth keeping, merge it and put `screenkit()` first — it forces the
output shape the packer reads, and tells you in the build log about every value it replaces.

## 2. Build

```sh
vite build --config vite.screenkit.config.js
```

Output lands in `dist-screenkit/` (not `dist/`): an `index.html`, a polyfills bundle carrying the
SystemJS loader, `assets/*-legacy-*.js` chunks, and your fonts and images.

## 3. Pack

```sh
npx screenkit bundle
```

This turns the build into `app.skpkg/` — a **directory**, not a zip: `manifest.json`, `app.hbc` and
your assets. The packer parses every chunk with the pinned `hermesc`, resolves the module graph at
build time, compiles to Hermes bytecode and records a sha256 per file.

```sh
screenkit bundle [dist] [--out <package>]
#   dist          the build output          (default: dist-screenkit)
#   --out <dir>   where to write the package (default: app.skpkg)
```

## 4. Run it

Point a host at the package. On macOS, during development:

```sh
screenkit-host app.skpkg
```

The host gates on `manifest.json` **before evaluating anything** from the package — a bytecode
version it cannot run, or a file whose hash does not match, is refused with a message rather than
crashing inside the engine.

## What will fail the build, on purpose

The packer is strict where a browser would be forgiving, because there is no module loader at runtime
to recover with:

- **A dynamic `import()` it cannot resolve.** `import(variable)`, a template with holes, or a literal
  naming a chunk that is not in the build fails the build, naming the chunk and the call site. An
  `import()` with a literal argument that resolves to a real chunk is supported and packed like any
  other.
- **Passing the import context around.** Stored, destructured or handed to a function — an `import()`
  made through it would go unchecked.
- **Code Hermes cannot compile**, reported with `hermesc`'s first diagnostic against the chunk it
  came from.
- **A font without its MSDF atlas.**
- **`@vitejs/plugin-legacy` twice**, as when a merged config brings its own preset.

:::note
`index.html` is read as a manifest of entry points. Anything beyond the polyfill and entry
`<script>`s is ignored — it is never rendered as markup.
:::

## Using Shaka Player

`shaka-player` — and every subpath of it — resolves to
[`@screenkit/shaka`](/reference/shaka/), which **mirrors Shaka Player 5's API** over the platform's
own player. Shaka's own code cannot run here (it feeds Media Source Extensions, and there are none),
but *your* code written against Shaka builds and runs unmodified: the same `Player`, the same
`configure` merge, the same networking-engine filters, the same events and the same error codes.

```js
import shaka from 'shaka-player'   // resolves to @screenkit/shaka

const player = new shaka.Player()
await player.attach(video)
await player.load('https://cdn.example/stream.mpd')
```
