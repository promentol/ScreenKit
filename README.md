# ScreenKit

Run an ordinary web build on TV hardware. Hermes, WebGL and a DOM shim over SDL3, on tvOS,
Android TV, Fire TV and embedded Linux.

Start with [`Architecture.md`](Architecture.md) for the design, or
[`apps/docs`](apps/docs) for the developer documentation.

## Layout

This repository is two halves that meet at a package format.

```
runtime/     the native runtime (C++/CMake) — the DOM shim in runtime/js
packages/    the published JavaScript surface: @screenkit/{cli,shaka,vite-plugin}
apps/        the documentation site, and tooling that is neither an example nor a package
examples/    real apps, and the fleet the device scripts run
tools/       prebuilts, device scripts (android, batocera), vendoring rules
poc/         native spikes kept for reference (SDL3, ANGLE on tvOS, the engine benchmark)
```

The JavaScript half is **one npm workspace**, driven by [Turborepo](https://turborepo.com).

## The JavaScript half

```sh
npm install          # once, at the root — installs every workspace
npm run build        # turbo run build across packages, apps and examples
npm test             # the @screenkit/* package suites
npm run package      # each example's build:screenkit → app.skpkg
```

An example depends on `@screenkit/cli` and `@screenkit/vite-plugin` as workspace packages, so it
always builds against the plugin in the tree beside it — never a published copy.

To work on one thing:

```sh
npm run dev -w @screenkit/docs          # the docs site
npm run build -w examples/pixi-hello    # one example
```

## The native half

```sh
cmake --preset macos && cmake --build runtime/build/macos
ctest --test-dir runtime/build/macos
runtime/build/macos/screenkit-host --window examples/lightning3-blits/app.skpkg
```

Presets live in `runtime/CMakePresets.json` (`macos`, `macos-asan`, `tvos-simulator`). Third-party
dependencies are fetched prebuilt and checksum-verified — a fresh clone should not compile any of
them.

## On a device

The device scripts take an example by directory name:

```sh
sh tools/android/android.sh run pixi-hello     # build, push, launch, tail the log
sh tools/batocera/pi.sh run lightning3-blits   # the same for a Raspberry Pi
```

## Where the rules are written down

- [`Architecture.md`](Architecture.md) — the design, the milestones, and the risks.
- [`runtime/js/README.md`](runtime/js/README.md) — the DOM shim's contract, interface by interface.
- [`_bmad-output/implementation-artifacts/`](_bmad-output/implementation-artifacts) — one spec per
  feature, each with its verification and its review triage log, plus `deferred-work.md`: the single
  list of what is knowingly unfinished.

The last of those is the one to read before believing anything runs on a target you have not tried.
