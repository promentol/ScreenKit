# SDL3 hello world — iOS / tvOS simulator PoC

A single-file C++ SDL3 app that draws an animated "HELLO, WORLD!" using a
built-in 5x7 bitmap font. No assets, no font library, nothing to bundle.

## Layout

    CMakeLists.txt        SDL3 pulled at a pinned tag via FetchContent, built static
    cmake/Info.plist.in   bundle plist (UILaunchScreen matters — see below)
    src/main.cpp          the whole app
    scripts/build.sh      configure + build for a simulator SDK
    scripts/run.sh        boot a sim, install, launch with the console attached

## Build and run

    scripts/build.sh ios     &&  scripts/run.sh ios
    scripts/build.sh tvos    &&  scripts/run.sh tvos

`run.sh` takes an optional device name as its second argument:

    scripts/run.sh ios "iPad Pro 11-inch (M5)"

The first build clones and compiles SDL3, so it takes a few minutes. Later
builds reuse `build/<platform>/_deps`.

## Notes

- **SDL3, not SDL2.** SDL3 is the current release line and has a much simpler
  Apple build story.
- **Static SDL3.** A simulator `.app` can't load an unsigned dylib out of its
  own bundle without extra signing work, so `SDL_SHARED=OFF`.
- **Main callbacks.** `SDL_MAIN_USE_CALLBACKS` + `SDL_AppIterate` instead of a
  `while (running)` loop. On iOS/tvOS a blocking loop in `main()` starves the
  UIKit run loop; the callback form hands control back between frames.
- **`UILaunchScreen` in Info.plist.** Without it iOS runs the app letterboxed
  at a legacy resolution instead of the device's native size.
- **`SDL_WINDOW_RESIZABLE`.** Not cosmetic on iOS: SDL locks a non-resizable
  window to the orientation implied by its requested size, so the 1280x720 hint
  forced landscape and the app rendered sideways on a portrait phone. The
  layout is derived from the drawable each frame, so resizable is correct here.
- **One SDL checkout, two build trees.** `build/sdl3-src` is shared; each
  platform gets its own `build/<platform>/_deps/sdl3-build`. Pointing
  `FETCHCONTENT_BASE_DIR` at a shared dir instead would make the two platforms
  overwrite each other's `libSDL3.a`.
- **tvOS safe area.** Content is inset 5% on tvOS to stay inside the
  title-safe area.
- **Code signing is off** (`CODE_SIGNING_ALLOWED=NO`). Simulator only — a
  device build needs a real team and provisioning profile.

## tvOS runtime

The tvOS *SDK* ships with Xcode but the *simulator runtime* is a separate
download. If `run.sh tvos` reports no matching device:

    xcodebuild -downloadPlatform tvOS
