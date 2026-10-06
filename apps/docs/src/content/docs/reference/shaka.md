---
title: '@screenkit/shaka'
description: A player whose API mirrors Shaka Player 5 over the platform's own player — the full surface, the deliberate divergences, and every error code.
---

Real Shaka Player cannot run in ScreenKit: it parses manifests and feeds segments to Media Source
Extensions, and there is no MSE. `@screenkit/shaka` does instead what react-native-video does — the
platform's own player owns manifests, buffering, ABR, decoding, DRM and presentation — and keeps
**Shaka Player 5's names, shapes, events and error codes** on top.

`@screenkit/vite-plugin` resolves `shaka-player` **and every subpath of it** to this package, so
Shaka code builds and runs unmodified. It is importable directly too.

```js
import shaka from 'shaka-player'   // or '@screenkit/shaka'
```

## Player lifecycle

`new shaka.Player()` · `attach` · `detach` · `load(uri, startTime, mimeType)` · `unload` · `destroy`

`load()` resolves once `loadedmetadata` has fired and the tracks, the variant playing and (for live)
the seek window are known. A failed load rejects, fires `error` at the player, and leaves the player
ready for the next one.

## Configuration

`configure` merges **exactly as Shaka does**: against Shaka 5's own defaults, with an unknown key or
a wrong type refused with an error, `undefined` restoring a default, and Shaka 4's preference names
mapped.

Also `getConfiguration`, `resetConfiguration`, `getNonDefaultConfiguration`.

## Networking

`getNetworkingEngine()` gives you request and response filters, retries with Shaka's retry
parameters, and `request()` as an abortable operation — all over the runtime's `fetch`.

:::caution[Filters see licence and certificate requests only]
Manifests and segments are fetched by the platform player, not by this code, so they never pass
through the networking engine. The one request this player performs itself is the licence exchange
(and, for FairPlay, the application certificate).
:::

## Tracks

`getVariantTracks` · `getAudioTracks` · `getTextTracks` · `getVideoTracks` · `selectVariantTrack`
(pins the variant, turning ABR off) · `selectAudioTrack` · `selectAudioLanguage` · `selectTextTrack`
(Shaka 5: a track shown, `null` for none) · `setTextTrackVisibility` / `isTextTrackVisible` (Shaka 4's
switch, kept) · the language lists.

Events: `variantchanged` · `adaptation` · `textchanged` · `texttrackvisibility` · `trackschanged`.

## State

`isLive` · `seekRange` · `isBuffering` and the `buffering` event · `getStats` · `getBufferedInfo` ·
`getMediaElement` · `getAssetUri` · `getManifestType` · `keySystem` · `drmInfo` · `trickPlay`
(forwards only) · `goToLive` · `Player.isBrowserSupported()` · `Player.probeSupport()`.

`getStats` reports the platform's decoded, dropped and corrupted frames and its bandwidths, plus
Shaka's own play, pause and buffering times, load latency, state history and switch history.

## The rest of the namespace

`shaka.util.Error` with Shaka's category, code, severity and data, for every code Shaka numbers ·
`shaka.polyfill.installAll()` (a no-op) · `shaka.log` · `shaka.util.FakeEvent`, `EventManager`,
`StringUtils`, `Uint8ArrayUtils` · `shaka.net.NetworkingEngine.RequestType` · `shaka.drm.FairPlay`'s
request and response helpers.

## DRM

Configured Shaka's way: `drm.servers`, `drm.clearKeys`, `drm.advanced` (robustness,
`serverCertificate` / `serverCertificateUri`, headers), `preferredKeySystems`.

The key system asks for a licence; this player performs the request as a `LICENSE` request through
the networking engine, so filters apply, and hands the answer back. For FairPlay the application
certificate is fetched first as a `SERVER_CERTIFICATE` request, the request's `initData` is the
`skd://` id and its body the SPC, sent as Shaka sends it.

A licence server that fails is **6007**; a certificate that cannot be fetched is 6007 too.

## Where it is deliberately not Shaka

- **Manifests and segments are the platform player's**, so request and response filters see only the
  licence and certificate requests.
- **The load mode is `SRC_EQUALS`** — the same mode Shaka itself uses for native HLS on Safari.
- **Absent:** out-of-band text tracks, thumbnails, chapters, `preload`, offline storage, ads, cast,
  low-latency tuning, and the UI (`shaka.ui`). The calls that add them reject the way Shaka rejects
  them in `src=` mode.
- **A key system that is configured but that the platform lacks fails the load with 6001**, whether
  or not the content is encrypted — there is no manifest of ours to inspect first.

## Error codes

All Shaka's own:

| Code | Name | When |
|---|---|---|
| 1001 | `BAD_HTTP_STATUS` | a manifest the network would not deliver |
| 1002 | `HTTP_ERROR` | the same, at the transport level |
| 3016 | `VIDEO_ERROR` (category MEDIA) | media it cannot play, or nowhere to show it |
| 4000 | `UNABLE_TO_GUESS_MANIFEST_TYPE` | a manifest the platform does not play |
| 6001 | `REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE` | the key system is not on this platform |
| 6007 | `LICENSE_REQUEST_FAILED` | the licence or certificate request failed |
| 7000 | `LOAD_INTERRUPTED` | a load replaced by another load, `unload()` or `destroy()` |
| 7002 | `NO_VIDEO_ELEMENT` | nothing attached |
| 7003 | `OBJECT_DESTROYED` | used after `destroy()` |

## Where the platforms differ

One element over three players, and they do not agree about everything. Each difference is
deliberate and asserted per platform by the test suite:

| | Apple (AVPlayer) | Android (ExoPlayer) | Linux (libvlc) |
|---|---|---|---|
| HLS, progressive MP4 | play | play | play |
| DASH | **4000** — AVPlayer has none | plays | plays |
| ClearKey | **6001** | plays | **6001** |
| Widevine | 6001 | licence through JS | 6001 |
| FairPlay | licence through JS | 6001 | 6001 |
| Variants | all listed and selectable | all listed and selectable | **only the playing stream** is listed and it cannot be switched |
| Captions | `cuechange` with `activeCues` | `cuechange` with `activeCues` | **drawn by VLC into the picture**; `cuechange` never fires |
| Cue times | a WebVTT cue's own start and end | `startTime` is when the cue appeared, `endTime` is NaN | — |
| Live window | AVPlayer's seekable range, three target durations behind the edge | ExoPlayer's window | VLC's |
| Buffered | AVPlayer's loaded ranges | ExoPlayer's buffered position | **an estimate** — libvlc 3 does not say |
| Two planes overlapping | stacked by `z-index` | **the newer plane is behind** | **the newer plane is behind** |
| No video output | — | — | no Wayland: **3016**, naming Wayland |
