---
title: 'M5: ScreenKit Vite plugin + `screenkit bundle` -> `.skpkg` the host runs'
type: 'feature'
created: '2026-09-16'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Getting a Vite app onto a TV is a hand-run recipe per app: copy a `vite.screenkit.config.js`,
run `pack.mjs --hbc`, stage `app.hbc` + `fonts/` in a scratch directory, rebuild the tvOS host with
`SCREENKIT_APP_DIR`. There is no package format, no manifest, and nothing checks that a bundle can run
on this runtime before it is evaluated.

**Approach:** Turn the recipe into the M5 pipeline — a Vite plugin the app adds, a `screenkit bundle`
CLI that turns the finished `dist/` into a `.skpkg` (bytecode, assets, `manifest.json`), and host support
that loads a `.skpkg` and refuses one it cannot run. Built from the pieces that already work (legacy
build, `tools/vite-screenkit/lightning.mjs`, `tools/vite-legacy-pack/pack.mjs`), not rewritten.

**Decisions taken at planning:**
- **Dynamic `import()` is supported when the build can resolve it.** A literal import of a chunk in the
  build is packed and served from the package; `import(variable)` or a chunk missing from `dist/` fails
  the build. This replaces M5's "a dynamic `import()` fails the build", written for the flatten-to-IIFE
  plan; Architecture §6.1, §6.4 and the M5 row are updated to match.
- **A `.skpkg` is a directory**, `app.skpkg/` (manifest + files). No native unzip; zipping for OTA is M12.
- **The tooling is `packages/@screenkit/vite-plugin` and `packages/@screenkit/cli`**, plain ESM JavaScript
  with no build step, consumed as `file:` dependencies. TypeScript and the pnpm workspace wait for M0.
- **Acceptance:** both POCs run from a `.skpkg` on tvOS; `poc/blits-example-app` only has to bundle.
  Making it run is a separate goal.
- **The spec is kept whole** (~3,100 tokens) as one pipeline, by choice.

## Boundaries & Constraints

**Always:**
- The app's browser build is unaffected; ScreenKit support is additive.
- `screenkit bundle` compiles with the pinned `hermesc` (`fetch.mjs --hermesc`) and `-O -Xes6-block-scoping`;
  `hermesBytecodeVersion` in the manifest is read from the produced `.hbc` header, never assumed.
- The host gates on the manifest **before evaluating anything** from the package: bytecode version must
  equal `Runtime::hermesBytecodeVersion()`, `runtimeVersion` must be supported. Refusal is a logged error
  naming both sides and a failed launch, never a crash.
- Asset root = the package directory, so HostIO confinement is unchanged.
- Every `screenkit bundle` failure exits non-zero with a message naming the cause.
- Works with Vite 7 + plugin-legacy 7 (POCs) and Vite 8 + plugin-legacy 8 (`poc/blits-example-app`);
  plugin-legacy is resolved from the app, not bundled.

**Never:**
- Dev server, module runner or HMR (M7); OTA, signing or hash verification at launch (M12); iframes (M9);
  fetching packages over a network.
- App-compatibility fixes in the DOM shim (e.g. the example app's `window.URL`) — a separate goal.
- Hand-editing vendored trees; per-app native compilation.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Bundle | legacy `dist/` of `poc/lightning3-blits` | `app.skpkg/` with `manifest.json`, `app.hbc`, assets | exit 0 |
| Not a ScreenKit build | plain `vite build` output (no `vite-legacy-entry`) | no package | exit ≠ 0: says to add the plugin |
| Missing chunk | a registered dependency absent from `dist/` | no package | exit ≠ 0, names the chunk |
| Resolvable `import()` | a literal `import('./pages/Loading.js')` whose chunk is in `dist/` | packed; loads from the package at runtime | exit 0 |
| Unresolvable `import()` | `import(variable)`, or a literal import of a chunk not in `dist/` | no package | exit ≠ 0, names the chunk and the call site |
| hermesc rejects code | a chunk with syntax Hermes cannot compile | no package | exit ≠ 0 with hermesc's first diagnostic |
| Host runs package | macOS `screenkit-host [--window] app.skpkg`; tvOS app embedding `app.skpkg` | app runs; `/fonts/...` resolves inside the package | — |
| Bytecode mismatch | manifest `hermesBytecodeVersion` ≠ runtime's | nothing evaluated | error naming both versions, launch fails |
| Runtime too old | manifest `runtimeVersion` > supported | nothing evaluated | error naming both, launch fails |
| Broken package | `manifest.json` missing/unparseable, or entry missing | nothing evaluated | error naming the file, launch fails |

</frozen-after-approval>

## Code Map

- `tools/vite-legacy-pack/pack.mjs` -- packer: reads `vite-legacy-polyfill`/`vite-legacy-entry` from `index.html`, captures each chunk's `System.register`, overrides `System.instantiate`; `--hbc` compiles with pinned hermesc flags. Moves into the CLI; existing POC `app.js` staying byte-identical is the regression check.
- `tools/vite-screenkit/lightning.mjs` -- `lightningOnScreenKit` (CanvasTextRenderer exclusion, Hermes bit-exact quad copy); id-matched transforms that fail loudly on anchor drift, harmless for non-Lightning apps. Moves into the plugin.
- `poc/lightning3-blits/`, `poc/lightning3-shaders/` -- `vite.screenkit.config.js`, `build:screenkit` script, README staging recipe: become plugin + `screenkit bundle`.
- `poc/blits-example-app/` -- upstream `lightning-js/blits-example-app` @ `7adc3e033f18daf4ec9318a3f13d7304f4f41558`; Vite 8.2.2, plugin-legacy 8.2.3, Blits 2.8.9. Verified at planning: legacy build + pack = 3 chunks incl. lazy `Loading`, 1.6 MB JS → 2.9 MB `.hbc`; boots, then fails in shaka-player on `window.URL` (out of scope). Its own `vite.config.js` is left alone.
- `runtime/apple/HostMain.mm` -- `bundledBundlePath()` (~L284, `app`/`triangle`/`timers`/`hello` `.hbc`), `setAssetRoot()` (~L201, asset root = bundle dir), `runWindowed`/`runBundle`/`main` (~L480–590), `SDL_AppInit` (~L387). Package open + gate goes here for both hosts; plain `.hbc` fixtures keep working.
- `runtime/apple/CMakeLists.txt` -- `SCREENKIT_APP_DIR` copy + `LINK_DEPENDS` (~L80–110); keep the stale-resource lesson in whatever replaces it. `runtime/scripts/build-tvos-simulator.sh` forwards it.
- `runtime/core/src/hermes/BytecodeLoader.cpp` -- reads the HBC header version, refuses mismatch (L21, L143): match its wording. `Runtime::hermesBytecodeVersion()` in `runtime/core/include/screenkit/Runtime.h`.
- `runtime/core/src/bindings/HostIO.cpp` -- write-once asset root, realpath confinement: reuse, do not loosen.
- `tools/prebuilts/manifest.json` (`hermes.bytecodeVersion` 99), `tools/prebuilts/fetch.mjs --hermesc`.
- `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`, `runtime/tests/make-fixtures.mjs` -- `host-runs-hello` / `host-refuses-throws` show how host smoke rows check exit code and output.
- No `packages/` directory or root `package.json` exists; Architecture §11 is the target layout, not the current one.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/VERSION` -- single `runtimeVersion` integer, read by the CLI (written into manifests) and compiled into the host (the gate) -- one source, so they cannot drift
- [x] `packages/@screenkit/vite-plugin/` -- `screenkit(options)` returning the Lightning fixes plus plugin-legacy (resolved from the app root; targets `defaults, not IE 11`, `renderModernChunks: false`, `polyfills: true`), default `outDir: 'dist-screenkit'`; delete `tools/vite-screenkit/` -- the "vite build" half
- [x] `packages/@screenkit/cli/` -- `screenkit bundle [dist] [--out app.skpkg]`: pack, resolve the chunk graph and every dynamic `import()` against the packed chunks (fail on unresolvable ones), hermesc compile, copy assets (not the legacy chunks or `index.html`), read `.hbc` version, write `manifest.json`; delete `tools/vite-legacy-pack/` -- the packaging half
- [x] `packages/@screenkit/cli/test/` -- `node --test` over small fixture `dist/` trees covering every bundle row of the I/O matrix
- [x] `runtime/apple/HostMain.mm` -- open a `.skpkg`: parse `manifest.json` with the runtime's `JSON.parse` (no new third-party code), gate versions and entry, set the asset root to the package, evaluate the entry; macOS accepts a package path in both run modes; tvOS prefers an embedded `app.skpkg`
- [x] `runtime/apple/CMakeLists.txt`, `runtime/scripts/build-tvos-simulator.sh` -- `SCREENKIT_APP_PKG` replaces `SCREENKIT_APP_DIR`: embeds the package as `app.skpkg/`, fatal without `manifest.json`, `LINK_DEPENDS` on its files
- [x] `runtime/tests/` -- fixture packages from `make-fixtures.mjs`; host rows: runs a package, refuses bytecode mismatch, refuses newer runtimeVersion, refuses missing entry / bad manifest
- [x] `poc/lightning3-blits/`, `poc/lightning3-shaders/`, `poc/blits-example-app/` -- ScreenKit config uses the plugin, `build:screenkit` ends in `screenkit bundle`; READMEs replace the staging recipe
- [x] `Architecture.md` -- §6.1, §6.4 and the M5 row match the decided pipeline and dynamic-import policy

**Acceptance Criteria:**
- Given `poc/lightning3-blits` and `poc/lightning3-shaders`, when `npm run build:screenkit` and the tvOS host is built with `SCREENKIT_APP_PKG`, then each renders on the Apple TV simulator with 0 errors and the same sampled pixels as its pre-M5 verified run.
- Given those packages, when run with `screenkit-host --window <pkg>` on macOS, then they render with 0 errors.
- Given `poc/blits-example-app`, when `npm run build:screenkit`, then a package is produced whose lazy `Loading` chunk is inside it.
- Given a hello/triangle `.hbc` fixture, when run as before, then the M3/M4 host rows still pass.
- Given the full ctest suite, when run on macOS and under ASan, then every row passes.

## Implementation Notes

- 2026-09-16, after review, at the user's request ("solve them all"): the 15 rejected findings were
  fixed anyway -- 9 distinct problems. Plugin: forces `outDir`/`emptyOutDir`/`base: '/'` with a note
  per override, refuses plugin-legacy twice, resolves it from root, then npm_package_json, then cwd;
  the example app's ScreenKit config drops `--browser_version` presets. CLI: symlinks followed only
  inside the build output (dangling, loop and escape refused by name); optional-call `context.import`
  followed, any other use of the context as a value refused, a destructured context parameter and a
  named `System.register` refused; the entry packed whatever its name; stale polyfills bundles neither
  packed nor shipped; a non-root base named in the missing-chunk error; runtime/VERSION bounded to
  1..4294967295 in the CLI and CMake. Host: a `.skpkg` path that is not a directory is refused by name.
  Tests: CLI 25, plugin 14, ctest 74 (macOS and ASan) incl. `host-refuses-package-file`/`-absent` and
  a `vite-plugin` row; each new test was confirmed to fail with its fix removed. All three apps rebuild
  to byte-identical packed scripts.

## Spec Change Log

## Review Triage Log

| Finding | Verdict | Evidence | Route |
|---|---|---|---|
| BH1 removePreviousPackage deletes any dir with a manifest.json or an empty dir | high | bundle.js isPackage = dir && (exists manifest.json || empty); `--out public` with a PWA manifest is rm -rf'd before any validation. Same root as ECH1. | patch |
| BH2 entry import failure only console.errors; host exits 0 | medium | pack.js wrapper `.catch(console.error)`; VG confirmed exit 0 on a throwing entry. The wrapper is byte-identical to pre-M5 tools/vite-legacy-pack (baseline has the same `entry failed` catch), so pre-existing; the fix needs a host-level unhandled-rejection signal. Same root as VG-O1. | defer |
| BH3 on-device stack traces cannot be mapped | medium | bundle.js writes app.js into staging and rmSync's it; pre-M5 pack.mjs left dist-screenkit/app.js beside app.hbc, so `app.js:line:col` from a TV was decodable before and is not now. A regression caused by the change. | patch |
| BH4 frozen banner/error prefixes name the deleted tools/vite-legacy-pack; the byte-identity 'regression check' comment is unrunnable | low | pack.js banner and `vite-legacy-pack:` messages reach host logs; pack.mjs is deleted in the same diff. Direct text correction. Same root as VG-O2. | patch |
| BH5 plugin yields to an existing outDir; example app's --browser_version presets; plugin-legacy resolved from cwd | low | Only reachable with `npm --browser_version=... run build:screenkit` or running vite from another directory, which fails with the plugin's own clear 'not installed in <root>' error. Fix needs new guards. Same root as ECH11, ECH17. | reject |
| BH6 legacy targets follow browserslist, not Hermes | maybe-false | Would need a list of built-ins the pinned Hermes lacks that `defaults, not IE 11` does not polyfill; none demonstrated (the example app's window.URL stop is a DOM-shim gap, not a transpile target). If real, medium. Targets predate M5. | defer |
| BH7 chunks outside the entry's directory get a misleading 'missing chunk' error | low | readLegacyLayout reads only dirname(entry); a custom chunkFileNames subdir fails loudly but says the existing file is 'not a chunk in dist'. Direct fix: say chunks must sit beside the entry. Same root as ECH16. | patch |
| BH8 copyAssets follows symlinks unguarded | low | Vite's dist-screenkit/ does not contain symlinks in the supported pipeline; guards for loops/escapes add complexity for an undemonstrated state. Same root as ECH4. | reject |
| BH9 staging dir leaks on SIGINT; new outputs not git-ignored | low | SIGINT cleanup needs signal handling (rejected); .gitignore gap is real and direct: poc/lightning3-blits/.gitignore and poc/blits-example-app/.gitignore lack dist-screenkit/ and app.skpkg/, and assertRefused never checks for a leftover .app.skpkg-* staging dir. | patch |
| BH10 host gate refusal branches without rows (format, non-object, runtimeVersion 0, entry dir, symlink escape, >16MB) | medium | make-fixtures.mjs has no fixture for format != 1, non-object manifest, runtimeVersion 0, or entry that is a directory. Same root as VG5. 16MB manifest fixture rejected as not worth its size. | patch |
| BH11 CLI BundleError paths untested; host-run skip passes; fixed 50ms wait; node tests not in ctest | medium | Grouped with VG2 (host skip + ctest wiring) and VG4 (safety refusals). The setTimeout(50) flake claim is unshown: the vm run settles in microtasks. | patch |
| BH12 `m.import?.(...)` misreported as non-literal | low | plugin-legacy's SystemJS transform only emits `context.import(literal)`; optional/aliased/destructured shapes do not occur in the supported pipeline. Same root as ECH8, ECH9, ECH18. | reject |
| BH13 Architecture.md: gate description omits format and entry-inside checks; dropped MSDF/markup-warning/import.meta.url bullets without saying where they went | low | Architecture §6.1 lists only the two version checks; the removed bullets are not marked deferred or covered. Direct doc correction. | patch |
| VG1 nothing tests -Xes6-block-scoping in the CLI compile | medium | Pre-verified: dropping the flag keeps every CLI and ctest row green; a for-let closure logs 3,3,3 without it. | patch |
| VG2 the only CLI-built-package-on-host check passes silently without a host; CLI/host manifest contract can drift | medium | Pre-verified: bundle.test.js returns as passed when runtime/build/macos/screenkit-host is absent; ctest package rows use hand-written manifests. | patch |
| VG3 @screenkit/vite-plugin has no tests | medium | Pre-verified: no test imports screenkit(); a dropped fix or broken outDir hook ships a package that fails only on device. | patch |
| VG4 bundle safety refusals untested (non-package --out, --out inside dist, app.hbc collision) | medium | Pre-verified: runBundle is never called with arguments; no fixture dist holds app.hbc. | patch |
| VG5 host format check untested | medium | Pre-verified: every readable fixture manifest spreads `good` (format 1). Grouped with BH10. | patch |
| VG6 no test runs a package with --window | medium | Pre-verified: host-row.sh never passes --window; runWindowed's gate and target.bundle are unexercised. | patch |
| VG7 tvOS embedded-package path is manual only | medium | Pre-verified: run-tvos-simulator.sh needs triangle.hbc and cannot run a package. tvOS is verified by simulator runs by design; a package mode for the script is its own change. | defer |
| VG-O1 a throwing package entry exits 0 | medium | Same root as BH2. | defer |
| VG-O2 pack.js comment promises an unrunnable regression check; banner names a deleted tool | low | Same root as BH4. | patch |
| ECH1 removePreviousPackage manifest.json check deletes foreign dirs | high | Same root as BH1. | patch |
| ECH2 mkdtempSync makes app.skpkg owner-only | low | Verified: both POC packages are drwx------. Direct fix: chmod the staging dir 0755 before rename. | patch |
| ECH3 App.hbc / Manifest.json collide case-insensitively on macOS | low | copyAssets compares case-sensitively while APFS default is case-insensitive, so an asset `App.hbc` overwrites the compiled entry. Direct fix: compare lower-cased. | patch |
| ECH4 symlink loops/escapes in copyAssets | low | Same root as BH8. | reject |
| ECH5 non-root `base` refused as missing chunk | low | Loud build failure; non-root base is also unsupported by the runtime's asset resolution. Fix is a new guard for an undemonstrated config. | reject |
| ECH6 entry basename without -legacy is not packed | low | plugin-legacy always names the entry *-legacy-*.js; guard for an undemonstrated state. | reject |
| ECH7 named System.register accepted by the graph check | low | plugin-legacy emits anonymous registers only; guard for an undemonstrated state. | reject |
| ECH8 destructured register context skips the import check | low | Same root as BH12. | reject |
| ECH9 aliased/optional context.import not seen | low | Same root as BH12. | reject |
| ECH10 Lightning fixes silently skipped if the module path moves | medium | lightning.js matches exact module ids; a Blits/renderer file move skips the transform with no error and the app crashes on device. The transforms predate M5 (tools/vite-screenkit) and a 'saw module but never matched' check is new surface. | defer |
| ECH11 example app config merges --browser_version presets | low | Same root as BH5. | reject |
| ECH12 runtime/VERSION above 2^32 narrows | low | Not a reachable value for a version counter; guard for an undemonstrated state. | reject |
| ECH13 a non-directory .skpkg path bypasses the gate | false | Verified: a zip-like app.skpkg file goes to evaluateBundle, fails 'Compiling JS failed', and is refused; a plain file path running as a bundle is the documented behaviour, not a gate bypass. | reject |
| ECH14 target.bundle keeps the unresolved entry path after the realpath check | low | openPackage validates `resolved` but evaluates `dir/entry`; a swap in between needs local write access during launch. Direct fix: evaluate the resolved path. | patch |
| ECH15 removed emptyOutDir; polyfill filter narrowed | low | dist-screenkit/ is inside the Vite root, which Vite empties by default; a stale polyfill would fail the graph check loudly ('does not call System.register'). | reject |
| ECH16 chunks outside the entry directory reported as missing | low | Same root as BH7. | patch |
| ECH17 plugin-legacy resolved from cwd, not the Vite root | low | Same root as BH5. | reject |
| ECH18 not every dynamic import shape is checked | low | Same root as BH12. | reject |

## Design Notes

A manifest (the directory form shown; hashes are recorded now and checked in M12):

```json
{ "format": 1, "runtimeVersion": 1, "hermesBytecodeVersion": 99, "entry": "app.hbc",
  "files": { "app.hbc": "<sha256>", "fonts/Lato-Regular.msdf.png": "<sha256>" } }
```

Real Apple TV hardware stays out of M5's gate, as it did for M3 and M4.

## Verification

**Commands:**
- `node --test packages/@screenkit/cli/test` -- expected: every bundle matrix row passes
- `(cd poc/lightning3-blits && npm run build:screenkit)` -- expected: `app.skpkg/manifest.json` with `hermesBytecodeVersion` 99
- `ctest --test-dir runtime/build/macos` and `--test-dir runtime/build/macos-asan` -- expected: 100% pass

**Manual checks:**
- tvOS simulator screenshots of both POCs from packages, pixels sampled against the earlier verified values
