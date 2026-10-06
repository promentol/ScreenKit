# runtime/js — the prelude

JavaScript that ships inside the runtime and is evaluated before the app bundle.
One file today: `dom-shim.js`, the `document`, element tree and canvas that let
an unmodified Lightning 3 build reach the GL context the host already created.

```
runtime/js/
└── dom-shim.js    document · element tree · selectors · events · style · canvas · fetch/XHR/WebSocket/EventSource · <video> · <iframe>   → dom-shim.hbc
```

It is hand-written: the M2 fallback in `Architecture.md` §3, not a composition of
WHATWG packages.

## Why this is JavaScript and not C++

`Architecture.md` §2 draws the line at native access: **native owns the things
only native can do, and everything browser-shaped is JavaScript.** Nothing in
this shim needs native access. `gl` is already a global — `installVendoredWebGL`
binds the vendored `WebGLRenderingContext` to it during the graphics bootstrap —
and the drawable size is reachable through that same object as
`gl.drawingBufferWidth`/`Height`.

Writing this in C++ would mean a `jsi::HostObject` and host functions for
property getters that a JS object gets for free, and it would make the next
eight shims harder rather than easier. It is also compiled with the pinned
`hermesc` into `dom-shim.hbc`, exactly as the test fixtures are, so a syntax
error is a build failure and cold start never pays to parse it.

## The rule: every interface traces to a row in the evidence

The scope of this file is **measured, not taken from the DOM spec**. Two files
are the evidence, both under `_bmad-output/implementation-artifacts/`:

- `m2-lightning-dom-trace.json` — a runtime trace of the Lightning bundle in
  `examples/lightning3-blits/`. Runtime-proven: it was actually called.
- `m2-lightning-dom-usage.json` — static analysis of the same bundle. A
  superset, and deliberately not the scope.

**Adding an interface means adding the row that justifies it.** The evidence
decides *which* interfaces exist; once it names one — `Node`, `Element`,
`EventTarget`, `Event`, `CSSStyleDeclaration`, `DOMTokenList`, `NodeList` — that
interface is implemented whole, to the DOM's semantics, rather than member by
member. So `Node.ELEMENT_NODE`, `HTMLCollection.namedItem`,
`DOMTokenList.toggle`/`replace`, `Event.cancelBubble`/`returnValue`/`composed`
and the camelCase style accessors are here without rows of their own, because a
half-implemented standard interface fails in stranger ways than a missing one.
An interface nothing names is speculative DOM surface, and speculative surface
is what turns a shim into a browser nobody asked for.

| Shimmed here | Evidence row (trace) |
|---|---|
| `document.createElement` | `Document.createElement` ×2 — `"link"`, `"canvas"` |
| `document.getElementById` | `Document.getElementById` ×1 — `"app"` |
| `canvas.width` / `canvas.height` (read) | `HTMLCanvasElement.width` ×11, `.height` ×11 |
| `canvas.width =` / `canvas.height =` | `HTMLCanvasElement.width =` ×1, `.height =` ×1 |
| `canvas.getContext` | `HTMLCanvasElement.getContext("webgl")` ×1 |
| `element.style` | `HTMLElement.style` ×3 |
| `element.appendChild` | `Node.appendChild` ×1 |

The element tree, selectors, event propagation and style go further than the
trace reached, and each traces instead to the static analysis
(`m2-lightning-dom-usage.json`: `documentElement`, `head`,
`querySelector("meta[property=csp-nonce]")`, `querySelectorAll`,
`getElementsByTagName("link")`, `setAttribute`, `getAttribute`,
`addEventListener`, `remove`, `id`) and to the matrix rows of
`spec-m5-m6-finish-pipeline-and-element-tree.md`, each with a `dom-*` ctest row.

| Shimmed here | Evidence | Row |
|---|---|---|
| `Node`/`Element`/`HTMLElement`/`Document`, parent/child links, `appendChild`/`insertBefore`/`removeChild`/`replaceChild`/`remove`/`contains`, siblings | usage `appendChild`, `remove`; spec "Tree move", "Cycle" | `dom-tree-move`, `dom-tree-cycle`, `dom-tree-operations` |
| `documentElement`/`head`/`body` in the tree; `getElementById`/`getElementsByTagName`/`getElementsByClassName` | usage `documentElement`, `head`, `getElementsByTagName` | `dom-document-tree` |
| attributes; `id`/`className`/`classList` reflecting them | usage `setAttribute`, `getAttribute`, `id` | `dom-attributes` |
| `querySelector(All)`/`matches`/`closest` over the selector subset | usage `querySelector("meta[property=csp-nonce]")`; spec "Selector" | `dom-selectors`, `dom-selector-syntax` |
| every node an `EventTarget`; capture → target → bubble to `window` | usage `addEventListener`; spec "Bubbling" | `dom-event-propagation`, `dom-key-target` |
| `CSSStyleDeclaration`; the CSS subset parsed on the canvas | trace `HTMLElement.style` ×3 (Lightning sets `style.width`/`height`); spec "Canvas style" | `dom-style-declaration`, `dom-canvas-style` |

Everything else an element carries is an ordinary own property. `link.rel = …`
sets it and reads back; an unset property reads as `undefined`. Only `id`,
`class` (`className`, `classList`) and `style` reflect their attributes — `rel`,
`href` and the rest do not, because nothing measured reads one back through the
other.

## The three contracts worth knowing

**Every canvas gets a real GL context; the first one is the frame.** The first
canvas of a page that CSS has not placed takes the context the graphics
bootstrap made -- the `gl` global, the window's framebuffer, the present path a
page has had since M4. Every canvas after it, and any canvas the CSS subset has
already placed, gets a **real GL context of its own** in the host's share group,
drawing into an FBO-backed layer composited at the element's CSS rect
(`__screenkit.canvas`, "Canvases" below). Two engines on two canvases cannot
disturb each other's bound program, textures or blend state, which is what WebGL
says two canvases are.

A canvas that asked and was never attached -- an engine's `isWebGLSupported`
probe -- still hands the frame to the next canvas rather than spending a context
on itself, so a probe costs nothing and leaves no layer behind.

`"webgl"`, `"webgl2"` and `"experimental-webgl"` all resolve to the same object
for a given canvas. ANGLE on Apple caps at ES 3.0 (`Architecture.md` §9), which
is exactly what WebGL2 is defined against, so one context satisfies every
spelling truthfully; on a GLES2-only GPU `"webgl2"` is null for every canvas.

**The canvas that is the frame ignores size writes; one with a layer honours
them.** On the frame's canvas `canvas.width` reads `gl.drawingBufferWidth` on
every access and assignment is accepted and changes nothing: Lightning sets
`canvas.width` and then trusts `gl.drawingBufferWidth`, so letting the write
stick would make the two disagree and render everything at the wrong scale. That
canvas *is* the drawable and there is no resizing a window from CSS.

A canvas with a layer of its own has a drawing buffer of its own: `canvas.width`
reads that layer's `drawingBufferWidth`, a write resizes it, and until something
is written the buffer follows the element's CSS rect -- so a HUD over half the
screen is drawn at half the screen's pixels. `getBoundingClientRect()` reports
that rect.

This is expressed as `defineBackedSize(element)` rather than as something canvas
does specially. `<video>` and `<iframe>` — the other two elements that ever paint
(`Architecture.md` §3.1) — are backed the same way, and a declaration that moves
one is applied rather than warned about.

**A gap says so, once.** `getContext("2d")` returns `null`, as the web spec
allows, and warns the first time. An ignored `width =` warns the first time.
Once and not per call: Lightning reads `canvas.width` eleven times a frame. A
silent stub returning `undefined` would instead turn a missing shim into a crash
somewhere unrelated, which is the failure mode this whole file exists to avoid.

## The element tree

The document starts as the markup every `index.html` has: `<html>` with `<head>`
and `<body>`. `index.html` itself is never parsed (`Architecture.md` §6.1), so
nothing else from it exists. Tree mutation follows the DOM: appending a node that
has a parent moves it, a cycle throws `HierarchyRequestError`, a missing
reference child throws `NotFoundError`, a non-node argument throws `TypeError`,
and a document holds one element.

`getElementById` searches the tree. `"app"` is the one exception: a web build's
`index.html` declares `<div id="app">`, and markup is never parsed, so when no
element has that id the lookup creates one and appends it to `<body>` — after
which every other lookup finds it too.

`childNodes`, `children` and `classList` are live. What a query returns —
`querySelectorAll`, `getElementsByTagName`, `getElementsByClassName` — is a
snapshot; in a browser the last two are live.

## Selectors

Type, `*`, `#id`, `.class`, `[attr]`, `[attr=value]` (value quoted or an
identifier), compound selectors, the descendant and `>` combinators, and
selector lists. Matching is right to left against the whole tree, so
`el.querySelector('div > p')` behaves as in a browser. Anything else — a
pseudo-class, `+`/`~`, `[attr^=value]`, a namespace — throws a `SyntaxError`
`DOMException` whose message says it is outside the subset, and a selector that
is not valid CSS says that instead. A selector this runtime cannot answer must
not quietly match nothing.

## Events

Every node is an `EventTarget`, and so is `window`. A dispatch follows the DOM's
path: capture from `window` down to the target's parent, the target (its capture
listeners, then the rest), and for a bubbling event back up to `window`. A
detached subtree's path ends at its root. `stopPropagation` finishes the current
node and halts; `stopImmediatePropagation` halts at once; a listener that throws
is reported and the rest run.

Host key events are dispatched at `document.activeElement`, which is always
`<body>`, one listener per step with a microtask checkpoint after each
(`dom-key-target`, `input-event-loop`). A key pressed before a runtime pauses is
dropped, never delivered on resume.

Event handler properties -- `el.onclick`, `document.onkeydown`, `window.onresize`,
`xhr.onload` -- are what HTML makes them: a listener placed where the handler was
first set, replaced in place, removed by `null`, run in the target and bubble
passes, and cancelling the event by returning `false` (`dom-event-handlers`).
There are no `ontouch*` properties: `'ontouchstart' in window` is how libraries
detect a touch screen.

Gamepads are the W3C Gamepad API, in the `standard` mapping. `navigator.getGamepads()`
returns at least four slots, `null` where no pad is, and a connection keeps one `Gamepad`
object that each call refreshes in place from the snapshot the host polls every loop iteration,
before `tickFrame` (`__screenkit.gamepads`; mapping, threads and rumble in `runtime/README.md`, "Gamepads").
`gamepadconnected` and `gamepaddisconnected` are trusted `GamepadEvent`s at `window`, with
`ongamepadconnected` / `ongamepaddisconnected`, fired as tasks that a pause holds rather than
drops. `timestamp` is on `performance.now()`'s clock and moves only when the pad's state does;
`connected` is writable. The app's first `getGamepads()` claims the gamepads for the process,
and from then on their buttons and sticks send no key events (`gamepad-*` rows).

`resize` fires at `window` when the window's pixel size changes -- the host turns
SDL's size event into one task (`ViewportEvents`), and the shim fires only when
the size a page can read differs from the last one it heard about. A drawable
taken before the resize is presented and let go first, so the page's answering
paint lands at the new size (`gl-drawing-buffer-resize`).

`MutationObserver` delivers records for tree and attribute changes (the `style`
attribute included) from a microtask, with `subtree`, `attributeOldValue`,
`attributeFilter`, `takeRecords`, `disconnect` and the DOM's transient
registrations for removed nodes; `characterData` is accepted and never fires,
since there are no text nodes (`dom-mutation-observer`).

## Style

Every element's `style` is a `CSSStyleDeclaration`: `cssText`,
`setProperty`/`getPropertyValue`/`removeProperty`/`getPropertyPriority`,
`length`/`item`, and camelCase properties for the common names, kept in step
with the `style` attribute. There is no cascade and no layout.

On a backed element — a canvas, a `<video>` and an `<iframe>` — the
`Architecture.md` §3.1 subset is parsed as a browser parses it: `position`,
`left`/`top`/`right`/`bottom`, `width`/`height`, `z-index`, `opacity`,
`display`, `transform: translate|scale`. An accepted value reads back normalised
(`'0'` is `'0px'`); one outside the subset is not applied and warns once.

On a canvas the subset is also what decides *what that canvas is*. A canvas the
subset has placed, resized or hidden **before it asks for a context** becomes a
layer at that rect; one it has not takes the page's frame, if the frame is free.
Values that match the drawable — Lightning's `style.width = '1920px'` on a
1920-wide surface — place nothing and say nothing. The one canvas that took the
frame and is moved, resized or hidden *afterwards* keeps the frame, and says so
once: its context is the window's, and there is no moving a window into a layer
after the fact (`deferred-work.md`).

## Deliberately absent

`OffscreenCanvas`, `Worker` (below), and from the DOM: comment and fragment
nodes (`createComment`, `createDocumentFragment`), `innerHTML` and any HTML
parsing (`DOMParser` takes XML only), `append`/`prepend`/`before`/`after`/
`replaceWith`/`replaceChildren`, `cloneNode`, `dataset`, `toggleAttribute`, a
cascade, layout, and a `ResizeObserver` or `IntersectionObserver` that fires (no
element has a layout box). Each is simply undefined: calling a missing method is
a TypeError at the call.

## Canvases: one GL context each, composited

A page gets as many WebGL canvases as the driver will give it. The first one is
the page's frame; every other is a **real GL context of its own** in the host's
share group, drawing into an FBO-backed texture the page's own present
composites at the element's CSS rect — the same `LayerList` an `<iframe>`
instance's layer goes through (`Architecture.md` §3.1, §5;
`core/src/bindings/Canvas.cpp`, `core/src/gfx/VendoredWebGL.cpp`,
`core/src/compositor/`).

```js
const game = document.createElement('canvas');   // the frame: gl, the drawable
document.body.appendChild(game);
const gg = game.getContext('webgl');

const hud = document.createElement('canvas');
hud.style.cssText = 'position:absolute; left:10%; top:10%; width:50%; height:50%; z-index:2';
document.body.appendChild(hud);
const hg = hud.getContext('webgl');              // its own context, its own layer
hud.width;                                       // the rect's pixels, not the screen's
```

- **The element, not the context, is what CSS places.** The rect comes from the
  subset every backed element parses (`planeFor` — `position`,
  `left`/`top`/`right`/`bottom`, `width`/`height`, `z-index`, `opacity`,
  `display`, `transform: translate|scale`), in drawable pixels. There is no
  second geometry path. The default size is 300x150, as a browser's is.
- **Layers are one list.** A canvas layer and an instance layer sort together, by
  `z-index` then insertion order, so a launcher may put a HUD canvas over the
  game it embeds. A `<video>` plane stays beneath every one of them, and a
  `z-index` that would lift it above any canvas warns once, as before.
- **Each canvas has its own paint flag.** "Present only a frame that painted"
  holds per layer, so an idle canvas keeps its last image and costs nothing. All
  of it runs on the page's own JS thread, so a canvas layer publishes its frame
  with no fence — there is nothing to synchronise between two contexts one thread
  drives in order.
- **Lifetime.** Leaving the document stops the layer being composited; putting it
  back shows it again. Dropping the canvas frees its context, its texture and its
  layer — the release is posted to the JS thread from the element's native state,
  as an `<iframe>`'s instance is. Shutdown releases every context of the page on
  the thread they are current on.
- **`getContext` answers null rather than throwing** when a canvas cannot have
  one: a headless runtime with no drawable, a platform with no compositor, or a
  driver out of contexts. It says which, once. This is the web's own
  "getContext may return null", and it is what an `<iframe>` instance's second
  canvas gets today (`deferred-work.md`).
- **A page with one canvas pays nothing.** No compositor is built, the default
  framebuffer stays 0 and the present path is the one it has always had
  (`iframe-none-costs`).

**Proven by** the `canvas-*` rows — `canvas-isolation` (two contexts, two sets of
bound state, read back rather than drawn), `canvas-placed`, `canvas-composite`
(the pixels of two layers in z-order with the upper one's transparent half
showing the lower through), `canvas-idle`, `canvas-remove`, `canvas-teardown`
and `canvas-over-iframe` — plus `dom-second-canvas` and the `dom-canvas` fixture.

## Canvas 2D: a software subset

`getContext('2d')` gives a real `CanvasRenderingContext2D`, drawn on the CPU into
an RGBA buffer the canvas owns (`dom-canvas-2d`). Engines use one beside WebGL:
Phaser probes alpha and blend modes with it the moment it is imported and keeps
one for pixel reads.

- **Drawn:** `fillRect`, `clearRect`, `strokeRect`; `drawImage` in all three forms
  from a 2D canvas, an `<img>`, an `ImageBitmap` or `ImageData`, nearest or
  bilinear by `imageSmoothingEnabled`; `getImageData`, `putImageData` (with a
  dirty rectangle), `createImageData`; `save`/`restore`; `translate`, `scale`,
  `rotate`, `transform`, `setTransform`, `resetTransform`, `getTransform`;
  `globalAlpha`; `globalCompositeOperation` `source-over`, `source-in`,
  `source-out`, `source-atop`, `destination-*`, `lighter`, `copy`, `xor`,
  `multiply`, `screen`, `darken`, `lighten`. Colours are any CSS colour string.
  Shapes are not antialiased.
- **Text** (`dom-canvas-text`): `fillText`, `strokeText`, `measureText` with real
  metrics, the `font` shorthand, `textAlign`, `textBaseline`. Glyphs are coverage
  masks from SDL3_ttf (`__screenkit.text`: FreeType and HarfBuzz), composited like
  any other pixel. Fonts are faces in `document.fonts` -- `FontFace` loads
  `url()` through `fetch`, `local()` and bytes -- then installed families
  (CoreText on Apple), then the system's sans-serif. Text is drawn upright at the
  transform's scale; `maxWidth` shrinks it rather than condensing; there is no
  `letterSpacing`, so Pixi and Phaser space letters themselves.
- **Not drawn:** paths (`beginPath` ... `fill`/`stroke`/`clip`), patterns,
  shadows, and the other blend modes. They exist, draw nothing and say so once. A
  gradient fills in its first colour.
- **As an image:** a 2D canvas uploads with `texImage2D` and feeds
  `createImageBitmap`; pixels are stored unpremultiplied, as `getImageData` reads
  them.
- **Sizes:** the canvas that is the page's frame reads the drawable's size and
  ignores writes (warning once if it is in the page). A canvas with a GL layer of
  its own reads and honours its own drawing buffer. A canvas with no GL context
  at all keeps the size written to it, and reads the fullscreen size until
  something is written. A canvas has one kind of context: a GL canvas answers
  `'2d'` with null, and a 2D canvas answers `'webgl'` with null. A canvas that
  took the frame but is not in the page -- an engine's WebGL probe -- gives it up
  to the next canvas that asks. `setAttribute('width', n)` reflects into the
  property, as on the web, so both spellings size the same buffer. A side is
  capped at 8192 px, which is said once: `canvas.width` and `gl.drawingBufferWidth`
  then read back the cap, and silence would make that look like a bug in the page.
- **A 2D canvas is not composited.** It stays CPU pixels and a texture source, so
  an overlay drawn in 2D is uploaded by the page as it always was; only a canvas
  with a WebGL context gets a layer (`deferred-work.md`).

## XML, text nodes and the document

- `new DOMParser().parseFromString(text, 'text/xml')` (also `application/xml`,
  `application/xhtml+xml`, `image/svg+xml`) builds an `XMLDocument` of elements,
  attributes and text: entities and character references decoded, CDATA as text,
  namespace prefixes resolved, comments, processing instructions and the doctype
  dropped. Malformed XML gives a document whose root is `<parsererror>`, as in a
  browser; `'text/html'` throws `NotSupportedError`. XML names are
  case-sensitive (`getAttribute('lineHeight')`), HTML ones are not
  (`dom-xml-parser`). PixiJS and Phaser read BMFont XML this way.
- Text nodes exist: `createTextNode`, `new Text()`, `data`, `nodeValue`, and
  `textContent` on every node (setting it on an element replaces its children).
  MutationObserver's `characterData` records come from them.
- `document.readyState` is `'loading'` while the app first runs. The host fires
  `DOMContentLoaded` (`'interactive'`) once the bundle has run -- for a package,
  once its entry module has resolved -- and `load` on `window` (`'complete'`) a
  task later (`dom-document-ready`). Phaser boots on `DOMContentLoaded`.
- `<html>` and `<body>` fill the screen: their `getBoundingClientRect()` and
  `clientWidth`/`clientHeight` are the drawable's size, which is what "fill the
  parent" scaling reads (Phaser's RESIZE mode). Every other element that does not
  paint is 0x0.
- `window.screen` is the drawable, landscape and 24-bit, with a
  `screen.orientation` that accepts listeners and never changes.
- `<video>` is a real `HTMLVideoElement` over the platform's player -- see
  "Video" below. `<audio>` is an `HTMLAudioElement` that plays nothing: its
  `canPlayType` answers `''` and a load fails as a source no player supports.

## When it runs

The host evaluates `dom-shim.hbc` **after `startGraphics` and before the app
bundle**, and a failure is fatal. Both halves of that order are load-bearing:
evaluated earlier, `getContext` has nothing to hand out; evaluated later, the
bundle has already asked.

The headless `screenkit-host <bundle>` path has no window, no GL context and
therefore no prelude — a canvas that cannot be backed would be a lie, and that
path exists to run bundles that never draw. Use `--window` for anything that
touches `document`.

```sh
runtime/build/macos/screenkit-host --window runtime/build/macos/fixtures/dom-canvas.hbc
```

## Full inventory

Every host global the Lightning bundle references (`tools/dom-usage`, 33 names) is accounted for:
**31 provided, 2 deliberately absent, 0 missing.** The Gamepad API row is beyond that trace: its
evidence is Phaser's gamepad plugin (`device/Input.js` enables it on `navigator.getGamepads`;
`input/gamepad/` polls it every frame and reads `id`, `index`, `buttons[i].value`, `axes`,
`timestamp`, a writable `connected` and `event.gamepad.index`), and `spec-web-gamepad-api-sdl3.md`.

| Group | Provided | Backing |
|---|---|---|
| Document / canvas | `document`, `HTMLCanvasElement`, `getContext` → `gl` | JS; `gl` from the vendored WebGL, and a context and layer per further canvas over `__screenkit.canvas` (see "Canvases") |
| Environment | `window`, `self`, `location`, `performance` | JS |
| Events / observers | `Event`, `CustomEvent`, `MutationObserver`, `ResizeObserver`, `IntersectionObserver` | JS |
| Encoding / binary | `Blob`, `URL`, `webkitURL`, `URLSearchParams`, `atob`, `btoa`, `ImageData` | JS |
| Gamepads | `navigator.getGamepads`, `Gamepad`, `GamepadButton`, `GamepadEvent`, `GamepadHapticActuator` | JS over `__screenkit.gamepads` (the host's per-frame snapshot, and rumble) |
| Loading | `XMLHttpRequest`, `fetch`, `Response` | JS over `__screenkit.readFile` (package) and `__screenkit.net` (network) |
| Images | `createImageBitmap`, `ImageBitmap`, `Image`, `HTMLImageElement` | JS over `__screenkit.imageInfo` (package) and `__screenkit.net.decodeImage` (bytes in memory, off the JS thread) |
| Fonts | `FontFace`, `document.fonts` | JS |
| Paths | `Path2D` | JS, records commands |
| Timers | `setTimeout` & co, `requestAnimationFrame`, `queueMicrotask` | native, since M3 |
| Video | `HTMLMediaElement`, `HTMLVideoElement`, `HTMLAudioElement` (plays nothing), `MediaError`, `TimeRanges`, `TextTrack`, `TextTrackList`, `TextTrackCue`, `VTTCue`, `TrackEvent`, `VideoPlaybackQuality` | JS over `__screenkit.media` (the platform's player; see "Video") |
| Instances | `HTMLIFrameElement`, `window.postMessage`, `MessageEvent` with `source`, `structuredClone`, `window.parent`/`top`/`frames`/`length`, `element.focus()`, `window` `pause`/`resume` | JS over `__screenkit.instances` (a second runtime; see "Instances") |

### Deliberately absent

`Worker`, `OffscreenCanvas`, and a `2d` context — and, from the instance surface, `MessagePort`,
`MessageChannel`, `BroadcastChannel` and `iframe.contentDocument` (see "Instances"). Lightning feature-detects all three
(`hasWorker = !!self.Worker`, `typeof OffscreenCanvas < "u"`, a `null` from `getContext("2d")`) and
takes a main-thread, WebGL-only path when they are missing. Defining any of them without a real
backend would flip that detection into something broken, so absence is asserted by
`dom-identity-and-absence` — a well-meaning future stub fails that row.

## How textures load

Lightning's loader, exactly as it runs:

```
XMLHttpRequest(responseType "blob") -> Blob -> createImageBitmap -> ImageBitmap -> texImage2D
```

A package asset stays a file: its Blob remembers the asset path, the `ImageBitmap` carries a
`localUri`, and the vendored `texImage2D` decodes it exactly once, at upload, through the
`stb_image` already compiled into the vendored archive (`readFile`, and `imageInfo` header-only via
`stbi_info`, in `core/src/bindings/HostIO.cpp`). `dom-texture-chain` proves that path by reading back
the texture's exact colour from the GPU.

A download has no file behind it, so its bytes are decoded in memory -- `__screenkit.net.decodeImage`,
the same `stb_image` on a queue of its own, off the JS thread, refusing anything over 16384 px on a
side or 8192x8192 in area before decoding -- into RGBA, and the `ImageBitmap` (or `<img>`) carries
`data`, which
`texImage2D` uploads directly (`readImageSource` in `core/src/gfx/VendoredWebGL.cpp`, natural size
for an `<img>`), under the same premultiplied-alpha rules. `net-image` proves it for `new Image()`,
`fetch` + `createImageBitmap(blob)` and Lightning's own XHR-blob path, by pixel readback;
`dom-image-decode-async` that a large decode lets a 0 ms timer run first.

`createImageBitmap(source, sx, sy, sw, sh)` crops as a browser does: `long` arguments, a negative
width or height extending the rectangle left or up, whatever falls outside the source transparent
black, and a zero width or height a `RangeError`. A cropped bitmap is pixels, decoded from whatever
the source was -- a sprite sheet asset included (`dom-image-bitmap-crop`).

## Confinement

`__screenkit.readFile` is a native file read, so an unconfined one is a sandbox escape
(`Architecture.md` §9). Both sides are `realpath`-canonicalised and the candidate must sit under the
root, which defeats `..`, symlinks and a sibling directory whose name merely starts with the root's.
The root is **write-once** and the host sets it to the bundle's directory before any app code runs,
so an app cannot widen it. `dom-asset-confinement` tries all three escapes.

## Networking

`fetch`, `XMLHttpRequest`, `WebSocket` and `EventSource` reach the network; everything else about a
URL is unchanged. The split is the §2 one and it is React Native's: native
(`runtime/core/src/net/`) is a thin translation onto the **platform's own HTTP client**, which owns
connections, TLS, redirects, compressed bodies and the cookie store; this file owns every browser
semantic on top.

```
                                                   ┌─ NSURLSession on Apple   (NetServiceApple.mm)
fetch / XHR / EventSource ─┐                       │
                           ├─ __screenkit.net ─────┼─ OkHttp on Android       (NetServiceAndroid.cpp
WebSocket ─────────────────┘   (Net.cpp)           │                          + HttpClient.java)
                                   │               │
                              NetService,          └─ cpp-httplib on Linux    (NetServiceLinux.cpp)
                       one implementation per platform
```

**The three clients are not identical, and the differences are listed rather than hidden** -- see
"Where the platforms differ" below. That is the price of not maintaining an HTTP stack, and it buys
HTTP/2, proxies and a TLS implementation kept current by the OS.

**Linux is the exception that proves the shape.** It has no vendor client to put behind the seam, so
it gets a vendored cpp-httplib -- but only for the protocol: TLS there is still the image's own
OpenSSL and the roots are still the image's own CA bundle (`spec-linux-http-client.md`).

**Which URL goes where.** `http:`/`https:` (and `ws:`/`wss:`) go to the network. A relative URL
resolves against the document, `screenkit:/`, so it still names a package asset, read by the
confined reader exactly as before -- `/abs`, `screenkit://` and `blob:` included. A missing asset is
still a 404 response.

**The handle API.** `__screenkit.net.request(options, onEvent)` returns an id and reports `head`,
`data` (one `ArrayBuffer` per chunk as it arrives), `upload` progress, and a terminal `end` or `error`
(`{kind, message}`: `dns`, `connect`, `tls`, `network`, `protocol`, `redirect`, `decode`, `url`,
`unsupported`);
`write`/`finish` stream a request body, `abort` closes the connection and silences the id.
`openSocket(url, protocols, onEvent)` reports `open`, `message`, `sent` (bytes flushed), `error` and
`close`; `send` and `close` drive it. Events arrive as event-loop tasks, in order, with a microtask
checkpoint after each -- so a promise one message resolves has settled before the next is dispatched
-- and they wait behind a paused runtime's freeze gate. From the call until the terminal event the
runtime is not `idle()`.

**Web semantics, and where they bend.**

- `fetch` rejects with `TypeError` only for a network failure -- its `cause` is the error kind -- and
  resolves a 404 or 500 with `ok: false`. An abort rejects with the signal's reason (`AbortError`,
  `TimeoutError` for `AbortSignal.timeout`), and a body already streaming errors with it.
  `response.body` is a `ReadableStream` fed as bytes arrive; `tee()`, `clone()`, `for await` and
  `cancel()` (which closes the connection) all work. A `ReadableStream` request body needs
  `duplex: 'half'` and is sent chunked as it yields.
- Redirects are followed natively, at most 20; 303 (and 301/302 after a POST) becomes GET, 307/308
  resend the body, `Authorization` is dropped across origins. `redirect: 'error'` rejects;
  `redirect: 'manual'` resolves with a browser's opaque redirect: `type: 'opaqueredirect'`,
  status 0, no headers and a null body -- a web build written against that sees the same thing here.
- Forbidden request headers (`Cookie`, `Host`, `Content-Length`, `Sec-*`, ...) are dropped
  silently, and `Set-Cookie` never appears in response headers, as in a browser. No `Origin` header
  is sent: the document has no origin to send.
- A `fetch` body is read no more than 1 MiB ahead of the page: a response nobody reads stops
  downloading once that much is queued in its stream -- TCP holds the server back -- and resumes as the
  page reads, instead of the whole download landing in memory. XHR, `EventSource` and `<img>` read as
  data arrives. At most **six requests to one origin** are on the wire at once, as in a browser over
  HTTP/1.1; the rest wait their turn, and an abort while waiting frees the place (`net-flow-control`).
  A long-lived `EventSource` holds one of the six.
- **The window reaches the server on every platform**, each client its own way: OkHttp is
  pull-based and the reader stops pulling; cpp-httplib is synchronous and the reader stops returning;
  NSURLSession's task is suspended -- which CFNetwork honours only while the delegate is caught up, so
  that delegate does nothing but count, suspend and hand the bytes on (NetServiceApple.mm). A 64 MiB
  body nobody reads stalls the server under 24 MiB on all three (`net-flow-control`).
- Response bodies are decoded as a browser decodes them on every platform: `gzip` and `deflate` are
  offered and decoded, `deflate` zlib-wrapped or raw, an empty compressed body is empty, and
  `Content-Encoding` stays on the response. A `text/plain` body streams like any other -- the Apple
  client turns CFNetwork's content sniffing off, which would otherwise hold back a `text/plain` head
  and its first 512 bytes.
- `cache` is accepted and ignored: there is no HTTP cache, so every request goes to the network. A
  non-empty `integrity` **rejects** the fetch with a `TypeError` -- subresource integrity is not
  implemented, and a check that is not made must not look as if it passed.
- A `HEAD` response, and a 204, 205 or 304, has `body === null`.
- `XMLHttpRequest` is the whole interface: every method, `responseType` text/json/arraybuffer/blob,
  `timeout`, `abort`, `overrideMimeType`, `upload` events, and the event order the XHR spec defines
  (`readystatechange` 2, 3 with `progress` at most every 50 ms, 4, then `load` or
  `error`/`abort`/`timeout`, then `loadend`). A package asset keeps its old answers -- status 200, and
  a missing one is `DONE` with status 404 and an `error` event, which Lightning was measured against.
  A synchronous request works for a package asset and throws `NetworkError` for the network: network
  I/O never blocks the JS thread.
- `WebSocket`: `binaryType` blob/arraybuffer, `protocol`, `extensions` (what the handshake
  negotiated), `bufferedAmount` (counted at `send`, drained as bytes are flushed), close codes and
  reasons from either side, pongs answered natively. A failed handshake, refused connection, rejected
  certificate or dropped connection fires `error` (with `readyState` already `CLOSED`) then `close`
  1006 -- and a handshake is refused, as RFC 6455 requires, if the server picks a subprotocol that
  was not offered or an extension the client did not offer. `close()` while `CONNECTING` fails the
  connection the same way, even if the handshake had already completed natively;
  `close(undefined, reason)` sends code 1000, and so does a bare `close()` -- neither platform client
  can send a close frame with no status code at all.
- `EventSource`: `message` and named events with `data`/`lastEventId`, `retry`, reconnection with
  `Last-Event-ID` after a dropped connection, `close()`. A non-200 answer or a wrong content type
  fails it for good (`error`, `CLOSED`), and so do a certificate the OS rejects, a URL the client
  cannot reach and a platform with no HTTP client; any other network failure reconnects, as
  the spec says.
- TLS is always the OS's verification -- trust, validity, host name -- and nothing reachable from JS
  can relax it. The test suite's CA is trusted through `RuntimeConfig::testTlsAnchors`, a native
  field no app can reach.

**Cookies are the platform's store**, as they are in React Native:
`NSHTTPCookieStorage` on Apple, `android.webkit.CookieManager` on Android. `Domain`, `Path`,
`Expires`/`Max-Age`, `Secure` and `HttpOnly` all work, and the store applies to fetch, XHR, WebSocket
handshakes and EventSource alike. What differs from a browser, and why: the document is
`screenkit://`, so under browser rules every request would be cross-origin and
`credentials: 'same-origin'` would never send a cookie. A TV app is closer to a native client --
React Native sends cookies by default -- so **the store applies to every request unless
`credentials: 'omit'`**, which neither sends nor stores. XHR's `withCredentials` is accepted and gates
nothing. `HttpOnly` cookies are never visible to JS and there is no `document.cookie`.
`__screenkit.net.getCookies(url)` / `setCookie(url, cookie)` are the native handle's view, without
HttpOnly. On Android that view is an allow-list, because `CookieManager` cannot say which of its
cookies are HttpOnly: it shows only cookies this runtime saw stored without the flag, so a cookie left
in the store by an earlier run stays hidden from it, and unsettable, until it is set again.
Everything beyond that -- the eviction policy, whether there is a public-suffix list, the
ordering of the `Cookie:` header, whether cookies outlive the runtime -- **is the platform store's and
differs between them**; do not rely on it.

**Where the platforms differ.** One JS layer sits on three clients, and they do not agree about
everything. Each of these is deliberate and recorded in `spec-platform-http-clients.md` and
`spec-linux-http-client.md`:

| | Apple (NSURLSession) | Android (OkHttp) | Linux (cpp-httplib) |
|---|---|---|---|
| A body with no `Content-Type` of its own | a **POST** sends the header **empty** (CFNetwork would otherwise invent `application/x-www-form-urlencoded`); any other method sends none | no header at all, as a browser sends | no header at all |
| A `Secure` cookie set from an `http:` origin | the store's business | the store's business | **refused** |
| `__Secure-` / `__Host-` cookie name prefixes | not enforced | Chromium enforces them | **enforced** |
| HTTP/2 | yes | yes | no: HTTP/1.1 only |
| Duplicate response headers | comma-joined into one value, several `Set-Cookie` included | kept separate | kept separate |
| `response.statusText` | the status code's canonical phrase, not what the server wrote | what the server wrote | what the server wrote |
| `Headers` iteration order | unspecified (`allHeaderFields` is a dictionary; this client sorts it by name) | as received | sorted by name |
| A chunked body cut short | **accepted as complete** | `TypeError` | `TypeError` |
| A compressed stream cut short inside a complete body | **accepted as complete** | `TypeError` | `TypeError` |
| Two disagreeing `Content-Length` lines | `TypeError`, cause `protocol` | `TypeError`, cause `protocol` | `TypeError`, cause `network` |
| Cookie rules | no public-suffix list; gone when the runtime ends | Chromium's list; the device-wide WebView store, and 127.0.0.1 counts as secure | RFC 6265 read straight (`LinuxCookieJar`); `Secure` only over https; gone when the runtime ends |
| WebSocket extensions offered | `permessage-deflate` | `permessage-deflate` | none |
| A host label longer than 63 octets | fails DNS (`dns`) | refused by the URL parser (`url`) | refused by the URL parser (`url`) |

**Absent, deliberately:** an HTTP cache, subresource integrity, WebSocket `permessage-deflate` on
Linux (Apple's and Android's clients offer it), `WritableStream`/`TransformStream` and so `pipeTo`/`pipeThrough`, byte streams and BYOB
readers, service workers, `document.cookie`, and synchronous XHR over the network. Each is simply
undefined or throws, never a stub; `dom-identity-and-absence` asserts it. **HTTP/2 and proxies are no
longer absent** -- they are whatever the platform's client does, which is the point of using it.

**Proven by** one `net-*` ctest row per line of the networking spec's matrix, against a local fixture
server ctest starts (`runtime/tests/net/server.mjs`: HTTP, HTTPS behind a CA generated at start, WS,
WSS, redirects, chunked and gzip bodies, slow streams, dropped connections, Set-Cookie,
event-stream). No row reaches the public internet.

## Video

`<video>` plays through **the platform's own player**, which owns the manifest, buffering, adaptive
bitrate, decoding, DRM and presentation -- what react-native-video does. The shim is
`HTMLMediaElement` on top; below it is one native seam with one player per platform
(`core/src/media/MediaPlayer.h`, `runtime/README.md` "Media"):

```
                                              ┌─ AVPlayer on Apple            (MediaPlayerApple.mm)
shaka.Player (@screenkit/shaka) ─┐            │
                                 ├─ <video> ──┼─ Media3 ExoPlayer on Android  (MediaPlayerAndroid.cpp
HTMLMediaElement ────────────────┘   │        │                              + dev/screenkit/media/)
                                     │        └─ libvlc on Linux              (MediaPlayerLinux.cpp)
                           __screenkit.media
                              (Media.cpp)
```

**Video never passes through the runtime's GL.** Each platform composites its own video plane
**beneath the app's canvas**, at the element's CSS rect -- an `AVPlayerLayer` beneath the metal
view, a `SurfaceView` beneath SDL's, a Wayland subsurface beneath the window -- and the app clears
the canvas transparent where the video should show. The canvas stays opaque to the compositor while
no plane is visible, so a page without video costs what it did before.

**The handle API.** `__screenkit.media.create(target)` makes one player whose events go to
`onevent(target, type, payload)`; `load(id, {url, startTime, mimeType, drm, abr, audioLanguage,
textLanguage})` returns the load's serial, and every event carries the serial of the load it
belongs to, so what an earlier load still had in flight is dropped. Then `play`, `pause`, `seek`,
`setRate`, `setVolume`, `setMuted`, `setPlane`, `selectVariant`, `setAbr`, `selectAudioLanguage`,
`selectText`, `provideLicence`, `unload`, `destroy`, and `capabilities()`. Events -- `metadata`,
`state` (loading, buffering, ready, ended), `time`, `seeked`, `buffered`, `size`, `tracks`,
`variant`, `cues`, `licence`, `stats`, `error` -- arrive as event-loop tasks, in order per player,
behind the freeze gate, like the network's (`Media.h` has the shapes).

### The element

`HTMLMediaElement` is HTML's, from the load algorithm down (`media-element`,
`media-element-model`):

- **Loading.** Setting `src` (or `load()`) runs the media element load algorithm: `abort` and
  `emptied` for what was loaded, `loadstart`, then `durationchange`, `loadedmetadata`, `loadeddata`,
  `canplay`, `canplaythrough` as the player gets there, with `readyState` and `networkState`
  following. `src` resolves against the document: `http(s):` plays from the network, a relative or
  `screenkit:` URL plays a **package asset** from its confined file (the path `readFile` would
  confine it to); a `blob:` or `data:` URL fails as a source nothing supports, because there is no
  MediaSource. An element whose load finds no source keeps nothing: its platform player is
  released.
- **Playing.** `play()` returns a promise that resolves at `playing` and rejects with `AbortError`
  when `pause()` or a new load interrupts it before the element has data, and with
  `NotSupportedError` when the source failed. `play`, `waiting`, `playing`, `pause`, `timeupdate`
  (about four times a second), `ratechange` and `volumechange` fire as HTML orders them; `autoplay`
  and `loop` work; `volume` outside 0..1 throws `IndexSizeError`; `playbackRate` takes 0 and
  1/16..16 -- 0 freezes the picture on every platform while `paused` stays false, as it does in a
  browser.
- **Seeking.** Writing `currentTime` fires `seeking`, then `timeupdate` and `seeked` at the new
  position, clamped to `seekable` -- `[0, duration]`, or the live window. Written before metadata it
  is the default start position. A stall while playing fires `timeupdate` and `waiting`, and
  `canplay`/`playing` when data is back.
- **The end.** `ended` becomes true, `pause` and `ended` fire and `paused` is true; `play()` starts
  again from the beginning.
- **Errors.** `error` is a `MediaError`: `MEDIA_ERR_NETWORK` for a manifest or media the network
  could not deliver (a 404 included -- HTML would say `MEDIA_ERR_SRC_NOT_SUPPORTED` for a 404
  before metadata; network keeps the element and Shaka's 1001 telling the same story),
  `MEDIA_ERR_SRC_NOT_SUPPORTED` for a manifest the platform does not play, no video output, a
  key system it lacks or no player at all, `MEDIA_ERR_DECODE` for media it cannot decode once
  metadata is in, or a licence refused. After an error nothing more of that load reaches the page.
- **The document.** An element removed from the document pauses -- after a stable state, as HTML's
  removal steps say, so one moved within the document keeps playing -- and its plane is hidden;
  put back, the plane shows again, still paused.
- **The rest:** `buffered`, `seekable` and `played` are `TimeRanges`; `videoWidth`/`videoHeight`
  and `resize`; `width`/`height` reflect their attributes; `getVideoPlaybackQuality()` has the
  platform's decoded and dropped frame counts; `canPlayType` answers from what this platform's
  player plays (`maybe` for a container or manifest type, `probably` when every listed codec is
  one it decodes, `''` otherwise -- DASH is `''` on Apple).

**Text tracks.** The platform's text tracks appear in `video.textTracks` as `TextTrack`s (kind,
label, language), `disabled` until a script -- or the Shaka layer -- sets a mode. The first in-band
track that is not `disabled` is the one the player selects; its active cues arrive as `VTTCue`s in
`activeCues` (and `cues`), with `cuechange`, for the app to draw: nothing is drawn natively
(except on Linux, below). `addTextTrack()` makes a track the app fills itself, whose `activeCues`
follow the clock.

### The plane

The element's rect in drawable pixels -- CSS pixels here -- from the subset its style parses:
`left`/`top` (or `right`/`bottom`) for an `absolute` or `fixed` element, offsets for `relative`,
`width`/`height` from CSS, else from the `width`/`height` attributes, else the video's own size
(300x150 until it is known), then `translate`/`scale`. Percent and viewport units are the
drawable's. The video is fitted inside the rect with its aspect kept (`object-fit: contain`).
The plane is hidden while the element is out of the document, `display: none` or `opacity: 0`.
Where the drawable is a fixed size scaled into the window (Android, Linux `--size`), the plane is
scaled the same way (`media::planeInWindow`).

**Video is always beneath the canvas.** A `z-index` that would put a `<video>` above the canvas --
higher, or equal and later in the document -- is composited beneath it anyway and warns once;
between videos, `z-index` then document order decide. The Blits example sets `z-index: -1`.

### The Shaka layer: @screenkit/shaka

Real Shaka Player cannot run here: it feeds MediaSource Extensions, and there are none. So
`packages/@screenkit/shaka` is a player whose API mirrors **Shaka Player 5**'s over the element's
controller, `video[Symbol.for('screenkit.media')]`, and **`@screenkit/vite-plugin` resolves
`shaka-player`** -- and every subpath of it -- **to it**, so Shaka code builds and runs unmodified
(the Blits `PlayerManager.js`). It is also importable directly.

- `new shaka.Player()`, `attach`/`detach`/`load(uri, startTime, mimeType)`/`unload`/`destroy`;
  `load` resolves once `loadedmetadata` has fired and the tracks, the variant playing and (live) the
  window are known. `configure` merges exactly as Shaka does -- Shaka 5's own defaults, an unknown key
  or a wrong type refused with an error, `undefined` restoring a default, the Shaka 4 preference
  names mapped -- and `getConfiguration`, `resetConfiguration`, `getNonDefaultConfiguration`.
- `getNetworkingEngine()`: request and response filters, retries with Shaka's retry parameters,
  `request()` as an abortable operation, over the runtime's `fetch`.
- Tracks: `getVariantTracks`, `getAudioTracks`, `getTextTracks`, `getVideoTracks`,
  `selectVariantTrack` (pins the variant; ABR off), `selectAudioTrack`/`selectAudioLanguage`,
  `selectTextTrack` (Shaka 5: a track shown, `null` for none), `setTextTrackVisibility`/
  `isTextTrackVisible` (Shaka 4's switch, kept: the selected track's `TextTrack` is `showing` or
  `hidden`), the language lists; `variantchanged`, `adaptation`, `textchanged`,
  `texttrackvisibility`, `trackschanged`.
- State: `isLive`, `seekRange`, `isBuffering` and `buffering`, `getStats` (the platform's decoded,
  dropped and corrupted frames and bandwidths; Shaka's own play, pause and buffering times, load
  latency and state and switch history), `getBufferedInfo`, `getMediaElement`, `getAssetUri`,
  `getManifestType`, `keySystem`, `drmInfo`, `trickPlay` (forwards only), `goToLive`;
  `Player.isBrowserSupported()`, `Player.probeSupport()`.
- `shaka.util.Error` with Shaka's category, code, severity and data, every code Shaka numbers;
  `shaka.polyfill.installAll()` is a no-op; `shaka.log`, `shaka.util.FakeEvent`/`EventManager`/
  `StringUtils`/`Uint8ArrayUtils`, `shaka.net.NetworkingEngine.RequestType`, and
  `shaka.drm.FairPlay`'s request and response helpers.
- **DRM**, configured Shaka's way: `drm.servers`, `drm.clearKeys`, `drm.advanced` (robustness,
  `serverCertificate`/`serverCertificateUri`, headers), `preferredKeySystems`. The key system asks
  for a licence; **this player performs the request** -- a `LICENSE` request through the networking
  engine, so the request and response filters apply -- and hands the answer back
  (`provideLicence`). For FairPlay the application certificate is fetched first
  (`SERVER_CERTIFICATE`), the request's `initData` is the `skd://` id and its body the SPC, sent as
  Shaka sends it (register `shaka.drm.FairPlay.*` filters for a server that wants another shape).
  A licence server that fails is **6007**; a certificate that cannot be fetched is 6007 too.
- Errors, all Shaka's own: 1001 `BAD_HTTP_STATUS` / 1002 `HTTP_ERROR` for a manifest the network
  could not deliver, 4000 `UNABLE_TO_GUESS_MANIFEST_TYPE` for one the platform does not play, 3016
  `VIDEO_ERROR` (category MEDIA) for media it cannot play or nowhere to show it, 6001
  `REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE`, 6007 `LICENSE_REQUEST_FAILED`, 7000
  `LOAD_INTERRUPTED` for a load replaced by another load, `unload()` or `destroy()`, 7002
  `NO_VIDEO_ELEMENT`, 7003 `OBJECT_DESTROYED`. A failed load rejects, fires `error` at the player
  and leaves it ready for the next.

**Where the Shaka layer is not Shaka, deliberately:** manifests and segments are the platform
player's, so request and response filters see only the licence and certificate requests; the load
mode is `SRC_EQUALS` (as Shaka's own native-HLS mode on Safari); out-of-band text, thumbnails and
chapters, `preload`, offline storage, ads, cast, low-latency tuning and the UI (`shaka.ui`) are
absent, and the calls that add them reject as Shaka rejects them in src= mode; a key system that is
configured but that the platform lacks fails the load with 6001 whether or not the content is
encrypted, since there is no manifest of ours to look at first.

### Where the platforms differ

One element over three players, and they do not agree about everything. Each difference is
deliberate, asserted per platform by the `media-*` rows (`NET_PER_PLATFORM`), and recorded here:

| | Apple (AVPlayer) | Android (ExoPlayer) | Linux (libvlc) |
|---|---|---|---|
| HLS, progressive MP4 | play | play | play |
| DASH | **4000** (AVPlayer has none) | plays | plays |
| ClearKey (`drm.clearKeys`) | **6001** | plays | **6001** |
| Widevine | 6001 | licence through JS | 6001 |
| FairPlay | licence through JS (device and macOS; the tvOS simulator has none) | 6001 | 6001 |
| Variants | every variant listed and selectable | every variant listed and selectable | **only the playing stream** is listed, and cannot be switched (selecting it is answered with `variantchanged`); ABR limits and the board's ceiling apply at load |
| Captions | `cuechange` with `activeCues` | `cuechange` with `activeCues` | **drawn by VLC into the picture**; tracks listed and selectable, `cuechange` never fires |
| Cue times | a WebVTT cue's own start and end, on the playback clock; a CEA-608 or TTML cue's start is when it appeared and its end the same | `startTime` is when the cue first appeared, `endTime` NaN: Media3 hands over only the cues active now | -- |
| Live window | AVPlayer's seekable range, which holds back three target durations from the live edge | ExoPlayer's window, on the period timeline | VLC's |
| Buffered | AVPlayer's loaded time ranges | ExoPlayer's buffered position | **an estimate**: from where playback last started to a second ahead (libvlc 3 does not say) |
| `decodedFrames` | pixel buffers an `AVPlayerItemVideoOutput` vends while playing (there is no public decoder counter) | decoder counters | pictures VLC's video output showed or dropped (`libvlc_media_get_stats`; VLC 3's own decoded counter counts each picture twice) |
| `droppedFrames` | the access log's `c-frames-dropped` | decoder counters | pictures VLC dropped as late, plus any the video plane had no free buffer for |
| Two videos, one over the other | stacked by `z-index` | **the newer plane is behind**: ExoPlayer's SurfaceViews are added in creation order | **the newer plane is behind** |
| No video output | -- | -- | no Wayland: **3016**, naming Wayland |

### Lifecycle

A player exists from an element's first load until the element is emptied (an unload -- Shaka's
`unload()`/`destroy()`, or a load with no source), the element is collected, or the runtime shuts
down; while one exists the runtime is not `idle()`. A **paused runtime pauses its players** -- the
video stops, not just the page's view of it -- and plays them again on resume, so the page sees no
change of its own `paused`. Shutdown stops every player synchronously: nothing is delivered after
it returns (`media-runtime-pause`, `media-shutdown`, and `leaks-media-shutdown` under leaks(1)).

**Deliberately absent:** MediaSource Extensions and `srcObject` (a warning, once), EME
(`requestMediaKeySystemAccess`, `MediaKeys` -- DRM is configured through the Shaka layer), `<audio>`
playback and Web Audio, picture-in-picture, `captureStream`, and native caption rendering (VLC's on
Linux is the one exception). `gl.texImage2D(..., video)` is not yet supported (`deferred-work.md`).
`dom-identity-and-absence` asserts the absences.

**Proven by** the `media-*` rows, against media the fixture server generates at start with the
build machine's `ffmpeg` (`tests/net/media.mjs`: a two-variant HLS stream with a WebVTT rendition,
a live window, DASH, MP4, CENC DASH, a FairPlay-keyed playlist, a corrupt file) and a fake licence
server that records what reached it -- one row per line of the spec's matrix, through the element
and through the Shaka API, on macOS, Android and the Pi. No row reaches the public internet.

## Instances: `<iframe>`

**One iframe = one Instance**: a second app on its own Hermes runtime, its own
thread and its own GL context in this page's share group, drawing into an
FBO-backed texture that this page composites at the element's CSS rect
(`Architecture.md` §5). The element, `contentWindow`, `sandbox` and the message
plumbing are here; making the runtime, gating its package and compositing its
frame are `__screenkit.instances`'s (`core/src/instance/`,
`core/src/compositor/`, `core/src/bindings/Instance.cpp`).

```js
const game = document.createElement('iframe');
game.src = 'games/tetris.skpkg';           // a package inside this app
game.sandbox = 'allow-media';
game.style.cssText = 'position:absolute; left:0; top:0; width:1920px; height:1080px';
document.body.appendChild(game);
game.onload = () => game.focus();          // the launcher pauses, the game runs

game.contentWindow.postMessage({ type: 'resume-save', slot: 3 });
window.addEventListener('message', e => e.source.postMessage({ ok: true }));
```

**The iframe boundary is not a security boundary.** `Architecture.md` §5.2
stands and is worth repeating where the API is: JS **is** isolated — two
runtimes share no object and `postMessage` is the only channel — but memory is
**not**. A native crash or an OOM in any instance kills every instance including
the launcher, there is no hard per-instance memory cap, and `sandbox` gates
capabilities, not memory and not CPU. This cannot be presented to third-party
developers as containment.

### The element

`HTMLIFrameElement` is HTML's, as far as a second browsing context here goes:

- **`src` names a package inside this app**, resolved against the document like
  any other asset and confined by the same asset root — a launcher ships the
  games it embeds. An `http(s)` URL fires `error` with a message that says so,
  and fetching one is a later milestone. So does a path that escapes the
  package, a missing package, and one the existing gates refuse (wrong `format`,
  a newer `runtimeVersion`, bytecode this engine does not speak): the same gate,
  and the same messages, the host gates its own app with.
- **`load` and `error`** fire on the element, with `onload`/`onerror`. `load`
  means the package's entry ran; an entry that throws is `error`, and the
  instance's own failure is reported rather than the parent's.
- **`sandbox` is a `DOMTokenList`** over the attribute, with `add`, `remove`,
  `contains` and `value` — the same object model `classList` is, over a
  different attribute. An element with no `sandbox` attribute is not sandboxed.
- **`contentWindow`** is a WindowProxy, never the other runtime's global: it has
  `postMessage`, `focus()`, `self`, `window`, `parent` and `top`.
  `contentDocument` is `null` — the other side is a separate runtime with a
  document of its own, and there is nothing to reach into.
- **`focus()`** hands the remote over, and `width`/`height` reflect their
  attributes as a `<video>`'s do.
- **The rect** is the CSS subset's, computed exactly as a `<video>`'s plane is
  (`position`, `left`/`top`/`right`/`bottom`, `width`/`height`, `z-index`,
  `opacity`, `display`, `transform: translate|scale`), in drawable pixels. The
  default size is 300x150, as a browser's is. Layers sort by `z-index` then
  document order, among themselves and against the canvas; no instance layer
  goes below a `<video>` plane, which is beneath everything.
- **Removing the element terminates the instance**, as HTML terminates a removed
  browsing context: the thread is joined, the heap and the GL go, and the layer
  goes with them. Putting it back loads it again, fresh. A node moved *within*
  the document is not torn down — the check is deferred to a microtask, exactly
  as the `<video>` removal steps are. The join is synchronous, so removing an
  iframe holds this page's own JS thread for as long as the instance's teardown
  takes; that is the price of "nothing of it is delivered afterwards".

### Lifecycle

`Running`, `Paused`, `Terminated` (`Architecture.md` §5.1; `Suspended` and the
memory-pressure LRU are deferred, and `suspend` and `memorywarning` are
therefore not dispatched).

`focus()` is atomic in the one sense that matters: the outgoing context is told
to freeze before the incoming one is told to thaw, so no context gets a frame the
other has already had. A frozen context fires **zero** rAF callbacks and zero
timer callbacks, receives no input and no `resize`, and keeps its last rendered
frame on screen — which is what makes a launcher's tile of a frozen game free.

`pause` and `resume` fire at `window` in the context they are about. The `pause`
dispatch and the freeze are one task on that context's own thread, in that
order: a task queued *after* the gate closed would wait behind it and arrive on
resume instead, which is precisely the burst of stale callbacks `Paused` exists
to prevent. `resume` is the other way round — thaw, then tell the page.

**A frozen launcher still composites.** `Paused` stops the *page* -- rAF,
timers, input, its own queued tasks -- but the window it draws into is not the
page's. A launcher that freezes itself to give a game the remote keeps serving
the present, and nothing else, so the game that now has the remote is what is on
screen. A page with no instances never does this, and neither does an instance:
its own pause still burns no wakeup at all.

`window.parent.focus()` in an instance hands the remote back. It reaches the
launcher from the instance's own thread rather than as a task, because the
launcher is frozen and a task would wait behind the very gate it means to open.
A `focus()` and a `parent.focus()` asked for in the same turn are allowed to
cancel out, and do.

### Messages

`postMessage` is task-to-task and asynchronous in both directions: no
`invokeSync`, no shared value, and no JSON bridge between the two runtimes. What
it carries is a structured-clone subset — primitives, plain objects and arrays,
and `ArrayBuffer` by value, with cycles preserved. Anything else throws a
`DataCloneError` `DOMException` at the sender, naming what it tripped over,
rather than arriving as something the receiver cannot tell apart from the real
thing — a typed array is one of those, so send its `.buffer`. Nesting is capped
at 64 deep, because the clone recurses and the alternative is the engine's own
stack overflow surfacing as a `RangeError` from somewhere unrelated-looking --
which is what 128 did where a stack frame is several times its usual size.
`structuredClone` is the same subset, with the same cap, and refuses `transfer`
for the same reason there is no `MessagePort`: the two runtimes share no memory,
so nothing can be moved, only copied.

A message arrives as a `MessageEvent` at the receiver's `window`, with `origin`
`''` (there are no origins here) and a `source` that can reply — the sending
window from the receiver's side, which is the iframe's `contentWindow` in the
launcher and `window.parent` in the instance.

`window.parent` and `window.top` are the launcher in an instance, and `window`
itself in a top-level page — which is also how a page detects that it is not
embedded. `window.frames` is a live-read array-like of the `contentWindow`s of
the `<iframe>`s in the document, in document order, and `window.length` is how
many there are. (In a browser `frames` is the window itself and `window[0]` is
the first child; here it is an object rather than numeric properties on the
global.)

### `sandbox`

An element with no `sandbox` attribute is not sandboxed and reaches everything,
as on the web. One *with* the attribute starts from nothing and gets back only
what its tokens name — an empty `sandbox=""` is the most restrictive there is —
and the gate is applied **where the binding is installed**, as `testTlsAnchors`
is, so nothing reachable from the instance's JS can widen it.

| Token | What it gates |
|---|---|
| `allow-network` | `__screenkit.net`: `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`. Added to the web's set, because networking is the capability an embedded app most obviously needs withheld |
| `allow-media` | `__screenkit.media`: `<video>` and the platform player |
| `allow-storage` | nothing today — this runtime has no storage API at all. Accepted and reserved, and says so once |
| `allow-background-audio` | what a `Paused` instance may keep running (the freeze gate is all-or-nothing today, so this is what would make it selective) |
| `allow-background-timers` | the same, for timers |

A withheld capability is **absent**, not stubbed: the instance gets the same
answer a platform without the thing gives — `fetch` rejects with "no network in
this runtime: `__screenkit.net` is missing", `video.canPlayType` answers `''` —
and the runtime says which token is missing, once. An unknown token is ignored
with a one-time warning, as a browser ignores one.

### Deliberately absent

`MessagePort`, `MessageChannel`, `BroadcastChannel` and transfer (nothing can be
moved between two runtimes that share no memory); `iframe.contentDocument` and
every other way into the other side's document; navigation inside an instance
beyond the fragment-only rule the shim already has; `suspend` and
`memorywarning`; and nesting past one level — an `<iframe>` inside an instance
fires `error` on that element and the instance keeps running, which is a
recorded divergence from `Architecture.md` §5 rather than a silently inert
element. `dom-identity-and-absence` asserts the absences.

**Proven by** the `iframe-*` rows, each driving a real launcher page with a real
child package (`tests/fixtures/iframe-child.js`, packaged as
`iframe-child.skpkg`) over a real ANGLE context — the share group, the freeze
gate and the composite are none of them visible from JS alone, so the composite
is asserted as pixels read back out of the launcher's own framebuffer.

## Known limits

- **`performance.now` is millisecond resolution** — `Date.now` is the only clock reachable from plain
  JS here. rAF callbacks already receive the finer native timestamp, and frame pacing should use that.
- **Navigation is fragment-only.** Setting `location.hash` (or `assign`/`replace`/`href` with a
  `#fragment`) updates `href` at once and fires `hashchange` on `window` as a task, which is what hash
  routers need; any other navigation is ignored with a warning, since there is no other document to
  load. `dom-location-hash` covers it, and `dom-fetch` checks that assets still resolve against
  `location.href` after a navigation.
- **The asset root is per instance.** Each runtime confines its own reads: write-once within one
  instance, never shared between two, and an `<iframe>`'s package is its own root. (It used to be one
  root per process, which was correct while there was one app.)
- **Without a Worker, images decode on the JS thread.** That is Lightning's own no-worker path and it
  works, but a large image can cost a frame.
- **The canvas that is the page's frame cannot be moved by CSS.** Every *other* canvas can: the
  subset places it, and its rect is where its layer composites (see "Canvases"), exactly as a
  `<video>`'s rect is where the platform composites the video and an `<iframe>`'s is where its
  instance's frame goes. What CSS cannot do is turn the drawable itself into a layer after a canvas
  has taken it, because the page is holding that context already (`deferred-work.md`).
- **An instance draws at the size it loaded at.** The layer texture is made once, from the element's
  rect at load time. A CSS change afterwards moves and scales the layer, so the game stays where the
  page puts it, but its resolution does not follow: a `<iframe>` grown from 320x180 to fullscreen is
  a 320x180 frame scaled up until it reloads.
- **Gamepads are the standard mapping only.** 17 buttons and 4 axes: no touchpads, motion sensors,
  `GamepadPose`, extra buttons or `trigger-rumble`, and nothing SDL cannot open as a gamepad. The
  state is sampled every host loop iteration, so a press and release between two samples is missed,
  as in a browser.
- **Rumble parameters are clamped, not rejected.** Out-of-range magnitudes, durations and start
  delays are clamped where a browser rejects them with a `TypeError`, and `duration` and `startDelay`
  are each clamped to 5 s separately -- up to 10 s in all, where Chrome caps their sum at 5 s.
- **The gamepad claim is one-way and process-wide.** Once anything calls `navigator.getGamepads()`,
  gamepads stop sending keys until the process exits -- including in a UI that only probed for a
  pad. There is no user-gesture gate on exposing them.
