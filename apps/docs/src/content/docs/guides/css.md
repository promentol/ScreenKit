---
title: The CSS subset
description: Which properties are parsed, which elements they apply to, and what happens to everything else.
---

There is no cascade and no layout engine. Every element has a `CSSStyleDeclaration` that stores and
reads back what you set — `cssText`, `setProperty`, `getPropertyValue`, `removeProperty`,
`getPropertyPriority`, `length`, `item`, and camelCase properties for the common names, kept in step
with the `style` attribute — but only a **backed** element parses the subset and does anything with
it.

Backed means `<canvas>`, `<video>` and `<iframe>`. Those three become compositor layers; nothing else
has a box.

## The subset

| Property | Notes |
|---|---|
| `position` | what makes a rect a rect |
| `left`, `top`, `right`, `bottom` | px and % |
| `width`, `height` | px and % |
| `z-index` | sorts canvas layers and instance layers in one list |
| `opacity` | applied by the compositor's alpha blend |
| `display: none` | hides the layer without releasing it |
| `transform` | `translate` and `scale` only |

An accepted value **reads back normalised**, the way a browser does it — `'0'` reads as `'0px'`. A
value outside the subset is not applied, and warns once.

```js
hud.style.cssText = 'position: absolute; left: 10%; top: 10%; width: 50%; height: 50%; z-index: 2'
hud.style.left                 // '10%'
hud.getBoundingClientRect()    // the placed rect, in drawable pixels
```

## On a canvas, the subset decides what the canvas *is*

A canvas the subset has **placed, resized or hidden before it asks for a context** becomes a layer at
that rect. One it has not takes the page's frame, if the frame is free. See
[Canvases and WebGL](/guides/canvases/).

Values that merely match the drawable place nothing and say nothing — Lightning's
`style.width = '1920px'` on a 1920-wide surface is not "placing" anything.

:::caution
A canvas that took the frame and is moved, resized or hidden *afterwards* keeps the frame, and says
so once. Its context is the window's, and there is no moving a window into a layer after the fact.
:::

## Everything else

`<div>`, `<span>` and friends accept style and read it back, and it means nothing — they never
render. There is no `ResizeObserver` or `IntersectionObserver` that fires, because no element has a
layout box.

For text, use [MSDF atlases](/start/what-screenkit-is/#text-comes-from-atlases-not-from-the-cascade)
or a 2D canvas. There is no text rendering from CSS.
