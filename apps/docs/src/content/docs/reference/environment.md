---
title: The environment contract
description: Exactly what a bundle runs against — what is provided, what is a subset, and what is absent by design.
---

"No DOM" is an endless source of surprises unless the surface is enumerated. This is the
compatibility contract.

## Provided

**Core**
`console` · `setTimeout` / `setInterval` · `queueMicrotask` · `Promise` · `performance` ·
`requestAnimationFrame` · `structuredClone` · `crypto.getRandomValues` · `localStorage` ·
`TextEncoder` / `TextDecoder` · `URL` · `URLSearchParams` · `Blob` / `File`

**Networking** — see [Networking](/guides/networking/)
`fetch` (with `Headers`, `Request`, `Response`, `AbortController` / `AbortSignal`, `FormData`, and
streaming bodies over `ReadableStream`) · `XMLHttpRequest`, upload progress included · `WebSocket` ·
`EventSource` · a cookie jar

**Graphics and media**
`WebGLRenderingContext` / `WebGL2RenderingContext` · `Image` (from the package or the network) ·
a software `CanvasRenderingContext2D` · `HTMLMediaElement` on `<video>`

**Document and instances**
a real element tree with events that capture, target and bubble to `window` · a selector subset ·
`CSSStyleDeclaration` on every element · `postMessage` / `MessageEvent` · `window.parent` ·
`window.top` · `document.body` (the root compositor layer) · `DOMParser`, **XML only**

**Input**
keyboard events from every remote · `navigator.getGamepads`, `Gamepad`, `GamepadButton`,
`GamepadEvent`, `GamepadHapticActuator`

## Subsets, with edges worth knowing

### Selectors

`querySelector`, `querySelectorAll`, `matches` and `closest` take type, `*`, `#id`, `.class`,
`[attr]`, `[attr=value]`, compound selectors, the descendant and `>` combinators, and lists. Anything
else throws a `SyntaxError` rather than quietly matching nothing.

### CSS

Only `position`, `left`/`top`/`right`/`bottom`, `width`/`height`, `z-index`, `opacity`,
`display: none` and `transform: translate|scale`, and only on `<canvas>`, `<video>` and `<iframe>`.
See [the CSS subset](/guides/css/).

### Canvas2D

A software subset drawn on the CPU: rectangles, images, pixel data, transforms, compositing and
text. Glyphs come from SDL3\_ttf. **Paths, patterns and shadows are not drawn**, a gradient fills in
its first colour, shapes are not antialiased, and only the common composite modes blend.

That is enough for what engines actually use a 2D canvas for beside WebGL: Phaser probes blend modes
with one at import; Pixi, Phaser and Lightning rasterise `Text` on one; and a 2D canvas uploads to
WebGL like an image.

### `postMessage`

Between instances, a structured-clone subset: primitives, plain objects and arrays, and
`ArrayBuffer` by value, with cycles preserved. Anything else throws `DataCloneError` at the sender.

## Absent by design

**Graphics** — Canvas2D paths, SVG, `OffscreenCanvas`, `<video>` via markup (create the element).

**Concurrency and storage** — Web Workers, Service Workers, IndexedDB.

**Document** — History, `innerHTML` and any HTML parsing, comment and fragment nodes,
`append`/`prepend`/`before`/`after`/`replaceWith`/`replaceChildren`, `cloneNode`, `dataset`,
`toggleAttribute`, a cascade, layout, flexbox and grid, CSS on non-composited elements, and a
`ResizeObserver` or `IntersectionObserver` that fires — no element has a layout box.

**Networking** — `WritableStream` / `TransformStream` (so `pipeTo` / `pipeThrough`), byte streams and
BYOB readers, synchronous network XHR, `document.cookie`, and `permessage-deflate` on Linux.

**Lifecycle** — `suspend` and `memorywarning` events, which belong to a deferred fourth instance
state and are deliberately not dispatched, so a page never waits for an event that will never come.

Each absent thing is simply undefined: calling a missing method is a `TypeError` at the call site,
not a silent no-op.

## Verify per target

`Intl` support follows whichever Hermes build React Native ships and **differs by platform**. Treat
it as unavailable until you have measured it on the target you care about.

## Known rough edges

- `gl.getSupportedExtensions()` lists GLES/ANGLE extension names, not WebGL's.
- `gl.getParameter` answers numbers where WebGL specifies booleans, for `BLEND` and `COLOR_WRITEMASK`.
- PixiJS needs `Assets.setPreferences({ preferWorkers: false })`, because it creates a Web Worker
  without checking that one exists.
