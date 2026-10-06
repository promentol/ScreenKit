# ANGLE on tvOS — M1 spike

**Result: PASSED.** ANGLE's Metal backend initialises and renders GLES2 on the
Apple TV 4K simulator (tvOS 26.5), at 3840×2160.

```
[angle] EGL_VERSION = 1.5 (ANGLE 2.1.1 git hash: aaebda1c5a40)
[angle] ES 3.2 unavailable (0x3009)
[angle] ES 3.1 unavailable (0x3009)
[angle] requested ES 3.0 -> context created
[angle] Hello, world from ANGLE on tvOS!
[angle] GL_RENDERER = ANGLE (Apple, ANGLE Metal Renderer: Apple tvOS simulator GPU,
                             Version 26.5 (Build 23L470))
[angle] GL_VERSION  = OpenGL ES 3.0 (ANGLE 2.1.1 git hash: aaebda1c5a40)
[angle] GL_SHADING_LANGUAGE_VERSION = OpenGL ES GLSL ES 3.00
[angle] ES3 limits: extensions=103 arrayTexLayers=2048 uboBindings=24 maxSamples=4
[angle] glGetStringi(GL_EXTENSIONS, 0) = GL_AMD_performance_monitor
[angle] 400 glyph instances x 6 verts, drawn in 1 instanced call
```

Two things are deliberate here. The app asks for
`EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` **explicitly**, so a fallback to another
backend fails rather than quietly passing. And it does not merely read
`GL_VERSION` — it *uses* ES3-only functionality, so the claim is load-bearing:
`#version 300 es` shaders with `in`/`out` and layout locations, a vertex array
object, `gl_VertexID` to draw the background with no vertex buffer at all,
`glGetStringi` plus ES3-only limit enums, and `glVertexAttribDivisor` +
`glDrawArraysInstanced` to draw all 400 glyph pixels in one call.

## ES 3.0 is the ceiling

**ES 3.1 and 3.2 both fail with `EGL_BAD_MATCH` (0x3009).** ANGLE's Metal
backend tops out at ES 3.0 here. That is enough for WebGL2, which is defined
against ES 3.0 — but it rules out ES 3.1 compute shaders, SSBOs and image load/
store. Anything in the renderer that would want compute has to be planned around
this, or wait for ANGLE's Metal backend to gain 3.1.

## Run it

```sh
node tools/prebuilts/fetch.mjs apple-tvos-simulator-arm64   # ~22 MB, verified + retagged
poc/angle-tvos/scripts/build-app.sh
```

Prebuilt handling (all platforms, checksums, and the tvOS retag) lives in
`tools/prebuilts/` — see its README.

Then install and launch `build/tvos-simulator/angle_hello.app` on an Apple TV
simulator with `xcrun simctl`.

## How it works, and why it is a spike and not a product

Building ANGLE from source needs depot_tools and **10–15 GB**; a first attempt
filled this machine's disk and died. Instead this uses prebuilt static ANGLE —
**22 MB total** — and three workarounds, each of which is a reason not to ship it:

1. **Prebuilts come from Godot.** `godotengine/godot-angle-static` publishes
   per-platform static ANGLE (tag `chromium/7578`). Google publishes no binaries;
   Expo has no ANGLE at all (`expo-gl` uses Apple's deprecated EAGL).
2. **There is no tvOS build, so we retag the iOS one.** `scripts/retag-tvos.py`
   flips `LC_BUILD_VERSION.platform` from 7 (iOS-simulator) to 8 (tvOS-simulator)
   across 392 objects. Without it: `ld: building for 'tvOS-simulator', but linking
   in object file built for 'iOS-simulator'`. It is an in-place 4-byte patch
   because `vtool -set-build-version` cannot resize load commands in a `.o`
   ("not enough space to hold load commands"), and this vtool's platform name
   table has `iossim` but no tvOS-simulator entry — numeric `8` works.
   This is sound only because iOS-sim and tvOS-sim arm64 are the same code on the
   same host, and ANGLE's Metal backend touches no iOS-only API.
3. **Godot's archives have holes.** Eight symbols are referenced but absent, so
   `src/angle_stubs.mm` supplies them: `angle::GetCurrentSystemTime` and
   `angle::SetCurrentThreadName` are implemented properly; the six `astcenc_*`
   entry points report "unavailable", which is harmless here because ASTC
   decoding only emulates texture formats and this app binds no textures.

## The production path

Build ANGLE from source with `gn target_platform="tvos"`. This is real and
supported, contrary to the original risk assessment:

- `build/config/apple/mobile_config.gni` declares `target_platform` with `"tvos"`
  valid when `is_ios`, in both `simulator` and `device` environments.
- `build/config/ios/ios_sdk.gni` maps it to the `appletvsimulator` / `appletvos`
  SDKs.
- `src/common/platform.h` defines `ANGLE_PLATFORM_APPLETV` from `TARGET_OS_TV`
  and requires a tvOS 17+ SDK. We have 26.5.
- One catch: that file asserts `use_blink=true` for tvOS, which a standalone
  ANGLE build must force even though ANGLE never uses Blink.

`scripts/fetch-angle.sh` and `scripts/build-angle.sh` carry the GN args for that
route. They need ~10–15 GB free, which this machine did not have.

## Not yet proven

- **Real Apple TV hardware.** Simulator only. The simulator GPU path differs from
  a device, and the device build needs `target_environment="device"` plus signing.
- **A properly built tvOS ANGLE.** What ran is iOS code wearing a tvOS label.
- **WebGL conformance.** One triangle and some quads is not a conformance suite.
