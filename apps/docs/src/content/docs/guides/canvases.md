---
title: Canvases and WebGL
description: The frame canvas, composited canvas layers, and what getContext actually gives you.
---

Every canvas that takes a WebGL context gets **a real GL context of its own**, in the host's share
group, with its own bound program, textures and blend state — as WebGL says two canvases have. A page
can put a HUD over a game without an `<iframe>` and a second runtime to carry it.

## The frame, and everything else

The first canvas of a page **that CSS has not placed** is the frame:

```js
const canvas = document.createElement('canvas')
document.body.appendChild(canvas)
const gl = canvas.getContext('webgl')   // the drawable itself
```

- It draws straight into the window: framebuffer 0, no compositor, no extra texture.
- `canvas.width` reads `gl.drawingBufferWidth`. A size write is **accepted and ignored**, and says so
  once.
- A page with one canvas builds no compositor at all, and keeps exactly the present path — and the
  cost — it had.

A canvas the CSS subset **places, resizes or hides before it asks for its context** becomes an
FBO-backed layer at that rect instead:

```js
const hud = document.createElement('canvas')
hud.style.cssText = 'position: absolute; left: 10%; top: 10%; width: 50%; height: 50%'
document.body.appendChild(hud)
const hg = hud.getContext('webgl')      // its own context, its own layer
```

- Its drawing buffer starts at the rect's pixels, so on a 1920×1080 screen that HUD is 960×540.
- `hud.width = 640` **is** honoured: the drawing buffer becomes 640 wide and `hg.drawingBufferWidth`
  agrees. From then on the buffer stops following the rect.
- `hud.getBoundingClientRect()` reports the placed rect.
- `setAttribute('width', 640)` reflects into the property, as on the web.

:::caution[The decision is made at `getContext`, once]
Whether a canvas is the frame or a layer is decided when it asks for its context. A canvas that took
the frame and is placed by CSS *afterwards* keeps the frame, and warns once. Place it first.
:::

## Layers compose with `<iframe>` instances

Canvas layers and instance layers are **one list**, sorted by `z-index` and then by insertion order,
so they compose against each other:

```js
hud.style.zIndex = '2'      // a HUD canvas
game.style.zIndex = '1'     // an <iframe> running a game
```

Video planes stay beneath all of them, whatever their `z-index` — the platform composites video under
the app's drawable, and the app clears transparent where the video should show through. A `<video>`
whose `z-index` would put it above a canvas is shown beneath it anyway, and says so once.

## What it costs

Making a canvas current is one comparison per GL call, and an actual `makeCurrent` only where a page
alternates between canvases — which is once per canvas per frame for any engine that draws each
canvas in one pass.

An idle canvas costs nothing: a layer that did not paint this frame keeps its last image, and the
frame still presents.

## `getContext` can answer `null`

As the web allows, and in preference to handing back a context that draws nowhere:

```js
const gl = second.getContext('webgl')
if (gl === null) { /* one canvas is all this page gets */ }
```

You get `null` when the runtime has no drawable to composite over (a headless host), when the
platform has no compositor, when the driver will not give another context, or when the page is itself
running inside an `<iframe>` — one level of nesting, so an instance's layer composites no layers of
its own. The reason is logged once, however many canvases ask.

## Sizes, spellings and limits

- A canvas has **one kind of context**: a WebGL canvas answers `'2d'` with `null`, and a 2D canvas
  answers `'webgl'` with `null`.
- `'webgl'` and `'experimental-webgl'` return the same object. `'webgl2'` answers `null` on a
  GLES 2-only GPU, as a browser does — feature-detect it.
- A drawing-buffer side is capped at **8192 px**, said once. `canvas.width` and
  `gl.drawingBufferWidth` read the cap back.
- A canvas whose rect computes to zero composites nothing.

## Probes are free, at startup

Engines feature-detect by making a throwaway canvas, asking it for a context and dropping it — Pixi's
`isWebGLSupported` and Phaser's `Features.webGL` both do. A probe that runs **before** any canvas has
taken the frame is handed the page's own context, so it allocates nothing at all. A probe run after
the frame is taken gets a context of its own and gives it back when the element is collected, which
is what a browser does with a detached canvas too.

## 2D canvases are not composited

`getContext('2d')` gives you [a software subset](/reference/environment/#canvas2d): CPU pixels and a
texture source, uploaded to WebGL like an image. It gets no layer and no GL context. That is what
Pixi, Phaser and Lightning rasterise `Text` on, and what Phaser probes blend modes with at import.

## Lifetime

Leaving the document stops a layer being composited; putting it back shows it again. Dropping the
canvas frees its context, its texture and its layer — the release rides on the element's collection,
the same way an `<iframe>`'s instance does. Shutdown releases every context of the page on the thread
they are current on.
