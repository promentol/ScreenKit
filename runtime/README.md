# runtime — the native shell

One `jsi::Runtime` owned by one thread, an SDL3 event loop driving timers and
`requestAnimationFrame`, bytecode loading that refuses bad input before the VM
sees it, the raw GLES entry points a bundle draws with, `.skpkg` packages
gated on their manifest before anything in them runs, and networking on the
OS's own stack. No compositor and no media — each of those arrives with its own
spec.

```
runtime/
├── core/include/screenkit/   Runtime.h · Log.h · Input.h · Viewport.h    the embedder API
├── core/src/hermes/          HermesHost · BytecodeLoader · Runtime impl
├── core/src/loop/            EventLoop · TimerRegistry · WorkQueue
├── core/src/bindings/        Console · Timers · HostIO · Net · ImageDecode · Text · Gamepads · WebGL (from gl.def)
├── core/src/net/             NetService: NSURLSession (Apple) · OkHttp over JNI (Android) · cpp-httplib (Linux)
├── core/src/gfx/             GlSurface · FrameStats · ANGLE stubs
├── core/src/input/           InputRouter: SDL keyboard/remote/gamepad -> DOM keys · GamepadRegistry -> Gamepad API
├── core/src/viewport/        ViewportEvents: SDL window size -> DOM resize
├── core/src/jsi/             SharedObject · SharedRef · EventEmitter · NativeModule · LazyObject
├── js/                       dom-shim.js — the prelude, compiled to .hbc
├── apple/                    screenkit-host for macOS and tvOS
├── android/                  the Android TV / Fire TV APK: Gradle project, Java activity, jni/HostMain.cpp
├── linux/                    screenkit-host for Linux (a Raspberry Pi on Batocera)
├── third_party/gl/           expo-gl, vendored — generated, never hand-edited
├── tests/                    one case per I/O-matrix row; net/ is the fixture server; leaks-row.sh
├── CMakePresets.json         macos · macos-asan · tvos-simulator
└── cmake/                    prebuilt Hermes · SDL3 · ANGLE lookup; the Linux and Android system libraries
```

`third_party/gl/` is a tracked port of `expo/expo`'s `packages/expo-gl/common/`,
written by `tools/vendor/expo-gl.sh` and read about in its own `VENDOR.md`.
Nothing consumes it yet: it builds as `screenkit-gl-vendored` so a bad
re-import is a build failure, and `core/src/bindings/gl.def` is still what JS
reaches. Re-import with `tools/vendor/expo-gl.sh && tools/vendor/verify.sh`;
never edit the tree by hand, and the script will not let you.

`js/dom-shim.js` is the DOM shim prelude: `document` with a real element tree,
selectors, events and style, and a canvas whose `getContext('webgl')` hands out
the `gl` global the graphics bootstrap already installed -- and a real GL context
and compositor layer of its own for every canvas after that one. It is compiled to `dom-shim.hbc` by the pinned `hermesc` and
evaluated by the host **after `startGraphics` and before the app bundle** — a
failure there is fatal. Its scope is measured from a runtime trace of an
unmodified Lightning 3 bundle rather than taken from the DOM spec; the rule and
the evidence table live in `js/README.md`.

## Build and run

```sh
runtime/scripts/build-macos.sh
runtime/build/macos/screenkit-host runtime/build/macos/fixtures/hello.hbc
ctest --test-dir runtime/build/macos --output-on-failure

# With a window and a GL context -- the spinning triangle, drawn from JS:
runtime/build/macos/screenkit-host --window runtime/build/macos/fixtures/triangle.hbc

# The same context, reached the way the web reaches it -- through
# document.createElement('canvas').getContext('webgl'):
runtime/build/macos/screenkit-host --window runtime/build/macos/fixtures/dom-canvas.hbc

# Save one frame as a PNG (read back from GL before it is presented; no
# screen-recording permission or simulator needed). Waits 2 s by default:
SCREENKIT_CAPTURE=frame.png SCREENKIT_CAPTURE_DELAY_MS=3000 \
  runtime/build/macos/screenkit-host --window examples/lightning3-blits/app.skpkg

# The window as the compositor shows it -- the canvas and a <video>'s plane
# beneath it, which the GL frame does not contain. A process may capture its
# own window without the Screen Recording permission. Waits 5 s by default:
SCREENKIT_WINDOW_CAPTURE=window.png SCREENKIT_WINDOW_CAPTURE_DELAY_MS=12000 \
  runtime/build/macos/screenkit-host --window examples/blits-example-app/app.skpkg

# A .skpkg package (`screenkit bundle` output), headless or windowed:
runtime/build/macos/screenkit-host runtime/build/macos/fixtures/packages/hello.skpkg
runtime/build/macos/screenkit-host --window examples/lightning3-blits/app.skpkg
```

A package is a directory. The host reads its `manifest.json` with the runtime's
own `JSON.parse` and refuses it (exit 65) unless `hermesBytecodeVersion` is this
engine's and `runtimeVersion` is no newer than `VERSION`, the file `screenkit
bundle` also reads, and unless its `entry` is a file inside the package. Only
then does anything in it run, with the package directory as the asset root.
If its entry module then rejects, the packed script reports it through
`__screenkit.reportFailure` and the host acts at its next loop iteration:
headless and `--window` exit 65; on tvOS the app ends with `SDL_APP_FAILURE`,
which SDL's UIKit main turns into process status 1.
`SCREENKIT_APP_PKG=<app.skpkg> runtime/scripts/build-tvos-simulator.sh` embeds
one in the tvOS app, which runs it in place of the fixtures.

`--window` is what loads the prelude. The headless `screenkit-host <bundle>`
path has no GL context, so it has no `document` either: a canvas that cannot be
backed would be a lie.

```sh
runtime/scripts/build-tvos-simulator.sh
runtime/scripts/run-tvos-simulator.sh    # asserts the log, saves a screenshot;
                                         # with an embedded app.skpkg, waits for its package line
```

Or without the wrappers — configure fetches Hermes into
`~/.screenkit/prebuilts/` on a cold cache, so a fresh clone needs no bootstrap
step:

```sh
cd runtime && cmake --workflow --preset macos      # configure, build, test
```

`runtime/CMakePresets.json` holds every build tree's settings -- **Ninja, never the Xcode generator**
(`Architecture.md` §11), one tree per platform under `runtime/build/<preset>` -- and the build
scripts use it rather than spelling the flags out. Presets: `macos`, `macos-asan` (ASan + UBSan,
Debug) and `tvos-simulator` (build only; `run-tvos-simulator.sh` runs it).

Sanitizers:

```sh
cd runtime && cmake --workflow --preset macos-asan
```

## The three rules

**One thread owns the runtime.** React Native (`RuntimeExecutor` over
`MessageQueueThread::runOnQueue`) and Expo (`BridgelessJSCallInvoker`) converged
independently on one executor owning the runtime, and this adopts the same shape
while there is still only one instance — retrofitting thread discipline is far
harder than starting with it. No mutex guards the runtime and no `jsi::Runtime&`
crosses a thread boundary.

**There is no `invokeSync`.** A synchronous native→JS call is an error, not a
missing feature. Expo's `invokeSync` throws outright; `JsExecutor` answers the
same way by not declaring one.

**Work that races teardown is dropped.** `HermesExecutor` holds the host weakly
and re-locks it inside the queued callback, so a callback queued against a
runtime that has gone loses the callback instead of touching freed memory. The
`teardown-race` test drives the two reachable drop points under ASan: `post()`
refusing once the host is stopping, and `invokeAsync` finding the weak reference
already dead. The third — the weak re-lock *inside* the queued lambda — is
belt-and-braces and is not reachable today, because `threadMain` stops
dispatching before the host can be destroyed; it stays because the loop's
ownership rules are the kind of thing a later change quietly breaks.

## Validate before evaluating

`evaluateJavaScript` takes a raw HBC buffer happily, which makes skipping the
check tempting. A mismatched or truncated `.hbc` then faults *inside* the VM
instead of raising anything catchable. So `BytecodeLoader` runs the version
check, `hermesBytecodeSanityCheck` and `isHermesBytecode` through
`IHermesRootAPI` before `prepareJavaScript` ever sees the buffer — and the
version error names both sides:

```
Hermes bytecode version mismatch in "app.hbc": bundle is version 100,
this runtime accepts version 99 -- recompile with the hermesc pinned in
tools/prebuilts/manifest.json
```

Note that the static `HermesRuntime::isHermesBytecode` no longer exists;
everything goes through `makeHermesRootAPI()` → `jsi::castInterface`.

## Hermes is prebuilt, never compiled

The pin lives in `tools/prebuilts/manifest.json` and nowhere else; CMake reads it
with `string(JSON …)` so the build and the manifest cannot drift. `hermesvm`
exports the JSI implementation itself, so this links that one framework and never
vendors ReactCommon or compiles `jsi.cpp`. See `tools/prebuilts/README.md`.

Fixtures are derived, not committed: a `.hbc` only means anything next to the
Hermes it was compiled by, so `tests/make-fixtures.mjs` builds them into the
build tree with the pinned `hermesc` — including the two deliberately broken ones
that prove the loader refuses them.

## The engine: Hermes, or SpiderMonkey on Linux

Everything above `core/src/engine/Engine.h` is JSI and does not know which engine
runs it. `SCREENKIT_ENGINE` picks one when the runtime is built:

- `hermes` (the default, every platform): the prebuilt above; packages run `app.hbc`.
- `spidermonkey` (Linux only): JSI over Firefox's engine, JIT included
  (`core/src/spidermonkey/`). No Hermes is linked; JSI's own `jsi.cpp` is vendored
  at the Hermes commit the prebuilts use (`third_party/jsi`, `tools/vendor/jsi.sh`),
  so both engines implement the same JSI. Packages run their source or a stencil
  precompiled for the device's engine build (`engines.spidermonkey` in the
  manifest; `screenkit bundle --spidermonkey`). The engine, the compiler and the
  reasons are in `tools/spidermonkey/README.md`; JSI's conformance suite runs
  against it with `sh runtime/tests/jsi/run.sh`.

## Graphics: SDL owns the window, ANGLE owns the context

This is the Apple path. Linux and Android have no ANGLE, and there SDL's own EGL
context on its window is the drawable (`core/src/gfx/GlSurfaceSdl.cpp`, and
"Targets" below).

SDL cannot create the GL context here, and the reasons are worth recording
because `SDL_GL_CreateContext` is the obvious first thing to try. SDL's UIKit
video backend has no EGL implementation at all — `src/video/uikit/` contains
zero `SDL_EGL` references — and `SDL_EGL` reaches EGL by `SDL_LoadObject`ing
`libEGL.dylib`, while our ANGLE is a static archive with no dylib to open. Both
are independently fatal.

The seam is Metal instead. `SDL_Metal_CreateView(window)` returns a view whose
layer is a `CAMetalLayer`, and that pointer is the `EGLNativeWindowType`
`eglCreateWindowSurface` wants. So SDL keeps the window, the lifecycle and the
frame tick; `gfx::GlSurface` creates the EGL display, config, context and
surface directly, asking for `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` by name so a
fallback to another backend is a failure rather than a silent pass. ES 3.0
exactly: 3.1 and 3.2 fail with `EGL_BAD_MATCH` on ANGLE's Metal backend
(`Architecture.md` §9).

**Entry points come from a table.** `core/src/bindings/gl.def` lists them and
`WebGL.cpp` expands it. Most rows are one line — the wrapper is deduced from the
C function's own type, so the argument order cannot be got wrong — and the rest
name a hand-written adapter for the strings, out-parameters and buffer pointers
that a JS value cannot drive directly. `gl` is **not** `WebGLRenderingContext`:
there are no wrapper objects and no validation layer, and JS sees the same
integer names GL itself uses. The object model above it is JS, and a separate
spec.

Typed-array uploads are zero-copy: `gl.bufferData(target, float32Array, usage)`
reads the `ArrayBuffer` behind the view in place, honouring `byteOffset` so a
`subarray` uploads its own window rather than the whole buffer.

**One clock.** `SDL_AppIterate` → `Runtime::tickFrame` → `requestAnimationFrame`
is the only frame source. GL work belongs inside the rAF callback, and the
present hangs off `Runtime::setFrameFinishedCallback`, which runs on the JS
thread after the frame's callbacks and their microtask checkpoint. A render
thread or a second timer would give two clocks racing over one context.

`gfx::FrameStats` logs the achieved rate once a second. It is measured, never
gated: a frame-time threshold in CI would be a flaky test on a simulator, and a
flaky test gets disabled.

**ANGLE is prebuilt and has holes.** Godot's published archives link a handful
of symbols from Godot's own tree, and which ones differ per slice — the macOS
archive carries `angle::GetCurrentSystemTime`, the iOS/tvOS ones do not.
`cmake/AnglePrebuilt.cmake` runs `nm` over the archive that was actually fetched
and defines only the stubs in `gfx/angle_stubs.mm` that are genuinely missing,
because supplying them unconditionally is a duplicate-symbol error on macOS.
All of it is spike scaffolding: a source build with `target_platform="tvos"`
needs none of it.

## Networking: the platform's own HTTP client behind one seam

`fetch`, `XMLHttpRequest`, `WebSocket` and `EventSource` (the DOM shim, see
`js/README.md`) sit on `__screenkit.net`, a narrow handle API in
`core/src/bindings/Net.cpp`, over `core/src/net/`:

- **`NetService` is the seam** (`NetService.h`), and its method list is React
  Native's `Networking` module's surface. Below it there is no protocol code of
  ours at all: name resolution, connection pooling, HTTP/1.1 **and HTTP/2**,
  redirects, `Content-Encoding`, proxies, certificate verification and the
  cookie store are the platform client's.
  - **Apple: NSURLSession** (`NetServiceApple.mm`), what RN's
    `RCTHTTPRequestHandler` uses. One serial dispatch queue per runtime is the
    session's delegate queue, so every delegate callback and every command JS
    sends runs on it, in order, with no locks; the sink calls themselves run on
    a second serial queue, so the delegate never falls behind (see backpressure
    below). Cookies are an `NSHTTPCookieStorage` of that runtime's own; sockets
    are `URLSessionWebSocketTask`.
  - **Android: OkHttp** (`NetServiceAndroid.cpp` over
    `dev.screenkit.net.HttpClient`), what RN's `NetworkingModule` uses. The Java
    object is driven by handle from C++; every OkHttp callback is re-posted onto
    the runtime's serial I/O queue. Cookies are
    `android.webkit.CookieManager`, the store every WebView on the device shares.
  - **Linux: cpp-httplib** (`NetServiceLinux.cpp` over
    `third_party/httplib/`, one MIT header pinned by sha256). This is the one
    platform with no vendor client to point at, so it is the one exception to
    the rule above -- but only for the protocol: TLS is the image's own OpenSSL
    and the roots are the image's own CA bundle, found at run time, and nothing
    about either ships in this repo. cpp-httplib is *synchronous*, so each
    in-flight request gets a worker thread of its own (bounded: six per origin,
    sixteen in all) and every callback is posted back onto the runtime's serial
    I/O queue. Redirects and cookies are followed and kept here rather than
    inside the library, because the jar has to see every hop; names are
    resolved here too, on a thread whose answer a cancel can abandon, and
    bodies are decoded here, because the library cannot read raw `deflate`.
    Every blocking point has a way out another thread can take -- a socket being
    connected is shut down under the library through a duplicate of its
    descriptor -- so an abort or a teardown never waits on the network. Cookies are
    `LinuxCookieJar` -- RFC 6265, in memory, this runtime's own -- because Linux
    has no platform store to point at, and it is the one place in the tree where
    the cookie prefixes and the "a `Secure` cookie needs a secure origin" rule
    are ours to enforce. WebSocket is `ws::WebSocketClient`, two threads a
    socket; the peer's close code reaches the page through the one patch the
    vendored header carries (`tools/vendor/httplib.rules`), since upstream
    discards a Close frame's payload.
- **The three clients do not agree about everything**, and where they do not it
  is recorded rather than papered over: response-head fidelity on Apple
  (duplicate headers comma-joined, a canonical `statusText`), cookie semantics,
  and what each does with a truncated body.
  `js/README.md` lists them, and `spec-platform-http-clients.md` and
  `spec-linux-http-client.md` say why.
- **Backpressure is real on all three.** A request carries a `flowWindow`; past
  it the client is told to stop taking bytes off the connection until JS
  acknowledges what it read (`__screenkit.net.acknowledge`). OkHttp is
  pull-based, so its reader loop simply stops pulling; cpp-httplib is
  synchronous, so its reader callback simply stops returning -- in both cases the
  socket stops being drained and the server stalls. NSURLSession pushes, so its
  task is suspended -- which CFNetwork ignores while delegate callbacks are
  queued behind a slow delegate, so the delegate does nothing but count, suspend
  and hand the bytes to the delivery queue. `net-flow-control` asserts the stall
  on all three, the ASan build included. At most six requests to one origin are
  in flight on all three, as in a browser.
- **Completions are tasks.** Events reach JS through the runtime's
  `JsExecutor`, so they wait behind a paused runtime's freeze gate like a timer.
  Each request or socket keeps at most one task queued and delivers what has
  accumulated in order, with a microtask checkpoint between events: the work
  queue is SDL's event queue, which holds 65,535 events, and a paused runtime
  with a busy WebSocket must not overflow it (`net-shutdown-idle` sends 70,000
  messages into a paused runtime).
  A request or socket holds the loop non-idle from its start until its terminal
  event is delivered (`EventLoop::holdWork`), which is what keeps
  `screenkit-host` from exiting under an in-flight fetch.
- **Shutdown is synchronous.** Teardown cancels every request and socket on the
  I/O queue and waits for it, then drops the JS callbacks while the runtime
  still exists: nothing is delivered afterwards.
- **Cookies are the platform store's** everywhere there is one, and so are their
  rules. They do not
  persist across a runtime restart on Apple -- each runtime gets an
  `NSHTTPCookieStorage` of its own rather than the process-wide one, so one
  app's cookies never leak into the next runtime or into the user's own cookie
  file -- and on Android they are the device's shared WebView store, which does
  persist. `RuntimeConfig::storageDirectory` went with the jar: nothing wrote to
  it once the platform stores took over, and a field that looks load-bearing and
  is not is worse than no field. Linux has no store to point at, so it has the
  one jar left in the tree (`LinuxCookieJar.{h,cpp}`): RFC 6265 read straight,
  in memory for the runtime's life, and not persisted for the same reason --
  there is no storage directory to persist into any more.
- **Trust** is the OS's: the system store on Apple and Android, and the image's
  CA bundle on Linux (`/etc/ssl/certs/ca-certificates.crt` and the usual
  alternatives, probed once at run time; `SSL_CERT_FILE` wins).
  `RuntimeConfig::testTlsAnchors` **adds** DER anchors for the test suite's
  generated CA and never replaces the store; no host sets it and JS cannot reach
  it.

The `net-*` rows run against a Node fixture server ctest starts and stops
around them (`tests/net/server.mjs`, `FIXTURES_SETUP net-server`): HTTP, HTTPS
with a CA generated at start plus untrusted, expired and wrong-host
certificates, WS/WSS, and every body and failure shape the matrix names. It
listens on loopback only. To run a row by hand, start it first:

```sh
node runtime/tests/net/server.mjs --dir runtime/build/macos/net-fixture &
runtime/build/macos/tests/screenkit-runtime-tests net-websocket runtime/build/macos/fixtures
```

The same fixture and the same transcripts run on both devices: `android.sh test`
publishes its ports with `adb reverse`, `pi.sh test` with `ssh -R`, and in both
cases the device sees them on 127.0.0.1 so the fixture's certificates still
match and no row touches a network beyond the two loopbacks. Neither device
build is registered with CTest -- CTest configures for the host -- so the row
list lives in each script and is cross-checked against
`tests/CMakeLists.txt` on every run, or a row added later would quietly run on
macOS only.

The tvOS app needs nothing extra for this -- NSURLSession is Foundation's --
and it carries `SDL3.framework` beside `hermesvm.framework` in `Frameworks/`,
signed, with `@executable_path/Frameworks` as its only rpath.

**ASan note.** The sanitizer suite runs with
`ASAN_OPTIONS=detect_container_overflow=0`. The prebuilt ANGLE is not
instrumented, and its calls to libc++ template instantiations it shares with our
instrumented objects resolve to the instrumented copies, which report a
container-overflow inside `eglInitialize` on every GL row -- ASan's documented
false positive for mixed instrumentation. Heap overflows, use-after-free and
UBSan still halt the run.

## Media: the platform's own player behind one seam

`<video>` (the DOM shim, see `js/README.md` "Video") and the Shaka-shaped player
above it (`packages/@screenkit/shaka`) sit on `__screenkit.media`, a narrow handle
API in `core/src/bindings/Media.cpp`, over `core/src/media/`. It is networking's
shape, deliberately:

- **`MediaPlayer` is the seam** (`MediaPlayer.h`): one player per element --
  `load`, `play`, `pause`, `seek`, `setRate`, `setVolume`/`setMuted`,
  `setPlane`, variant/ABR/audio/text selection, `provideLicence`, `unload`,
  `shutdown` -- with `MediaSink` events (metadata, state, time, buffered, tracks,
  variant, size, cues, licence request, stats, error with a kind). No JSI below
  it; every entry point is safe from any thread and none waits for the main
  thread, which may be blocked joining the JS thread at teardown. The platform's
  player owns the manifest, buffering, ABR, decoding, DRM and presentation, as
  in react-native-video; nothing here parses HLS or DASH.
  - **Apple: AVPlayer** (`MediaPlayerApple.mm`). One serial queue per player
    holds every AVPlayer call and every piece of state; the state machine is a
    poll of the item on it, not KVO, so the headless rows (no main run loop)
    behave like the host. The plane is an `AVPlayerLayer` in a view of its own
    inserted **beneath the SDL metal view**; the metal layer is made non-opaque
    only while a plane is visible, so the canvas-only path costs what it did.
    Variants come from `AVURLAsset.variants`, pinned and restricted with
    `preferredPeakBitRate`/`preferredMaximumResolution`; audio and text are
    `AVMediaSelectionGroup`s, cues an `AVPlayerItemLegibleOutput` that
    suppresses AVPlayer's own rendering. AVPlayer has no public decoded-frame
    counter, so an `AVPlayerItemVideoOutput` rides along and the poll counts the
    frames it vends (which also makes AVPlayer decode with no layer attached --
    the headless rows); dropped frames are the access log's. FairPlay is an
    `AVContentKeySession`: the SPC goes out as a licence request, the CKC comes
    back through `provideLicence`.
  - **Android: Media3 ExoPlayer 1.9.4** (`MediaPlayerAndroid.cpp` over
    `dev.screenkit.media.VideoPlayer`, resolved and registered in `JNI_OnLoad`),
    on a looper of its own (`screenkit.media`), never the main thread; every
    public method only posts. The plane is a `SurfaceView` beneath SDL's in
    SDL's own layout (`VideoLayer`): SDL's surface is a media overlay with an
    RGBA config, created opaque to the compositor as it always was, and the
    first player a page makes switches it to `TRANSLUCENT` -- once, since a
    `SurfaceView`'s opacity cannot change in place -- so a page with no video is
    composited exactly as before. Tracks and selection are `Tracks` and
    `TrackSelectionParameters`; cues are `onCues`; frames are the decoder
    counters. Widevine and ClearKey go through a `DefaultDrmSessionManager`
    whose `MediaDrmCallback` blocks ExoPlayer's DRM thread until JS answers the
    licence request (`drm.clearKeys` is a local ClearKey licence, no network).
    An encrypted load passes over the emulator's host-backed decoders
    (`c2.goldfish.*`), which take decrypted input and never produce a frame; the
    emulator's software decoders play it, and no device has a goldfish codec.
    Media3 is pinned in `gradle/verification-metadata.xml` and kept by
    `app/proguard-rules.pro`.
  - **Linux: libvlc** (`MediaPlayerLinux.cpp`, `media/linux/`), VLC 3.0.21 as
    the image ships it, linked dynamically. One libvlc instance is shared by the
    players alive in the process and a `libvlc_media_player_t` belongs to each
    element; a worker thread per player owns every libvlc call and polls what
    libvlc only answers when asked (time, tracks, stats). Pictures come out of
    libvlc's memory output (`libvlc_video_set_callbacks`) into linear dma-bufs
    committed to a desynchronised Wayland subsurface placed below the window
    (`linux/WaylandVideo.cpp`); with no Wayland a load fails 3016. VLC opens
    each stream paused and the player plays it once VLC has prerolled; what
    libvlc says only in its log -- an HTTP status, what ScreenKit's manifest
    probe found -- is read from its log callback. The ScreenKit VLC plugin
    (`linux/vlc-plugin/`, `vlc-plugins/libscreenkit_plugin.so` beside the
    executable, put on `VLC_PLUGIN_PATH` before `libvlc_new`) supplies the
    hardware decoder, the `ts` demuxer VLC's HLS support needs, and the manifest
    probe (`tools/batocera/README.md`, "Video").
  - **Unavailable** (`MediaPlayerUnavailable.cpp`) anywhere else: every load
    fails `Unavailable`, and `canPlayType` answers `""` (`media-unavailable`).
- **`__screenkit.media`** turns the sink's calls into event-loop tasks through
  the shared `EventChannel` (`bindings/EventChannel.h`, also networking's): one
  channel per player, at most one task queued, a microtask checkpoint between
  events, behind the freeze gate. Each load has a serial and the binding drops
  events of an earlier one. A player holds the loop non-idle from `create` to its
  release; it is released by `destroy`, when the object its events go to is
  collected (a `NativeState` on it), and at shutdown.
- **Lifecycle.** `HermesHost` installs the binding after `__screenkit.net` and
  shuts it down first at teardown -- synchronously, so no player is still
  producing events or waiting on a licence when the network goes. A
  `MediaRegistry` made with the host holds the same players for other threads:
  pausing the runtime pauses them directly (a task would wait behind the gate
  just closed) and resuming plays those the page had playing.
- **Where video goes.** The host registers its window as the `VideoHost`
  (`media::setVideoHost`: the SDL window, on Apple the metal view, and the fixed
  drawable size) once the drawable exists, and clears it before the window goes
  (`host/Host.cpp`, `apple/HostMain.mm`, `android/jni/HostMain.cpp`). A plane
  arrives in drawable pixels and `media::planeInWindow` maps it the way a fixed
  drawable is scaled into the window. With no host -- the headless rows -- a
  player still plays and its plane goes nowhere.

**Tests.** The `media-*` rows play media the fixture server generates at start
with this machine's `ffmpeg` (`tests/net/media.mjs`) and a fake licence server,
through the element and through `@screenkit/shaka` (compiled for the rows into
`shaka.hbc` beside the prelude, `tests/ShakaScript.cmake`), with one transcript
per platform where the players genuinely differ (`js/README.md`, "Where the
platforms differ"). `media-element-model` drives the element against a scripted
player, and `media-licence-exchange` the licence flow against a scripted key
system, so both run everywhere. `leaks-media-shutdown` runs a shutdown
mid-playback under leaks(1). The same rows run on the Android emulator
(`android.sh test`) and the Pi (`pi.sh test`). The JS player has a suite of its
own, for what the rows cannot see -- the configuration merge, Shaka's error codes
and the networking engine's retries: `cd packages/@screenkit/shaka && npm test`,
beside `cd packages/@screenkit/vite-plugin && npm test` for the `shaka-player`
resolution.

## Instances: one `<iframe>`, one runtime

**One iframe = one Instance** = one `Runtime` on its own thread, with its own GL
context in the host's share group, drawing into an FBO-backed texture the host
composites at the element's CSS rect (`Architecture.md` §5). Everything it is
made of already existed separately — a runtime, a shared context, the freeze gate
that is what `Paused` means, and a layer rect — and M9 is where they became one
object with a lifecycle.

```
runtime/core/
├── include/screenkit/Instance.h   what a host reaches: the registry, the focus
│                                  target, SandboxPolicy, terminateInstances
├── src/instance/                  Instance.cpp (the runtime, the bootstrap
│                                  thread, terminate) · StructuredClone.cpp
├── src/compositor/Compositor.cpp  the layer list, the composite pass, fences
├── src/bindings/Instance.cpp      __screenkit.instances, shaped like Media.cpp
└── src/bundle/Package.cpp         the .skpkg gate -- shared with the host, so an
                                   instance's package is refused the same way
```

A host's whole integration is four calls:

```cpp
config.instances = makeInstanceRegistry();
setInstanceEnvironment(*config.instances, domShimPath);
input->setFocusTarget(focusTarget(*config.instances));   // and the viewport's
... each frame:   for (auto& i : liveInstances(*registry)) i->tickFrame(ms);
... before it returns:     terminateInstances(*registry);
```

**Where the composite runs.** `GlSurface`'s contract is that every GL call
happens on the thread that owns the context, and the main thread only pumps SDL
— so the compositor is not a main-thread pass. It runs at the *host* runtime's
frame end, inside `GlSurface::swap()`, in a second context that shares only
textures, so the app's own GL state (its program, its buffers, its blend mode) is
never disturbed. The child's `swap()` is not a present at all: it finishes into
its own texture and signals.

**Ordering between the producer and the compositor** is a fence where the
platform has one and the producer's flush where it does not — `glFenceSync` /
`glWaitSync`, resolved at run time through `eglGetProcAddress` (or
`SDL_GL_GetProcAddress`) rather than named at link time, because a Raspberry Pi
3's VideoCore IV is ES 2.0 and the Android NDK's `libGLESv2.so` exports the ES 2
set alone. The wait is server-side; `glFinish` is never in the frame path.

**Why the host's frame moves into a texture.** After a swap the window's back
buffer is undefined, so a frame in which only a child painted would have nothing
of the page left to put back. `enableCompositing()` therefore turns the host's
own frame into an offscreen texture the first time an instance is made — and
*only* then. A page with no iframe compiles no program, makes no second context
and takes exactly the present path it took before: `presentFrame` asks
`layersDirty()`, which is false for a surface with no layers.

**Why an instance boots on a thread of its own.** The package gate and
`evaluateBundle` both block their caller, and the caller here is the launcher's
JS thread. So `createInstance` makes the shared context synchronously — it has
to, because SDL shares only with the context current on the calling thread — and
hands the rest to a short-lived bootstrap thread that gates the package, adopts
the context, evaluates the prelude, confines the asset root and runs the entry,
then reports `load` or `error` back through the channel.

**`Paused` stops the page, not the window.** A launcher that freezes itself to
give an `<iframe>` the remote still owns the compositor, so a freeze that also
stopped the present would run the game and show nothing. `EventLoop::
setPresentWhilePaused` is what a compositing runtime turns on: while frozen it
serves the frame-finished hook and nothing else -- no queued task, no microtask
checkpoint, no rAF callback. It is off by default and set only while a page has
layers, so an instance's own pause burns no wakeup, which is what `Paused`
promises.

**Two process-wide things became per-runtime** for this, both of them latent
bugs the moment a second runtime existed: the `EventEmitter` log tag, which was
an unsynchronised cross-thread write, and the asset root, which would have let
either app read the other's package. Both are now keyed by the runtime, as the
vendored GL registry keys its contexts, and dropped at that runtime's teardown.

**This is not an isolation boundary** (`Architecture.md` §5.2). JS is isolated;
memory is not. A native crash or an OOM in any instance takes every instance with
it including the launcher, and `sandbox` gates capabilities only.

## Input: SDL events in, DOM key events out

One path on every platform, because SDL already normalises the hardware.
`InputRouter` (`core/include/screenkit/Input.h`) maps SDL events to a DOM
`KeyboardEvent` shape -- `key`, `code` and the legacy `keyCode` TV frameworks
still key on -- and the DOM shim dispatches it at `document.activeElement` (`<body>`), capturing
down from `window` and bubbling back up through `<html>`, `document` and `window`.

| Source | Arrives as | Becomes |
|---|---|---|
| Siri Remote (tvOS) | keyboard events, via `SDL_HINT_TV_REMOTE_AS_JOYSTICK=0` | arrows, select → `Enter`, Menu → back, play/pause → `Pause` |
| Android TV remote | keyboard events | arrows, D-pad centre → `Enter`, Back (`AC_BACK`) → back, media keys |
| Keyboard (macOS, Linux) | keyboard events | the key; `Backspace` (keyCode 8) is back to Blits |
| Gamepad (Batocera, and common on TV boxes) | `SDL_EVENT_GAMEPAD_*` | D-pad and left stick → arrows, A → `Enter`, B / Back → back, Start → `MediaPlayPause` -- until the app claims the gamepads (below) |

Back is `{key: 'GoBack', code: 'BrowserBack', keyCode: 8}` whichever button
sent it. SDL does not repeat gamepad input, so held buttons repeat natively
(400 ms, then every 80 ms); the stick presses past half travel and releases
under a quarter, so it cannot chatter at the threshold.

**It is a browser event-loop task.** Each event is one task, FIFO with timers.
Because it is a user-agent dispatch rather than a script's `dispatchEvent`, a
microtask checkpoint runs after *every listener callback*: a promise a
`document` listener resolves has settled before the `window` listener runs.
The shim returns a stepper and the router runs
`JsExecutor::performMicrotaskCheckpoint` between steps, since a checkpoint
needs an empty JS stack. A paused runtime gets no input: presses and repeats
are dropped rather than queued, and a press already queued when the pause
lands drops itself (`JsExecutor::pauseEpoch`), so nothing stale lands on resume.
`input-event-loop` asserts all of this.

A host wires it in three calls -- `configureHints()` before `SDL_Init`,
`handleEvent` per event, `tick` per frame. `SCREENKIT_LOG_INPUT=1` logs every
key with its source (`SIMCTL_CHILD_SCREENKIT_LOG_INPUT=1` through `simctl
launch`), which is how to see what a remote actually sends.

Verified: macOS keyboard, the tvOS simulator with its hardware keyboard, and the
Android TV emulator's remote (`adb shell input keyevent`), each driving the
Blits example app (navigate, open a page, back). Not verified: the Siri Remote's
own press path, which could not be driven from automation, and a physical
Android TV or Fire TV remote. On Linux (a Raspberry Pi on Batocera) a gamepad
connects and its Guide button quits; navigating an app with it is not verified.

**A keyboard attached on tvOS** -- a Bluetooth keyboard, or the simulator's *I/O ›
Keyboard › Connect Hardware Keyboard* -- makes SDL 3.4 ignore every remote press
(`SDL_uikitview.m` forwards presses only `if (!SDL_HasKeyboard())`). The tvOS
host wraps those handlers (`apple/RemotePresses.mm`): while a keyboard is
attached, a press that is not a keyboard key is pushed as the key event SDL
would have sent, so the remote's arrows, Select and Menu keep working. Built and
running on the simulator; the forwarded presses themselves are unverified, for
the same reason as the Siri Remote above.

### Gamepads: the W3C Gamepad API

The same gamepads are also `navigator.getGamepads()`, `Gamepad`, `GamepadButton`,
`gamepadconnected` / `gamepaddisconnected` at `window` (with `ongamepad*` handlers)
and `GamepadHapticActuator`, so a web game reads sticks, triggers and face buttons
as it would in Chrome -- Phaser's `input: { gamepad: true }` included. Every pad SDL
opens as a gamepad uses the `standard` mapping:

| `buttons[i]` | | `axes[i]` | |
|---|---|---|---|
| 0-3 | South, East, West, North | 0, 1 | left stick X, Y |
| 4, 5 | left, right shoulder | 2, 3 | right stick X, Y |
| 6, 7 | left, right trigger, analog 0..1 | | each -1..1, up and left negative |
| 8, 9 | Back, Start | | |
| 10, 11 | left, right stick click | | |
| 12-15 | D-pad up, down, left, right | | |
| 16 | Guide | | |

`pressed` (and `touched`) is `value > 0.1`. `id` is Chrome's
`<name> (STANDARD GAMEPAD Vendor: vvvv Product: pppp)`; `index` is the lowest free
slot when the pad connects and stays put while it is connected; the array
`getGamepads()` returns is at least four long, `null` where no pad is. No
touchpads, motion sensors, `GamepadPose` or buttons past 16.

**Threads.** Only the main thread reads SDL. `GamepadRegistry`
(`core/src/input/Gamepads.h`, one per process, since gamepads are) holds the
snapshot: InputRouter connects a pad when SDL adds it, disconnects it when SDL
removes it, and `tick` polls every pad every host loop iteration, before
`tickFrame`, so a `getGamepads()` inside `requestAnimationFrame` sees the state of
that iteration -- a tap between two polls can be missed, as in a browser. The JS thread copies the
snapshot under its lock through `__screenkit.gamepads` (`bindings/Gamepads.h`)
into a buffer it reuses, and the shim refreshes one `Gamepad` object per
connection in place: a poll that finds nothing changed allocates no `Gamepad` or
`GamepadButton` (`getGamepads()` still returns a new array).
`timestamp` is the wall-clock time of the pad's last change moved onto
`performance.now()`'s clock, so it moves only when the state does. A pad that
leaves keeps its object, `connected` false and no longer refreshed.
`connected` is writable, because Phaser assigns it.

**Connection events are tasks** like a key's, but a pause drops neither: the
freeze gate holds them, and on resume the page hears of every pad that came or
went, against a snapshot that is already current. Pads present at launch fire
`gamepadconnected` once each, from the first loop iteration.

**The claim.** Until the app asks, a gamepad navigates the UI with keys, as the
table above says. The app's first `navigator.getGamepads()` claims the gamepads
for the rest of the process: InputRouter synthesizes no more keys from gamepad
buttons or sticks, and keys held at that moment get their `keyup`. Keyboards,
remotes and Linux's Guide-to-quit (which the host checks before the router) are
unaffected, and an app that never calls `getGamepads()` keeps key navigation. A
connection event does not claim.

**Rumble.** `vibrationActuator` is a `GamepadHapticActuator` with
`effects: ['dual-rumble']`, or `null` when SDL reports no rumble for the pad.
`playEffect('dual-rumble', {duration, startDelay, strongMagnitude,
weakMagnitude})` clamps magnitudes to 0..1 and times to 5000 ms, then calls
`SDL_RumbleGamepad` -- strong on the low-frequency motor, weak on the high -- from
the JS thread under `SDL_LockJoysticks`, finding the pad by its `SDL_JoystickID`:
the one SDL call JS reaches. It resolves `'complete'` when the effect ends and
`'preempted'` when another `playEffect`, a `reset()` or the pad leaving comes first;
another effect type rejects with `NotSupportedError`. `reset()` stops the motors
and resolves `'complete'`.

`gamepad-*` rows drive a virtual SDL gamepad (`SDL_AttachVirtualJoystick`) through
a real InputRouter into the prelude, one per matrix line of
`spec-web-gamepad-api-sdl3.md`, plus Phaser's gamepad plugin reduced to the calls it
makes; they skip where SDL cannot attach one. Not verified: a physical controller
on the Pi or an Apple TV.

Window size changes reach the page the same way: `ViewportEvents`
(`core/include/screenkit/Viewport.h`) turns `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`
into one coalesced task that fires `resize` at `window`. Both hosts wire it in.

## Targets

macOS and the tvOS simulator. tvOS device should work — Hermes ships a real
`tvos-arm64` slice and the build selects it — but it is unproven and needs
signing.

**Android TV and Fire TV** (`android-arm64`, `android-arm32`; verified on the
`screenkit-tv` emulator, Android TV 14 arm64): `runtime/android/` is a Gradle
project -- Java only, Groovy build scripts, AGP 8.13 on the Gradle 8.14.3 the
wrapper pins by sha256, every resolved artifact checked against
`gradle/verification-metadata.xml` -- that builds a debug APK for arm64-v8a and
armeabi-v7a (Fire TV sticks run a 32-bit userland), minSdk 24, target 35, in the
Leanback launcher, landscape and fullscreen.

```sh
sh tools/android/android.sh sdk                 # cmdline-tools 23, NDK r27d, android-tv arm64 image (API 34)
sh tools/android/android.sh avd                 # create and boot the screenkit-tv emulator
sh tools/android/android.sh build [app]         # the APK, with poc/<app> bundled (pixi-hello)
sh tools/android/android.sh install
sh tools/android/android.sh run bundled         # the APK's own app; run <app> pushes poc/<app> and runs that
sh tools/android/android.sh shot phaser-hello   # frame capture + screencap into tools/android/out/
sh tools/android/android.sh key DPAD_DOWN DPAD_CENTER BACK
sh tools/android/android.sh test                # the runtime's net rows on the device, over adb reverse
```

Needs a JDK (17+; 23 here), node and npm, and CMake 3.24+ with ninja on `PATH` --
`runtime/CMakeLists.txt` asks for a newer CMake than the SDK's own packages.
`android.sh` itself is macOS on Apple silicon: it installs the mac_arm64
cmdline-tools and an arm64 emulator image, and refuses to run anywhere else. The
APK is not macOS-only -- with the SDK, NDK r27d and CMake in place, `./gradlew
assembleDebug -Pscreenkit.app=examples/pixi-hello` builds it on any host (write
`runtime/android/local.properties` with `sdk.dir` and `cmake.dir` first, which is
what android.sh does for you).

- **The shell** is SDL's `SDLActivity` (`ScreenKitActivity`), loading SDL3, SDL3_ttf,
  fbjni, hermesvm and `libscreenkit.so`, whose `SDL_main` is
  `android/jni/HostMain.cpp`: the `SDL_MAIN_USE_CALLBACKS` host, like tvOS's, with
  `SDL_AppIterate` paced at the display's refresh rate.
- **Packages** are the same `.skpkg` directories every host runs. The one bundled
  in the APK's assets, and the `dom-shim.hbc` beside it, are copied to internal
  storage (`files/bundle/`) once per installed APK -- keyed on versionCode and the
  install time, built in a staging directory, synced, and renamed into place --
  because every file read in the runtime is a POSIX path. A debug launch runs a
  pushed package instead: `am start -S -n dev.screenkit.host/.ScreenKitActivity
  --es package <name>`; `android.sh push` copies one into `files/apps/` with
  `run-as`. Extras `size` (`640x480`) and `capture` / `captureDelayMs` are `--size`
  and `SCREENKIT_CAPTURE`. They are honoured only in a debuggable build, and name
  files inside the app's own storage -- `package` in `files/apps/`, `capture` in
  `files/captures/`, a name and not a path: the activity is exported, so in a
  release build the extras would be another app's choice of what ScreenKit runs.
  A package the gate refuses is logged and the activity finishes.
- **Graphics** are SDL's EGL context on its window through `GlSurfaceSdl.cpp`, as on
  Linux -- there is no ANGLE for Android. The window asks for ES 3 and falls back to
  ES 2 (the WebGL1 path) when the device has no ES 3 config or refuses the
  context. The frame is always drawn offscreen at a fixed size (the screen's, or
  `size`): Android destroys the window's drawable whenever the app leaves the
  screen, and an offscreen frame is what can be shown again at once on return.
- **Lifecycle.** When the activity pauses (or its surface goes), the Java side
  calls into the host before SDL hears of it: the runtime pauses and releases the
  drawable on the JS thread (`GlSurface::suspend`), and the UI thread waits for
  that, so SDL never destroys a surface the JS thread still has current. Not from
  SDL's own `WILL_ENTER_BACKGROUND`: SDL delivers app events inside
  `SDL_PushEvent`'s watcher lock, and the timer thread holds runtime locks while it
  pushes, so waiting there deadlocks. On return, `SDL_AppIterate` binds the new
  drawable, presents the last frame and resumes. The context, the page's GL
  objects, its timers and its state survive.
- **Back and leaving.** SDL is told to trap Back, so it reaches the page as a
  `GoBack` key and stops there: no rule the host could apply tells a page that
  handled Back from one that ignored it -- an overlay closes without
  `preventDefault()`, a page may act on keyup, and a gamepad's B maps to `GoBack`
  too. A page that wants Back at its root to leave calls `window.close()`, which
  finishes the activity (`__screenkitClose`, `js/dom-shim.js`). A finished
  activity ends its process, so the next launch starts clean.
- **Logs**: everything `screenkit::log` writes goes to logcat, tag `ScreenKit`
  (`android.sh logs`).
- **Fonts**: `/system/fonts`, read with SDL_ttf (`core/src/text/SystemFontsAndroid.cpp`):
  Roboto for `sans-serif`, Noto Serif for `serif`, Droid Sans Mono for `monospace`.
- **Hermes** is the `hermes-android` AAR, which unlike the Apple framework links a
  separate `libjsi.so` and `libfbjni.so` (`tools/prebuilts/README.md`, "Android").
  Its `Intl` is Java, through fbjni, and a class lookup from the JS thread -- a
  native thread -- cannot see the app's classes, so the host touches every `Intl`
  entry point once on SDL's Java-started main thread before the runtime starts.
- **Network**: OkHttp over JNI (`core/src/net/NetServiceAndroid.cpp` and one Java
  file, `dev/screenkit/net/HttpClient.java`) — the whole client, as React
  Native's `NetworkingModule` uses it, so `fetch`, XHR, `WebSocket`,
  `EventSource` and network images work without a line of protocol code of ours.
  OkHttp is the first Kotlin in this build: `kotlin-stdlib` and okio come with
  it, pinned by sha256 in `gradle/verification-metadata.xml`. TLS is the platform
  trust store, cookies are `android.webkit.CookieManager`, and
  `android.permission.INTERNET` is in the manifest. The classes are resolved in
  `JNI_OnLoad`, the one place the app's class loader is reachable — the same trap
  `Intl` hit above.
- **Tests**: `sh tools/android/android.sh test` runs the suite's networking rows
  on the emulator against the same node fixture ctest uses, reached over `adb
  reverse` — the 15 `net-*` rows, plus two that exist only here:
  `net-no-javavm` (the client with no VM captured: every request fails
  `unsupported`, nothing crashes) and `net-shutdown-mid-call` (teardown while a
  response streams and a socket is open: no sink call once the seam has been told
  to stop, and `net::liveJavaRefCount()` back to zero — a leaked JNI global
  reference is otherwise invisible). The rest of the suite is macOS-only (it
  drives the macOS host and ANGLE's offscreen surfaces), and `net-image` is left
  out of the device list because it uploads to a texture and this binary has no
  drawable.

**Linux** (`linux-arm64`, verified on a Raspberry Pi 3 B+ running Batocera 42):
`linux/HostMain.cpp` runs a package headless, in a window, or `--fullscreen`,
and `--size 640x480` fixes the canvas at that size: the app draws into an
offscreen framebuffer and each present scales it into the window, centred with
its aspect ratio kept (`core/src/gfx/GlSurfaceSdl.cpp`). On macOS, `--window
--size` is a window of that size.
It links the system's SDL3, SDL3_ttf, EGL, GLES and **OpenSSL**
(`cmake/LinuxSystem.cmake`) and our own Linux Hermes prebuilt
(`tools/prebuilts/README.md`), and draws through SDL's GLES context -- no ANGLE
(`core/src/gfx/GlSurfaceSdl.cpp`); on a GLES 2 GPU that is WebGL1 only. System
fonts come from `/usr/share/fonts`. Networking is cpp-httplib over that OpenSSL (above), WebSocket included.
`tools/batocera/pi.sh` builds it in Docker and installs it as Batocera ports,
and `pi.sh test` runs the `net-*` rows **on the device** -- the fixture stays on
the build machine and the Pi reaches it through an ssh reverse tunnel on the
same port numbers, which is `adb reverse` by another name. An image with no
libssl fails at configure time with a message naming it, never silently without
TLS; `pi.sh probe` says what an image carries before anything is built. The rest
of the suite is Apple-only.
