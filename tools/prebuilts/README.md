# tools/prebuilts

Third-party binaries, fetched and verified rather than built: archives by sha256,
source checkouts by commit id.

> **Acceptance test: a fresh clone reaches a running app without compiling any
> third-party code.**

```sh
node tools/prebuilts/fetch.mjs --list                     # targets and known gaps
node tools/prebuilts/fetch.mjs                            # every dep, this host
node tools/prebuilts/fetch.mjs apple-tvos-simulator-arm64
node tools/prebuilts/fetch.mjs android-arm64 android-arm32  # the AARs the Android build consumes
node tools/prebuilts/fetch.mjs --dep hermes               # one dep only
node tools/prebuilts/fetch.mjs --hermesc                  # prints the host hermesc path
node tools/prebuilts/fetch.mjs --all                       # every target, ~270 MB more since Android joined
```

A target one dep does not publish is skipped with its reason rather than failing
the run — Hermes has no Windows runtime, ANGLE has no Android one.
Naming a dep with `--dep` makes it exact again. `--all` now includes
`android-arm64` and `android-arm32`, which download the three AARs and
`react-android` (about 270 MB before caching); an Android build asks for those
two targets and those three deps by name instead.

Cache root: `~/.screenkit/prebuilts/<dep>/<version>/`, shared across checkouts
and overridable with `SCREENKIT_PREBUILTS`. Below that the two deps differ,
because their upstreams do:

```
angle/chromium-7578/<target>/          one archive per target
hermes/260318099.0.2/apple/            one archive for every Apple slice
hermes/260318099.0.2/android/          the hermes-android .aar, libjsi.so per ABI, the JSI headers
hermes/260318099.0.2/linux-<arch>/     one archive per Linux target, built by our recipe
hermes/260318099.0.2/source/           the pinned facebook/hermes checkout the recipe builds from
hermes/260318099.0.2/compiler/         hermesc, all hosts
sdl3/3.4.16/{apple,android}/           SDL3.xcframework; SDL3-3.4.16.aar
sdl3_ttf/3.2.2/{apple,android}/        SDL3_ttf.xcframework; SDL3_ttf-3.2.2.aar
```

ANGLE publishes a separate archive per target, so each gets its own directory.
Hermes ships every Apple slice in one `destroot`, so it unpacks once and a
target key only selects which framework path inside it the build links. Android
works the same way one level up: each dependency publishes one `.aar` carrying
every ABI, so both Android targets are that file, and the build picks the ABI.

Every download is verified against the sha256 in `manifest.json` before it is
unpacked, and a `.verified` stamp in each directory makes re-runs free. The stamp
is trusted only while the files a build reads out of the entry still exist, so a
cache with a deleted framework or `hermesc` is refetched rather than reported as
cached. An entry is fetched under `<entry>.lock` and unpacked into a staging
directory that replaces it only when complete: two fetches at once -- CMake's
auto-fetch and a build script, say -- download once, and the second waits and
then finds it cached. A lock whose process has exited is taken over.

## ANGLE

`manifest.json` pins `chromium/7578` from `godotengine/godot-angle-static`.

**Why Godot's builds.** Google publishes no ANGLE binaries at all, and Expo ships
none either — `expo-gl` uses Apple's deprecated EAGL. Godot's static builds are
the only public option. Building from source instead needs depot_tools and
10–15 GB; prebuilt libs plus headers are **~22 MB per target**. Shipping should
run our own ANGLE build in CI and publish the artifacts, rather than depending on
another project's release cadence.

Headers come from a sparse checkout of `google/angle`'s `include/` (~1 MB), so no
depot_tools is needed for those either. The checkout is pinned to
`angle.headers.ref`, the commit the libraries were built from. It is the head of
`chromium/7578`, and the libraries report it in `GL_VERSION`
(`git hash: aaebda1c5a40`), which the `gl-context` test checks. A commit id is
content-addressed, so fetching by it is the verification. The cache is reused
only when `HEAD` is that commit and `include/` is unmodified, and CMake refetches
when the stamp disagrees with the manifest. Before this pin the fetch took the
default branch's tip, which had drifted 22 headers from the libraries, including
`gl3.h`, `gl2ext.h` and `eglext_angle.h`.

### Targets

| | |
|---|---|
| Apple | `apple-{tvos,ios}-{simulator,device}-arm64`, `apple-ios-simulator-x86_64`, `apple-macos-{arm64,x86_64}` |
| Linux | `linux-{x86_64,x86_32,arm64}` |
| Windows | `windows-{x86_64,arm64}` |

### ANGLE tvOS is derived, not published

There is no upstream tvOS build. Both tvOS targets are produced from the iOS
archives by `retag-macho.py`, which rewrites `LC_BUILD_VERSION.platform`:

| target | from | to |
|---|---|---|
| `apple-tvos-simulator-arm64` | `ios-simulator` (7) | `tvos-simulator` (8) |
| `apple-tvos-device-arm64` | `ios` (2) | `tvos` (3) |

Without it the linker refuses outright: *"building for 'tvOS-simulator', but
linking in object file built for 'iOS-simulator'"*. It is an in-place 4-byte
patch because the field keeps its size — `vtool -set-build-version` cannot resize
load commands in a `.o` ("not enough space to hold load commands"), and that
vtool's platform table has `iossim` but no tvOS-simulator entry at all.

This is sound only because iOS and tvOS arm64 are the same code for the same
CPU, and ANGLE's Metal backend touches no iOS-only API. **It is spike
scaffolding.** The real fix is a source build with gn `target_platform="tvos"`,
which Chromium's build system genuinely supports — see `poc/angle-tvos/README.md`.

### Android has no prebuilt ANGLE

None exists publicly. Android ships ANGLE as an optional *system driver*, not a
linkable library. Android also has universal native GLES 3.x, so ANGLE there buys
driver normalisation rather than capability — worth a source build only if driver
bugs force it. `fetch.mjs` fails with that explanation rather than a 404.

## Hermes

`manifest.json` pins `260318099.0.2`, which is `HERMES_VERSION_NAME` from
`facebook/react-native`'s `sdks/hermes-engine/version.properties`. That one
number is the pin for **both** the engine and `hermesc`, which is the point: a
`.hbc` compiled by any other `hermesc` is refused at load time rather than
faulting inside the VM.

| | |
|---|---|
| Runtime | `com.facebook.hermes:hermes-ios` on Maven Central, `hermes-ios-$V-hermes-ios-release.tar.gz` (24 MB) |
| Compiler | npm `hermes-compiler@$V`, `hermesc/{osx-bin,linux64-bin,win64-bin}/hermesc` (19 MB) |
| Bytecode version | 99 — recorded in the manifest and asserted against the engine by `runtime/tests` |

**One archive, sliced per target.** The tarball's `destroot/` holds the macOS
framework *and* a universal xcframework — hence the `apple/` cache directory
described above. `manifest.json` owns the target-key-to-framework-path mapping;
CMake reads it with `string(JSON …)`.

Three facts that are easy to get wrong:

- The framework is **`hermesvm`**, not `hermes`.
- **Headers live only under `destroot/include/`** (`jsi/jsi.h`, `hermes/hermes.h`).
  The frameworks carry no `Headers` directory, so that path has to be on the
  header search path explicitly.
- **`hermesvm` exports the JSI implementation itself** — 466 `facebook::jsi::*`
  symbols. A non-React-Native embedder links that one framework and never
  vendors ReactCommon or compiles `jsi.cpp`.

Unlike ANGLE, Hermes publishes **first-class `tvos-arm64` and
`tvos-arm64_x86_64-simulator` slices**, so `retag-macho.py` is never applied to
it. macOS comes from a separate versioned framework under `Frameworks/macosx`;
the xcframework has no macOS slice, only `ios-arm64_x86_64-maccatalyst`.

### Hermes on Linux: our own prebuilt

Nobody publishes a Linux Hermes runtime — Maven carries only `hermes-android` and
`hermes-ios`, and `facebook/hermes` releases stop at v0.13.0 with host executables
only. So Linux is the one place the "never build" rule is kept by building once
ourselves: `recipes/hermes-linux/build.sh` builds the tag `hermes-v260318099.0.2`
(commit `hermes.linux.commit`, the same Hermes the Apple framework and `hermesc`
come from) in Docker, and `manifest.json` pins each archive by sha256.

```sh
tools/prebuilts/recipes/hermes-linux/build.sh              # linux-arm64 and linux-x86_64
tools/prebuilts/recipes/hermes-linux/build.sh linux-arm64  # one target
node tools/prebuilts/fetch.mjs --dep hermes linux-arm64    # verify and install into the cache
```

| | |
|---|---|
| Targets | `linux-arm64` (a Raspberry Pi 3B+ or newer on a 64-bit OS, Batocera aarch64), `linux-x86_64` |
| Toolchain | Debian 12 (bookworm) image pinned by digest, GCC 12, CMake, Ninja — `recipes/hermes-linux/Dockerfile` |
| Runs on | glibc ≥ 2.34 and libstdc++ from GCC 12 (`GLIBCXX_3.4.30`) or newer, as measured from the library's symbol versions: Raspberry Pi OS bookworm, Debian 12, Ubuntu 22.04+. `BUILDINFO.json` in each archive records them |
| Size | 16 MB compressed per target, most of it ICU's data |
| Archive | `lib/libhermesvm.so` with the ICU 72 libraries it loads beside it, `bin/hermesc` (native to the target), `include/{hermes,jsi}` laid out like the Apple `destroot/include`, `LICENSE`, `BUILDINFO.json` |

Built with React Native's release flags for its Apple framework (`MinSizeRel`, no
debugger, Intl on). **Intl on Linux is ICU**, and the archive carries it: the
system's ICU has a different soname on every distribution (72 on Debian 12, 73 on
Batocera 42), and Hermes cannot link a static ICU into a shared library, so Debian's
ICU 72 shared libraries sit in `lib/` beside `libhermesvm.so`, found through an
`$ORIGIN` runpath. ICU versions its symbols, so another ICU loaded in the same
process does not collide. Beyond ICU the library needs only libc, libm, libgcc and
libstdc++. Hermes' ICU backend is itself unfinished: `Intl.Collator` and
`Intl.DateTimeFormat` are real, `Intl.NumberFormat` and `toLocaleUpperCase` /
`toLocaleLowerCase` are placeholders, which the DOM shim replaces with plain
versions. Like the Apple framework, **the library exports the JSI implementation**
— an embedder links `libhermesvm.so` alone.

Before an archive is written, the recipe smoke-tests it in its own architecture
(`recipes/hermes-linux/smoke.cpp`): a program links the packaged headers and
library the way the runtime would, loads bytecode compiled by the **pinned
`hermesc` on the host** (the bytecode-version parity the manifest promises), runs
the same program from source -- including Intl collation and date formatting, with
the system's ICU moved aside so the bundled one is what loads -- and checks the
library's own `hermesc` writes bytecode it accepts.

**Where `fetch.mjs` gets the archive**, first match wins: `hermes.linux.url` once
the archives are hosted, `$SCREENKIT_PREBUILTS_MIRROR/<asset>` (a directory or a
base URL), then `tools/prebuilts/dist/` where the recipe writes them. All three are
checked against the same sha256. Until they are hosted, a broad run skips a Linux
target nobody has built rather than failing.

The packaging is deterministic (sorted, root-owned, commit timestamp, `gzip -n`),
but the compile is only as repeatable as the image's GCC: a rebuild after a Debian
update can differ, which is why the manifest pins the published archive's bytes,
not the recipe.

### Hermes with the JIT (Linux arm64)

`HERMES_JIT=2 recipes/hermes-linux/build.sh linux-arm64` builds the same Hermes with its arm64 JIT
and writes a separate `hermes-jit-linux-arm64-<version>.tar.gz`, leaving the shipping archive alone.
The flag has to reach the compiler, not only CMake: `HERMESVM_ALLOW_JIT` as a CMake variable gates
the JIT *unit tests*, while `VM/JIT/Config.h` reads it as a preprocessor macro, so passing it to
cmake alone silently produces a library without a JIT. The recipe puts it in `CMAKE_CXX_FLAGS` and
its smoke test refuses the archive unless the JIT beats the interpreter by 20% on optimised bytecode
(it measures about 3.6x). The runtime still has to ask for it: `SCREENKIT_HERMES_JIT=1` or `force`.
Measured on a Raspberry Pi in `EMBEDDED_LINUX_EXPERIMENTS.md`.

### Hermes gaps

- **No 32-bit ARM (`armhf`) build.** A Raspberry Pi 3B+ on 32-bit Raspberry Pi OS
  needs one; the recipe's Docker host here has no `linux/arm/v7` emulation. Hermes
  builds for 32-bit ARM (Android ships it), so it is a matter of a host or
  emulator, not of the code.
- **No `android-x86_64`.** Deferred by choice, not missing upstream: the AARs
  carry x86 and x86_64, but the Android build targets the ARM ABIs TVs ship, and
  an arm64 Mac runs the arm64 emulator image. `--list` says so.

## Android: three AARs, and what they leave out

The Android build (`runtime/android/`, through Gradle) consumes official AARs as
prefab packages. `fetch.mjs android-arm64 android-arm32` verifies each against
`manifest.json` and caches it under `<dep>/<version>/android/`; nothing is built.

| | Artifact | Prefab | Size |
|---|---|---|---|
| Hermes | `com.facebook.hermes:hermes-android:260318099.0.2`, release `.aar` from Maven Central | `hermes-engine::hermesvm` (api 24, NDK 27, `c++_shared`) | 81 MB |
| SDL3 | `SDL3-devel-3.4.16-android.zip` from the SDL release → `SDL3-3.4.16.aar` | `SDL3::SDL3-shared` (api 21, no STL), plus `org.libsdl.app.SDLActivity` | 16 MB |
| SDL3_ttf | `SDL3_ttf-devel-3.2.2-android.zip` → `SDL3_ttf-3.2.2.aar` | `SDL3_ttf::SDL3_ttf-shared` (api 19), FreeType and HarfBuzz inside | 21 MB |

Only the `.aar` (and the licence) is kept from SDL's zips; entries are read out
of the verified download in memory, so a release zip is never written to disk.

**Hermes on Android is not self-contained**, which the Apple framework is:

- **The prefab excludes `jsi/**`.** `include/jsi/` is copied from this Hermes's
  Apple `destroot` -- byte-identical to React Native 0.88.0-rc.0's copy.
- **`libhermesvm.so` exports no JSI implementation.** Its `DT_NEEDED` are
  `libjsi.so`, `libfbjni.so`, `libc++_shared.so` and the system's `liblog`/`libm`/
  `libdl`/`libc`. The Apple framework's 466 `facebook::jsi` symbols are 0 here.
  `libjsi.so` is taken from `com.facebook.react:react-android:0.88.0-rc.0` -- the
  React Native release that pins `hermes-compiler` 260318099.0.2 -- whose 164 MB
  `.aar` is read in memory for `jni/<abi>/libjsi.so` alone, each checked against
  its own sha256 in the manifest. It needs only `libc++_shared.so`.
- **fbjni** is `com.facebook.fbjni:fbjni:0.7.0`, the version hermes-android's POM
  names, resolved by Gradle and checked by `runtime/android/gradle/verification-metadata.xml`.
  Hermes's `Intl` on Android is Java (`com.facebook.hermes.intl`) reached through
  it; the host loads `libfbjni.so` from Java so fbjni has its `JavaVM`.

The cache entry `hermes/260318099.0.2/android/` is stamped with all three sha256s
(the AAR, React Native's AAR, the Apple destroot), so moving any pin refetches it.
