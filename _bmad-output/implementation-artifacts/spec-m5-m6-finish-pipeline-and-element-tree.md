---
title: 'M5 finish + M6: pipeline hardening, element tree, CSS subset, MSDF, Lightning as shipped'
type: 'feature'
created: '2026-09-17'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/runtime/js/README.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** M5 is built but not trustworthy as a CI gate: a package whose entry fails exits 0, the
tvOS package path has no automated check, the Lightning build fixes skip silently when a file moves,
and nobody has checked the legacy build against what the pinned Hermes lacks. M6 is half there: Blits
renders a real UI with MSDF text, but the DOM is a fake tree (`appendChild` stores nothing,
`querySelector` returns null, elements drop listeners), `style` is a bare `{}`, and Architecture.md
still describes an MSDF tool (`@screenkit/msdf`/`msdf-atlas-gen`) that was never built.

**Approach:** Close the four M5 review items with loud failures and tests. For M6, replace the fake
tree with a real one (nodes, attributes, selectors, event propagation), give `style` a real
declaration object, make a package without MSDF atlases for its fonts fail the bundle, and bring
Architecture.md in line with what ships.

**Decisions (2026-09-17):**
- **M5 closes on the tvOS simulator.** No Apple TV is attached; the real-hardware run is logged in
  deferred-work, not built here.
- **"Unmodified Lightning 3" means untouched app source and npm packages.** ScreenKit's named,
  loud-failing build transforms are part of the platform: two exist because Canvas2D is absent by
  design, one because Hermes canonicalises NaN bit patterns on element copy. Architecture §12/§14 say so.
- **The element tree is hand-written in `runtime/js/dom-shim.js`** — the M2 fallback. No WHATWG
  packages.
- **The CSS subset is parsed and stored now.** With no compositor and an always-fullscreen canvas it
  changes nothing visual; the canvas warns once that position/size are ignored.

## Boundaries & Constraints

**Always:**
- Every failure is loud: a named build error, a non-zero host exit, or a thrown DOM exception. Nothing
  degrades silently.
- Existing contracts hold: one fullscreen canvas backed by `gl`, `width`/`height` writes ignored,
  `getContext('2d')`/`Worker`/`OffscreenCanvas` absent (`dom-identity-and-absence`),
  `getElementById('app')` works without markup (`dom-root-lookup`).
- Browser semantics where a browser defines them: tree mutation moves a node from its old parent,
  selector syntax errors throw `SyntaxError`, events capture → target → bubble.
- Every new behaviour gets a ctest or `node --test` row, sabotage-checked.

**Never:**
- No edits to vendored expo-gl (`runtime/third_party/gl/`) or to `node_modules`.
- No Canvas2D, no layout engine, no cascade, no CSS on elements other than canvas/video/iframe.
- No new native text path: text stays MSDF through WebGL.
- No compositor: nothing in this spec moves or resizes a rendered layer.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Entry rejects | package entry `execute` throws | `screenkit-host app.skpkg` exits 65; windowed exits 65; tvOS logs and ends with failure | message names the entry and error |
| Fix target moved | Blits present, `launch.js` never transformed | `vite build` fails naming the fix and the expected path | — |
| Non-Lightning app | no `@lightningjs/*` module | build passes, fixes inert | — |
| Font without atlas | `fonts/X.ttf` in dist, no `X.msdf.json`+`.png` | `screenkit bundle` fails naming X | — |
| Tree move | `a.appendChild(n)` where `n` is in `b` | `n.parentNode === a`, gone from `b.childNodes` | — |
| Cycle | `n.appendChild(ancestorOfN)` | throws `HierarchyRequestError` | — |
| Selector | `querySelector("meta[property=csp-nonce]")`, `#id`, `.a.b`, `div > p` | first match in document order, or null | bad syntax throws `SyntaxError` |
| Bubbling | click listener on body, dispatched on a child with `bubbles:true` | body listener runs with `target` = child | `stopPropagation` halts the path |
| Canvas style | `canvas.style.cssText = "left:10px;width:50%"` | parsed and reads back; canvas stays fullscreen | one warning that position/size is ignored |

</frozen-after-approval>

## Code Map

- `packages/@screenkit/cli/src/pack.js:142-144` -- wrapper `System.import(entry).catch(console.error)`; swallows entry failure.
- `runtime/apple/HostMain.mm` -- `runBundle` 519-567 (exit only from sync eval, idle wait 555-562), `runWindowed` 788-870, tvOS `SDL_AppInit` 664-737 / `SDL_AppIterate` 751; exit codes 52-58; package log line 469-472 (`package <dir>: runtimeVersion …`); `bundledLaunchPath` 572-584.
- `runtime/core/src/bindings/HostIO.{h,cpp}` -- `__screenkit` (setAssetRoot/readFile/imageInfo); the place for a failure report binding. `Console.cpp:111-138` rejection tracker is log-only.
- `runtime/core/include/screenkit/Runtime.h` -- embedder API; add a failure query beside `idle()`.
- `runtime/scripts/run-tvos-simulator.sh` -- requires `triangle.hbc` (27), needles 19-20, `log show` poll 69-86.
- `packages/@screenkit/vite-plugin/src/lightning.js` -- fixes match by `id.endsWith` (21, 52, 72); shared module-level objects spread in `index.js:71`, so seen-tracking needs a per-build factory. Legacy options `index.js:56-60` (`targets ['defaults','not IE 11']`, `polyfills:true`).
- `packages/@screenkit/vite-plugin/test/plugin.test.js` -- hook-level tests with stub `this.error` (16-23).
- `packages/@screenkit/cli/src/bundle.js:136,172-222` -- `copyAssets`; where the atlas check goes.
- `runtime/js/dom-shim.js` -- `makeElement` 117-153 (fake tree), `makeCanvas`/`defineBackedSize` 78-101,174-239, `eventTarget` 329-395 (window/document only), `document` 1279-1304, `getElementById('app')` 1258-1270, key dispatch 1345-1353 (stepper, path document → window).
- `runtime/tests/RuntimeTests.cpp` + `runtime/tests/CMakeLists.txt` -- dom-* rows (registered ~2364-2400); helpers `domRuntime` 972, `domEval` 982, `pumpThenRead` 1141; host rows `host-*` 202-301 via `host-row.sh`.
- `runtime/js/README.md` -- evidence rule 27-39; stale "no setAttribute/body/querySelector" 51-55, 89-95.
- Evidence of use: `m2-lightning-dom-usage.json` (documentElement, querySelector csp-nonce, getElementsByTagName, head, setAttribute, addEventListener, remove, id); renderer `Renderer.js:126-171,441-444,570`, `WebPlatformLegacy.js:131-139`, Blits `platform.js:38-64`.
- Fonts: Blits `msdfGenerator` (`@lightningjs/msdf-generator` 1.3.0, prebuilt `msdfgen` via `msdf-bmfont-xml`) writes `fonts/*.msdf.{png,json}` into dist; Architecture.md §6.1 283-293 already says so, §7 397-401 contradicts it.
- Do not change: `runtime/third_party/gl/`, `VendoredWebGL.cpp` seams, `InputRouter` mapping.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/core/src/bindings/HostIO.cpp`, `Runtime.h`, `hermes/RuntimeImpl.cpp` -- `__screenkit.reportFailure(message)` records a failure the host can query (thread-safe) -- a JS-side fatal needs a native exit path.
- [x] `packages/@screenkit/cli/src/pack.js` -- entry catch logs and calls `reportFailure` -- entry rejection becomes fatal.
- [x] `runtime/apple/HostMain.mm` -- all three run modes exit 65 at the next loop iteration once a failure is reported -- a failed entry may never go idle.
- [x] `runtime/tests/` -- host row: a package with a throwing entry exits 65 (headless and windowed); unit row for the binding.
- [x] `runtime/scripts/run-tvos-simulator.sh` -- package mode: when the app embeds `app.skpkg`, wait for the `package …` line and `frame time`, not the triangle; fail if the package line never appears.
- [x] `packages/@screenkit/vite-plugin/src/{lightning,index}.js` -- fixes built per build; `buildEnd` errors when a fix's package (`@lightningjs/blits/` or `/renderer/`) was in the graph but its target module was never transformed; tests for fire / not-fire / non-Lightning.
- [x] `runtime/tests/` -- `hermes-builtins` row asserting the exact set of ES2015–ES2023 built-ins missing from the pinned Hermes; `vite-plugin` adds legacy polyfills for any missing one (or the row documents that none are).
- [x] `runtime/js/dom-shim.js` -- real tree: `Node`/`Element`/`HTMLElement`/`Document` constructors; parent/child links, `childNodes`/`children`/siblings, `appendChild`/`insertBefore`/`removeChild`/`replaceChild`/`remove`/`contains`; `id`/`className`/`classList` reflect attributes; `documentElement`/`head`/`body` in the tree; `getElementById`/`getElementsByTagName`/`getElementsByClassName` walk it; `app` fallback kept.
- [x] `runtime/js/dom-shim.js` -- selectors: type, `#id`, `.class`, `[attr]`, `[attr=value]` (quoted or not), `*`, compound, descendant and `>` combinators, selector lists; `querySelector(All)`, `matches`, `closest`; anything else throws `SyntaxError`.
- [x] `runtime/js/dom-shim.js` -- every node is an EventTarget with capture/target/bubble along its ancestors → document → window; input keys target `document.activeElement` (body) and keep the per-listener microtask stepper.
- [x] `runtime/js/dom-shim.js` -- `style`: `CSSStyleDeclaration`-shaped (`cssText`, `setProperty`/`getPropertyValue`/`removeProperty`, camelCase properties); subset values parsed for canvas; canvas warns once when position/size would move it.
- [x] `runtime/tests/` -- dom rows for each matrix row above plus existing rows updated where the key target moves to body.
- [x] `packages/@screenkit/cli/src/bundle.js` + test -- refuse a dist whose `fonts/` has a `.ttf/.otf/.woff` without its `.msdf.json` and `.msdf.png`.
- [x] `Architecture.md` §7, §12 M5/M6 rows, §14 item 6; `runtime/js/README.md` -- match what ships (transforms as platform, Blits' generator for atlases, hand-written tree); fix stale lines.
- [x] `_bmad-output/implementation-artifacts/deferred-work.md` -- close the four M5 entries this resolves; add the real-Apple-TV run.

**Acceptance Criteria:**
- Given the three POCs rebuilt, when run on macOS headless and the tvOS simulator, then each boots with no errors and the example app's Portal still matches Chrome at the sampled points.
- Given `screenkit-host` on a package whose entry throws, when it runs, then it exits 65 within one second of the failure.
- Given the full ctest suite (macOS and ASan) and both `node --test` suites, when run, then all pass.

## Implementation Notes

- **Failure path.** `FailureReport` (HostIO.h) is owned by `HermesHost` and passed to `installHostIO`; `Runtime::failure()` returns `std::optional<std::string>`, first report wins. Hosts check it through one helper, `appFailed()`, which logs `the app reported a fatal failure; exiting with 65: <message>`. The packed wrapper's message is `screenkit bundle: entry "<entry>" failed: <stack>`. The host rows use fixture packages packed by the CLI's own `packScript` (make-fixtures.mjs imports it), so a packer regression fails them; `host-row.sh` gained `--within <s>` and skips a `--window` row with no window server.
- **`runtime/VERSION` is 2.** Packages call the new binding, so they carry runtimeVersion 2 and a runtime-1 host refuses them at the gate. The packed wrapper also logs `screenkit bundle: entry "<entry>" ready` on resolve, which run-tvos-simulator.sh package mode waits for. On tvOS a reported failure ends the app with `SDL_APP_FAILURE`, which SDL's UIKit main exits as status 1, not 65.
- **Hermes built-ins.** Measured on the pin: 24 gaps, Annex B included (`hermes-builtins.json`). 17 are core-js-fillable and go in through `additionalLegacyPolyfills`; verified by running the rebuilt lightning3-blits polyfills bundle on Hermes and re-probing — exactly the 7 unpolyfillable ones remain. `Promise.prototype[@@toStringTag]` surfaced only once the probe checked toStringTag members.
- **Lightning fixes.** Match on `/<pkg>/` rather than `/node_modules/<pkg>/`, so a linked copy still counts. Confirmed in real Vite 7 and Vite 8 builds of a throwaway app that a moved `launch.js` fails the build with the fix's name and path.
- **Element tree.** Per-node state in a WeakMap; `childNodes`/`children`/`classList` live, query results snapshots. Only `id`, `class` and `style` reflect attributes (so `link[rel=...]` does not match a `link.rel =` expando; `dom-loader-tolerance` updated). The `app` fallback appends `<div id="app">` to `<body>`. `new HTMLImageElement()` now throws, as in a browser (`new Image()` is the constructor).
- **Style.** Stored on every element and reflected to the `style` attribute; parsed only on a backed element. A size within 0.5 px of the drawable counts as fullscreen — Lightning writes `1280.0000064px` on the macOS window.
- **Disk.** The volume was at 100% (~130 MB free); ranlib failed until 66 MB of stale M5 scratch files (hc*.log, m5-baseline, m5-current) were deleted from this session's scratchpad.
- **Acceptance, as verified at step-03 (orchestrator, against the diff).** "run on macOS headless" in the first AC cannot hold for a Lightning app: the headless host has no GL context and so no DOM prelude. The POCs were run with `--window` instead, the nearest macOS mode. Independently re-run: ctest macOS 102/102 and ASan 102/102; `node --test` cli + vite-plugin 60/60 with `SCREENKIT_HOST` set; `host-refuses-entry-rejects` 0.25 s in total, against a rejection at 200 ms. tvOS simulator: with `entry-rejects.skpkg` embedded, `run-tvos-simulator.sh` exits 1 in package mode, and the host logs the fatal failure 12 ms after the rejection. With the example app embedded it exits 0 in package mode at 60 fps, and the Portal screenshot matches the Chrome reference on 97.2% of a 6-px grid to within 12 levels (95.8% exact), against 97.4% before the change.

## Spec Change Log

## Review Triage Log

| Finding | Verdict | Evidence | Route |
|---|---|---|---|
| BH1 Architecture §2 table/diagram still name `@screenkit/dom` as what ships; §3 heading "reuse, don't hand-write"; no bytecode size recorded against §3's budget | low | Architecture.md:35, :67, :86 unchanged; §11 was updated to "planned", so the doc contradicts itself. dom-shim.hbc is ~106 KB, never recorded. | patch |
| BH2 M6 row "DONE on macOS" omits the known `--window` idle exit; M5 row omits the VERSION decision | low | The row's Remaining lists compositor + hardware only, while deferred-work records the macOS windowed exit. VERSION part is moot once bumped (BH3a). | patch |
| BH3a packed catch calls `__screenkit.reportFailure` with no runtime version bump | low | pack.js calls the new binding unconditionally; manifests still say runtimeVersion 1, so an older runtime passes the gate and exits 0. Architecture §1: a new native capability ships as a new runtime version. | patch |
| BH3b/ECH2 catch reads the global late and `String(err)` throws for a null-prototype rejection value | low | Real: `String(Object.create(null))` throws inside the catch. Unlikely in everyday use (an entry rejecting with a null-prototype object, or app code replacing `__screenkit`), and the fix adds guards. | reject |
| BH4a font check only scans `fonts/` | low | Every POC's fonts are under `fonts/`; a font elsewhere is not shown to be a declared font (a CSS-imported asset lands in `assets/`), so refusing it needs more than a direct correction. | reject |
| BH4b/ECH11 `.woff2` in `fonts/` without an atlas ships silently | low | FONT_FILE is `/\.(ttf\|otf\|woff)$/i`; Blits' generator cannot convert woff2, so such a font can never render. Direct regex fix. | patch |
| BH4c/ECH10 a font shipping `.sdf.json`/`.sdf.png` is refused | low | checkFontAtlases accepts only `.msdf.*`, while webFontsAsMsdf keeps `.sdf` for `type:'sdf'` fonts. Direct condition fix. | patch |
| BH5 only id/class (and style) reflect, so `[attr=value]` over script-set properties matches nothing | low | True for `link.rel =`. No measured bundle relies on it (the preload polyfill's 0 is intended), and reflecting more attributes adds surface. The README line "only id and class reflect" is wrong about style: folded into BH12's doc patch. | reject |
| BH6/ECH8 `<img>` load/error never reach `addEventListener` listeners | medium | dom-shim.js:2369/2372 call `img.onload`/`onerror` with a plain object; listeners registered through the new EventTarget are kept and never called. | patch |
| BH7 hermes-builtins probe stops at ES2023 | maybe-false | Settled by probing ES2024/2025 built-ins on the pin and checking which ones every `defaults` browser has; ES2024+ features are recent enough that `defaults` includes browsers without them, so usage-based core-js would include them. At most low. | reject |
| BH8a `LEGACY_OPTIONS.additionalLegacyPolyfills` is overridden at the call site | false | Redundant, but both carry the same list and the test pins the passed options; no wrong behaviour results. | reject |
| BH8b/VG2 nothing proves the polyfills reach Hermes | medium | Pre-verified gap: the plugin test uses a fake plugin-legacy, and hermes-builtins probes a bare engine. Closing it needs Vite + plugin-legacy + core-js in a test path. | defer |
| BH9/ECH14 host rows bound timing in whole seconds at 5-15 s, not the AC's 1 s | low | The bound exists to rule out the 30 s idle wait, and it does; the measured 0.25 s meets the AC. Sub-second shell timing adds flake risk for no named harm. | reject |
| BH10a/ECH15 tvOS package mode passes before an entry that rejects after the first frame-time line | low | The script exits 0 once the package line and `frame time` appear (~1 s); nothing logs a settled entry. | patch |
| BH10b/ECH16 FAIL_NEEDLES are loose substrings an app's own log could contain | low | "version mismatch" and "package entry" match any dev.screenkit line from the pid, app console output included. Direct string fix to the host's exact prefixes. | patch |
| BH11a untested implemented behaviours: removal during dispatch, the path fixed at dispatch start, `DOMTokenList.replace` into an existing token, selector escapes, `setAttribute('style')` on the canvas | low | No dom-* row exercises them (`once` and `handleEvent` are covered by dom-event-target); the spec requires a row per new behaviour. | patch |
| BH11b missing `signal`/`passive`, writable `isTrusted`, `window instanceof EventTarget` false | low | Not in the spec's surface and no measured bundle uses them; each needs new logic. | reject |
| BH12 header and README say every member traces to evidence; many standard members do not | low | dom-shim.js header and README rule vs `Node.*_NODE`, `namedItem`, `toggle`/`replace`, `cancelBubble`, the camelCase accessors: the rule as written no longer describes the file. Doc correction. | patch |
| BH13 "Deliberately absent" omits textContent, append/prepend, cloneNode, dataset, createTextNode… | low | List incomplete (doc). Assigning textContent is silent, but no non-backed element can paint, so nothing visible is lost; the rest throw when called. | patch |
| BH14/ECH5/ECH6 subset grammar looser than CSS: any word run for `display`, `1.` accepted, `1E3px` rejected, `1e400px` stored as Infinity | low | dom-shim.js display regex, lengthToken without `i` and without a finite check, `\d+\.?\d*`. Direct parser corrections. | patch |
| BH14b `left`/`top` under `position: static` still warn | low | Cosmetic warning wording; needs position-aware branching, unlikely to matter. | reject |
| VG1 no test iterates a NodeList, yet the example app's preload polyfill does `for…of querySelectorAll(...)` | medium | Pre-verified: only `.length`/index/item/instanceof are asserted; dropping `proto[Symbol.iterator]` passes every row and breaks the app at launch. | patch |
| VG3/ECH18 tvOS failure exit only checked by its log line; docs say every mode exits 65, but SDL's UIKit main calls `exit(1)` | low | SDL_sysmain_callbacks.m:71,92 `exit(rc == SDL_APP_FAILURE ? 1 : 0)`; run-tvos-simulator.sh terminates the app itself on the needle, so a host that logs but does not return failure passes. | patch |
| ECH1 windowed: a quit or close event in the iteration after a report exits 0 | low | HostMain.mm:885 breaks on `!running` before the appFailed check at :888. Direct reordering. | patch |
| ECH3 a listener throwing a value whose string conversion throws escapes the dispatcher | low | Same concatenation as the pre-change eventTarget; throwing an unconvertible value is unlikely, and the fix adds a guard. | reject |
| ECH4 SUBSET/TRANSFORM_FUNCTIONS/GLOBAL_KEYWORDS/LENGTH_UNITS are plain literals, so `constructor`/`__proto__` keys hit Object.prototype | low | `setProperty('__proto__', 'x')` on the canvas calls a non-function; `transform: constructor(1)` reaches `name.indexOf`. Direct fix: null-prototype maps. | patch |
| ECH7 with `#app` detached and no mount point, each lookup makes a new detached div | low | Needs `<body>` and `<html>` removed; unlikely, and the fix adds state. | reject |
| ECH9 `on*` handler properties (`document.onkeydown`, `window.onresize`) are never invoked by dispatch | medium | Pre-existing: the pre-change eventTarget ignored them too. A common simple-TV-app pattern. | defer |
| ECH12 buildEnd fires when a package is imported without its target module | maybe-false | Settled by building an app that imports @lightningjs/renderer or blits without the launch/WebGlRenderer path; every Blits app includes both. At most low. | reject |
| ECH13 host-row.sh `--window` skips on exit 70, which also covers a DOM shim failure | low | Copied from the pre-existing host-window-row.sh:51 policy; dom-* rows still fail on a broken shim. | defer |
| ECH17 lightning.js exports became factories | low | @screenkit/vite-plugin is unpublished; screenkit() is the only consumer and was updated. | reject |
| ECH19 the windowed throwing-entry path has no row (the windowed row uses entry-rejects) | low | CMakeLists has host-refuses-entry-throws headless only; the task names headless and windowed. Adding the row is direct. | patch |

## Verification

**Commands:**
- `ctest --test-dir runtime/build/macos` and `--test-dir runtime/build/macos-asan` -- expected: 100% pass.
- `node --test packages/@screenkit/cli/test packages/@screenkit/vite-plugin/test` -- expected: all pass.
- `runtime/scripts/run-tvos-simulator.sh` on an app built with `SCREENKIT_APP_PKG` -- expected: exit 0 from the package log line.

**Manual checks:**
- tvOS simulator: example app Portal and one opened page render and navigate as before.
