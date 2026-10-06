# ScreenKit Architecture

A runtime that takes an **ordinary web build** — whatever `vite build` emits — and runs it on TV
hardware through Hermes and WebGL. HTML5 semantics without a rendering DOM.

**Targets:** tvOS (primary), Android TV + Fire TV, embedded Linux (Batocera ports), macOS (dev only).

---

## 1. The model is Capacitor, not Expo

The runtime is a prebuilt binary shell. An app is a web build plus assets; it **never triggers a
native compile**. Adding a native capability means shipping a new *runtime version*, not rebuilding
an app.

| | Expo / React Native | **ScreenKit** |
|---|---|---|
| App source | JS importing native modules | a plain web project |
| Build | Metro + per-app native build | **Vite → `dist/`**, then packaged |
| Per-app native compile | yes — autolinking, config plugins, prebuild | **never** |
| Native surface | extensible per app | **fixed, versioned with the runtime** |
| Rendering | native view tree | **WebGL only** |
| Embedding another app | bespoke API | **`<iframe>`** |

No autolinking, no config plugins, no generated Podfile or Gradle.

---

## 2. Where the browser lives: JavaScript, not C++

The native runtime is small on purpose.

> **Native owns the things only native can do. Everything browser-shaped is JavaScript.**

| Native (C++, `runtime/`) | JavaScript (`runtime/js/dom-shim.js`, hand-written, ships inside the runtime as `dom-shim.hbc`) |
|---|---|
| SDL3 event loop, window, input, audio | `document`, `Element`, `Event`, `EventTarget`, tree ops |
| Hermes host, HBC loading, instance threads | `HTMLCanvasElement`, `HTMLVideoElement`, `HTMLIFrameElement` |
| GL context management + raw GLES entry points | `WebGLRenderingContext` object model and validation |
| Compositor: layer tree → swapchain, fences | CSS subset → layer rects |
| Platform media players | `window`, `navigator`, timers, `rAF`, `fetch` wrappers |
| Sockets/TLS, file + asset IO, image decode | `URL`, `Blob`, `postMessage`, `MessageEvent` |
| `.skpkg` loading | everything else |

Native is seven small subsystems:

```
runtime/core/src/
├── sdl/          event loop · window · input · audio
├── hermes/       runtime host · HBC loader · source eval · instance threads
├── gl/           context management + GLES entry points (generated from .def tables)
├── compositor/   layer tree → swapchain · EGL fences
├── media/        player interface + Apple / Android / Linux backends
├── io/           fetch · WebSocket · TLS · file · stb_image decode   (today: net/ + bindings/HostIO)
└── bundle/       .skpkg loader · manifest · update store
```

The surface exposed to JS is a **narrow handle API** — create a layer, set its rect, hand me a
texture, play this URL — not a browser. That keeps the C++ auditable and puts the large, fast-moving,
easy-to-debug part in JS.

### Layering

```
App                     a web build — Lightning 3 UI · games · launcher
──────────────────────────────────────────────────────────────────────
runtime/js/dom-shim.js  elements · events · CSS subset  (+ vendored expo-gl WebGL object model)
──────────────────────────────────────────────────────────────────────
JSI bindings            SharedObject / EventEmitter — no bridge, no JSON
──────────────────────────────────────────────────────────────────────
runtime/core            sdl · hermes · gl · compositor · media · io · bundle
──────────────────────────────────────────────────────────────────────
runtime/platform        Apple (Swift/ObjC++) · Android (Java/JNI) · Linux (C++)
──────────────────────────────────────────────────────────────────────
SDL3  +  ANGLE  +  platform media framework
```

The **main thread** owns the SDL3 event pump (mandatory on Apple), the swapchain and the compositor.
Each instance renders into its own FBO-backed texture; the compositor composites visible layers and
drives `rAF` from vsync. GLES share groups share *textures and buffers* but **not** FBOs or VAOs, so
each context creates its own, and cross-context sync uses `EGL_KHR_fence_sync` — the compositor waits
on a per-instance fence rather than calling `glFinish`.

---

## 3. The DOM layer: planned as reuse, shipped hand-written

The plan was that reusing an existing implementation beats hand-writing browser APIs: **jsdom itself
does not fit, but its building blocks do.** What ships is the gate's fallback, below.

Measured, not assumed — `jsdom@30.0.1`:

- **7.1 MB unpacked**, 21 direct dependencies including `undici` (a complete HTTP/1.1+2 stack),
  `parse5`, `tough-cookie`, `decimal.js`, `saxes`.
- `engines: node >=22`. It targets Node and needs built-ins (`vm`, streams, `Buffer`, net/tls via
  undici) that Hermes does not have.
- **No WebGL at all**, and its canvas requires the `canvas` npm package — a native Cairo binding. The
  two elements we care most about are exactly the two it cannot help with.
- Most of its bulk is an HTML parser, a cookie jar and a network stack — all things we already have
  natively or do not want at runtime.

So: **compose from the same WHATWG packages jsdom is built from, and skip jsdom.** These are
separately published, mostly dependency-free, and not Node-bound.

| Package | Gives us |
|---|---|
| `symbol-tree` | the DOM tree structure jsdom itself uses |
| `webidl-conversions` | spec-correct argument coercion, cheaply |
| `whatwg-url` | `URL`, `URLSearchParams` |
| `@asamuzakjp/dom-selector` | `querySelector` / `matches` |
| `css-tree` + `@asamuzakjp/css-color` | parsing for the CSS subset |
| `whatwg-mimetype`, `data-urls` | `Blob`, `data:` URLs |

Deliberately skipped: `undici` (native fetch), `parse5` (no runtime HTML parsing), `tough-cookie`,
`saxes`, `w3c-xmlserializer`, `decimal.js`.

**This is a gated spike.** Budget: the whole DOM layer under **500 KB of bytecode** and under
**300 ms cold start** on the weakest target (Fire TV). If composition misses that, the fallback is a
hand-written minimal shim — worse for compatibility, but shippable. Measure before committing.

**What ships is the fallback.** The DOM layer, element tree included, is hand-written in
`runtime/js/dom-shim.js`, compiled into the runtime as `dom-shim.hbc`; none of the packages above
is used. Its scope is measured against the Lightning bundle (`runtime/js/README.md`), not taken from
the DOM spec. Against the budget: `dom-shim.hbc` is **373 KB** of bytecode (382,248 bytes, with runtime networking,
the platform media element, the software 2D canvas and M9's `<iframe>` instances; 195,699 with
networking alone, 106,970 at M6), about three quarters of the 500 KB. The instance surface —
`HTMLIFrameElement`, `postMessage`, `structuredClone` and the window tree — is 15 KB of that.
Cold start on Fire TV is **not measured** — the Android host (M10) has run only on an Android TV
emulator, not on a Fire TV.

### 3.1 The element model

A real element tree exists: `Node`, `Element`, `HTMLElement` and `Document`, with attributes,
`id`/`className`/`classList`, the tree operations and their DOM exceptions, and events that capture,
reach the target and bubble along the tree to `window`. `querySelector`/`querySelectorAll`,
`matches` and `closest` take a selector subset — type, `*`, `#id`, `.class`, `[attr]`,
`[attr=value]`, compound selectors, the descendant and `>` combinators, and lists — and throw a
`SyntaxError` for anything else rather than matching nothing. **But only three elements ever paint**,
because only three have a native layer behind them:

| `document.createElement(…)` | Backed by |
|---|---|
| `'canvas'` | a GL framebuffer; `getContext('webgl'\|'webgl2')` — what Lightning 3 is handed. **Each canvas gets its own GL context**, and each but the page's frame its own compositor layer |
| `'video'` | the platform player — AVPlayer / Media3 ExoPlayer / libvlc |
| `'iframe'` | a full Instance: its own Hermes runtime, thread, GL context and layer |

Every other tag constructs an ordinary, inert element. It lives in the tree, dispatches events, and
never renders. Libraries that build DOM structure keep working; nothing shows up unless it is one of
the three.

**CSS applies only to those three elements.** `position`, `left`/`top`/`right`/`bottom`,
`width`/`height`, `z-index`, `opacity`, `display: none` and `transform: translate|scale` are parsed
and mapped onto compositor layer rects. There is no cascade for anything else, no layout engine, and
no text rendering from CSS. Every element has a `CSSStyleDeclaration` that stores and reads back what
is set; only a backed element parses the subset.

**Every canvas that takes a WebGL context gets a real GL context of its own**, in the host's share
group, with its own bound program, textures and blend state as WebGL says two canvases have -- so a
page may put a HUD over a game without an `<iframe>` and a second runtime to carry it. The first
canvas of a page that CSS has not placed **is the frame**: it draws into the window, `canvas.width`
reads `gl.drawingBufferWidth`, a size write is accepted and ignored, and a page with one canvas
builds no compositor and keeps exactly the present path -- and the cost -- it had. A canvas the
subset places, resizes or hides before it asks for its context becomes an FBO-backed **layer at that
rect** instead, with a drawing buffer of its own that a size write does change. Canvas layers and
`<iframe>` instance layers are **one list**, sorted by `z-index` then insertion, so they compose
against each other; video planes stay beneath all of them. Making a canvas current is one comparison
per GL call and a `makeCurrent` only where a page alternates between canvases
(`spec-m6-composited-canvases.md`, `runtime/js/README.md` "Canvases"). A 2D canvas is not
composited: it stays CPU pixels and a texture source.

### 3.2 The environment contract

"No DOM" is an endless source of surprises unless the surface is enumerated. This is the
compatibility contract, published as TypeScript defs in `@screenkit/types`.

**Provided:** `console`, `setTimeout`/`setInterval`, `queueMicrotask`, `Promise`, `fetch` (with
`Headers`, `Request`, `Response`, `AbortController`/`AbortSignal`, `FormData`, and streaming request and
response bodies over `ReadableStream`), `XMLHttpRequest` (the whole of it, upload progress included),
`WebSocket`, `EventSource`, a cookie jar, `URL`, `URLSearchParams`, `TextEncoder`/`Decoder`,
`performance`, `requestAnimationFrame`, `crypto.getRandomValues`, `localStorage`, `structuredClone`,
`Blob`/`File`, `WebGLRenderingContext` / `WebGL2RenderingContext`, `Image` (package or network),
`postMessage` / `MessageEvent`, `window.parent`, `window.top`, `document.body` (the root compositor layer).

**Networking** is each platform's own HTTP client, driven from native
(`runtime/core/src/net/`) and surfaced as browser APIs in the DOM shim
(`runtime/js/README.md`, "Networking") -- React Native's shape. `NetService` is the seam: an
interface with one implementation per platform, **NSURLSession** on Apple (`NetServiceApple.mm`, what
RN's `RCTHTTPRequestHandler` uses) and **OkHttp** over JNI on Android (`NetServiceAndroid.cpp` plus
`dev/screenkit/net/HttpClient.java`, what RN's `NetworkingModule` uses). ScreenKit owns no HTTP,
WebSocket or cookie implementation on either: name resolution, connection pooling, HTTP/1.1 **and
HTTP/2**, redirects, `Content-Encoding`, proxies, certificate verification against the system trust
store and the cookie store (`NSHTTPCookieStorage`, `android.webkit.CookieManager`) are all the OS's.
No networking or TLS library is bundled and no CA roots ship. What follows from that is per-platform
divergence where the clients disagree, recorded rather than worked around
(`spec-platform-http-clients.md`). Package-asset URLs still read from the package.

**Linux is the one exception, and only for the protocol.** RN has no Linux target and the
distributions have no client to point at, so it gets a vendored **cpp-httplib** -- one MIT header
pinned by sha256 (`runtime/third_party/httplib/`), with one small patch table applied on import
(`tools/vendor/httplib.rules`: it keeps the WebSocket Close payload upstream discards) -- behind the
same seam (`NetServiceLinux.cpp`, `spec-linux-http-client.md`). The rules that matter still hold:
**TLS is the image's own OpenSSL and the roots are the image's own CA bundle**, found at run time and
never shipped, and an image without libssl fails at configure time rather than falling back to
plaintext. The client does more of the work there than on the other two -- redirects, name
resolution that a cancel can abandon, and `Content-Encoding` are its own -- and what does not
survive the substitution is recorded as a platform divergence rather than worked around: there is
**no HTTP/2**, and no WebSocket extensions. Cookies there are the one jar left in the tree, `LinuxCookieJar` -- RFC 6265, in memory for
the runtime's life -- because there is no platform store to hand them to, which also makes it the one
place where the cookie name prefixes and the secure-origin rule are ours to enforce.

**Canvas2D is a software subset**, drawn on the CPU in the shim: rectangles, images, pixel data,
transforms, compositing and text -- what engines use a 2D canvas for beside WebGL (Phaser probes blend
modes with it at import; Pixi, Phaser and Lightning rasterise `Text` on one; a 2D canvas uploads to WebGL
like an image). Glyphs come from SDL3_ttf (§7). Paths, patterns and shadows are not drawn. **`DOMParser` parses XML** (bitmap-font descriptions,
data files); HTML parsing stays absent.

**Absent by design:** Canvas2D paths, SVG, `<video>` via markup (create it), Web Workers, Service Workers,
IndexedDB, History, layout, flexbox/grid, CSS on non-composited elements, and from networking:
WebSocket `permessage-deflate` on Linux (NSURLSession and OkHttp offer it), `WritableStream`/`TransformStream` (so
`pipeTo`/`pipeThrough`), byte streams and BYOB readers, synchronous network XHR, and
`document.cookie`. HTTP/2 and proxies are no longer absent and no longer ours: they are whatever the
platform's client does.

**Verify per target:** `Intl` support follows whichever Hermes build React Native ships and differs by
platform. Treat it as unavailable until measured.

---

## 4. `<video>`

```js
const v = document.createElement('video');
v.src = 'https://cdn.example/stream.m3u8';
v.style.cssText = 'position:absolute; left:0; top:0; width:1920px; height:1080px; z-index:-1';
document.body.appendChild(v);
await v.play();
```

Standard `HTMLMediaElement`: the load algorithm and its events, `readyState`/`networkState`,
`play()` promises, seeking, `ended`, `loop`, `autoplay`, `TimeRanges`, `MediaError`, and text tracks
(`TextTrack`, `VTTCue`, `cuechange`) whose cues the app draws. Each element is backed by **the
platform's own player**, which owns the manifest, buffering, ABR, decoding, DRM and presentation --
what react-native-video does: **AVPlayer** on Apple, **Media3 ExoPlayer** on Android, **libvlc** on
Linux (§10.4). They sit behind one native seam, `MediaPlayer` (`runtime/core/src/media/`), shaped
like the network's: no JSI below it, events out through a sink as event-loop tasks.

Above the element sits **`@screenkit/shaka`**, a player whose API mirrors Shaka Player's. Real Shaka
cannot run here -- it feeds MediaSource Extensions, and there are none -- so `@screenkit/vite-plugin`
resolves `shaka-player` imports to it and Shaka code (the Blits `PlayerManager.js`) runs unmodified.
It configures DRM Shaka's way and performs the licence request itself, through its networking
engine, so request and response filters apply: **FairPlay** on Apple (`AVContentKeySession`),
**Widevine** and ClearKey on Android (`MediaDrm`). Linux has no DRM (6001). There is no EME.

**Direct play: video never passes through the runtime's GL.** The plane is the element's CSS rect,
composited by the platform **beneath the app's canvas** -- an `AVPlayerLayer` beneath the metal view,
a `SurfaceView` beneath SDL's, a Wayland subsurface beneath the window with libvlc's frames in
dma-bufs (sway on Batocera) -- and the app clears the canvas transparent where video shows. Zero-copy
on Apple and Android, HDR where the platform does it, DRM-safe. A `z-index` that would lift video
above a canvas warns once; video is beneath every one of them regardless. The canvas stays opaque to the
compositor while no plane is visible, so a page without video costs what it did before.

The earlier plan's second path, sampling video in WebGL through `gl.texImage2D(target, level, fmt,
fmt, type, videoElement)` (`CVMetalTextureCache` / `SurfaceTexture` / a decoder frame through EGL,
unavailable under DRM), is **deferred** (`deferred-work.md`).

What differs between the three players is recorded, not hidden, and asserted per platform by the
`media-*` rows (`runtime/js/README.md`, "Where the platforms differ"): no DASH on Apple (4000), no
ClearKey on Apple or Linux (6001), and on Linux only the playing variant listed and captions drawn by
VLC into the picture.

---

## 5. `<iframe>`: the multi-app model

**One iframe = one Instance** = one Hermes runtime + one thread + one `EGLContext` + one compositor
layer.

```js
const game = document.createElement('iframe');
game.src = 'games/tetris.skpkg';           // a package inside this app
game.sandbox = 'allow-media allow-storage';
game.style.cssText = 'position:absolute; inset:0; width:1920px; height:1080px';
document.body.appendChild(game);
game.onload = () => game.focus();          // launcher pauses, game runs

game.contentWindow.postMessage({ type: 'resume-save', slot: 3 });
window.addEventListener('message', e => { /* child → parent */ });
```

`postMessage` is the only cross-instance channel. It maps onto the per-instance task queues, so there
is no second bespoke IPC surface to design, document or secure.

**What M9 shipped, and where it diverges from this section.** One iframe is one Instance: a
`Runtime` on its own thread with its own GL context in the host's share group, drawing into an
FBO-backed texture the host composites at the element's CSS rect
(`core/src/instance/`, `core/src/compositor/`, `core/src/bindings/Instance.cpp`,
`spec-m9-iframe-instances.md`). Three divergences from the paragraphs above, each deliberate and each
asserted by an `iframe-*` row:

- **`src` names a local package**, not a URL. A path confined to the parent's own package, which the
  asset root already enforces; a launcher ships the games it embeds, so nothing is downloaded, there
  is no cache and no integrity check to write. An `http(s)` src is refused with a documented error,
  and fetching one stays M12's job.
- **Instances nest one level deep.** An `<iframe>` inside an instance fires `error` on that element
  and the instance keeps running. A tree is what this section describes; a launcher embedding a game
  is what it is for, and a third level buys nothing while costing a registry per instance.
- **`postMessage` carries a structured-clone subset**: primitives, plain objects and arrays, and
  `ArrayBuffer` by value, with cycles preserved. Anything else throws `DataCloneError` at the sender.
  The two runtimes share no memory, so there is no transfer and no `MessagePort`.

Instances receive input and `resize` only while focused, and the focused context is the one that
runs.

### 5.1 Lifecycle — exactly one browsing context runs

```
Loading ──▶ Running ──▶ Paused ──▶ Suspended ──▶ Terminated
               ▲          │            │
               └──────────┴────────────┘   resume
```

`focus()` is atomic: it pauses the outgoing context before resuming the incoming one. Removing the
iframe terminates its instance.

| State | rAF | Timers | Input | Audio/Video | JS heap | GL resources |
|---|---|---|---|---|---|---|
| `Running` | firing | firing | routed | playing | live | live |
| `Paused` | stopped | **frozen** | none | stopped | retained | retained |
| ~~`Suspended`~~ | stopped | frozen | none | stopped | retained | regenerable ones dropped |
| `Terminated` | — | — | — | — | freed | freed |

- `Paused` freezes the task queue rather than letting timers pile up, so a backgrounded game burns no
  CPU and fires no burst of stale callbacks on resume. Resume is instant.
- A `Paused` context keeps its **last rendered frame** as a compositor texture, so a launcher can show
  a live-looking tile of a frozen game for free.
- `Paused` stops the *page*, not the *window*. A launcher that freezes itself to give a game the
  remote still owns the compositor, so it keeps serving the present — and nothing else: no queued
  task, no microtask checkpoint, no rAF callback. A context with no layers, an instance included,
  burns no wakeup at all while frozen.
- Background work is opt-in through `sandbox`: `allow-background-audio`, `allow-background-timers`.

**M9 ships three states: `Running`, `Paused`, `Terminated`.** They fall out of the freeze gate, which
already stops rAF, timers, input and the media players. `Suspended` — dropping regenerable GL
resources while keeping the heap — and the memory-pressure LRU escalation are **deferred**: both need
a memory-pressure signal per platform (Apple's warning, Android's `onTrimMemory`, nothing on Linux)
and an audit of which GL objects can genuinely be rebuilt, and neither exists yet. Risk 10's "LRU
escalation must be real by M9" is therefore **not met**; on a tvOS device, retained `Paused` instances
plus their last-frame textures still stand against Apple's per-app limit, and a launcher that embeds
many games must terminate what it is not showing.

Contexts receive `pause` and `resume` on `window`. `suspend` and `memorywarning` belong to the
deferred fourth state and are not dispatched — `dom-identity-and-absence` pins that, so a well-meaning
stub fails the row rather than making a page wait for an event that never comes.

### 5.2 The iframe boundary is **not** a security boundary

On the web an iframe implies origin isolation and a separate address space. Here it does not:

- JS **is** isolated — separate Hermes runtimes share no objects, `postMessage` is the only channel.
- Memory **is not** — a native crash or OOM in any instance kills every instance including the
  launcher, and there is no hard per-instance memory cap.
- `sandbox` gates **capabilities**, not memory or CPU.

Best-effort mitigations only: a watchdog on unresponsive instances, per-instance GL accounting, Hermes
heap limits. This cannot be presented to third-party developers as containment.

---

## 6. Build pipeline

### 6.1 Packaging: `dist/` → `.skpkg`

```
app/                      ordinary web project — Vite + @screenkit/vite-plugin
  └── vite build
dist-screenkit/           index.html · polyfills + SystemJS chunks (assets/*-legacy-*.js) · fonts · images
  └── screenkit bundle
      ├── parse index.html → polyfills + legacy entry   (markup is a manifest, never rendered)
      ├── hermesc -dump-ast every chunk → its System.register deps and every
      │   import() must name a chunk in the build        ← else the build fails
      ├── pack polyfills + every chunk into ONE script   (SystemJS serves them from memory)
      ├── hermesc -emit-binary -O -Xes6-block-scoping   → app.hbc
      ├── copy assets                                    (not index.html, not the chunks;
      │                                                   a font without its MSDF atlas fails the build)
      └── manifest.json  (format, runtimeVersion, hermesBytecodeVersion read from app.hbc, entry, sha256 per file)
app.skpkg/                what ships, what an <iframe src> points at, what OTA delivers
```

**The dominant constraint: Hermes has no ES-module loader.** Vite emits ESM with code splitting by
default and that cannot run. Rather than flatten the build into one IIFE, `@screenkit/vite-plugin`
makes Vite emit what `@vitejs/plugin-legacy` emits for browsers without modules: Babel-transpiled
**SystemJS** chunks plus a polyfills bundle that carries the SystemJS loader. `screenkit bundle` then
resolves that module graph at build time. It runs every chunk once to capture its anonymous
`System.register`, and overrides `System.instantiate`, SystemJS's own extension point for where a
URL's code comes from, to serve those captured registrations. SystemJS stays the module loader and
nothing is ever fetched. The plugin:

- adds plugin-legacy output-only (`renderModernChunks: false`, `polyfills: true`, targets
  `defaults, not IE 11`), resolved from the app so it matches the app's Vite (7 or 8): from
  `screenkit({ root })`, else the package npm is running a script for, else the working directory;
- adds the core-js polyfills for the ES built-ins the pinned Hermes lacks, which browser targets never
  ask for (`additionalLegacyPolyfills`, from `hermes-builtins.json`; the `hermes-builtins` ctest row
  holds the engine to that list, and records why the rest cannot be polyfilled);
- refuses a build that has plugin-legacy twice, as when a merged app config brings its own preset;
- carries the build-time fixes a Lightning 3 / Blits app needs on Hermes, each matched to the exact
  code it rewrites and failing the build if that code changes shape, or if the fix's package is in the
  build but its target module never is (moved or renamed);
- forces the output shape `screenkit bundle` reads, over whatever a merged app config says, with a
  note for each value it replaces: `outDir` `dist-screenkit/` (or `screenkit({ outDir })`), so the
  browser build is untouched; `emptyOutDir`, so no stale chunk is packed; and `base: '/'`, because a
  package is served from its own root. ScreenKit support is a second Vite config, never a change to
  the first.

Not built, of what this section once planned: the plugin does not rewrite `import.meta.url`, pin
`build.target`, or warn about meaningful markup in `index.html`, and it generates no MSDF atlases of
its own. A Lightning / Blits app's atlases come from Blits' own `msdfGenerator` Vite plugin, which
writes them into the build like any other asset, and `screenkit bundle` copies them into the
package.

**A dynamic `import()` is supported when the build can resolve it.** Legacy renders
`import('./pages/Loading.js')` as `context.import('./Loading-legacy-<hash>.js')`, a URL relative to
the importing chunk. `screenkit bundle` parses every chunk with the pinned `hermesc` and checks that
each such call has a literal argument, and that it and each static dependency name a chunk in
`dist/`. That chunk is packed like any other and served
from the package at runtime. `import(variable)`, a template with holes, or a literal naming a chunk
missing from `dist/` **fails the build**, naming the chunk and the call site, rather than becoming
a runtime fetch with no loader to service it. The optional-call forms of the same call are followed
too. Any other use of the import context -- passed to a function, stored, destructured -- also fails
the build, because an `import()` made through it would go unchecked; so does a named
`System.register`, which the packed loader cannot serve. Code Hermes cannot compile fails the same
way, with `hermesc`'s first diagnostic reported against the chunk it came from.

`index.html` is read as a **manifest of entry points**, not as markup. Anything beyond the legacy
polyfill and entry `<script>`s is ignored.

**A `.skpkg` is a directory**: `manifest.json`, `app.hbc` and the app's assets. Zipping it for OTA
is M12's concern, and so is checking the recorded hashes. The host gates on the manifest **before
evaluating anything** from the package. It parses the manifest with the runtime's own `JSON.parse`,
and refuses the package when:

- the manifest is missing, is not valid JSON, is not an object, or its `format` is not `1`;
- `hermesBytecodeVersion` is not its engine's;
- `runtimeVersion` is below 1 or newer than its own, which comes from `runtime/VERSION`, the same
  file `screenkit bundle` writes into manifests;
- `entry` does not resolve, after following symlinks, to a regular file inside the package.

Each refusal is a logged error naming the manifest file, and both sides for a version, and a
failed launch. The package directory is the app's asset root, so `/fonts/…` resolves inside it.

A package that passes the gate can still fail: its entry module can reject, which happens in a
microtask after evaluation has returned. The packed script reports that through
`__screenkit.reportFailure`, naming the entry and the error, and the host acts at its next loop
iteration: headless and `--window` exit 65, so `screenkit-host app.skpkg` is a usable CI assertion,
and tvOS ends the app with `SDL_APP_FAILURE` (process status 1, from SDL's UIKit main) rather than
showing a blank screen. A resolved entry logs `screenkit bundle: entry "<entry>" ready`, which the
tvOS launch check waits for. The packed script needs that binding, so packages carry
`runtimeVersion` 2 and an older runtime refuses them at the gate.

**Packaging is a separate process from the dev server.** `screenkit bundle` is a CLI step that
consumes a finished `dist/` and emits a `.skpkg`. It is not part of `vite dev`, does not run on file
change, and never needs to be fast.

### 6.2 Who runs the dev server: Vite does

We do not build a dev server. The app already has one — whatever it uses, normally `vite dev` — and it
owns watching, transforming, HMR and the module graph. Our job is only to let a TV execute what that
server serves.

**How React Native solves the same problem, for contrast:**

| | React Native | ScreenKit |
|---|---|---|
| Dev server | **Metro** — Facebook's own bundler *and* server, `react-native start` on :8081 | **Vite**, owned by the app |
| Dev payload | one concatenated JS bundle re-sent per reload (`/index.bundle?dev=true`) | **per-module**, fetched on demand |
| HMR | Metro's `/hot` WebSocket → `HMRClient` → React Refresh | **Vite's own HMR protocol**, unmodified |
| Tooling | `@react-native/dev-middleware` — CDP inspector proxy, `/symbolicate` | Vite's, plus a small dev menu |
| Release | `react-native bundle` → `hermesc` → `.hbc` embedded | `screenkit bundle` → `.hbc` in a `.skpkg` |

React Native had to write Metro because nothing existed for its module semantics. We do not, because
Vite 6 introduced — and Vite 8 ships — an API designed for exactly this case.

### 6.3 `vite/module-runner`: how Vite-transformed code runs on Hermes

Vite's dev mode normally serves **native ESM** and lets the browser fetch modules with `import`.
Hermes cannot do that. The Environment API solves it: `vite/module-runner` exports a `ModuleRunner`
that executes Vite-transformed modules in any JS runtime, with two pluggable seams we implement.

```
TV device                                   dev machine
┌──────────────────────────────┐            ┌────────────────────┐
│ @screenkit/module-runner     │            │   vite dev         │
│  ModuleRunner                │◀──── WS ──▶│  transform · HMR   │
│   ├ transport  → WebSocket   │            │  module graph      │
│   └ evaluator  → __screenkit_evaluate()   │                    │
└──────────────┬───────────────┘            └────────────────────┘
               ▼
        Runtime::evaluateJavaScript   (native, dev builds only)
```

- **Transport** — a WebSocket to the Vite server, in place of the default fetch transport.
- **Evaluator** — a `ModuleEvaluator` whose `runInlinedModule(context, code, module)` calls a native
  host function `__screenkit_evaluate(source, sourceURL)` → `Runtime::evaluateJavaScript`. This
  matters: it means we never depend on JS `eval` or the `Function` constructor, which Hermes builds
  may have compiled out.
- **HMR comes free.** `ModuleRunner` embeds Vite's `HMRClient`, so `import.meta.hot` works and only
  changed modules are re-sent — strictly better than React Native's whole-bundle reload.

`__screenkit_evaluate` is the **only** native API that exists solely for development. It is compiled
out of release builds, where the app is a single pre-compiled `.hbc` and nothing is ever evaluated
from source.

### 6.4 Dev and release run different module semantics

This is the sharp edge of the design and deserves stating plainly:

| | Dev | Release |
|---|---|---|
| Module graph | live, per-module via `ModuleRunner` | SystemJS chunks resolved at build time, packed into one `.hbc` |
| Code form | transformed source, evaluated | `.hbc` bytecode |
| Dynamic `import()` | **works** — the runner resolves it | **works if the build can resolve it**: a literal path to a chunk in the build is packed; `import(variable)` **fails the build** |

So code can work in dev and break at package time, and an `import()` whose target is computed at
runtime is the obvious case. Two defences, both required: `screenkit bundle`
must be runnable locally against a dev tree so the release path can be checked on demand, and CI must
run the full release pipeline on every change rather than only at ship time. React Native has the same
divergence between a Metro dev bundle and a release `.hbc`, and it is a well-known source of
late-breaking surprises.

---

## 7. Text: MSDF atlases, and canvas text through SDL3_ttf

Two paths, and an app picks per font, as it would in a browser:

- **MSDF atlases generated at build time**, sampled by a shader: Lightning 3's SDF renderer, PixiJS's
  MSDF bitmap fonts (BMFont XML, read through `DOMParser`), Phaser's BitmapText. Nothing is rasterised
  on the device; this is the fast path for a TV UI.
- **Canvas text**: Pixi's `Text`, Phaser's `Text` and Lightning's canvas renderer draw with
  `fillText` on a 2D canvas. Glyphs come from **SDL3_ttf** -- SDL's own font library, consumed prebuilt
  like SDL3, with FreeType and HarfBuzz inside it -- through `__screenkit.text`, as coverage masks the
  shim composites (`runtime/js/README.md`, "Canvas 2D"). Fonts are what the app loads with `FontFace`,
  or installed families (CoreText on Apple). Each run is rasterised on the CPU, so it is for labels that
  change rarely, not per-frame text.

`examples/pixi-hello`, `examples/phaser-hello` and `examples/lightning3-blits` show both side by side.

ScreenKit generates no atlases of its own. A Blits app's come from Blits' `msdfGenerator` Vite plugin
(`@lightningjs/msdf-generator`, over the prebuilt `msdfgen` in `msdf-bmfont-xml`), which converts
every `.ttf`/`.otf`/`.woff` under `public/` and writes `<font>.msdf.json` and `<font>.msdf.png` into
the build beside it, like any other asset. A `web` or `canvas` font is not converted into anything:
it draws through the 2D context from the font file.

---

## 8. Hermes: consumed from React Native, never built

Pin **one React Native version** as the single source of truth for compiler and runtime, so `hermesc`
and the engine can never come from different commits. Verified against `facebook/react-native`:

| Artifact | Source |
|---|---|
| Hermes commit pin | `sdks/hermes-engine/version.properties` → `HERMES_VERSION_NAME` (e.g. `260318099.0.2`) |
| `hermesc` | npm package **`hermes-compiler`** (version == `HERMES_VERSION_NAME`), at `hermesc/{osx-bin,linux64-bin,win64-bin}/hermesc`. It moved out of the `react-native` tarball in 0.87 |
| Apple runtime | `hermes-ios-$V-hermes-ios-release.tar.gz` from `repo.reactnative.dev/maven2` or Maven Central. Root is `destroot/`; the framework is **`hermesvm`**, not `hermes` |
| Android runtime | **`com.facebook.hermes:hermes-android:$V`** — prefab package `hermes-engine`, module `hermesvm`. The old `com.facebook.react:hermes-android` stops at 0.82.1. It links `libjsi.so` (taken from `com.facebook.react:react-android`, the React Native release that pins `$V`) and `libfbjni.so` |

Verified by unpacking the real artifacts: `destroot/Library/Frameworks/universal/hermesvm.xcframework`
carries **first-class `tvos-arm64` and `tvos-arm64_x86_64-simulator` slices**, so Hermes needs none of
the retagging that ANGLE did. Headers live only under `destroot/include/` (`jsi/jsi.h`,
`hermes/hermes.h`); the frameworks contain no `Headers` directory. Critically, **`hermesvm` exports the
JSI implementation itself** — 466 `facebook::jsi::*` symbols — so a non-React-Native embedder links
`hermesvm` alone and never compiles `jsi.cpp` or vendors ReactCommon. Android is the exception on
both counts: its prefab excludes `jsi/**`, so the headers come from the Apple `destroot` (they are
byte-identical), and its `libhermesvm.so` exports no JSI implementation at all -- it links React
Native's prebuilt `libjsi.so`, which the APK carries beside it. Nothing is compiled either way.
Hermes's `Intl` on Android is Java, reached through fbjni (`runtime/README.md`, "Targets").

The manifest records `hermesBytecodeVersion`; the loader refuses a mismatch rather than crashing
inside the engine. The bytecode cache is content-addressed on `hash(source, hermesc version, flags)`.
`--output-source-map` feeds symbolication.

**The gap:** React Native ships a Hermes *runtime* for Apple and Android only — for Linux there is
only a `linux64` host `hermesc`. So Linux builds Hermes from the pinned commit once, in our own
prebuilt pipeline: `tools/prebuilts/recipes/hermes-linux` produces `libhermesvm.so` for `linux-arm64`
and `linux-x86_64` in Docker, smoke-tests each against bytecode from the pinned `hermesc`, and the
manifest pins the archives by sha256 (`tools/prebuilts/README.md`, "Hermes on Linux"). Contributors
fetch it like any other prebuilt; only a Hermes bump rebuilds it. Nothing runs that check in CI yet.

---

## 9. Graphics

`gfx::GraphicsBackend` keeps the ANGLE decision reversible:

```cpp
class GraphicsBackend {
  virtual Surface createSurface(const SurfaceDesc&) = 0;
  virtual Context createContext(Context shareWith) = 0;
  virtual Fence   insertFence() = 0;
  virtual void    present(Surface, Fence) = 0;
};
```

Backends are **probed at startup, first success wins**:

| Platform | Chain |
|---|---|
| tvOS / macOS | ANGLE→Metal → SDL_GPU → native GLES (deprecated but present) |
| Android TV | ANGLE→Vulkan → **ANGLE→GLES3 → ANGLE→GLES2** |
| Fire TV | ANGLE→Vulkan *(probed, rarely usable on Fire OS 7/8)* → **ANGLE→GLES3 → ANGLE→GLES2** |
| Linux | ANGLE→Vulkan → ANGLE→GLES3 → ANGLE→GLES2 → native GLES via Mesa |

Android cannot bottom out: GLES2 has been guaranteed since Android 2.2 and GLES3 since 4.3. The probe
result is cached on `(GPU vendor, renderer, driver version)` and overridable by config.

**What M10 ships on Android** is the bottom of that chain without ANGLE, because no linkable ANGLE
exists for Android (§10): SDL's EGL context on the device's own GLES, through the same
`GlSurfaceSdl.cpp` as Linux -- ES 3 when the device has a config for it (WebGL2), else ES 2 (the
WebGL1 path a Raspberry Pi 3 takes). The frame is drawn offscreen and presented, so the page's last
frame survives Android taking the window's surface away in the background. minSdk is 24,
hermes-android's floor. There is no probe cache and no config override yet, and no Vulkan.

**Consequence:** WebGL2 requires GLES3, so on a GLES2-only device `getContext('webgl2')` returns
`null` and apps must feature-detect. Lightning 3 targets WebGL1, so the UI layer is unaffected — this
only constrains games.

**SDL3 cannot create the GL context on Apple.** `src/video/uikit/` has zero `SDL_EGL` references — the
backend is EAGL and Metal only — and `SDL_EGL` reaches EGL by `dlopen`ing `libEGL.dylib` while our ANGLE
is a static archive. Both are independently fatal, so `SDL_GL_CreateContext` is a dead end here. The
working seam is `SDL_Metal_CreateView` → `SDL_Metal_GetLayer`, whose `CAMetalLayer` is the
`EGLNativeWindowType` ANGLE's Metal backend wants.

**Hermes needs `withMicrotaskQueue(true)`.** Off by default (`RuntimeConfig.h`), and without it Hermes
falls back to a JS `Promise` polyfill in `InternalBytecode.js` that schedules through `setImmediate` —
so `Promise` throws `ReferenceError` unless the host supplies one. Supplying `setImmediate` is the
wrong fix: it makes promise callbacks macrotasks and loses "microtasks drain before the next timer".
The native queue is also what gives `jsi::Runtime::drainMicrotasks()` meaning.

**SDL3 must own the app lifecycle on UIKit.** `SDL_InitSubSystem(SDL_INIT_EVENTS)` fails outright under
a hand-written `UIApplicationMain` delegate ("did you include SDL_main.h…"), so the tvOS host uses
`SDL_MAIN_USE_CALLBACKS`. macOS tolerates the plain `main`; tvOS does not.

**Measured ceiling on Apple** (tvOS simulator, `poc/angle-tvos/`): ANGLE's Metal backend grants an
**ES 3.0** context; ES 3.1 and 3.2 both fail with `EGL_BAD_MATCH`. ES 3.0 is exactly what WebGL2 is
defined against, so the full WebGL2 surface is reachable — but ES 3.1 compute shaders, SSBOs and
image load/store are **not** available on Apple, and the renderer must not assume them.

Raw GLES entry points are generated from `.def` tables, a technique taken from `expo-gl/common/`
along with `EXTypedArrayApi` for zero-copy typed arrays and `stb_image.h` for decoding. The
`WebGLRenderingContext` object model and its validation live in JS, above that. Untrusted JS must
never reach a raw GL call.

---

## 10. Prebuilts and linking

**Nobody should have to build ANGLE to contribute.** Every third-party dependency ships as a CI-built,
checksum-verified binary; source builds are an opt-in escape hatch. The PoC showed what this avoids:
SDL3 alone is a 108 MB checkout and ~250 objects *per platform*; ANGLE is far worse.

```
tools/prebuilts/
├── manifest.json     # per dep: pinned version + sha256 per target   [EXISTS]
├── fetch.mjs         # download → verify sha256 → unpack → derive    [EXISTS]
├── retag-macho.py    # derives tvOS targets from iOS archives        [EXISTS]
└── recipes/          # CI scripts producing our own artifacts        [todo]
```

ANGLE, SDL3 and Hermes are wired up today, in one `manifest.json` rather than a file per dep.
Networking adds no entry here: on Apple the client is NSURLSession, which is the OS's, and on Android
it is OkHttp, resolved through Gradle from Maven Central and pinned by sha256 in
`runtime/android/gradle/verification-metadata.xml` like every other resolved artifact (§3.2). ANGLE has 12 targets across Apple, Linux and Windows, pinned to
`chromium/7578` from `godotengine/godot-angle-static`, each verified against the sha256 in the
manifest. `node tools/prebuilts/fetch.mjs --list` prints them. Two notes carried in the manifest
itself rather than tribal knowledge:

- **tvOS is derived, not published.** Both tvOS targets come from the iOS archives via
  `retag-macho.py`, which rewrites `LC_BUILD_VERSION.platform` (7→8 for simulator, 2→3 for device).
  Spike scaffolding; the real fix is a source build with `target_platform="tvos"`.
- **Android has no public prebuilt ANGLE** — it ships as an optional system *driver*, not a linkable
  library. Android has universal native GLES 3.x, so ANGLE there buys driver normalisation rather
  than capability. `fetch.mjs` fails with that explanation rather than a 404, and the Android host
  draws on the device's GLES directly (§9).

### 10.1 On-disk layout

Artifacts live **outside the repo**, in a cache shared across checkouts. Root is
`~/.screenkit/prebuilts/`, overridable with `$SCREENKIT_PREBUILTS`.

```
~/.screenkit/prebuilts/angle/chromium-7578/
├── include-src/                    3.3 MB   sparse clone of google/angle include/
├── apple-tvos-simulator-arm64/      14 MB   ← derived, retagged 7→8
├── apple-tvos-device-arm64/         14 MB   ← derived, retagged 2→3
├── apple-ios-simulator-arm64/       14 MB
├── apple-ios-device-arm64/          14 MB
└── apple-macos-arm64/               15 MB
```

Each target directory holds the archives plus a **`.verified` stamp containing the sha256**, so a
re-run is free rather than a re-download. The path shape is `<root>/<dep>/<version>/<target>/`, which
means several pinned versions coexist and moving the pin in `manifest.json` does not invalidate the
old tree.

Two consequences worth stating, because both were problems earlier in this project:

- **Nothing large enters the repo.** `poc/angle-tvos/` is ~40 KB of text while the 76 MB sits in the
  cache, so the project stays clonable without LFS.
- **It survives `rm -rf build/`.** Wiping a build tree triggers no re-download. The earlier SDL3
  arrangement put its 108 MB source checkout under `poc/build/`, which had to be rescued by hand when
  the PoC moved.

Consumers read the pin rather than hardcoding it: `poc/angle-tvos/CMakeLists.txt` pulls
`angle.version` out of `manifest.json` with CMake's `string(JSON …)`, so the build and the manifest
cannot drift. `-DSCREENKIT_BUILD_FROM_SOURCE=sdl3` overrides a single dep back to a source build.

### 10.2 Why prebuilt at all

Measured, not estimated: an ANGLE **source** build needs depot_tools and 10–15 GB, and a first
attempt filled this machine's disk mid-`gclient sync`. Prebuilt static ANGLE plus headers is
**22 MB**. That ratio is the whole argument for this section. Note that Google publishes no ANGLE
binaries — the spike used Godot's `godot-angle-static` — so shipping means running our own ANGLE
build in CI and publishing the artifacts ourselves.

> **Acceptance test: a fresh clone reaches a running app without compiling any third-party code.**

### 10.3 Linking policy

| Target | SDL3 / ANGLE | Media player | Hermes | Shared between apps? |
|---|---|---|---|---|
| tvOS / macOS | SDL3 **dynamic** framework, embedded in the app's `Frameworks/`; ANGLE static | AVFoundation, the OS's | **dynamic** `hermesvm` framework, embedded | No — Apple has no mechanism |
| Android | SDL3 / SDL3_ttf prefab AARs, `.so` in the APK; no ANGLE | Media3 in the APK (Java, pinned by sha256) | hermes-android prefab AAR, with `libjsi.so` and fbjni | No — APK-scoped |
| **Linux** | **dynamic, system-shared** (SDL3, GLES, **OpenSSL**) | **dynamic, system-shared**: the image's libvlc and the libavcodec it links; our VLC decoder module beside the runtime | dynamic | **Yes** |

Networking links nothing of ours on Apple or Android: the client is NSURLSession, with
Security.framework only for the test suite's trust anchors, and OkHttp -- the first Kotlin in that
build, so `kotlin-stdlib` and okio ride in with it, pinned by sha256 like every other resolved
artifact. On **Linux** it links the image's `libssl` and `libcrypto`, dynamic and system-shared like
everything else there: the HTTP protocol comes from a vendored header compiled into
`screenkit-core`, and the TLS and the CA roots come from the image. `tools/batocera/pi.sh sysroot`
copies the device's OpenSSL beside its SDL3, and the headers come from the build container for the
one reason they cannot be pinned to a source release -- `opensslconf.h` is generated by OpenSSL's
own Configure and is in no tarball. The
tvOS app carries `hermesvm.framework` and
`SDL3.framework` in `Frameworks/`, signed, with `@executable_path/Frameworks` as its only rpath, so it
loads nothing from the build machine.

**Linux is the only platform where sharing is real**, so it gets a different shape: the runtime
installs once as `libscreenkit.so` plus its shared dependencies, and an app is then only a manifest, a
`.hbc` and assets. A Batocera image with ten games carries one runtime, not ten. This works precisely
because the app model already forbids per-app native code — there is nothing app-specific to link.

Three consequences to design for rather than discover:

- **Soname versioning** on `libscreenkit.so`; `runtimeVersion` gating must handle a system runtime
  *older* than the app expects, failing with a clear message rather than a symbol-resolution crash.
- **System libraries may be absent or too old.** SDL3 only reached first stable release in January
  2025 and many Batocera images ship SDL2 only. Resolution order: the system library if it meets the
  minimum, otherwise the copy shipped beside the port. Load-bearing, not hypothetical — it doubles the
  Linux test matrix.
- **LGPL is why libvlc and FFmpeg are dynamic.** Static-linking LGPL into a binary obliges us to
  distribute relinkable object files; linking the image's own libraries avoids that entirely, and
  ships none of them.

### 10.4 Media backends

| Platform | Backend | Why |
|---|---|---|
| Apple | AVFoundation (AVPlayer), Objective-C++ | Hardware decode, HDR, FairPlay; HLS and progressive, no DASH |
| Android | Java + Media3 ExoPlayer, over JNI | HLS, DASH, progressive; Widevine and ClearKey; Fire TV compatibility |
| Linux | **libvlc** -- the image's VLC 3 -- plus ScreenKit's own VLC decoder module | Batocera ships it: HLS/DASH (VLC's `adaptive`), decode, A/V sync and audio (ALSA) are VLC's |

DRM is what splits the stack three ways: FairPlay needs AVFoundation and Widevine needs `MediaDrm`.

**Linux is libvlc, not a player of ours** (decided 2026-09-19, replacing a planned FFmpeg player). An
FFmpeg player is a set of libraries, not a pipeline: demux, decode, the master clock, A/V sync, seek,
buffering and subtitles would all be ours to write. VLC has them, and Batocera already carries it. What
libvlc 3 cannot give is recorded rather than worked around: no CENC keys (ClearKey is 6001, as on
Apple), no variant list (only the playing stream is listed; ABR limits apply at load), and no cue
text (captions are drawn by VLC into the picture -- the one place captions are rendered natively).
We own presenting its frames: libvlc's memory output (`libvlc_video_set_callbacks`) writes each frame,
when it is due, into linear NV12/I420 dma-bufs, committed to a desynchronised Wayland subsurface
beneath the app's window (`zwp_linux_dmabuf_v1`, `wp_viewporter`; Batocera 42 runs **sway**). No GL,
and video on Linux needs Wayland: under X11 or KMS a load fails with 3016, naming it.

**Hardware decoding follows the board.** The image's VLC decodes in software only (its `avcodec`
has no hardware module, and its `gstdecode` bridge crashes), so ScreenKit ships a **VLC plugin** of
its own (`libscreenkit_plugin.so`), loaded through `VLC_PLUGIN_PATH`. Its decoder module outranks
`avcodec` and, per stream codec, takes a stateless V4L2 decoder where the image's libavcodec offers
one; else the board's stateful V4L2 memory-to-memory decoder, **driven directly**, with capture
buffers allocated from the kernel's CMA heap so the CPU reads decoded frames through a cached mapping
(libavcodec's own `h264_v4l2m2m` offers only uncached buffers, too slow to read at 1080p on a Pi 3);
else that same decoder through libavcodec's `*_v4l2m2m`; else it declines and VLC's software decoder
plays. A probe of the V4L2 devices and the display mode sets the ABR ceiling, and sustained dropped
frames step it down one rung. The Raspberry Pi 3 plays 1080p30 H.264 through its `bcm2835-codec`;
faster boards follow from the same probe. The same plugin carries two smaller modules the image's VLC
lacks: an MPEG-TS demuxer by the name VLC's HLS support asks for (the image has no `ts` module, so HLS
with TS segments would not play at all), delegating to the image's `avformat`; and a stream filter
that reads an HLS or DASH manifest as VLC opens it and logs its ladder, liveness and protection for
the player, since libvlc 3 reports none of them (`tools/batocera/README.md`, "Video").

---

## 11. Folder structure

```
ScreenKit/
├── Architecture.md
├── package.json · turbo.json          # npm workspaces + Turborepo: the JS half of the repo
│
├── runtime/                        # small native shell — no per-app build
│   ├── core/src/{sdl,hermes,gl,compositor,media,io,bundle}/   (networking: core/src/net/)
│   ├── core/include/screenkit/     # the narrow handle API exposed to JS
│   ├── apple/                      # Swift + ObjC++ — tvOS & macOS
│   ├── android/                    # Gradle app: Java SDLActivity shell + libscreenkit.so (JNI) — no Kotlin
│   ├── linux/                      # builds libscreenkit.so, system-shared
│   ├── js/                         # the DOM shim — the browser, in JavaScript
│   └── tests/
│
├── packages/@screenkit/            # the published surface
│   ├── vite-plugin/                # primary dev-facing surface
│   ├── cli/                        # bundle · (run · deploy · doctor planned)
│   ├── shaka/                      # Shaka Player 5's API over the platform players
│   ├── types/                      # planned: the environment contract as TS defs
│   ├── module-runner/              # planned: on-device Vite ModuleRunner
│   └── updates/                    # planned: OTA client, manifest signing/verification
│
├── apps/                           # things that are not examples and not packages
│   ├── docs/                       # the documentation site (Astro Starlight)
│   ├── screenkit-go/               # planned: dev client + runtimeVersions.json
│   └── test-suite/                 # planned: on-device JS conformance suite
│
├── examples/                       # real apps, and the device test fleet
│   ├── pixi-hello/ · pixi-stress/            # Pixi, WebGL1
│   ├── phaser-hello/ · phaser-stress/        # Phaser
│   ├── lightning3-blits/ · lightning3-shaders/  # Lightning 3 / Blits
│   ├── blits-example-app/          # upstream Blits app, unmodified src/
│   ├── threejs-cube/               # three.js — WebGL2, so not deployed to the Pi
│   └── system-info/                # what the runtime reports about its host
│
├── tools/prebuilts/ · android/ · batocera/ · vendor/ · dom-usage/ · cmake/
│
└── poc/                            # native spikes kept for reference
    ├── CMakeLists.txt · src/main.cpp · scripts/ · cmake/ · README.md
    ├── angle-tvos/ · engine-bench/ · system-info is now examples/
    └── build/sdl3-src/             # shared SDL3 release-3.4.16 checkout
```

**The JS half is one npm workspace** (`package.json` `workspaces`, driven by Turborepo). `examples/*`
depend on `@screenkit/cli` and `@screenkit/vite-plugin` as workspace packages rather than by relative
`file:` path, so an example always builds against the plugin in the tree beside it. `turbo run build`
builds every workspace; `turbo run test` runs the package suites; `npm run package` runs each
example's `build:screenkit`. The device tooling (`tools/android/android.sh`, `tools/batocera/pi.sh`)
names an example by directory: `sh tools/android/android.sh run pixi-hello` builds
`examples/pixi-hello` and pushes it.

The original SDL3 PoC still lives in `poc/`, alongside the ANGLE tvOS spike and the engine
benchmark. Its two hard-won lessons carry into
`runtime/CMakePresets.json` (beside the CMake project it configures; `macos`, `macos-asan`, `tvos-simulator`): **Ninja, never the Xcode generator** — SDL3's ~200 `check_symbol_exists` probes
cost 41s under Ninja versus more than 12 minutes under Xcode, which spawns a full `xcodebuild` per
probe — and **one shared source checkout with a separate build tree per platform**, because a shared
`FETCHCONTENT_BASE_DIR` makes platforms silently overwrite each other's `libSDL3.a`. Both become moot
for contributors once prebuilts land, which is the point.

---

## 12. Milestones

| # | Milestone | Exit criterion |
|---|---|---|
| ~~M1~~ | **ANGLE tvOS spike — PASSED on simulator** | Done: ANGLE Metal renders GLES2 on Apple TV 4K sim at 3840×2160 (`poc/angle-tvos/`). Remaining: real Apple TV hardware, and ANGLE built properly for tvOS rather than retagged iOS binaries. |
| **M2** | **DOM composition spike — GATE** | The WHATWG-package DOM layer fits under 500 KB bytecode and 300 ms cold start on Fire TV. On failure, hand-written minimal shim. |
| M0 | Monorepo skeleton, presets, prebuilt pipeline | Fresh clone → running shell on tvOS + macOS, **zero third-party compilation** |
| ~~M3~~ | **Hermes + SDL3 loop — DONE on macOS and tvOS sim** | Met: an `.hbc` logs and runs `setTimeout`/`setInterval`/`queueMicrotask`/Promises in correct order on both. Timers are SDL3 timers; the work queue is SDL3's event queue; the tvOS host runs on `SDL_MAIN_USE_CALLBACKS`, so `SDL_AppIterate` is the frame tick. Remaining: real Apple TV hardware. |
| ~~M4~~ | **WebGL via vendored expo-gl — DONE on macOS and tvOS sim** | Met: a triangle drawn from JS at **60.0 fps** (16.67 ms mean) on Apple TV 4K, `GL_RENDERER` = ANGLE Metal, ES 3.0. SDL owns the window, ANGLE the context, `SDL_Metal_GetLayer` is the seam. The GL path is vendored expo-gl (`tools/vendor/expo-gl.sh`): real `WebGLRenderingContext` / `WebGL2RenderingContext` with `WebGLBuffer`/`WebGLProgram` objects, 225 methods, 568 constants. A **validation layer is still missing** — `gl` is ungated, so 9's rule that untrusted JS must never reach a raw GL call is not yet enforced. Remaining: that gating, and real hardware. |
| ~~M5~~ | **Vite plugin + `.skpkg` pipeline — DONE on macOS and tvOS sim** | Met: `vite build` → `screenkit bundle` → a `.skpkg` the host gates on and runs; a dynamic `import()` the build can resolve is packed, one it cannot fails the build loudly. Every failure is loud: an entry module that rejects fails the run (`__screenkit.reportFailure`; exit 65, status 1 on tvOS), a Lightning fix whose target module moved fails `vite build`, and the ES built-ins the pinned Hermes lacks are polyfilled or recorded as unpolyfillable (`hermes-builtins`). `run-tvos-simulator.sh` gates an embedded package. Remaining: real Apple TV hardware. |
| ~~M6~~ | **Element tree + canvas + CSS subset + MSDF — DONE on macOS and tvOS sim** | Met: **unmodified Lightning 3** — untouched app source and npm packages — renders a real UI with MSDF text (the Blits example app). The element tree, selector subset, event propagation and `CSSStyleDeclaration` are hand-written in `runtime/js/dom-shim.js`; atlases come from Blits' own generator. ScreenKit's named build transform is platform, not a patch: it exists because Hermes canonicalises NaN bit patterns on element copy, and fails the build if its target changes or moves (the two text transforms went once the 2D context drew text). The compositor behind the CSS subset is there now: **every canvas has its own GL context, and every canvas but the page's frame its own layer at its CSS rect**, sorting against `<iframe>` layers in one list (3.1; seven `canvas-*` rows, clean under ASan). Remaining: real hardware, and promoting the canvas that already took the frame into a layer when CSS moves it afterwards (`deferred-work.md`). A windowed host (macOS `--window`, tvOS) runs until it is closed rather than exiting once an app goes idle. |
| M7 | Module runner + HMR + Go client | `vite dev` → edit → **only the changed module** re-sent and applied on a TV over LAN |
| M8 | **`<video>` + AVPlayer — composited path met** | Video plays from JS on tvOS, composited and via `texImage2D`. Met for the composited path on macOS and the tvOS simulator, with ExoPlayer on Android and libvlc on Linux too, behind a Shaka-shaped player (§4); `texImage2D` deferred. |
| ~~M9~~ | **`<iframe>` instances + lifecycle — met on macOS and the tvOS simulator** | Met: one iframe is one Instance — a `Runtime` on its own thread with its own GL context in the host's share group, drawing into an FBO-backed texture the host composites at the element's CSS rect (§5). `focus()` pauses the outgoing context before resuming the incoming one, a `Paused` instance fires measurably zero rAF and zero timer callbacks while its last frame keeps showing, `postMessage` round-trips a copy in both directions, and removing the iframe joins the thread and frees the heap and the GL. `sandbox` gates `allow-network` and `allow-media` where the binding is installed. Thirteen `iframe-*` rows, clean under ASan. Remaining: the Pi's screenshot (macOS, the tvOS simulator and the Android emulator have run it), the `Suspended` state and the memory-pressure LRU (§5.1), and an instance whose layer changes size after it loads keeps the resolution it started with. |
| M10 | **Android TV / Fire TV shell — partly met** | Same `.skpkg` on both; falls back to GLES2 on a Vulkan-less device. Met on an Android TV 14 arm64 emulator: the APK (arm64-v8a + armeabi-v7a, Java `SDLActivity` shell, prebuilt hermes-android / SDL3 / SDL3_ttf AARs) runs the same packages as macOS, tvOS and the Pi -- PixiJS, Phaser, Lightning with MSDF text -- in ES 3; the remote navigates Blits, Back at the root exits, and backgrounding keeps the app's state and last frame. Not met: no Fire TV device has run it, the ES 2 fallback has not run on a GLES 2-only device (an API 34 emulator will not boot without ES 3), and there is no ANGLE for Android, so no Vulkan rung. |
| M11 | Linux / Batocera + libvlc | Two apps share one `libscreenkit.so`; A/V stays in sync across a seek (VLC's clock) |
| M12 | OTA updates | Signed manifests, runtimeVersion gating, rollback |

**M1 and M2 run before M0.** They are the only items that can invalidate the architecture.

---

## 13. Risks

1. **ANGLE on tvOS** — *largely retired.* The Metal backend is verified rendering on the tvOS
   simulator, and tvOS is a supported Chromium GN target (`target_platform="tvos"` in
   `build/config/apple/mobile_config.gni`, mapped to the AppleTV SDKs in `ios_sdk.gni`; it asserts
   `use_blink=true`, which a standalone ANGLE build must force). What remains is real Apple TV
   hardware and a source build, since the spike ran retagged iOS binaries. `GraphicsBackend` keeps
   the fallback cheap regardless.
2. **The DOM layer is the real product surface**, and it is where third-party web libraries will break
   unpredictably at import time. Composing from WHATWG packages buys correctness cheaply but not
   completeness; expect a long tail. `apps/test-suite` is the defence.
3. **jsdom's building blocks were not designed to be used without jsdom.** Some carry implicit
   assumptions about its internals; the M2 spike is what proves the composition holds together.
4. **`<iframe>` sets an expectation of isolation we do not provide.** Web developers assume a crash and
   a memory boundary; we have neither. A genuine mismatch between the borrowed API and threads-only.
   M9 shipped the API, so this is now live rather than prospective: `Instance.h`, `Instance.cpp`'s
   header comment and `runtime/js/README.md` each say it where the API is described, and `sandbox`
   is documented as gating capabilities only.
5. **Dev and release run different module semantics** — per-module `ModuleRunner` versus one
   flattened `.hbc`. Code can work in `vite dev` and fail at package time; a dynamic `import()` is
   the obvious case. Only a release build in CI catches it, and React Native has the identical
   Metro-vs-`.hbc` divergence to prove it bites.
6. **Video on Linux rests on the image's VLC 3** — A/V sync, seek, buffering and subtitles are
   VLC's, which is why it was chosen over a player of our own, but so are its limits (no CENC keys,
   no variant list, no cue text), and hardware decoding is a VLC module of ours built against the
   image's exact VLC and FFmpeg: an image update that moves either breaks it.
7. **Batocera may not ship SDL3** — the bundled-fallback path is load-bearing and doubles the Linux
   test matrix.
8. **Shared `libscreenkit.so` means version skew is a real failure mode.**
9. **Hermes on Linux breaks the "never build" rule** — CI must assert bytecode-version parity.
10. **tvOS memory ceiling** — retained `Paused` instances plus their last-frame textures will hit
   Apple's per-app limit; LRU escalation was to be real by M9 and is **not** (§5.1: `Suspended` and
   the memory-pressure escalation are deferred for want of a per-platform pressure signal). Until it
   is, a launcher that embeds many games must terminate the ones it is not showing.
11. **Apple review of downloaded bytecode** — why the dev client stays off the App Store.
12. **No platform accessibility** — a WebGL-only UI gets no VoiceOver or TalkBack for free. A dedicated
    workstream if store requirements apply.

---

## 14. Verification

1. **Toolchain audit** — the Android SDK, NDK r27d and an arm64 Android TV emulator are verified on this machine
   (`tools/android/android.sh sdk`, M10); the build machine's `ffmpeg` generates the media test fixtures (§15).
   `depot_tools` is no longer needed while ANGLE is consumed prebuilt.
2. **M1 spike** — *simulator half done* (`poc/angle-tvos/`): `eglInitialize` succeeds and
   `GL_RENDERER` reports `ANGLE Metal Renderer: Apple tvOS simulator GPU`, requested through an
   explicit `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` so a silent fallback cannot pass. Still to do:
   build ANGLE from source with `target_platform="tvos"`, run on real Apple TV hardware, and measure
   added binary size.
3. **M2 spike** — assemble the DOM layer from the WHATWG packages, compile to `.hbc`, measure bytecode
   size and cold start on Fire TV against the stated budget.
4. **Cold-clone test (M0)** — on a machine with no caches, clone and run; any third-party compilation
   is a failure. Make it a CI job, since it regresses silently the moment someone adds a dependency.
5. **Hermes parity (M3)** — the `.hbc` from the npm `hermesc` must load in the Maven runtime on Apple
   and Android, and the Linux self-built runtime must report the same bytecode version.
6. **Unmodified-Lightning test (M6)** — install Lightning 3 from npm with zero patches and render text.
   "Unmodified" means the app's source and its npm packages: nothing in `node_modules` is edited, and
   no `patch-package`. ScreenKit's own build transforms in `@screenkit/vite-plugin` are part of the
   platform and are allowed, under three rules: each is named and documented with the runtime property
   that requires it (no Canvas2D; Hermes' NaN canonicalisation), each fails the build when the code it
   rewrites changes shape or its module is never seen, and a new one is a design decision recorded
   here, not a workaround. Any other patch required is a bug in the DOM layer, tracked as such.
7. **Lifecycle proof (M9)** — *done for the callbacks, partly for the memory.* `iframe-lifecycle`
   compares the rAF and timer counters a frozen instance reports inside its own `pause` and `resume`
   handlers across sixty ticked frames and a third of a second of wall clock: the two numbers are
   equal, so a regression is a failed row rather than a judgement call. `iframe-remove` runs the
   launch/remove loop ten times and asserts that no instance survives it and that the launcher can go
   idle again, with the GL freed on the thread that owned it (a warning if not) and the whole row
   clean under ASan. What is still missing is the RSS measurement itself — flat memory across the ten,
   rather than "nothing is still registered" — which wants a platform RSS reading the suite does not
   have.
8. **Release-path check (M7 onward)** — run `screenkit bundle` in CI on every change, not only at
   ship time, so dev-only constructs fail immediately instead of at release.
9. **Backend fallback proof (M10)** — force each rung of the Android chain via config override,
   including a GLES2-only run. Not done: M10 has only the ES 3 → ES 2 rungs, without an override, and
   the ES 2 one needs a GLES 2-only device.
10. **Linux sharing proof (M11)** — install two apps on one Batocera image, confirm `ldd`/`lsof` shows a
   single shared `libscreenkit.so`, then repeat on an image *without* system SDL3.
11. **A/V sync proof (M11)** — measure drift over long playback and across seeks on real hardware (VLC's clock on Linux).

---

## 15. Environment status

**Verified on this machine:** Xcode 26.6, tvOS 26.5 SDK, CMake 4.2.1, Ninja 1.13.2, Homebrew, arm64,
Node v25.9.0, npm 11.12.1, yarn 1.22.22, turbo 2.11.2, gh 2.83.2 (authenticated). The JS half is
an npm workspace driven by Turborepo (`package.json` `workspaces` + `turbo.json`); pnpm is not
installed and the earlier plan's `pnpm-workspace.yaml` was not adopted.

**tvOS 26.5 simulator runtime: installed** (3.76 GB via `xcodebuild -downloadPlatform tvOS`) and
verified — both the SDL3 PoC and the ANGLE spike run on Apple TV 4K at 3840×2160.

**Disk is the live constraint:** this volume sits at ~98% used with under 10 GB free, which is why
ANGLE is consumed prebuilt rather than built from source.

**Android (M10):** Android SDK (cmdline-tools 23, platform 35, build-tools 35), NDK r27d
(`27.3.13750724`), emulator 37.1 with `system-images;android-34;android-tv;arm64-v8a` and the
`screenkit-tv` AVD, JDK 23, and Gradle 8.14.3 through the project's wrapper -- all installed by
`tools/android/android.sh sdk` / `avd` and verified by building and running the APK.

**Unverified — confirm before M0:** `depot_tools` (required to build ANGLE); the plan assumes it
exists, and it has not been checked. `ffmpeg` is verified on this machine (8.0, Homebrew): the media
test fixtures are generated with it (`runtime/tests/net/media.mjs`). No FFmpeg is linked on any
platform except as the image's own on Linux, beneath VLC (§10.4).
