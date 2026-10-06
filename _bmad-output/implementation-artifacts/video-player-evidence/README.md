# Video player: manual acceptance evidence

Screenshots for the acceptance criteria of
`_bmad-output/implementation-artifacts/spec-video-player.md`.

| File | What it shows |
|---|---|
| `macos-blits-player.png` | The Blits example app's Player page on macOS: the HLS stream playing on an AVPlayer layer beneath the app's canvas, the Blits controls drawn above it, progress 00:11 / 10:34. |
| `tvos-simulator-blits-player.png` | The same page in the tvOS simulator. |
| `android-emulator-blits-player.png` | The same page on the Android emulator, resumed with the remote's play/pause: 00:42 / 10:34 after a pause at 00:38, so progress advances and the remote key works. ExoPlayer on a SurfaceView beneath the translucent SDL surface. |
| `android-emulator-blits-player-paused.png` | The same page paused from the remote at 00:38; the next frame 6 s later is identical. |
| `android-emulator-widevine-demo.png` | Shaka's public Widevine demo stream (Angel One) decrypting and playing on the emulator, loaded through `@screenkit/shaka` with the public licence server. |

The Widevine run's own log, from a one-page package built on
`packages/@screenkit/shaka` and run by the shipping host (not the test binary):

```
I ScreenKit: [screenkit-host] log: widevine loaded 192x144 duration=60 licence requests=1
I ScreenKit: [screenkit-host] log: widevine played currentTime=1.8 decoded=29 dropped=4 licences=1
```

One licence request went out through the networking engine and came back; the
emulator's Widevine is the software L3 HAL, which is why the frame rate is low.

| `pi-blits-player.png` | The same page on the Raspberry Pi 3 under sway: the stream on a Wayland subsurface beneath the app's window, decoded by the ScreenKit VLC plugin on `/dev/video10`, with the Blits controls over it at 00:19 / 10:34. |
| `pi-blits-player-paused.png` | The same page paused from the remote (a virtual keyboard on `/dev/uinput`, since the image has no `wtype`). |

The Pi shots are what found the window-transparency defect: before the fix the
same page decoded every picture and showed none (`2160 committed, 0 presented`).

The Pi's hardware-decode measurement is not a screenshot: it is the
`media-hardware` row (`SCREENKIT_MEDIA_SOAK=1 sh tools/batocera/pi.sh test media-hardware`),
recorded in the spec's Implementation Notes.
