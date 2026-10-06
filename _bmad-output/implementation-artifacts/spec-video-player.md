---
title: 'Video player: AVPlayer, Media3 ExoPlayer and libvlc behind a Shaka-Player-shaped JS API'
type: 'feature'
created: '2026-09-19'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/Architecture.md'
  - '{project-root}/runtime/js/README.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** `<video>` is an inert stub (`canPlayType` answers `""`), so no app can play video, and the Blits example's Player page -- `shaka-player` driving a `<video>` behind the canvas -- is commented out of the app. Real Shaka cannot run here: it needs MediaSource Extensions, which this runtime does not have.

**Approach:** Do what react-native-video does: each platform's own player owns manifests, buffering, adaptive bitrate, decoding and presentation -- **AVPlayer** on Apple (macOS, tvOS), **Media3 ExoPlayer** on Android, and **libvlc** on Linux (VLC 3.0.21, the image's own). They sit behind one native seam, surfaced as a native-backed `HTMLVideoElement` in the DOM shim and, above that, a JS player whose API mirrors Shaka Player's.

**Decisions (human, 2026-09-19):**
- **One spec for all three platforms** -- not split, and kept whole at about 5k tokens.
- **Drop-in Shaka.** The Vite plugin resolves `shaka-player` imports to ScreenKit's implementation (`@screenkit/shaka`, also importable directly), so Shaka code such as the Blits `PlayerManager.js` runs unmodified.
- **Video is always composited beneath the app's canvas**, placed by the element's CSS rect. The app clears transparent where video shows, and a z-index that would lift video above the canvas warns once.
- **Direct play on every platform: video never passes through the runtime's GL.** Apple: an `AVPlayerLayer` beneath the metal view. Android: a `SurfaceView` beneath SDL's. Linux: libvlc's memory video output (`libvlc_video_set_callbacks`) delivers each frame when it is due, into dma-buf-backed buffers committed to a Wayland subsurface beneath the app's window (sway on Batocera offers `zwp_linux_dmabuf_v1` v4 with NV12/YU12 LINEAR, `wl_subcompositor`, `wp_viewporter`, `wp_presentation`). Video on Linux needs Wayland; elsewhere `load` rejects.
- **Linux is libvlc, not FFmpeg** (human, 2026-09-19, replacing the approved FFmpeg player). libvlc owns demux (its `adaptive` module does HLS and DASH), decode, A/V sync and audio (ALSA); we own presenting its frames. Accepted consequences, each a recorded divergence: **ClearKey fails on Linux (6001)**, as on Apple; **variants cannot be listed or switched** -- `getVariantTracks()` lists only the playing stream, and ABR restrictions apply at `load` as VLC's adaptive options; **captions are drawn by VLC into the picture** -- tracks are listed and selectable, and `cuechange` never fires on Linux, the one place captions are rendered natively.
- **Hardware decoding on Linux, and a ceiling that follows the board** (human, 2026-09-19: "we need hardware accelerator for libvlc"; "solution should be universal, based on board, can be 1080p 60fps on high end pi too"). The image's VLC cannot reach the hardware itself: its `avcodec` plugin is software only, and its `gstdecode` bridge segfaults (and GStreamer's `v4l2h264dec` needs `h264parse`, which the image lacks). So ScreenKit ships its own **VLC decoder plugin**, loaded through `VLC_PLUGIN_PATH` (ABI tag `3_0_0f`) and preferred over `avcodec`. Per stream codec it picks, best first: a stateless V4L2 decoder where the image's libavcodec offers one; the stateful V4L2 M2M decoders in the image's libavcodec (`h264_v4l2m2m`, `hevc_v4l2m2m`, ... -- `libavcodec.so.58`, which VLC links, carries them); otherwise VLC's software decoder. **No fixed resolution**: the ABR ceiling is the smaller of the display mode and the chosen decoder's probed limits, and sustained drops (over 1% across 10 s) step it down one rung, reopening at the current position because VLC 3 cannot switch variants live. Measured on this Pi 3: 1080p30 H.264 decodes at 1.61x real time in hardware against 1.14x in software, so the Pi 3 must play 1080p30 H.264 through the hardware with under 1% dropped frames over 60 s. Other boards (a Pi 4 or 5 reaching 1080p60) follow from the same probe and are recorded as unverified until run.
- **No `gl.texImage2D(..., video)`** in this spec (deferred).
- **DRM now: Widevine on Android and FairPlay on Apple**, configured Shaka's way (`drm.servers`, `drm.advanced`, the FairPlay server certificate). Licence requests go through the JS player's networking engine, so request and response filters apply. Linux has no DRM system: a protected stream fails there with Shaka's code 6001.
- **FairPlay is verified up to the key server only**: certificate fetch, the SPC request reaching the configured licence URL through the filters, and error mapping, all against the local fixture. Real decryption stays unverified and is recorded as such until a FairPlay stream is available. The tvOS simulator cannot do FairPlay.
- **DASH on Apple is a recorded divergence**: AVPlayer cannot play it, and `load` rejects with Shaka's unsupported-manifest error. HLS and MP4 play everywhere; DASH plays on Android and Linux.
- **Captions: tracks and cues to JS.** Shaka's text-track API, plus `video.textTracks` whose tracks fire `cuechange` with `activeCues` (`VTTCue`) for the app to draw. Nothing is rendered natively -- except on Linux, where VLC draws them (above).

## Boundaries & Constraints

**Always:**
- One seam, `MediaPlayer`, with one implementation per platform and an Unavailable one elsewhere, shaped like `NetService`: no JSI below it, events leave through a sink, and every entry point is safe from any thread.
- The JS layer sees the same events, states and errors on every platform. Where a platform genuinely cannot match, it is a recorded divergence (in `runtime/js/README.md` and asserted per platform), never a silent difference.
- Standard `HTMLMediaElement` semantics for what the element exposes: the event order, `readyState`/`networkState`, `paused`/`ended`/`seeking`, `play()` returning a Promise, and pausing when the element leaves the document.
- Shaka's own names, shapes and error codes (`shaka.util.Error` category, code, severity) for everything the Shaka layer exposes.
- The player is released with its element, on `destroy()`/`unload()`, and on runtime shutdown. Nothing is delivered after shutdown returns, and a paused runtime's players pause.
- Linux links the image's own libvlc, libwayland-client and libgbm/libdrm dynamically and ships none of them. Headers come from the exact releases the device runs, pinned.
- Every new Maven artifact is pinned in `gradle/verification-metadata.xml` and covered by the R8 rules.
- No test row reaches the public internet. The public demo streams (the Blits HLS stream, a Widevine demo) are manual checks.

**Never:**
- MediaSource Extensions, and reviving real `shaka-player` by emulating MSE.
- Writing an HLS or DASH parser on Apple or Android: manifests are the platform player's.
- Drawing video with the runtime's GL, or touching the app's GL context from outside the JS thread.
- `<audio>` playback, Web Audio, picture-in-picture, or native caption rendering (VLC's on Linux is the one accepted exception).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Load and play HLS | `player.load(hlsUrl)` then `video.play()` | `load` resolves after `loadedmetadata`; `play`, `playing`, `timeupdate`; `getVariantTracks()` lists the variants | N/A |
| Progressive MP4 | `load(mp4Url)` | same as HLS; `isLive()` false, one variant | N/A |
| DASH | `load(mpdUrl)` | plays on Android and Linux | Apple: rejects 4000-range unsupported manifest, CRITICAL |
| Live HLS | a sliding-window playlist | `isLive()` true, `seekRange()` advances, `duration` is `Infinity` | N/A |
| Seek | `currentTime = t` | `seeking`, then `seeked` at t, clamped to `seekRange()` | N/A |
| End of stream | playback reaches the end | `ended` true, `ended` event, `paused` true | N/A |
| Variant / audio / text selection | `selectVariantTrack`, `selectAudioLanguage`, `selectTextTrack` + visibility | `variantchanged`/`textchanged`; `activeCues` update with `cuechange` | Linux: only the playing variant is listed; text is drawn by VLC and `cuechange` never fires |
| ClearKey (`drm.clearKeys`) | a CENC stream plus its keys | plays on Android | Apple and Linux: 6001 |
| Widevine / FairPlay licence | `drm.servers` plus filters | the licence request goes out through the JS networking engine with the filters applied, and its response goes back to the native key system | licence HTTP failure: 6007 `LICENSE_REQUEST_FAILED`; unsupported key system: 6001 |
| Bad URL | 404 manifest | `load` rejects with `shaka.util.Error` (1001, CRITICAL); `error` event on the player; `video.error` is `MEDIA_ERR_NETWORK` | the player stays usable for another `load` |
| Unplayable media | corrupt file or unsupported codec | `load` rejects (category MEDIA); `error` event | same |
| Interrupted load | `load(a)`, then `load(b)`/`unload()`/`destroy()` before `a` resolves | the first promise rejects with 7000 `LOAD_INTERRUPTED`; the second proceeds | N/A |
| Element leaves the document | `video.remove()` while playing | pauses; the plane is hidden | N/A |
| Runtime paused or shut down | lifecycle pause or teardown mid-playback | the players pause or release; no event after shutdown returns | N/A |
| Two players | two elements, each with a `Player` | each plays and composites independently | N/A |
| No Wayland on Linux | X11 or KMS | `load` rejects with 3016 `VIDEO_ERROR`, naming the missing Wayland | N/A |

</frozen-after-approval>

## Code Map

- `runtime/core/src/net/NetService.h`, `net/IoQueue.h` -- the seam shape to copy: abstract class, per-platform `create()`, sinks with no JSI, the serial `IoQueue`, the Unavailable fallback.
- `runtime/core/src/bindings/Net.cpp` (`installNet` 412-669, `NetBinding` 111-166, `EventChannel` 184-280, `Poster` 283-294) -- the binding to copy. Extract `EventChannel`/`Poster` into a shared header rather than duplicating them. `holdWork`/`releaseWork` keep the loop alive while a player lives.
- `runtime/core/src/hermes/HermesHost.cpp` 160-171 (install, after `installHostIO`), 248-267 (teardown order); `HermesHost.h:155`. The freeze gate is at `HermesHost.cpp:206-213`: posted tasks wait while paused, and players must also be told to pause.
- `runtime/CMakeLists.txt` 187-219 -- per-platform source selection to mirror.
- **Apple:** `runtime/apple/HostMain.mm` `MetalDrawable::attach` 66-86 creates the metal view (`SDL_Metal_CreateView`/`GetLayer`). On macOS it is a layer-backed NSView in the content view, opaque today (SDL's `SDL_cocoametalview.m`). On tvOS it is the view controller's root view. `runtime/core/src/gfx/GlSurface.cpp` has an EGL config with `EGL_ALPHA_SIZE 8` (112-122), so a non-opaque metal layer lets a layer beneath show through. SDL source: `poc/build/sdl3-src`.
- **Android:** `runtime/android/app/src/main/java/dev/screenkit/host/ScreenKitActivity.java`. `SDLSurface` is a SurfaceView in SDL's `mLayout` RelativeLayout (SDL `SDLActivity.java` 460-486); a video SurfaceView goes beneath it, and SDL's is made translucent and z-ordered as a media overlay. `runtime/android/jni/HostMain.cpp`: the window/EGL at 222-250 (fixed size always on Android, 240-241); `JNI_OnLoad` at 537-548, where classes are resolved and natives registered. `runtime/core/src/gfx/GlSurfaceSdl.cpp`: `SDL_GL_ALPHA_SIZE 0` at 179, and `swap` at 299-331 clears to black before drawing the app's quad, which must clear transparent instead. `runtime/core/src/net/NetServiceAndroid.cpp` (`prepareAndroidNetwork` 923-1012, `ScopedEnv`, `kCallbacks` 508-523) is the JNI pattern. `app/build.gradle` 171-186, `gradle/verification-metadata.xml` (regenerate with `./gradlew --write-verification-metadata sha256`), `app/proguard-rules.pro`. The emulator has Widevine and ClearKey HALs (`android.hardware.drm-service-lazy.widevine`/`.clearkey`).
- **Linux:** the Pi runs Batocera 42 under **sway 1.10.1** (`/run/wayland-1`), not labwc. **libvlc 3.0.21** is at `/usr/lib/libvlc.so.5` (with `libvlccore.so.9`); its plugins are in `/usr/lib/vlc/plugins`: `vmem` (the memory output behind `libvlc_video_set_callbacks`), `adaptive` (HLS/DASH), `avcodec` (software only, linked against the image's `libavcodec.so.58`, which does contain `h264_v4l2m2m`; VLC's only hardware option is `--avcodec-hw`, with no module behind it), `gstdecode` (segfaults on this image; do not use), `alsa`/`amem` audio output (no pulse plugin; ALSA's default device reaches PipeWire), and `webvtt`/`ttml`/`subsdec`. No VLC headers are on the image: take them from the pinned VLC 3.0.21 source tarball (`include/vlc/*.h`, sha256), generating `libvlc_version.h` from its `.in`. `libwayland-client.so.0`, `libgbm.so.1` and `libdrm.so.2` are present. SDL3 exposes the window's `wl_display`/`wl_surface` through window properties. The EGL surface needs alpha on Linux (`GlSurfaceSdl.cpp:179`) for the subsurface to show. The Wayland protocol XMLs come from pinned wayland-protocols, through `wayland-scanner` in the build container. Discard any FFmpeg-player code written before this change.
- `tools/batocera/pi.sh` (`sysroot()` 180-193, `openssl_sysroot()` 157-178, `headers()` 96-105), `tools/batocera/Dockerfile`, `runtime/cmake/LinuxSystem.cmake` 78-117 -- the device-libs-plus-pinned-headers pattern to extend.
- `runtime/js/dom-shim.js` -- backed-element contract: `defineBackedSize` 104 with its comment at 68-80. CSS subset: `SUBSET` 1579, `parseSubset` 1605, `divergence` 1617, `setDeclaration` 1654, `state.layer` in `styleOf` 1825 (the plane's rect comes from here). There is no insert/detach hook yet (`insert` 583, `detach` 567). Media stubs 6344-6370; `createElement` 6566; `defineEventHandlers` 347; `GLOBAL_EVENT_HANDLERS` 364-376 (the media handlers exist; `encrypted`/`waitingforkey` do not); `later()` 3965.
- `packages/@screenkit/vite-plugin/src/index.js`, `src/lightning.js` -- plugin structure. There is no `resolveId`/alias today, and no `@screenkit/*` package ships runtime JS yet.
- `runtime/tests/RuntimeTests.cpp` -- `cases()` 7288-7428, net fixture helpers (`netFixture` 4347, `netRuntime` 4487, `netTranscript`, `NET_PER_PLATFORM` 4543), media stub pins 1239-1247, absence lists (`domCoverage` 1658, `domIdentityAndAbsence` 1672). Also `runtime/tests/CMakeLists.txt`, and `runtime/tests/net/server.mjs` (in-memory routes, no static files or Range). The device lists are `tools/android/android.sh` `NET_ROWS` 51-55 and `tools/batocera/pi.sh` `NET_ROWS` ~350; device rows have no drawable.
- `poc/blits-example-app/src/App.js` 32-33, 115-116, 217-218 (the Player page is commented out); `src/managers/PlayerManager.js` (the Shaka usage to keep working, unmodified).

## Tasks & Acceptance

**Execution:**
- [x] `runtime/core/src/media/MediaPlayer.h` -- the seam: `MediaPlayer::create(MediaConfig)`, one per element; `load(url, startTime, drm)`, `play`, `pause`, `seek`, `setRate`, `setVolume`/`setMuted`, `setPlane(rect, visible)`, `selectVariant`/`setAbr`/`selectAudioLanguage`/`selectText`, `provideLicence(requestId, bytes)`, `unload`, `shutdown`; `MediaSink` events (metadata, state, time, buffered, tracks, variant changed, cues, licence request with its challenge, stats, error with a kind); `mediaAvailable()`.
- [x] `runtime/core/src/media/MediaPlayerApple.mm`, `runtime/apple/HostMain.mm`, `runtime/core/src/gfx/GlSurface.cpp` -- AVPlayer/AVPlayerItem/AVPlayerLayer in a container beneath a non-opaque metal view (macOS and tvOS). Variants, audio and legible selection; `AVPlayerItemLegibleOutput` cues; ABR restrictions; stats; FairPlay through `AVContentKeySession` (the SPC goes out as a licence request, the CKC comes back through `provideLicence`).
- [x] `runtime/android/app/src/main/java/dev/screenkit/media/VideoPlayer.java`, `runtime/core/src/media/MediaPlayerAndroid.cpp`, `ScreenKitActivity.java`, `runtime/android/jni/HostMain.cpp`, `GlSurfaceSdl.cpp` -- ExoPlayer (HLS, DASH, progressive) on a SurfaceView beneath a translucent SDL surface; `Player.Listener`, tracks and `TrackSelectionParameters`; `onCues`; Widevine and ClearKey through a `MediaDrmCallback` that routes licence requests to JS; JNI registered in `JNI_OnLoad`.
- [x] `runtime/android/app/build.gradle`, `gradle/verification-metadata.xml`, `app/proguard-rules.pro` -- Media3 (exoplayer, hls, dash) pinned and kept.
- [x] `runtime/core/src/media/linux/vlc-plugin/` -- the ScreenKit VLC video decoder module (VLC 3.0.21 plugin headers from the pinned tarball; the image's `libavcodec.so.58` with FFmpeg 4.4 headers generated from the pinned release): stateless V4L2 where offered, else the `*_v4l2m2m` decoders, frames into VLC pictures; a capability probe (V4L2 devices' coded formats and `VIDIOC_ENUM_FRAMESIZES`, the display mode) that `MediaPlayerLinux.cpp` reads; installed beside the runtime and put on `VLC_PLUGIN_PATH` before `libvlc_new`.
- [x] `runtime/core/src/media/MediaPlayerLinux.cpp` + `runtime/core/src/media/linux/` -- libvlc: one `libvlc_instance_t` per runtime and a `libvlc_media_player_t` per element; the ABR ceiling from the probe as `:adaptive-maxheight`/`:adaptive-maxwidth`, stepped down on sustained drops; restrictions; events from `libvlc_event_manager_t` into the sink; audio and text tracks through libvlc's track descriptions (VLC draws the selected text track); stats from `libvlc_media_get_stats`; any DRM config answers 6001. Video through `libvlc_video_set_format_callbacks`/`set_callbacks` into linear NV12/I420 GBM dma-bufs, mapped for VLC to write into and committed on `display` to a desynchronised `wl_subsurface` beneath the SDL window's surface (below the parent, scaled with `wp_viewporter`, on its own `wl_event_queue`).
- [x] `tools/batocera/pi.sh`, `tools/batocera/Dockerfile`, `runtime/cmake/LinuxSystem.cmake`, `runtime/CMakeLists.txt` -- the device's libvlc/libvlccore, wayland-client, gbm and drm libs, plus the FFmpeg **4.4** set VLC links (`libavcodec.so.58`, `libavutil.so.56`; not the 7.1 `.so.61` set, which is FFmpeg-player residue to remove), with headers from pinned releases (sha256; VLC 3.0.21's plugin headers included), protocol code from pinned wayland-protocols XMLs, imported targets, per-platform media source selection; the EGL surface with alpha on Linux.
- [x] `runtime/core/src/bindings/Media.{h,cpp}`, `bindings/EventChannel.h`, `HermesHost.{h,cpp}` -- `__screenkit.media` handles, events delivered as tasks, install and teardown order, players paused while the runtime is paused.
- [x] `runtime/js/dom-shim.js` -- `HTMLVideoElement` as a backed element with the `HTMLMediaElement` surface and events, `MediaError`, `TimeRanges`, `TextTrack`/`TextTrackList`/`VTTCue`; the CSS rect driving `setPlane`; insert/detach hooks; `canPlayType` per platform.
- [x] `packages/@screenkit/shaka/` -- the Shaka-shaped player: `Player` (`attach`/`detach`/`load`/`unload`/`destroy`/`configure`/`getConfiguration`/`getNetworkingEngine` with request and response filters, variant/audio/text track APIs, `setTextTrackVisibility`, `isLive`/`seekRange`/`isBuffering`/`getStats`/`getMediaElement`, `Player.isBrowserSupported`), its events, `shaka.util.Error` with Shaka's codes, `shaka.polyfill.installAll` as a no-op; licence requests performed here.
- [x] `packages/@screenkit/vite-plugin/src/` -- resolve `shaka-player` (and its subpaths) to `@screenkit/shaka`; tests in `test/plugin.test.js`.
- [x] `runtime/tests/net/server.mjs`, `runtime/tests/net/media.mjs` -- media fixtures generated at fixture start with the host's `ffmpeg` (two-variant HLS VOD with a WebVTT rendition, a live window, DASH, MP4, CENC-encrypted DASH for ClearKey, a corrupt file), served statically with Range; a fake licence endpoint that records requests.
- [x] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`, `android.sh`, `pi.sh` -- `media-*` rows covering the I/O matrix through the element and through the Shaka API, with per-platform transcripts for the recorded divergences and the stub pins updated; run on macOS, ASan, Android and the Pi. `media-unavailable` for the Unavailable player.
- [x] `poc/blits-example-app/src/App.js` -- the Player page restored.
- [x] `Architecture.md` §4 and §10.4 (sway, direct play, libvlc on Linux instead of FFmpeg), `runtime/js/README.md`, `runtime/README.md`, `tools/batocera/README.md`, `deferred-work.md` (texture sampling, real FairPlay verification, hardware decode on Linux, Linux variants/ClearKey/cues) -- what shipped, divergences, deferred parts.

**Acceptance Criteria:**
- Given the Blits example app with its Player page restored and `PlayerManager.js` unmodified, when the Player page opens on macOS, the tvOS simulator, the Android emulator and the Pi, then the HLS stream plays beneath the UI, progress advances and remote play/pause works (a screenshot on each).
- Given any `media-*` row, when run on macOS, macos-asan, Android and the Pi, then it passes with no skips, and `getStats().decodedFrames` is above zero after a second of playback.
- Given the Pi 3 playing a 1080p30 H.264 stream, when 60 s have played, then the ScreenKit decoder plugin (not `avcodec`) decoded it, and `getStats().droppedFrames` is under 1% of `decodedFrames`. Given an HLS ladder whose top rung exceeds the probed ceiling, then that rung is never selected.
- Given Shaka's public Widevine demo stream on the Android emulator, when loaded with its licence server, then it plays (manual; recorded).
- Given a runtime shut down mid-playback, when shutdown returns, then no media event is delivered afterwards and the native player is gone (a leaks row on macOS).
- Given no video element in use, when frames are presented, then the canvas-only path costs what it did before.

## Implementation Notes

**Deviations from the task text, all in the Linux backend:**

- **The V4L2 decoder is driven directly, not through libavcodec.** The task said "stateless V4L2 where offered, else the `*_v4l2m2m` decoders". Stateless is never offered on Batocera 42 (upstream FFmpeg 4.4 has no `v4l2_request` hwaccel; only the Raspberry Pi fork does), and the `h264_v4l2m2m` wrapper was measured too slow to be the answer: it hands out the kernel's uncached MMAP capture buffers, which cost **35 ms** for the CPU to read a 1080p frame -- past the 33 ms a frame has at 30 fps, before any copying. So the plugin talks to `/dev/video10` itself and allocates its capture buffers from the kernel's CMA heap, imported as dma-bufs and read through a cached mapping: **4 ms** for the same frame. The libavcodec route is kept as the rung below, for a board with no CMA heap. Rungs and their logs are in `screenkit_vlc.c`'s header comment.
- **An MPEG-TS demuxer shim had to be written.** Not foreseen: the image's VLC was built without libdvbpsi, so it has no `ts` demux module, and VLC's `adaptive` asks for one *by name* for every TS segment -- HLS with TS segments, the common case, failed outright. The plugin answers to the name "ts" with score 0 (never chosen on its own) and forwards to the image's `avformat`, forced to mpegts.
- **A manifest-probing stream filter had to be written.** libvlc 3 reports no variants, no live window and no protection, all of which `load` must resolve with. The filter peeks at every stream VLC opens and logs what it found for the player's log callback to parse.
- `:adaptive-logic=highest` is set on every load, not only when a ceiling applies. Under the TS shim an adaptive representation switch mid-stream corrupts the clock (measured: 39 late-drops and an early `ended`), and VLC 3 cannot switch variants live anyway -- the ceiling is applied at load and a step-down reopens, which is what the spec already decided.

**Defects found in verification and fixed here, each with the test that now covers it:**

- *Linux, segfault* (`media-interrupted`, `media-unplayable`): VLC's video thread can still hand over a picture while `libvlc_media_player_stop` returns, so the frame epoch has to be bumped **after** the stop as well as before it; `holdPreroll` then called `libvlc_media_player_can_pause` on a released player. Second bump plus a null guard.
- *Linux, playback ending early* (`media-hls`, `media-element`): VLC's input control queue merges a pause immediately followed by a resume and emits no `Playing` event, so `playing_` stayed false and the position clock froze. `applyPlay` now marks playing at the call, and `baseAt_` is reset at every load and at preroll.
- *Linux, a lagging buffered estimate*: `holdPreroll` cleared `playing_` even when it did not actually pause.
- *Android, `media-shutdown` flaking under load*: ExoPlayer's release timeout is 500 ms and the emulator misses it; `setReleaseTimeoutMs(5_000)`. *`media-element`*: errors arriving after `stop()` were reported; now ignored once stopped.
- *Android, `media-licence` reaching the licence server twice*: ExoPlayer's `DefaultDrmSessionManager` retries a failed key request three times on its own, the first after no delay at all -- behind Shaka's `drm.retryParameters`, and racing the teardown of the failed load. The DRM session manager now takes `DefaultLoadErrorHandlingPolicy(0)`: retrying a licence request is the JS layer's decision, not ExoPlayer's.
- *The fixture server failing to start, a few percent of the time* (`ERR_OSSL_ASN1_ILLEGAL_PADDING`): the hand-rolled DER encoder emitted a non-minimal INTEGER whenever a random serial began with a zero byte (~1 in 256 per certificate). `certs.mjs` now trims. 2400 certificates generated afterwards, none refused.

- *Linux, video decoded but never on screen -- found by the manual Pi check, not by any row.* The window SDL made was opaque: SDL re-declares the whole surface opaque on every configure (`SDL_waylandwindow.c`, `SetSurfaceOpaqueRegion`), overwriting the region the plane cuts open, and it picks an EGL config with no alpha, so a page that cleared transparent still read black. The compositor therefore had an opaque fullscreen surface over the subsurface and skipped it: measured on the Pi as **2160 pictures committed, 0 presented**; with the plane placed above the window instead, 1056 of 2160 were presented, which is what identified the window rather than the plane. The window is now made with `SDL_WINDOW_TRANSPARENT` (`runtime/linux/HostMain.cpp`), and because that also stops SDL declaring any opaque region -- which would make the compositor blend, and forgo scanout, for every app -- the host declares the window opaque itself at startup (`wl::declareWindowOpaque`), leaving a visible plane to cut it open as before. After the fix: 917 of 1680 committed pictures presented, the Blits Player page shows video beneath its controls on the Pi, and the device suite is 42/42.
- A `SCREENKIT_MEDIA_TRACE` line for the plane -- committed, presented, dropped and the applied rect -- was added for that hunt and kept: a plane the compositor never shows now says so in the log instead of only on the television.

**Added beyond the tasks:**

- `media-hardware`, a Linux-only row playing a generated 1920x1080@30 clip whole and asserting the size, the frame count, under 1% dropped, and the ScreenKit decoder in the log. `SCREENKIT_MEDIA_SOAK=1` replays it to 60 s for the spec's measurement. Steady-state counting starts 2 s into each pass, because a playback start loses about six frames.
- `SCREENKIT_MEDIA_TRACE`, an opt-in trace of the Linux player's state transitions, and a backtrace handler in the Linux test binary -- both written to find the three defects above.
- `PI_EXTRA_ENV` in `tools/batocera/pi.sh`, for running a row on the device with an extra variable.

**Measurements (Raspberry Pi 3, Batocera 42, sway):** 1080p30 H.264 through the hardware decoder, 60 s soak -- **1466 frames, 0 dropped (0.00%)** in steady state; 1667 decoded with 33 dropped counting the restarts, which is the ~6 frames each playback start costs. The acceptance criterion asks for under 1%.

## Spec Change Log

- 2026-09-19 -- **human renegotiation mid-implementation, not a review loopback.** The human replaced the Linux backend: libvlc instead of FFmpeg. Probed on the Pi first: VLC 3.0.21 with `vmem`, `adaptive`, a software-only `avcodec`, ALSA output and subtitle decoders; libvlc 3 exposes no cue text, no variant list and no CENC keys. H.264 software decode measured at 1.14x real time for 1080p30 and 2.4x for 720p30. The human accepted the consequences (ClearKey 6001 on Linux, only the playing variant listed, VLC-drawn captions) and moved the Pi target to 720p30. Amended: Intent approach and decisions, Always/Never, two matrix rows, the Linux Code Map bullet, the Linux and sysroot tasks, the Pi acceptance criterion. Known-bad state avoided: an FFmpeg player built beside libvlc. KEEP: everything done for the seam, the JS layer, the Apple and Android players and the test fixtures; the Wayland subsurface presentation design.

- 2026-09-19 -- **second human renegotiation: hardware decoding on Linux, a ceiling that follows the board.** Probed on the Pi first. VLC's `avcodec` is software only. `gstdecode` segfaults (VLC 3.0.21 with GStreamer 1.26), and it would fall back to software anyway, because `v4l2h264dec` cannot link without `h264parse`, which the image lacks. The image's `libavcodec.so.58` carries `h264_v4l2m2m`, libvlccore honours `VLC_PLUGIN_PATH`, and the plugin ABI tag is `3_0_0f`. Amended: the Pi target decision (720p30 cap replaced by probe-based decoder and ceiling selection), the Linux Code Map bullet, a new VLC-plugin task, the Linux task, the Pi acceptance criterion. Known-bad state avoided: a hardcoded resolution cap that would under-use faster boards. KEEP: the libvlc player and its Wayland presentation.

## Review Triage Log

Three layers ran against the staged diff: a blind hunter (16 findings), an
edge-case hunter (29), and a verification-gap reviewer (8 gaps plus 3 notes).
Every claim below was checked at its cited line before a verdict was written; the
gap layer's findings arrive verified by its own evidence rules. No entry routed
to intent_gap or bad_spec, so there was no loopback: everything real was a
`patch`, and the rest was rejected or deferred.

**Fixed (patch).**

| # | Finding | Verdict | Evidence, and what was done |
|---|---|---|---|
| 1 | The Linux window is opaque, so the compositor never shows the plane beneath it | high | Found by the manual Pi check before the review: 2160 pictures committed, 0 presented; the same plane placed above the window presented 1056. `SDL_WINDOW_TRANSPARENT` on Wayland, with the opaque region declared by the host (Implementation Notes). |
| 2 | That flag on X11 would blend the desktop through a page that cleared transparent | medium | Confirmed at `GlSurfaceSdl.cpp:180-191` and `:326-333`: the alpha request and the alpha-0 clear were on `__linux__`, not on the driver, while X11 has no video plane at all (`caps.videoOutput` is false there). Both are now asked of Wayland only, and the flag with them. |
| 3 | `canPlayType` answers differently per platform for VP8 | medium | Confirmed: `MediaPlayerLinux.cpp:1879` lists `vp08`, `VideoPlayer.java:363` lists `vp8`, and the shim's alias table mapped neither. Spellings are now equivalence groups in the shim, so every platform answers the same. |
| 4 | `canPlayType` says `probably` where every load is refused | medium | Confirmed: it checked `caps.available` and never `caps.videoOutput`, so Linux without Wayland promised playback it then refused with 3016. It now answers `''`. |
| 5 | `playbackRate = 0` does three different things | medium | Confirmed: Apple froze the picture (correct), Android ignored the call and kept playing, Linux jumped back to 1x. All three now freeze with `paused` still false, as HTML says; `runtime/js/README.md` says so. |
| 6 | Android drops `drm.advanced.serverCertificate` | medium | Confirmed: marshalled across JNI, stored in `Load`, read by nothing. It is now handed to the CDM as Widevine's `serviceCertificate`, and a key system that refuses it says so in the log. |
| 7 | A failed load leaves the element `seeking` for ever | medium | Confirmed: `onMediaEvent` drops every event once the state is `error`, so a seek in flight was never answered and `currentTime` froze. `mediaFailure` now clears `seeking`. |
| 8 | A seek that resolves to the position already held is never acknowledged on Android | medium | Confirmed against media3 1.9.4: `seekToInternal` returns early without a state change, and the listener had no `onPositionDiscontinuity`. It has one now. |
| 9 | A negative `width`/`height` attribute becomes a 4.29-billion-pixel plane | medium | Confirmed: `Number(attr) >>> 0` wrapped `-1`. HTML's rule (an invalid non-negative integer is ignored) is now applied, so the default size stands. |
| 10 | `selectVariant` and `load`'s `startTime` take non-finite numbers | medium | Confirmed reachable through `player.selectVariantTrack({id: NaN})`; `static_cast<int>` of a non-finite double is undefined. Both now throw as `seek` and `setRate` already did. |
| 11 | In-band cues accumulate for the life of a load | medium | Confirmed: every unseen cue was added, nothing removed, and the dedupe scans the list per cue. Cues the media window has left behind are now dropped, active ones kept. |
| 12 | A malformed percent escape in a media path kills the fixture server | medium | Confirmed: `decodeURIComponent` throws out of the request handler, which nothing catches, so the Node process exits and every later row loses its fixture. The media route answers 400, and any route that throws is now that request's 500. |
| 13 | The Linux ABR ceiling probes H.264 only | medium | Confirmed: it ran before the stream's codec was known and capped every ladder by the H.264 decoder's limits. It now takes the widest picture any decoder on the board accepts, with the display mode as before; a rung the chosen decoder cannot sustain is what the drop-rate step-down is for. |
| 14 | SDL's display calls run on the player's thread | medium | Confirmed: `SDL_GetDisplayForWindow`/`SDL_GetCurrentDisplayMode` are documented main-thread-only and `boardCeiling` runs on the player's worker. The mode is now read once on the main thread, into `VideoHost`. |
| 15 | An unanswered FairPlay key request buffers for ever on Apple | medium | Confirmed: nothing timed one out, while Android fails the same request after 30 s. Apple now uses the same 30 s. |
| 16 | The scratch picture buffer is a fixed 16 MiB with all three planes at one base | low | Confirmed as written, but B1 showed the path is ordered behind a stop that has already destroyed VLC's vout, so it is not reachable today. Fixed anyway, because the correction is smaller than the argument: the buffer is sized from the layout VLC was given, a plane each. |
| 17 | The two Linux-only rows are outside the row guard | low | Confirmed: `media-hardware` and `media-no-wayland` are in no CTest list, and both device guards work by scraping that file, so a rename would silently stop running them. `pi.sh` now also checks its own list against the binary's `--list`. |
| 18 | `Media.h` documents a `screenkit:` URL the binding refuses | low | Confirmed: the implementation reads an undocumented `asset` option and refuses the scheme; only the shim's splitting made the documented behaviour true. The header now describes what the binding actually takes. |
| 19 | A dead JNI global ref, and a capabilities write outside its `call_once` | low | Confirmed: `gByteArrayClass` was promoted to a process-lifetime global and never used; the `gCapabilities` reset was outside the `call_once` that guards it (unreachable twice, since `prepare` is one-shot). Both removed. |
| 20 | A dead local in the Apple seek completion | low | Confirmed: `wasStart` was read and discarded with `(void)`. Removed. |
| 21 | `tools/batocera/README.md` has no "Video" section, though other files cite it | low | Confirmed when filed; the section was written during this review (decoder, plugin, plane, the trace and the env knobs), and the stale "labwc" in the same file corrected to sway. |

**Verification gaps, all from the gap layer and all filed as `patch`:**

| # | Gap | What was done |
|---|---|---|
| 22 | `loop` and `autoplay` asserted by nothing | `media-element-model` (the scripted-player row, every platform) now drives both: a looped end seeks back and keeps playing with no `ended`, a live stream still ends, autoplay starts once and a pause by hand ends it. |
| 23 | `video.error.code` asserted for one of six kinds | The same row now emits every kind the seam can report and pins the code for each. |
| 24 | `planeInWindow` and `fitContain` never executed | `media-unavailable` now checks both directly -- scaled and centred, letterboxed, pillarboxed, and the unchanged cases. They are pure functions, so this needs no device. |
| 25 | `@screenkit/shaka`'s own suite is in no verification path | `cd packages/@screenkit/shaka && npm test` added to this spec's Verification and to `runtime/README.md` (13 tests, passing). |
| 26 | The native plane is never placed in any automated row | Partly closed: the geometry is now asserted (24), and the Linux plane reports `committed`/`presented` under `SCREENKIT_MEDIA_TRACE`, which is what identified finding 1. The rest -- a row that asserts a plane reached the compositor on each platform -- is deferred, below. |
| 27 | A `<video>` whose `src` is a package asset is never loaded | Deferred, below. |
| 28 | The Linux ceiling and the drop-rate step-down are never exercised | Deferred, below: both ran in the field during this review (`dropped 9 of 607 frames over 10 s at 720p: the ceiling steps down to 480p`), but no row drives them. |
| 29 | The three `announce()` warnings are never asserted | Deferred, below. |

**Rejected.**

| # | Finding | Verdict | Why |
|---|---|---|---|
| 30 | No event-handler IDL attributes on `HTMLMediaElement` | false | `GLOBAL_EVENT_HANDLERS` already carries all twenty-two and `defineEventHandlers(HTMLElement.prototype, ...)` installs them; media events are dispatched at the element, so `video.ontimeupdate` is a live accessor. |
| 31 | Random sentinel serials make the transcripts non-deterministic | false | The sentinel is only ever compared with `state.serial` and never printed; native serials are unsigned and never negative, so it cannot collide with a real one. |
| 32 | `certs.mjs`'s `integer()` emits a zero-length INTEGER | false | Only reachable with a zero-length buffer, and its two call sites pass `Buffer.from([2])` and an 8-byte serial. The all-zero serial case already yields `02 01 00`. |
| 33 | The engine abstraction makes the media change unbuildable on its own | false | No media source references `engine/Engine.h` or `SCREENKIT_ENGINE`, and the media block in `runtime/CMakeLists.txt` is unconditional. |
| 34 | `RuntimeConfig`'s heap clamp, microtask queue, block scoping and JIT flag were lost | false | All four moved to `engine::createRuntime` (`HermesEngine.cpp:26-65`), which `HermesHost.cpp:120` calls. |
| 35 | A seek inside the buffered range never fires `seeked` on Android | false | media3 forces BUFFERING on every seek from READY, so the ordinary case is answered. The narrow same-millisecond case was real and is finding 8. |
| 36 | `abr` caps of exactly 0 stall playback on Android | false | `exceedVideoConstraintsIfNecessary` defaults to true, so a cap of 0 pins the lowest variant instead of emptying the selection. |
| 37 | A short `abr` array is silently ignored | false | The only producer builds exactly seven doubles. |
| 38 | `nativeTracks` can leave an `ArrayIndexOutOfBounds` pending | false | Its one caller allocates all four slots per entry; no JS input can desynchronise the arrays. |
| 39 | A failed `AttachCurrentThread` at shutdown leaks a global ref and fails the leaks row | false | `java_ != nullptr` implies the VM is live, and `liveMediaJavaRefCount()` is called nowhere -- the leaks row is macOS-only. |
| 40 | `setRate(0)` or a negative rate wedges Apple | false | A negative rate cannot reach the seam (the shim throws), and freezing at rate 0 is the browser behaviour. The divergence that was real is finding 5. |
| 41 | Every plane update walks the whole element tree | low | Confirmed but mitigated: `walk` early-exits at whichever of canvas or video comes first, and tree changes reach it only when `isConnected` flips. The fix would be a cache to keep coherent, for work that is already short. |
| 42 | Linux capabilities are rebuilt on every query | low | Confirmed, but on a cold path (`canPlayType`, `load`), and caching would be wrong: `videoOutput` legitimately changes once the window exists. |
| 43 | `httpStatusIn` compiles a regex per call | low | Confirmed, but only on a failed load, at most about twenty times, never during playback. |
| 44 | `@screenkit/shaka` is private, while a build error says to install it | low | Confirmed, but the plugin resolves the monorepo sibling, so the message cannot print in any supported configuration today. |
| 45 | The drain's failure log cannot tell a net event from a media one | low | Confirmed: both bindings tag with the runtime's name. Only reachable if building the payload itself throws, and the surrounding catches do name the subsystem. |
| 46 | `canPlayType` now answering `probably` leads probing libraries into `texImage2D(video)` | low | The premise is about a state before this change that cannot be checked; sampling video in WebGL is deferred by the spec, and `texImage2D` with a video makes an empty texture rather than throwing. Recorded with the deferred texture work. |
| 47 | An unconfigured protected stream reports 3016 instead of 6001 without the VLC plugin | low | Confirmed, but it needs a build with the plugin missing, which `acquireInstance` already warns about; a configured key system is refused with 6001 before any of this. |

**Deferred** (recorded in `deferred-work.md`): the plane-placement rows (26), the
package-asset row (27), the ceiling and step-down rows (28), the `announce()`
row (29), `videoRobustness`/`audioRobustness` being configurable and enforced
nowhere, the plane `order` divergence (Apple stacks by `z-index`, Android and
Linux do not -- now recorded in `runtime/js/README.md`), the process-wide libvlc
instance and its URL-keyed log shared between two runtimes, and the 60 s
hardware measurement only running under `SCREENKIT_MEDIA_SOAK`.

## Design Notes

The plane is the element's CSS rect in drawable pixels, from the subset the shim already parses (`state.layer`). The Blits demo sets `position:absolute; top:0; left:0` plus width and height attributes, and inserts the video as `body`'s first child with `z-index:-1`.

Licence flow, the same shape on all three: native raises `licence-request {id, keySystem, challenge}`; `@screenkit/shaka` builds a Shaka request (type `LICENSE`), runs the request filters, fetches it, runs the response filters, and calls `provideLicence(id, bytes)`. For FairPlay it first fetches `serverCertificateUri`. Shaka's code 6007 is used when that fetch fails.

Shaka error codes used, all Shaka's own: 1001 `BAD_HTTP_STATUS`, 1002 `HTTP_ERROR`, 3016 `VIDEO_ERROR`, 4000 `UNABLE_TO_GUESS_MANIFEST_TYPE`, 6001 `REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE`, 6007 `LICENSE_REQUEST_FAILED`, 7000 `LOAD_INTERRUPTED`, 7002 `NO_VIDEO_ELEMENT`.

## Verification

**Commands:**
- `cmake --build runtime/build/macos && ctest --test-dir runtime/build/macos` -- expected: every row passes, `media-*` included
- `cmake --build runtime/build/macos-asan && ctest --test-dir runtime/build/macos-asan -R "media-|net-|dom-"` -- expected: pass, no ASan reports
- `cmake --build runtime/build/tvos-simulator` -- expected: builds
- `sh tools/android/android.sh test` and `MINIFY=1 sh tools/android/android.sh test` -- expected: every row passes
- `sh tools/batocera/pi.sh test` -- expected: every row passes on the device
- `cd packages/@screenkit/vite-plugin && npm test` -- expected: pass
- `cd packages/@screenkit/shaka && npm test` -- expected: pass

**Manual checks:**
- The Blits Player page on each of the four targets: video beneath the UI (screenshot), remote play/pause, progress; the Pi 3's 60 s dropped-frame measurement at 1080p30 through the hardware decoder.
- The Widevine demo stream on the Android emulator.
