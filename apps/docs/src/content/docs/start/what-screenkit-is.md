---
title: What ScreenKit is
description: A prebuilt native runtime that runs an ordinary web build on TV hardware, and the reasoning behind the parts it leaves out.
sidebar:
  order: 1
---

ScreenKit takes an ordinary web build — whatever `vite build` emits — and runs it on a television
through [Hermes](https://hermesengine.dev/) and WebGL. The targets are tvOS, Android TV, Fire TV and
embedded Linux (Batocera ports); macOS is a development target.

## The model is Capacitor, not Expo

The runtime is a **prebuilt binary shell**. An app is a web build plus assets; it never triggers a
native compile. There is no autolinking, no config plugins, no generated Podfile or Gradle file.
Adding a native capability means shipping a new runtime version, not rebuilding every app.

Embedding another app is `<iframe>`.

## Native owns only what native can do

An earlier design put the DOM, the WebGL object model and the element shims in C++, which is what
made it enormous. The rule now:

| Native (C++) | JavaScript (the DOM shim, inside the runtime) |
|---|---|
| SDL3 event loop, window, input, audio | `document`, `Element`, `Event`, `EventTarget`, tree ops |
| Hermes host, bytecode loading, instance threads | `HTMLCanvasElement`, `HTMLVideoElement`, `HTMLIFrameElement` |
| GL context management and the GLES entry points | the `WebGLRenderingContext` object model and its validation |
| The compositor: layer list → swapchain, fences | the CSS subset → layer rects |
| Platform media players | `window`, `navigator`, timers, `rAF` |
| Sockets, TLS, file and asset IO, image decode | `URL`, `Blob`, `postMessage`, `MessageEvent` |

The native surface exposed to JavaScript is a narrow handle API — make a layer, set its rect, hand me
a texture, play this URL — not a browser.

## Only three elements paint

A real element tree exists: `Node`, `Element`, `HTMLElement` and `Document`, with attributes,
`id`/`className`/`classList`, the tree operations and their DOM exceptions, and events that capture,
reach the target and bubble to `window`. `querySelector`, `querySelectorAll`, `matches` and `closest`
take a selector subset and throw a `SyntaxError` for anything outside it rather than quietly matching
nothing.

But only three elements have something native behind them:

| `document.createElement(…)` | Backed by |
|---|---|
| `'canvas'` | a GL framebuffer. Each canvas gets **its own GL context**, and each but the page's frame its own compositor layer |
| `'video'` | the platform player — AVPlayer, Media3 ExoPlayer, or libvlc |
| `'iframe'` | a full instance: its own Hermes runtime, thread, GL context and layer |

Every other tag constructs an ordinary, inert element. It lives in the tree, dispatches events, and
never renders.

## CSS applies to those three, and to nothing else

`position`, `left`/`top`/`right`/`bottom`, `width`/`height`, `z-index`, `opacity`, `display: none`
and `transform: translate|scale` are parsed and mapped onto compositor layer rects. There is no
cascade for anything else, no layout engine, and no text rendering from CSS. Every element has a
`CSSStyleDeclaration` that stores and reads back what you set; only a backed element parses the
subset. See [the CSS subset](/guides/css/).

## Text comes from atlases, not from the cascade

A Lightning 3 UI draws text from **MSDF atlases generated at build time**, so the runtime needs no
FreeType, no HarfBuzz and no Canvas2D path rasteriser for it. A 2D canvas can still draw text —
glyphs come from SDL3\_ttf — which is what Pixi, Phaser and Lightning rasterise their `Text` objects
on.

The cost is real: no arbitrary runtime font loading, no complex-script shaping unless it was baked
ahead of time, and a fixed glyph set per font. Right for a TV UI; wrong for a text-heavy app.

## What that buys, and what it costs

The environment is enumerated rather than approximated — see
[the environment contract](/reference/environment/) for the full list of what is provided and what is
absent by design. The short version: `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, a cookie
jar, `localStorage`, `structuredClone`, `Blob`/`File`, WebGL 1 and 2, `Image`, `postMessage` and the
timers all work. Web Workers, Service Workers, IndexedDB, History, SVG, layout and flexbox do not,
and will not.
