---
title: 'M7: module runner, HMR and the screenkit-go dev loop'
type: 'feature'
created: '2026-09-17'
status: 'draft'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/runtime/README.md', '{project-root}/runtime/js/README.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Every change to a ScreenKit app today means `vite build` → `screenkit bundle` → rebuild and
reinstall the host. There is no dev loop: the runtime has no network, no way to run Vite-served modules,
no dev client, and the macOS window closes once an app goes idle.

**Approach:** Architecture.md §6.2–6.4 as designed: the app's own `vite dev` stays the dev server; a
dev-only client (`screenkit-go`), launched with the dev server's LAN URL, runs the app through Vite's
`ModuleRunner` with a WebSocket transport and a native evaluator, and on an edit receives **only the
changed module** and applies it. Release hosts and `.skpkg` packages are unchanged.

**Decisions (2026-09-17):**
- **Networking is SDL3_net**, consumed prebuilt like SDL3 (official release 3.2.0 `.dmg` carries an
  `SDL3_net.xcframework` with macOS, iOS, tvOS and tvOS-simulator slices). The WebSocket client and HTTP
  GET are ours, over its stream sockets.
- **"Applied" for Blits is a restart with only the changed module re-sent:** the app restarts in a fresh
  runtime, unchanged modules come from cache, app state resets. A module that accepts HMR is applied in
  place.
- **Verified on the tvOS simulator reaching the Mac's LAN IP** (not localhost) and on the macOS client;
  real Apple TV hardware is deferred as for M5/M6.
- **Scope split:** Bonjour discovery and the on-screen server picker are their own spec
  (deferred-work.md). Here the client takes the URL directly: `--dev <url>` on macOS,
  `SCREENKIT_DEV_SERVER` on tvOS.

## Boundaries & Constraints

**Always:**
- Dev-only capability stays out of release: the release host has no `__screenkit_evaluate`, no network,
  no `WebSocket` (asserted by a test).
- Vite's own protocol and module graph, unmodified: no fork of Vite or of `vite/module-runner`; the
  runner the device runs comes from the app's own Vite version (7 and 8 both work).
- Only changed modules travel after the first load: an unchanged module is answered from cache.
- Every failure is loud and recoverable in dev: an unreachable server, a disconnect, a transform or
  evaluation error is logged with its URL and file, and the client keeps waiting for the next change.
- New behaviour gets a ctest or `node --test` row, sabotage-checked.

**Never:**
- No Vite fork, no Metro-style bundle re-send, no JS `eval`/`new Function` in the evaluator.
- No change to `screenkit bundle`, the `.skpkg` format or the release host's behaviour.
- No TLS, no remote (non-LAN) dev servers, no App Store distribution of the dev client.
- No edits to `node_modules` or vendored expo-gl; SDL3_net is never compiled from source.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| First load | client launched with the dev server's LAN URL | entry and its graph fetched, app renders as in the release package | — |
| Edit, no HMR boundary | a Blits component file saved | change on screen; exactly one module delivered with code, every other fetch answered from cache | — |
| Edit, accepted | a module with `import.meta.hot.accept` saved | applied in place, no restart, one module delivered | — |
| Transform or eval error | saved file has a syntax error | error logged with file and line; next good save applies | client stays connected |
| Server unreachable / stopped | wrong URL, or `vite dev` killed mid-session | logs the URL, retries every 2 s; on reconnect reloads | no crash, no exit |
| Dev asset | font atlas or image under the dev server | fetched over HTTP from the dev server and rendered | 404 behaves as the browser's loader error |
| Hermes syntax gap | app code with an async generator or regex `v` flag | lowered by the dev transform, module evaluates | — |
| Release host | `.skpkg` or `http://` URL on `screenkit-host` | unchanged: no network, no dev globals | as today |

</frozen-after-approval>

## Code Map

- `Architecture.md` §6.2–6.4 (L357–420) -- the design: WebSocket transport, `__screenkit_evaluate`, dev-only; §10 prebuilts; §11 L662–667 `module-runner/`, `apps/screenkit-go`.
- `tools/prebuilts/manifest.json` sdl3 entry (~L96: official `.dmg`, sha256, `copy`) + `runtime/cmake/Sdl3Prebuilt.cmake` -- the pattern for SDL3_net: `release-3.2.0/SDL3_net-3.2.0.dmg`, sha256 `0f28e62ce224f2edd8badfc6d607854ec7d34650f56dfbf7f16377cb0f1086a5`, 423 469 bytes, slices `macos-arm64_x86_64`, `tvos-arm64`, `tvos-arm64_x86_64-simulator`, `ios-*`. API: `NET_Init`, `NET_ResolveHostname`, `NET_CreateClient`, `NET_WriteToStreamSocket`, `NET_ReadFromStreamSocket` (non-blocking).
- Vite (V7 `poc/lightning3-blits/node_modules/vite/dist/node`, V8 `poc/blits-example-app/.../vite/dist/node`) -- `ModuleRunner(options, evaluator)` (V8 module-runner.js 1122); evaluator `runInlinedModule(context, code, module)` gets code expecting `__vite_ssr_exports__, __vite_ssr_import_meta__, __vite_ssr_import__, __vite_ssr_dynamic_import__, __vite_ssr_exportAll__, __vite_ssr_exportName__` (V8 1018, V7 910; V7 has no leading `\n` in startOffset); `createWebSocketModuleRunnerTransport({createConnection})` (V8 678, V7 587) needs `addEventListener(message|open|close, {once})`, `readyState`, `OPEN`, `send`, `close`; invoke `{type:'custom',event:'vite:invoke',data:{name:'fetchModule',id,data:[url,importer,{cached,startOffset}]}}`; runner uses `new URL('file://…')`, `TextDecoder` (V8), `Proxy`, `atob`, `Promise.allSettled`, `setTimeout`.
- Vite server -- the stock HMR socket only feeds the `client` environment (`disableFetchModule:true`, no SSR transform: V8 36277/36403). Custom environment: `environments.screenkit.dev.createEnvironment → new DevEnvironment(name, config, {hot:true, transport: <own HotChannel>})`; do not pass the shared `ws` (single handler slot, V8 26875). Non-client environments get the SSR transform. `resolve.noExternal: true` or deps come back `externalize` (V8 28148, 34306). `full-reload` (no boundary) clears `evaluatedModules` and re-imports entries (V8 776–790); `fetchModule` with `cached:true` answers `{cache:true}` for untouched modules (V8 1221). Dev transform target is esnext; per-environment lowering needs an `applyToEnvironment` plugin with `transformWithEsbuild`/`transformWithOxc`.
- Hermes syntax (pinned hermesc, probed) -- accepts optional chaining, logical assignment, class fields, private fields/methods, static blocks, async/await, BigInt, numeric separators, named groups; rejects async generators, regex `v` flag, `import.meta`, `import()` (the SSR transform removes the last two).
- `runtime/core/include/screenkit/Runtime.h:101` `evaluateSource` (tested as the dev path `RuntimeTests.cpp:185`); `HermesHost.cpp:135` runtime config (eval on by default, async generators off). No dev/release compile split exists (`runtime/CMakeLists.txt:18`).
- `runtime/js/dom-shim.js` -- `resolveResource` 2121 refuses http(s) (2128), XHR 2185–2270, fetch 2291; `location` fragment-only (257–310); `backedCanvas` single, never reset (1469–1512). Texture decode reads a file path (`localUri`), so downloaded images need a cache file.
- `runtime/apple/HostMain.mm` -- tvOS `SDL_AppInit` 686 / `bundledLaunchPath` 594 (argv ignored); `runWindowed` 818 exits on `idle()` (877); runtime created once (713, 829); `startGraphics` 107–135 reusable over the same CAMetalLayer; `Info.plist.in` has no network keys.
- Blits (2.8.9/2.9.0) -- no HMR anywhere; its Vite plugins all run in dev (`msdfGenerator` serves `*.msdf.{json,png}` via middleware, `msdfGenerator.js:99`). `@screenkit/vite-plugin` Lightning fixes match `/@lightningjs/.../launch.js` ids, which apply only when deps are transformed per module — true with `noExternal` in the screenkit environment.
- `deferred-work.md` L258 -- macOS `--window` exits on idle, flagged as an M7 dev-loop bug.
- Do not change: `screenkit bundle`/`pack.js` release path, `runtime/third_party/gl/`, `InputRouter` mapping.

## Tasks & Acceptance

**Execution:**
- [ ] `tools/prebuilts/manifest.json`, `runtime/cmake/Sdl3NetPrebuilt.cmake` (new) -- SDL3_net 3.2.0 from the official `.dmg`, checksum-verified, linked only into the dev client; Android/Linux recorded as unavailable for now.
- [ ] `runtime/core/src/net/` (new) -- over SDL3_net stream sockets: RFC 6455 WebSocket client (`ws://`, text + binary, ping/pong, close) and HTTP/1.1 GET (Content-Length and chunked); I/O off the JS thread, results posted as tasks.
- [ ] `runtime/core/src/bindings/` -- dev-only: browser-shaped `WebSocket` (url, protocols, readyState, send, close, on*/addEventListener), `__screenkit.httpGet(url) → Promise<ArrayBuffer>`, `__screenkit_evaluate(source, sourceURL)` -- compiled in only with `SCREENKIT_DEV_CLIENT`.
- [ ] `runtime/CMakeLists.txt`, `runtime/apple/CMakeLists.txt`, `Info.plist.in` -- `SCREENKIT_DEV_CLIENT` builds `screenkit-go` (bundle id `dev.screenkit.go`, `NSLocalNetworkUsageDescription`, `NSAllowsLocalNetworking`) beside the unchanged release host.
- [ ] `runtime/js/dom-shim.js` -- dev client only: `location` is the dev server origin; `resolveResource` sends dev-origin http URLs to `httpGet`, images through a per-session cache file for the decoder; release keeps refusing http.
- [ ] `runtime/js/dev-client.js` (new, dev prelude) -- fetch `/@screenkit/client.json`, load the served runner via `httpGet` + `__screenkit_evaluate`, create `ModuleRunner` (WebSocket transport to `/@screenkit/hmr`, evaluator on `__screenkit_evaluate`, `sourcemapInterceptor:false`), import the entry; keep module code in a native cache that survives restarts and fetch with `cached`; on `full-reload` restart the session; log every module delivered with code; retry every 2 s.
- [ ] `runtime/apple/HostMain.mm` -- `screenkit-go`: session from `--dev <url>` (macOS) or `SCREENKIT_DEV_SERVER` (tvOS), a clear log and wait when neither is given; restart = shutdown runtime + new runtime and GL surface on the same window, layer and input; the dev window keeps running while open.
- [ ] `packages/@screenkit/vite-plugin/src/dev.js` (new) + `index.js` -- `apply:'serve'`: `environments.screenkit` DevEnvironment with its own HotChannel on the `/@screenkit/hmr` upgrade path, `resolve.noExternal:true`, a Hermes lowering transform (async generators, regex `v`) scoped to the environment, `/@screenkit/client.json` (entry from `index.html`), `/@screenkit/runner.js` (IIFE from the app's own `vite/module-runner`), and the LAN URL printed on listen; Vite 7 and 8.
- [ ] tests -- ctest: WebSocket and HTTP against a local fixture server (framing, ping, close, chunked, 404), evaluate binding, release host lacks every dev global, restart keeps the window; `node --test`: environment and endpoints on Vite 7 and 8, lowering; end-to-end: `vite dev` on a fixture app + `screenkit-go --dev`, edit a leaf module → exactly one module delivered with code and the new value observed; an accepted module → applied without restart.
- [ ] `Architecture.md` §6.3, §10, §12 M7; `runtime/README.md`; `runtime/js/README.md`; `deferred-work.md` -- what ships; close the macOS idle-exit entry for dev; log real-hardware verification and Android/Linux networking.

**Acceptance Criteria:**
- Given `vite dev --host` on `poc/blits-example-app` and `screenkit-go` on the tvOS simulator launched with `SCREENKIT_DEV_SERVER` at the Mac's LAN IP, when it starts, then the Portal renders; when a component's text is edited and saved, the new text renders and the client logs exactly one module delivered with code for that reload.
- Given the macOS client started with `--dev <url>`, when a module that accepts HMR is edited, then it applies without a runtime restart.
- Given the release `screenkit-host` and every POC package, when the full ctest (macOS, ASan) and `node --test` suites run, then all pass and the release host exposes no dev global.

## Implementation Notes

## Spec Change Log

- 2026-09-17, at CHECKPOINT 1, before approval: the human asked for SDL3_net to provide full `WebSocket`,
  `fetch` and other HTTP APIs to the JS runtime (release builds too), with TLS from mbedTLS over SDL3_net,
  planned as its own spec first (`spec-runtime-networking.md`). This draft stays `draft` and resumes after
  that spec lands. When it does: drop the networking tasks (SDL3_net prebuilt, `runtime/core/src/net/`, the
  dev-only `WebSocket`/`httpGet` bindings, the "release host has no network, no WebSocket" boundary and
  the "Release host" matrix row) and build on the runtime's networking; keep `__screenkit_evaluate`
  dev-only. KEEP: the Vite environment, runner, cached restart and Hermes-lowering design above.
- 2026-09-17, networking spec replanned: TLS is each OS's native stack (Network.framework on Apple for
  `https:`/`wss:`), not mbedTLS; the networking spec also adds cookies, `EventSource` and streams.
- 2026-09-17, networking revised again: no SDL3_net at all; Network.framework carries every Apple connection.

## Review Triage Log

## Design Notes

**Why "only the changed module" needs a cache that outlives a reload.** Blits has no HMR boundary, so
every edit reaches the runner as `full-reload`, which by default drops `evaluatedModules` and re-fetches
the whole graph. Keeping each module's delivered code in a native cache keyed by URL, and fetching with
`cached:true`, lets the server answer `{cache:true}` for everything it has not invalidated — so a
restart re-evaluates the graph locally and re-downloads one file. Measure early whether Vite's soft
invalidation also re-sends importers of the edited module; if it does, the cache must follow the
server's invalidation, not bypass it.

**Why a fresh runtime rather than re-running in place.** One backed canvas, listeners on
`document`/`window`, timers, Blits singletons and GL objects (no `WEBGL_lose_context`) all survive an
in-place re-run; a new runtime and GL context start clean and cannot leak across edits.

## Verification

**Commands:**
- `ctest --test-dir runtime/build/macos` and `--test-dir runtime/build/macos-asan` -- expected: 100% pass.
- `node --test packages/@screenkit/cli/test packages/@screenkit/vite-plugin/test` -- expected: all pass.
- `(cd poc/blits-example-app && npx vite dev --host)` then launch `screenkit-go` on the tvOS simulator with `SIMCTL_CHILD_SCREENKIT_DEV_SERVER=http://<LAN IP>:5173` -- expected: Portal renders; an edit logs one module delivered and shows the change.

**Manual checks:**
- tvOS simulator: Portal renders from the dev server, an edit applies, stopping `vite dev` logs retries without a crash.
