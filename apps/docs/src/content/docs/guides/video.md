---
title: Video
description: A standard HTMLMediaElement backed by the platform's own player, with a Shaka Player 5 API on top — so Shaka code runs unmodified.
---

There are two ways to play video, and they are the same video:

1. **The `<video>` element** — a standard `HTMLMediaElement`, if you want to drive playback yourself.
2. **`shaka-player`** — the API your app probably already speaks. It resolves to
   [`@screenkit/shaka`](/reference/shaka/), which **mirrors Shaka Player 5**.

Underneath both, each element is backed by **the platform's own player**, which owns the manifest,
buffering, ABR, decoding, DRM and presentation:

| Platform | Player |
|---|---|
| tvOS, macOS | AVPlayer / AVFoundation |
| Android TV, Fire TV | Media3 ExoPlayer |
| Embedded Linux | libvlc |

## Shaka code runs unmodified

Real Shaka Player cannot run here: it parses manifests and feeds segments to Media Source Extensions,
and there is no MSE. So `@screenkit/vite-plugin` resolves **`shaka-player` and every subpath of it**
to `@screenkit/shaka`, a player that keeps Shaka's names, shapes, events and error codes on top of
the platform player. Application code written against Shaka — the Blits example's
`PlayerManager.js`, for instance — builds and runs without edits.

```js
import shaka from 'shaka-player'   // resolves to @screenkit/shaka

const video = document.createElement('video')
video.style.cssText = 'position: absolute; left: 0; top: 0; width: 1920px; height: 1080px; z-index: -1'
document.body.appendChild(video)

const player = new shaka.Player()
await player.attach(video)

player.configure({ drm: { servers: { 'com.widevine.alpha': LICENCE_URL } } })
player.getNetworkingEngine().registerRequestFilter((type, request) => {
  if (type === shaka.net.NetworkingEngine.RequestType.LICENSE) {
    request.headers['Authorization'] = token
  }
})

player.addEventListener('error', (e) => console.error(e.detail.code, e.detail))
await player.load(MANIFEST_URL)
```

`load()` resolves once `loadedmetadata` has fired and the tracks, the variant playing and — for live
— the seek window are known. `configure` merges exactly as Shaka does, against Shaka 5's own
defaults, and Shaka 4's preference names are mapped for you.

The full surface, the deliberate divergences and every error code are in
**[the `@screenkit/shaka` reference](/reference/shaka/)**.

## Or drive the element directly

```js
const v = document.createElement('video')
v.src = 'https://cdn.example/stream.m3u8'
v.style.cssText = 'position: absolute; left: 0; top: 0; width: 1920px; height: 1080px; z-index: -1'
document.body.appendChild(v)
await v.play()
```

You get the load algorithm and its events, `readyState` and `networkState`, `play()` promises,
seeking, `ended`, `loop`, `autoplay`, `TimeRanges`, `MediaError`, and text tracks (`TextTrack`,
`VTTCue`, `cuechange`) whose cues your app draws.

:::note
There is no `<video>` via markup — `index.html` is never rendered. Create the element.
:::

## Direct play: video never passes through the runtime's GL

The plane is the element's CSS rect, composited **by the platform, beneath the app's canvas** — an
`AVPlayerLayer` beneath the Metal view, a `SurfaceView` beneath SDL's, a Wayland subsurface beneath
the window with libvlc's frames in dma-bufs. Your app clears the canvas transparent where video
should show through.

That means zero-copy on Apple and Android, HDR where the platform does it, and DRM-safe by
construction.

```js
// clear transparent where the video shows, opaque everywhere else
gl.clearColor(0, 0, 0, 0)
gl.clear(gl.COLOR_BUFFER_BIT)
```

A `z-index` that would lift video above a canvas warns once; **video is beneath every canvas
regardless**. The canvas stays opaque to the compositor while no plane is visible, so a page without
video costs exactly what it did before.

:::caution[`texImage2D` from a video element is deferred]
Sampling video in WebGL — `gl.texImage2D(target, level, fmt, fmt, type, videoElement)` — is not
implemented yet on any platform. Composite the plane instead.
:::

## DRM

Configured Shaka's way — `drm.servers`, `drm.clearKeys`, `drm.advanced` (robustness,
`serverCertificate` / `serverCertificateUri`, headers) and `preferredKeySystems`. The key system asks
for a licence, **this player performs the request** through its networking engine so your request and
response filters apply, and hands the answer back.

| DRM | Where |
|---|---|
| FairPlay (`AVContentKeySession`) | Apple — device and macOS; the tvOS simulator has none |
| Widevine (`MediaDrm`) | Android |
| ClearKey | Android only |
| — | Linux has no DRM |

For FairPlay the application certificate is fetched first, the request's `initData` is the `skd://`
id and its body the SPC, sent the way Shaka sends it — register the `shaka.drm.FairPlay.*` filters if
your server wants another shape. There is no EME.

## Hardware decoding on a Raspberry Pi

Decoding is chosen from what the board actually has, so a Pi 3 and a Pi 5 both play without the app
knowing which it is running on. What a given board sustains is a property of the board — a Pi 3 is
capped well below a Pi 5.
