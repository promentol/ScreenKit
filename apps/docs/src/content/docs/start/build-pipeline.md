---
title: How a build becomes a package
description: Why Hermes has no module loader, what SystemJS is doing in the output, and what ends up inside a .skpkg.
sidebar:
  order: 3
---

## The dominant constraint

**Hermes has no ES-module loader.** Vite emits ESM with code splitting by default, and that cannot
run. Rather than flatten everything into one IIFE, `@screenkit/vite-plugin` makes Vite emit what
`@vitejs/plugin-legacy` emits for browsers without modules: Babel-transpiled **SystemJS** chunks plus
a polyfills bundle carrying the SystemJS loader.

`screenkit bundle` then resolves that module graph **at build time**. It runs every chunk once to
capture its anonymous `System.register`, and overrides `System.instantiate` — SystemJS's own
extension point for where a URL's code comes from — to serve those captured registrations. SystemJS
stays the module loader, and nothing is ever fetched at runtime.

## The pipeline

```
app/                      an ordinary web project — Vite + @screenkit/vite-plugin
  └── vite build
dist-screenkit/           index.html · polyfills + SystemJS chunks · fonts · images
  └── screenkit bundle
      ├── parse index.html → polyfills + legacy entry   (a manifest, never rendered)
      ├── hermesc -dump-ast every chunk → its deps, and every import() must
      │   name a chunk in the build                     ← else the build fails
      ├── pack polyfills + every chunk into ONE script  (served from memory)
      ├── hermesc -emit-binary -O -Xes6-block-scoping   → app.hbc
      ├── copy assets                                   (not index.html, not the chunks)
      └── manifest.json (format, runtimeVersion, hermesBytecodeVersion, entry, sha256 per file)
app.skpkg/                what ships, what an <iframe src> points at, what OTA will deliver
```

## What the plugin adds

- **`@vitejs/plugin-legacy`, output only** (`renderModernChunks: false`, `polyfills: true`, targeting
  `defaults, not IE 11`), resolved from your app so it matches your Vite.
- **core-js polyfills for the ES built-ins the pinned Hermes lacks** — the ones browser targets never
  ask for. A test row holds the engine to that list and records why the rest cannot be polyfilled.
- **Build-time fixes a Lightning 3 / Blits app needs on Hermes**, each matched to the exact code it
  rewrites, failing the build if that code changes shape.
- **The output shape the packer reads**, forced over whatever a merged config says, with a note for
  each replaced value: `outDir` `dist-screenkit/`, `emptyOutDir`, and `base: '/'` (a package is
  served from its own root).

## Dev and release are not the same

Release ships `app.hbc`. Development skips `hermesc` and evaluates source, so reload is fast and
stack traces are real — which also means the two run different module semantics. Code that works in
dev and not in release is nearly always an `import()` the packer could not resolve.

## Hermes itself is never built

One React Native version is pinned as the single source of truth, so `hermesc` and the engine can
never come from different commits: the compiler comes from the `react-native` npm tarball, the Apple
runtime from the `hermes-engine` tarball, and the Android runtime from the
`com.facebook.react:hermes-android` AAR.

The manifest records `hermesBytecodeVersion`, and the loader refuses a mismatch rather than crashing
inside the engine.

:::caution[The one exception]
React Native publishes no Hermes runtime for Linux. Embedded Linux builds it from the pinned commit
through our own prebuilt pipeline, and CI has to assert bytecode-version parity. It is the one place
"never build Hermes" fails.
:::

## What a `.skpkg` is

A directory: `manifest.json`, `app.hbc`, and the app's assets. Zipping it for over-the-air delivery,
and checking the recorded hashes on install, is later work. The host gates on the manifest before
evaluating anything from the package.
