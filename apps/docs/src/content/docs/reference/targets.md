---
title: Targets and what runs where
description: The four targets, the graphics and media backend on each, and an honest account of what has actually been run on hardware.
---

| Target | Status | Graphics | Media | HTTP |
|---|---|---|---|---|
| **tvOS** | primary | ANGLE → Metal | AVPlayer | NSURLSession |
| **Android TV / Fire TV** | shipping | native GLES 3, with a GLES 2 fallback | Media3 ExoPlayer | OkHttp |
| **Embedded Linux** (Batocera) | shipping | native GLES via Mesa | libvlc | vendored cpp-httplib |
| **macOS** | development only | ANGLE → Metal | AVPlayer | NSURLSession |

The same `.skpkg` runs on all four.

## Graphics

WebGL 2 needs GLES 3. On a GLES 2-only device `getContext('webgl2')` returns `null` and your app must
feature-detect — Lightning 3 targets WebGL 1, so a Lightning UI is unaffected.

Raw GLES entry points are generated from tables rather than hand-written, and the
`WebGLRenderingContext` object model and its validation sit above them in JavaScript.

:::note
ANGLE is not yet built for Android, so the ANGLE → Vulkan and ANGLE → GLES rungs, the backend probe
cache and the config override are Apple-only for now.
:::

## Linking

Every third-party dependency ships as a CI-built, checksum-verified binary. A fresh clone should
reach a running app without compiling any third-party code.

| | SDL3 / ANGLE | FFmpeg | Hermes | Shared between apps? |
|---|---|---|---|---|
| tvOS / macOS | static (xcframework) | — | static, from the RN tarball | No — Apple has no mechanism |
| Android | `.so` in the APK | — | prefab AAR | No — APK-scoped |
| **Linux** | dynamic, system-shared | the image's own, beneath VLC | dynamic | **Yes** |

Linux is the only platform where sharing is real: the runtime installs once as `libscreenkit.so` plus
shared deps, and an app is a manifest, a `.hbc` and assets. A Batocera image with ten games carries
one runtime — which works precisely because the app model already forbids per-app native code.

## What has actually run on hardware

Worth being blunt about, because "builds for" and "runs on" are different claims:

| Check | State |
|---|---|
| tvOS simulator (Apple TV 4K, 3840×2160) | ✅ runs |
| Android TV emulator (`screenkit-tv`, Android 14 arm64) | ✅ runs, APK built and installed |
| Raspberry Pi / Batocera | ✅ video verified on device |
| **Real Apple TV hardware** | ❌ never run |
| **A physical Fire TV or Android TV** | ❌ never run |
| **A GLES 2-only device** (the ES 2 fallback) | ❌ never exercised |
| **`armeabi-v7a`** | built and shipped for Fire TV, executed by nothing |
| **x86\_64 Android** | not built |

The composited-canvas and `<iframe>` paths on the SDL backends (Android, Linux) compile and are
covered by tests on macOS, but **have never executed on those platforms**. A defect found in review —
a second `<iframe>` embedded after a canvas had drawn would have failed on SDL and only on SDL —
is the standing argument for not treating macOS coverage as coverage.

## Not yet built

- **`texImage2D` from a `<video>` element** on any platform.
- **The `Suspended` instance state** and memory-pressure LRU escalation.
- **A dev server with HMR**, and the `screenkit-go` development client.
- **Over-the-air updates**: signed manifests, `runtimeVersion` gating, rollback.
