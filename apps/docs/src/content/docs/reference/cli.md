---
title: screenkit bundle
description: Turn a finished ScreenKit Vite build into a .skpkg package, and the manifest gate the host applies to it.
---

```sh
screenkit bundle [dist] [--out <package>]
```

| Argument | Default | Meaning |
|---|---|---|
| `dist` | `dist-screenkit` | the finished Vite build to pack |
| `--out <dir>` | `app.skpkg` | where to write the package |

Packaging is a **separate process from the dev server**. It consumes a finished build and emits a
package; it is not part of `vite dev`, does not run on file change, and never needs to be fast.

## What it does

1. Parses `index.html` for the polyfill and legacy entry `<script>`s — a manifest of entry points,
   never markup.
2. Parses every chunk with the pinned `hermesc` to find its `System.register` dependencies, and
   checks every `import()`.
3. Packs the polyfills and every chunk into one script, served from memory by SystemJS.
4. Compiles it with `hermesc -emit-binary -O -Xes6-block-scoping` → `app.hbc`.
5. Copies your assets — not `index.html`, not the chunks.
6. Writes `manifest.json`.

## The manifest

```json
{
  "format": 1,
  "runtimeVersion": 2,
  "hermesBytecodeVersion": 96,
  "entry": "app.hbc",
  "files": { "app.hbc": "<sha256>" }
}
```

A `.skpkg` is a **directory**, not a zip. The package directory is the app's asset root, so
`/fonts/…` resolves inside it.

## The gate

The host validates the manifest **before evaluating anything** from the package, and refuses it when:

- the manifest is missing, is not valid JSON, is not an object, or its `format` is not `1`;
- `hermesBytecodeVersion` is not its engine's;
- `runtimeVersion` is below 1, or newer than the runtime's own;
- `entry` does not resolve, after following symlinks, to a regular file inside the package.

Each refusal is a logged error naming the manifest file — and both versions, for a version mismatch —
and a failed launch, rather than a crash inside the engine.

## When the entry itself fails

A package that passes the gate can still fail: its entry module can reject, which happens in a
microtask after evaluation returns. That is reported with the entry name and the error, and the host
acts on it:

- headless and `--window` **exit 65**, so `screenkit-host app.skpkg` is a usable CI assertion;
- tvOS ends the app with `SDL_APP_FAILURE` rather than showing a blank screen.

A resolved entry logs `screenkit bundle: entry "<entry>" ready`.

## Build failures worth recognising

| Message | Cause |
|---|---|
| an `import()` that cannot be resolved | `import(variable)`, a template with holes, or a literal naming a chunk not in the build |
| the import context used indirectly | stored, destructured or passed to a function |
| a named `System.register` | the packed loader cannot serve one |
| a `hermesc` diagnostic against a chunk | code Hermes cannot compile |
| a font without its MSDF atlas | the atlas was not written into the build |

:::note
`run`, `deploy` and `doctor` are listed as later work. `bundle` is the only subcommand today.
:::
