---
title: 'M10: Android TV / Fire TV host — an APK that runs a .skpkg on SDL3, Hermes and GLES'
type: 'feature'
created: '2026-09-17'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/runtime/README.md', '{project-root}/tools/prebuilts/README.md', '{project-root}/EMBEDDED_LINUX_EXPERIMENTS.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** ScreenKit runs on macOS, tvOS and Linux, but Android TV and Fire TV — two of its four targets — have no host at all. Beyond that, the OkHttp network backend (spec-android-okhttp-network.md) is waiting for this host.

**Approach:** An Android application (Gradle + CMake/NDK) whose activity extends SDL's `SDLActivity`, linking the existing C++ core against the official prebuilt AARs:
- `com.facebook.hermes:hermes-android` 260318099.0.2, with JSI headers from the cached Apple destroot;
- SDL3 3.4.16;
- SDL3_ttf 3.2.2.

It renders through SDL's GLES context the way the Linux host does (no ANGLE exists for Android). It runs the same `.skpkg` packages as macOS, tvOS and the Pi, and is verified on an arm64 Android TV emulator.

## Boundaries & Constraints

**Always:**
- **Packages:** the same `.skpkg` and prelude (`dom-shim.hbc`) as the other hosts, with no Android-specific package format.
  - A package bundled in the APK's assets is copied to internal storage when the APK's version changes, and run from there, because all file I/O is POSIX paths.
  - A debug launch can instead name a package directory on the device through an intent extra (`--es package <dir>`), for `adb push` workflows.
- **ABIs and SDK levels:** `arm64-v8a` and `armeabi-v7a` (Fire TV sticks ship a 32-bit userland). minSdk 24 (Hermes' floor), compile/target SDK 35. Leanback launcher category and TV banner, landscape, fullscreen.
- **Graphics:** GLES through SDL (`GlSurfaceSdl`), ES 3 when the device offers it, else ES 2 with the WebGL1 path the Pi uses. `--size`-style fixed size is available through an intent extra.
- **Lifecycle:**
  - Backgrounding the app pauses the runtime (`Runtime::pause`) and releases nothing JS can see.
  - Returning resumes rendering on a recreated surface without restarting the app.
  - **Decision (exit, review pass 1):** Back never finishes the activity by itself. A page leaves the
    app by calling `window.close()`, as a webOS or Tizen app calls its platform's exit; the host
    finishes the activity on that call, and where no host takes it (macOS, Linux, tvOS) `window.close()`
    does nothing. The heuristic this replaces — exit when no listener called `preventDefault()` and the
    URL did not change — exited under ordinary pages: an overlay closed without `preventDefault()`, Back
    handled on keyup or after a timeout, or a gamepad's B button, which `InputRouter` maps to `GoBack`.
- **Logging:** everything the runtime logs reaches logcat under tag `ScreenKit`.
- **Fonts:** system fonts come from `/system/fonts` (Roboto for `sans-serif`, Noto Serif for `serif`, Droid Sans Mono for `monospace`) through the existing `SystemFonts.h` interface.
- **Prebuilts:** Hermes, SDL3 and SDL3_ttf Android artifacts are pinned by sha256 in `tools/prebuilts/manifest.json` and fetched by `fetch.mjs`. Gradle dependencies and the Gradle wrapper are pinned with dependency verification.
- **Unchanged elsewhere:** the macOS, tvOS and Linux builds and the 159-row macOS suite are unaffected.
- **Decision (toolchain):** install into `~/Library/Android/sdk`:
  - cmdline-tools 23.0 (https://dl.google.com/android/repository/commandlinetools-mac_arm64-16111833_latest.zip);
  - NDK r27d `ndk;27.3.13750724`, the NDK the Hermes AAR was built with;
  - `system-images;android-34;android-tv;arm64-v8a`;
  - an AVD `screenkit-tv` created from that image.

  `android.sh sdk` and `android.sh avd` do this, and build and emulator verification run on this Mac.
- **Decision (shell language):** Java, with no Kotlin in the build. This supersedes Architecture.md's "Kotlin AAR + JNI" for the shell, and the doc is updated to say so.

**Never:**
- ANGLE or Vulkan on Android in this milestone.
- Networking beyond the existing Unavailable backend; OkHttp is the next spec.
- Media and `<video>`, iframes, Widevine.
- Building Hermes, SDL3 or SDL3_ttf from source.
- A Play Store listing, signing keys or release publishing (debug signing only).
- Edits to vendored expo-gl.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Bundled app | APK built with `poc/pixi-hello` as its package, launched from the TV launcher | the scene renders fullscreen; canvas text visible | N/A |
| Pushed package | `adb push` of `phaser-hello` and `lightning3-blits` packages, then `am start --es package …` | each renders (Lightning MSDF text included) | a missing or invalid package logs the reason to logcat and finishes the activity |
| Remote input | `adb shell input keyevent DPAD_DOWN/DPAD_CENTER/BACK` in Blits | focus moves, Enter selects, Back goes back | N/A |
| GLES level | emulator ES 3 context | `getContext('webgl2')` available; an ES 2-only device takes the WebGL1 path | N/A |
| Background and return | HOME, then relaunch from recents | same app instance resumes drawing, JS state kept | no crash, no black screen |
| Frame capture | launch with the capture extra | frame PNG written to app storage and pulled with adb | N/A |
| New APK version | reinstall with a changed bundled package | the new package is extracted before running | stale extraction never runs |

</frozen-after-approval>

## Code Map

**Host**
- `runtime/host/Host.{h,cpp}`: the building blocks the Android host uses: `startGraphics` :80, `setAssetRoot` :176, `announceDocumentLoaded` :199, `evaluateDomShim` :206, `appFailed` :464, `resolveLaunch` :475. `runWindowed` (:632-742) owns a blocking loop with Guide-quit, so Android follows the tvOS callbacks model instead.
- `runtime/apple/HostMain.mm:133-310`: the `SDL_MAIN_USE_CALLBACKS` host to mirror. The bundle is evaluated on another thread (:241), frames tick in `SDL_AppIterate` (:276-277), and `reclaimRuntimeEvent` runs first in `SDL_AppEvent` (:287). The bundled package path and storage are :113 and :67-79.
- `runtime/linux/HostMain.cpp`, `runtime/linux/CMakeLists.txt`: model for dom-shim location (:34-45), storage (:49-57), window flags plus `prepareWindowAttributes` (:99-103), and the shell target.

**Core**
- **File I/O is all POSIX**, hence extraction:
  - Host.cpp: `stat` :260, `ifstream` :359, `realpath` :417, `access` :489.
  - `hermes/BytecodeLoader.cpp:82-116` (`mmap`).
  - `bindings/HostIO.cpp:46,127,146,172` (`realpath`, `ifstream`, `stbi_info`).
  - `third_party/gl/SKGLImageUtils.cpp:143` (`stbi_load`), reached via a `file://` path (`js/dom-shim.js:6072`).
- **Graphics:** `core/src/gfx/GlSurfaceSdl.cpp` has nothing Linux-only except its error text (:184,188). It uses the ES 2.0 request (:161-180), creates the context on the JS thread (:196-207), and has the fixed-size present context (:82-157). Risk: SDL's Android pause/resume recreates the EGL surface while the context is current on the JS thread, so the host must pause the runtime, then let the surface be re-bound on resume.
- `core/src/gfx/VendoredWebGL.cpp:124-148` (ES 2/ES 3 extension reading) and `third_party/gl/SKGLNativeContext.cpp:152-154` (WebGL2 from `GL_VERSION`).
- `core/src/text/SystemFonts.h:22-30`: interface. Implementations are `SystemFontsLinux.cpp` (scan :48-84, generics :129-131, the model) and `SystemFontsNone.cpp`. CMake selection is at `runtime/CMakeLists.txt:187-195`.
- `core/src/Log.cpp:31` (stderr) and `apple/PlatformLog.mm`: model for an `__android_log_write` sink.
- `core/src/input/Input.cpp:194-207`: `SDL_SCANCODE_AC_BACK` is back. The D-pad, media and channel keys are at :40-79, with no Android `#if` yet.

**CMake and prebuilts**
- `runtime/CMakeLists.txt`:
  - :25-66: `SCREENKIT_TARGET` mapping; FATAL_ERROR at :63-65 for Android.
  - :100-103 and :154-161: the `else()` branch pulls in `LinuxSystem.cmake` and `GlSurfaceSdl.cpp`.
  - :172-181 net backend: Unavailable on Android.
  - :196 `find_package(ZLIB)` (NDK has it).
  - :299-303 shell subdirectories.
  - :307 tests only `APPLE AND NOT tvOS`.
- `runtime/cmake/LinuxSystem.cmake` (pkg-config/sysroot): not used on Android. Prefab packages come in through Gradle's `find_package(... CONFIG)`.
- `runtime/cmake/HermesPrebuilt.cmake:21-22,50-100`: `lib`-entry path (imported SHARED, `IMPORTED_NO_SONAME`). The HERMESC fallback (:89-97) uses the host hermesc, which suits Android.
- `tools/prebuilts/manifest.json`:
  - Hermes :227-229 is the pin; :328-330 lists Android as unavailable, with the jsi note.
  - SDL3 Android :148-156 is pinned (sha256 `e1da8d92…5765`) but not cached.
  - SDL3_ttf :164-223 is Apple only.
  - `tools/prebuilts/fetch.mjs` must learn the Android slices.
- **Artifacts:**
  - Hermes release AAR: https://repo1.maven.org/maven2/com/facebook/hermes/hermes-android/260318099.0.2/hermes-android-260318099.0.2-release.aar, 85,322,876 B, sha256 `fc61629c65aa471fbbed5d373e0156f5e6082e1f25c8bbb48c8623548194b2d2`. Prefab `hermes-engine`/`hermesvm`, api 24, NDK 27, `c++_shared`; `jni/<abi>` has stripped `.so` files and there are no `jsi/` headers.
  - SDL3_ttf: https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-devel-3.2.2-android.zip, 21,601,182 B, sha256 `48e534daf88eec5c7f0c95fa6ca0e8a5f38f5fc7276824573131cebc403ee14c`.
  - SDL3 AAR: prefab `SDL3`/`SDL3-shared`, api 21, stl none. `classes.jar` carries `org.libsdl.app.SDLActivity`.
- `poc/build/sdl3-src/android-project/app/build.gradle:9-25`, `build.gradle:9`: SDL's app template (AGP 8.7.3, NDK 28.2, minSdk 21, arm64-v8a), the shape for `runtime/android/`.

**POCs and docs**
- `poc/*/app.skpkg` via `npm run build:screenkit` (`tools/batocera/pi.sh apps`): the packages to run. `tools/batocera/pi.sh` is the model for an `android.sh` (build, install, push, run, shot, logs).
- `Architecture.md:6,73,475-483,516-520,578-580,629,681,732,775,801,820`: Android claims to update. The GLES direct path replaces the ANGLE chain until an ANGLE for Android exists, and the M10 exit is only partly met (no Fire TV device, no ANGLE).
- `_bmad-output/implementation-artifacts/deferred-work.md:247-248`: Android input unverified.

**Machine**
- Present: Android SDK platforms 34–36, build-tools 35, platform-tools, emulator 35.1.21 (arm64); Gradle 9.0.0; JDK 23/25.
- Missing: NDK, cmdline-tools, any arm64 TV image (local `android-23/android-tv` is armeabi-v7a only), AVDs.
- arm64 `android-tv` images exist for API 31, 33, 34 and 36.

## Tasks & Acceptance

**Execution:**
- [x] `tools/prebuilts/manifest.json`, `tools/prebuilts/fetch.mjs`, `tools/prebuilts/README.md`:
  - Android entries for Hermes (release AAR, sha256), SDL3 (existing zip → AAR) and SDL3_ttf (android zip → AAR).
  - `fetch.mjs` caches the AARs under `~/.screenkit/prebuilts/<dep>/<ver>/android/`, with Hermes' jsi headers merged from the Apple destroot.
  - Android is removed from `unavailable`.
- [x] `runtime/CMakeLists.txt`, `runtime/cmake/AndroidSystem.cmake` (new):
  - an `Android` system branch: target keys `android-arm64`/`android-arm32`;
  - `find_package` of the prefab `hermes-engine`, `SDL3`, `SDL3_ttf`;
  - GLESv3/EGL/log from the NDK;
  - `GlSurfaceSdl.cpp`, `SystemFontsAndroid.cpp`, `NetworkUnavailable.cpp`;
  - `add_subdirectory(android)` for the shell `.so`.
- [x] `runtime/core/src/text/SystemFontsAndroid.cpp` (new), `runtime/core/src/Log.cpp` or an android log sink: fonts from `/system/fonts` with generic mapping, and logcat output.
- [x] `runtime/android/` (new Gradle project, wrapper pinned; Java sources):
  - `app` module (debug-signed APK) with `externalNativeBuild` → `runtime/CMakeLists.txt`, prefab enabled, and the ABIs and SDK levels from Boundaries;
  - Leanback manifest;
  - an activity extending `SDLActivity` (library list `SDL3`, `SDL3_ttf`, `hermesvm`, `screenkit`);
  - asset extraction keyed by APK version;
  - intent extras for `package`, `size` and capture;
  - `libc++_shared.so` packaged;
  - a Gradle task that compiles the package and `dom-shim.hbc` into assets.
- [x] `runtime/android/jni/HostMain.cpp` (new): the `SDL_MAIN_USE_CALLBACKS` host after `apple/HostMain.mm`. It:
  - resolves the package and dom-shim paths the activity hands over;
  - starts graphics through `GlSurfaceSdl`;
  - pauses and resumes the runtime on SDL's background/foreground events, re-binding the surface;
  - finishes when the page calls `window.close()`;
  - passes storage from `SDL_GetAndroidInternalStoragePath()`.
- [x] `tools/android/android.sh` (new, sh functions like `pi.sh`): `sdk` (installs the toolchain in the decision above), `avd`, `build`, `install`, `push <app>`, `run <app>`, `shot <app>` (capture plus `adb exec-out screencap`), `logs`.
- [x] Docs:
  - `runtime/README.md`: an Android target section.
  - `Architecture.md`: Android graphics chain today, minSdk 24, M10 status.
  - `deferred-work.md`: Fire TV device verification, ANGLE for Android, and x86_64 left out.
  - `tools/prebuilts/README.md`.

**Acceptance Criteria:**
- Given `sh tools/android/android.sh build`, when it runs, then a debug APK containing arm64-v8a and armeabi-v7a libraries is produced.
- Given the `screenkit-tv` emulator (android-tv API 34 arm64), when each matrix row is exercised via `android.sh`, then the observed behavior matches the row, with screenshots saved to `tools/android/out/`.
- Given the macOS suite and `sh tools/batocera/pi.sh build`, when run after these changes, then 159/159 pass and the Linux host still builds.

## Implementation Notes

Where the build differs from the Code Map, and why (the frozen intent is unchanged):

- **Android's hermesvm is not self-contained.** The Code Map says the AAR lacks only the `jsi/` headers. It also
  exports no JSI implementation: `libhermesvm.so`'s `DT_NEEDED` are `libjsi.so`, `libfbjni.so` and
  `libc++_shared.so` (0 `facebook::jsi` symbols, against Apple's 466). So:
  - `libjsi.so` comes prebuilt from `com.facebook.react:react-android:0.88.0-rc.0` (the React Native release
    pinning `hermes-compiler` 260318099.0.2), release AAR sha256-pinned in `manifest.json`, read in memory by
    `fetch.mjs` for `jni/<abi>/libjsi.so` only, each file sha256-pinned too. The JSI headers are still the Apple
    destroot's -- byte-identical to React Native's.
  - fbjni is `com.facebook.fbjni:fbjni:0.7.0` (the version hermes-android's POM names) plus its
    `com.facebook.soloader:nativeloader:0.10.5`, resolved by Gradle under dependency verification. The activity's
    library list is `SDL3, SDL3_ttf, fbjni, hermesvm, screenkit`: fbjni's own `JNI_OnLoad` gives it the JavaVM,
    after `NativeLoader.initIfUninitialized(new SystemDelegate())`.
  - Hermes's Intl on Android is Java through fbjni, and the JS thread (a native SDL thread) cannot load the app's
    classes, so `HostMain.cpp` touches every Intl entry point once on SDL's Java-started main thread first
    (`warmIntl`, deferred-work.md).
- **HermesPrebuilt.cmake** carries the Android branch (prefab `hermes-engine::hermesvm` + `hermes::jsi` + the host
  hermesc); `AndroidSystem.cmake` has SDL3, SDL3_ttf, GLES/EGL and log.
- **Leaving the screen is driven from Java, not from SDL's background event.** SDL dispatches
  `WILL_ENTER_BACKGROUND` inside `SDL_PushEvent`'s watcher lock, and the SDL timer thread holds the runtime's
  locks (TimerRegistry, HermesHost, WorkQueue) while it pushes, so a host that queues a task or pauses the
  runtime there deadlocks -- which the first version did, on the emulator, with Blits' timers running.
  `ScreenKitActivity.onPause` and its surface's `surfaceDestroyed` call `nativeLeaveScreen` (UI thread: pause +
  `GlSurface::suspend` on the JS thread, waited ≤ 1 s) before SDL is told; the return is a lock-free flag that
  `SDL_AppIterate` acts on once SDL's loop resumes with the new surface.
- **The launch extras are debug-only and confined** (review pass 1): the activity is exported, so `package`,
  `size` and `capture` are honoured only when `ApplicationInfo.FLAG_DEBUGGABLE` is set, and `package` and
  `capture` name a file in `files/apps/` and `files/captures/` -- a name, never a path. What is refused is logged.
- **The frame is always offscreen on Android** (fixed size = the screen's unless `size`), so resume re-presents
  the last frame for an idle page. `GlSurface` gained `suspend()` / `resume()` (SDL surface only), and
  `host::startGraphics` an optional weak reference to the surface.
- **Leaving the app** (the exit decision above): SDL traps Back so it reaches the page as a `GoBack` key and
  nothing more. `window.close()` calls a host-defined `__screenkitClose` when there is one, and the Android host
  returns `SDL_APP_SUCCESS` on it; a finishing activity `System.exit`s so the next launch starts clean. Hosts
  that define no hook -- macOS, tvOS, Linux -- make `window.close()` a no-op, as a browser does for a window a
  script did not open. `dom-window-close` covers all three.
- **Remote-input row**: `poc/lightning3-blits` is a static hello world with nothing to focus, so D-pad/Enter/Back
  were driven in `poc/blits-example-app` (the app the tvOS input check used). None of the poc apps calls
  `window.close()`, so leaving the app was checked with a package that does.
- **Toolchain**: AGP 8.13.2, Gradle 8.14.3 (wrapper jar and distribution sha256-checked), Groovy DSL; CMake and
  Ninja from PATH via `cmake.dir` (the root CMakeLists needs 3.24+). `android.sh avd` sets a 2 GB data partition
  and the host GPU. On this Mac the disk was too full for `sdkmanager` to unpack the 8.6 GB android-tv image, so it
  was extracted sparse by hand into the same SDK path (registered `package.xml`); `android.sh sdk` itself is the
  normal sdkmanager install.
- **GLES level row**: ES 3 / WebGL2 verified on the emulator; the ES 2 fallback is unverified (an API 34 emulator
  does not boot with ES 3 disabled -- deferred-work.md).

## Spec Change Log

## Review Triage Log

Pass 1 (review_loop_iteration 0): blind-hunter (B), edge-case-hunter (E), verification-gap (V).

| # | Finding | Verdict | Evidence | Route |
|---|---|---|---|---|
| B1 / E6 | Back closes the app when the page handled it without `preventDefault()` or a URL change (closing an overlay, handling on keyup, an async route change, a gamepad B before `getGamepads()`) | high | `dom-shim.js` `__screenkitKey` exits on "no preventDefault and href unchanged"; `Input.cpp:257` maps gamepad East to GoBack. The frozen intent says "Back at the app's root finishes the activity" but not how "root" is recognised, and there are several readings (heuristic, explicit `window.close()`, listener presence) | intent_gap |
| V1 | No test covers the Back-unhandled hook | medium | Pre-verified: `__screenkitBackUnhandled` appears in no test; the only `__screenkitKey` test sends ArrowUp without the hook | patch (after B1) |
| B3 / E1 | Leaving during `SDL_AppInit` (after graphics, before `gLifecycle` is published) never releases the drawable, and the return never rebinds: black screen | medium | `gLifecycle`/`surface` are set only at the end of `SDL_AppInit`; `returnToScreen` exits early when `backgrounded` is false | patch |
| E4 | A `returnPending` left from a recent `onResume` lets `SDL_AppIterate` resume and rebind during `leaveScreen`'s wait, onto the surface being destroyed | medium | `leaveScreen` sets `backgrounded` but never clears `returnPending`; `SDL_AppIterate` checks it every tick | patch |
| E2 | Tasks queued while paused run before the rebind task, so a synchronous GL call there has no current context | maybe-false | `returnToScreen` queues the rebind and then thaws, so FIFO puts earlier tasks first. expo-gl batches most calls to the frame flush; whether a queued task makes a synchronous GL call (getter or flush) before the rebind is unsettled. If true, medium | defer |
| B4 / E3 | `GlSurface::resume()` clears `suspended_` before the bind succeeds; a failed bind is never retried | low | SDL resumes its loop only once the new surface exists, so the bind failing is unlikely; a retry adds logic | reject |
| B5 | `configChanges` lacks `density`/`colorMode`/`touchscreen`: such a change recreates the activity and SDL's `onCreate` exits the process | low | `SDLActivity.java:355-364` calls `System.exit(0)` on recreate unless allowed; the manifest list lacks those entries. Adding them is a direct correction | patch |
| E7 | An activity destroyed without finishing restarts SDL in the same process | false | `SDLActivity.java:355-364` exits the process on recreate, so the runtime never starts twice in one process | reject |
| B6 | Debug extras (`package` absolute path, `size`, `capture` with absolute or `..` paths) work in every build of an exported activity | medium | `ScreenKitActivity.prepareLaunch` never checks `FLAG_DEBUGGABLE`; any app can launch it pointing `package` at world-readable storage, running that JS in ScreenKit's sandbox | patch |
| B7 / V-o1 / E15 / E16 | `screenkitAssets` ignores `manifest.json` (hermesc pin) and the `packages/@screenkit` sources, and swallows `fetch.mjs --hermesc` stderr | medium | A Hermes pin bump leaves the task UP-TO-DATE, shipping a `dom-shim.hbc` the loader refuses; the build fails with a bare exit code | patch |
| B8 / V-o2 / E17 | `HermesPrebuilt.cmake` says Gradle passes `-DHERMESC`; it does not | low | `app/build.gradle` cmake arguments have no `-DHERMESC`; direct correction | patch |
| B9 | `GlSurface.h`'s platform table puts the Android label on Linux's continuation line | low | Header comment lines 6-10 read as garbled; direct correction | patch |
| B10 | A failed asset copy drops a valid `package` extra and logs "bundles none"; the stale bundle is kept alongside staging on a full disk | medium | `main()` catches `IOException` and empties `mArguments`; `extractBundle` deletes `bundle/` only after staging is copied | patch |
| B15 / E10 | Frame pacing reads the refresh rate only at init | low | `paceFrames` runs once in `SDL_AppInit`; one event case re-applies it on `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED` | patch |
| B16 / E13 | `android.sh` assumes Darwin arm64, and the README lists no prerequisites (JDK, node/npm, CMake ≥ 3.24, ninja) | low | Hard-coded mac_arm64 URL, BSD `sed -i ''`; direct host check and doc line | patch |
| E12 | `android.sh shot` reports a frame when `exec-out` wrote an error text | low | `[ -s ]` passes for any non-empty file; a PNG signature check is a direct correction | patch |
| E14 | `android.sh build` runs `fetch.mjs` for every dep, cloning ANGLE headers an Android build never uses | low | The fetch output shows ANGLE headers fetched or cached for Android targets; `--dep` per dependency is a direct correction | patch |
| B19 / E19 | `manifest.json` lost the `android-x86_64` explanation | low | The hermes `unavailable` entry was deleted with the arm entries; re-adding one line is a direct correction | patch |
| B14 | The deferred-work Intl entry says the robust fix needs Hermes to expose a class loader; fbjni's `ThreadScope::WithClassLoader` exists | low | fbjni ships `facebook::jni::ThreadScope::WithClassLoader`; correct the entry's text | patch |
| E18 | `fetch.mjs --all` now downloads the Android AARs (~270 MB) | low | Android targets joined the normal per-target loop; a README note is a direct correction | patch |
| V2 | Background/return (suspend, rebind, re-present) is verified only by hand | medium | Pre-verified: no harness runs SDL EGL surfaces or the Android lifecycle; filed as defer | defer |
| V3 | Bundle extraction and version keying are verified only by hand | medium | Pre-verified: no `test/` or `androidTest/` source set; filed as defer | defer |
| V4 | Nothing checks the Hermes-Android cache stamp in `fetch.mjs` | medium | Pre-verified: `fetch.mjs` has no test harness; filed as defer | defer |
| B2 | No tests for `Log.cpp` logcat chunking, the zip reader, or Linux suspend/resume | low | Chunking is `__ANDROID__`-only; a broken zip reader fails the fetch loudly; test gaps with no shown defect | reject |
| B11 | First launch copies the package file by file, unmeasured against the 300 ms budget | low | A performance concern with no measurement; a file index or archive is new design | reject |
| B12 | `SystemFontsAndroid.cpp` duplicates `SystemFontsLinux.cpp` | low | Fixes would land twice, but merging is a refactor, not a correction | reject |
| B13 | The font scan opens every font file on the JS thread at first system-font text | maybe-false | Not measured on a device; reading `fonts.xml` instead would settle it. If slow on a Fire TV, medium | defer |
| B17 | Plain `./gradlew assembleDebug` fails without `android.sh` (`cmake.dir`), and the config-time throw breaks `clean`/`tasks` | low | `build.gradle`'s header advertises `./gradlew`; correcting the header is direct, restructuring the check is not | patch (header only) |
| B18 | Release builds (signing, R8 keep rules, versionCode, lint) are neither covered nor tracked | low | The frozen Never excludes release publishing; the gap belongs in deferred-work | defer |
| E5 | A `startGraphics` timeout (20 s) while the context is still being created destroys the window under it | low | Needs a 20 s context creation; the fix reorders teardown | reject |
| E8 | Directory entries inside staging are never fsynced | low | Needs power loss during the first launch after install; files themselves are synced | reject |
| E9 | `onPause` racing `HostState` teardown blocks the UI thread up to 1 s | low | Only at shutdown, bounded at 1 s | reject |
| E11 | `android.sh avd` with another device attached fails `booted()` | low | The script documents `ANDROID_SERIAL`; developer-only | reject |
| E20 | Spec task said pause on SDL's background events; the host pauses from Java | false | The frozen behaviour (backgrounding pauses the runtime) holds; the departure is recorded in Implementation Notes with the deadlock that forced it | reject |

## Verification

**Commands:**
- `sh tools/android/android.sh build`: APK built; `unzip -l` lists `lib/arm64-v8a/{libscreenkit.so,libhermesvm.so,libSDL3.so,libSDL3_ttf.so,libc++_shared.so}` and the same for `armeabi-v7a`.
- `sh tools/android/android.sh run pixi-hello && sh tools/android/android.sh shot pixi-hello`: the screenshot shows the scene; logcat `ScreenKit` has no errors.
- `ctest --test-dir runtime/build/macos`: 159/159.
- `sh tools/batocera/pi.sh build`: succeeds.

**Manual checks (if no CLI):**
- On the emulator: D-pad navigation in `lightning3-blits`, HOME and return resumes, Back at root exits.
