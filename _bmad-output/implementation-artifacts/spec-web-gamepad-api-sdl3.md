---
title: 'Web Gamepad API on top of SDL3'
type: 'feature'
created: '2026-09-17'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/runtime/README.md', '{project-root}/runtime/js/README.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Controllers only reach apps as synthesized DOM key events (D-pad → arrows, South → Enter, East → back). No `navigator.getGamepads()`, `Gamepad` or `gamepadconnected` exists, so web games that read gamepads never see one. Phaser's gamepad plugin turns itself off without `navigator.getGamepads`, and games cannot read sticks, triggers or face buttons on a Batocera Pi or an Apple TV.

**Approach:** Expose SDL3's gamepads as the W3C Gamepad API with the `standard` mapping. The main thread keeps a thread-safe snapshot, updated from SDL events and a per-frame poll. A `__screenkit.gamepads` binding reads that snapshot on the JS thread. The DOM shim builds `Gamepad` objects, `navigator.getGamepads()` and the connect/disconnect events from it.

## Boundaries & Constraints

**Always:**
- Only the main thread reads SDL gamepad state. The JS thread reads a mutex-guarded copy. The one JS-thread SDL call is rumble, done under `SDL_LockJoysticks` with a lookup by `SDL_JoystickID`.
- Every SDL gamepad maps to the `standard` layout. It has 17 buttons:
  - 0–3: South, East, West, North
  - 4, 5: the shoulders
  - 6, 7: the triggers, analog 0..1
  - 8: Back
  - 9: Start
  - 10, 11: the stick clicks
  - 12–15: D-pad up, down, left, right
  - 16: Guide

  It has 4 axes: left X, left Y, right X, right Y, each scaled to −1..1 with up negative. `pressed` means `value > 0.1`. `touched` equals `pressed`.
- `id` uses Chrome's format: `<name> (STANDARD GAMEPAD Vendor: vvvv Product: pppp)`. `index` is the lowest free slot when the pad connects, and stays stable while it is connected. `timestamp` is on `performance.now()`'s clock and advances only when the pad's state changes. `connected` is writable, because Phaser assigns it.
- `getGamepads()` returns an array of length max(4, highest index + 1), with `null` in empty slots.
  - A connection keeps the same `Gamepad` object, refreshed in place on each call. A call with no changes allocates no `Gamepad` or `GamepadButton` objects.
  - A host without SDL gamepads, such as the headless `runBundle`, returns all nulls.
- `gamepadconnected` and `gamepaddisconnected` are trusted `GamepadEvent`s on `window`, with `.gamepad` set. `ongamepadconnected` and `ongamepaddisconnected` handler attributes work. Pads present at startup fire `gamepadconnected` once each.
- Linux's Guide-to-quit and the existing key mapping, repeat and stick navigation stay intact until the app claims the gamepads.
- **Decision (keys):** the app's first `navigator.getGamepads()` call claims the gamepads for the rest of the process.
  - From then on, `InputRouter` synthesizes no key events from gamepad buttons or sticks.
  - Keys held when the claim lands get their `keyup`.
  - Keyboards, remotes and Linux's Guide-to-quit are unaffected.
  - An app that never calls `getGamepads()` keeps today's key navigation.
- **Decision (rumble):** `vibrationActuator` is a `GamepadHapticActuator` (`effects: ['dual-rumble']`), or null when SDL reports no rumble for the pad.
  - `playEffect('dual-rumble', {duration, strongMagnitude, weakMagnitude, startDelay})` clamps magnitudes to 0..1 and duration to 5000 ms, then calls `SDL_RumbleGamepad`.
  - It resolves `'complete'` when the effect ends, or `'preempted'` if another `playEffect` or `reset()` comes first.
  - An unsupported effect type rejects with `NotSupportedError`.
  - `reset()` stops the rumble and resolves `'complete'`.

**Never:**
- JS calling SDL.
- Browser user-gesture gating of gamepad exposure.
- Non-standard mappings, or exposing joysticks that SDL cannot open as gamepads.
- Touchpads, motion sensors, `GamepadPose`, extra buttons beyond 16.
- Edits to vendored expo-gl.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|---|---|---|---|
| Pad at startup | one pad connected before the loop | `getGamepads()[0]`: `mapping 'standard'`, 17 buttons, 4 axes, `connected true`; one `gamepadconnected` | N/A |
| Button | South held | `buttons[0]` is `{pressed:true, value:1}` next frame; `timestamp` grows | N/A |
| Trigger | right trigger at half | `buttons[7].value` ≈ 0.5, `pressed true` | N/A |
| Stick | left stick fully up | `axes[1] === -1` | N/A |
| Slots | pads A (0), B (1); A leaves; C joins | slot 0 null then C at 0; B stays at 1 | N/A |
| Disconnect | pad removed | `gamepaddisconnected` whose `gamepad.connected` is false; the old object is no longer refreshed | N/A |
| Paused runtime | pad connects while paused | the event is delivered after resume; the snapshot is already current | not dropped, unlike keys |
| No gamepad subsystem | headless run or test runtime | `[null,null,null,null]` | no throw |
| App writes | `pad.connected = false` in strict mode | no throw | N/A |
| Claim | D-pad up held; app calls `getGamepads()` | one `ArrowUp` keyup; later D-pad presses give no key events, only `buttons[12]` | N/A |
| Rumble | `playEffect('dual-rumble', {duration: 200, strongMagnitude: 1})` | `SDL_RumbleGamepad(pad, 65535, 0, 200)`; resolves `'complete'` | unsupported type → rejects `NotSupportedError`; pad gone → resolves `'preempted'` |

</frozen-after-approval>

## Code Map

- `runtime/core/include/screenkit/Input.h`, `core/src/input/Input.cpp`: `InputRouter`
  - It already opens and closes pads on `SDL_EVENT_GAMEPAD_ADDED` and `SDL_EVENT_GAMEPAD_REMOVED` (`gamepads_`), maps buttons and the stick to keys, and repeats held keys in `tick()`.
  - `deliver()` is the task pattern to copy: `invokeAsync`, a JS hook, then `runUserAgentDispatch`. Unlike keys, the gamepad events skip the `paused()` and `pauseEpoch` drops.
- `runtime/core/src/viewport/Viewport.cpp:21-50`: the smallest precedent for a native event → `__screenkitX` hook → `runUserAgentDispatch`.
- `runtime/core/src/input/UserAgentDispatch.h`: steps the JS dispatcher, with a microtask checkpoint per step.
- `runtime/host/Host.cpp`:
  - `SDL_Init(... | InputRouter::requiredSubsystems())` at :634.
  - The event loop at :703-720 (the Guide quit at :709-713 runs before the router).
  - `input->tick` and then `runtime->tickFrame` at :728-729.
  - The tvOS path is `apple/HostMain.mm:276-292`. Hosts need no changes.
- `runtime/core/src/bindings/Text.{h,cpp}`: the synchronous binding pattern. `installText` reads `global.__screenkit`, a `method()` helper wraps `createFromHostFunction` (:92-98), and the result is attached with `setProperty` (:204). Its registration in `core/src/hermes/HermesHost.cpp` is the include at :18, install at :152, shutdown at :246.
- `runtime/core/src/bindings/HostIO.h:13-31`: `FailureReport`, the mutex-guarded cross-thread state precedent.
- `runtime/CMakeLists.txt:114-146`: core sources (bindings :116-122, `input/Input.cpp` :126).
- `runtime/js/dom-shim.js`:
  - `dispatcher`: :254-297
  - `WINDOW_EVENT_HANDLERS`: :377-381
  - `performance`: :3217-3226, `Date.now() - TIME_ORIGIN`
  - `__screenkitResize` template: :3300-3307
  - `navigator` literal: :3314-3327
  - `Event`: :3343-3381
  - `__screenkitKey`: :6860-6870
  - `tests/check-shim-names.mjs` rejects duplicate top-level names.
- Phaser 4.2.1 (`phaser/src/input/gamepad/GamepadPlugin.js`) needs:
  - `navigator.getGamepads` to exist (`device/Input.js:49`);
  - a `getGamepads()` poll each frame (:470);
  - `id`, `index`, `buttons[i].value`, numeric `axes`, `timestamp`, and a writable `connected` (:324);
  - `event.gamepad.index` on window events (:485-495).

  Pixi and Blits never use the Gamepad API.
- `runtime/tests/RuntimeTests.cpp`:
  - Existing rows: `inputGamepadMap` :1720, `inputRouterDispatch` :1853.
  - Row pattern: `domRuntime`, then `domEval`, `pumpThenRead` :1278, `test::skip`.
  - Registration: `kCases` :5472; `tests/CMakeLists.txt:114-219`.
  - Tests never `SDL_Init` gamepads today. `SDL_AttachVirtualJoystick` (type `SDL_JOYSTICK_TYPE_GAMEPAD`) is exported by the macOS prebuilt.
- Docs: `runtime/README.md:273-326` (Input); `runtime/js/README.md` "Events" :145-176, inventory :286-301, "Known limits" :451-468.

## Tasks & Acceptance

**Execution:**
- [x] `runtime/core/src/input/Gamepads.{h,cpp}` (new): a process-wide, mutex-guarded registry (slots, id, 17 button values, 4 axes, change timestamp as epoch ms, connected) with the standard-mapping table. Main thread: `connect(SDL_Gamepad*)`, `disconnect(SDL_JoystickID)`, `poll()`. Any thread: a read. Gamepads are process-wide, so one registry serves every runtime.
- [x] `runtime/core/src/input/Input.cpp`, `Input.h`: on add and remove, update the registry and queue `__screenkitGamepad(type, index)`. `tick()` calls `poll()`. Once the registry is claimed, synthesize no gamepad keys and release those held.
- [x] `runtime/core/src/bindings/Gamepads.{h,cpp}` (new), `core/src/hermes/HermesHost.{h,cpp}`, `runtime/CMakeLists.txt`: install `__screenkit.gamepads`. It reads the registry into caller-owned storage (the first read sets the registry's atomic claim flag), and adds `rumble(index, strong, weak, ms)`.
- [x] `runtime/js/dom-shim.js`: add `Gamepad`, `GamepadButton`, `GamepadEvent`, `navigator.getGamepads`, the `__screenkitGamepad` hook, the handler attributes, `GamepadHapticActuator`, and the conversion of epoch ms to `performance.now()` time.
- [x] `runtime/tests/RuntimeTests.cpp`, `runtime/tests/CMakeLists.txt`: add rows driving a virtual gamepad through `InputRouter` for every I/O matrix row, including a Phaser-shaped poll loop. Skip with 77 where SDL cannot attach a virtual joystick.
- [x] `runtime/README.md`, `runtime/js/README.md`: document the Gamepad API, the mapping, the key claim and rumble.

**Acceptance Criteria:**
- Given the full test suite on macOS, when it runs, then every existing row still passes and the new gamepad rows pass.
- Given a Phaser 4 game with `input: { gamepad: true }` on ScreenKit, when a controller is used, then `this.input.gamepad.pad1` reports its buttons and sticks.
- Given the linux-arm64 build (`sh tools/batocera/pi.sh build`), when it compiles, then the host builds with the new sources.

## Implementation Notes

- `__screenkitGamepad(type, index, serial, id, rumble)`: `type` is the DOM event type, and the hook also receives the connection's serial, id and rumble flag. With only `(type, index)`, a pad that comes and goes while paused (or a slot reused before the task runs) would be attributed to whichever pad holds the slot when the task runs; the serial ties each event to its connection, and the id lets a pad already gone still fire `gamepadconnected` with an object describing it (`connected` false).
- `__screenkit.gamepads` is `read(ArrayBuffer, claim)`, `id(index)` and `rumble(index, strong, weak, ms)`. `read` takes an explicit `claim` flag rather than claiming on every first read: the connection hook reads the snapshot too, and a connection event must not claim. Only `navigator.getGamepads()` passes `true`.
- The registry refcounts connections per opened pad (a second InputRouter on the same pad shares its slot), frees the slot on the first disconnect, and guarantees each change moves the slot's stamp strictly forward; the shim refreshes a pad only when its stamp moved.
- `HermesHost.h` needed no change: the binding's storage lives in its host functions, and there is nothing to shut down.
- A rumble with duration 0 is sent as a stop: SDL keeps a rumble with no duration going indefinitely. `startDelay` is clamped to 5000 ms like `duration`.
- Test pads that share a vendor and product share SDL's mapping and its name, so the multi-pad rows give each pad its own product id. Physical controllers are hidden from the rows with SDL's HIDAPI, MFi and IOKit hints.
- Beyond the Phaser-shaped row, a scratch run (not committed) loaded a real Phaser 4.2.1 package with `input: { gamepad: true }` into a test runtime with a virtual pad: `this.input.gamepad.pad1` reported A, D-pad up, R2, both sticks and `connected`, and the plugin emitted `connected`, `down` and `disconnected`.

## Spec Change Log

## Review Triage Log

Pass 1 (review_loop_iteration 0): blind-hunter (B), edge-case-hunter (E), verification-gap (V).

| # | Finding | Verdict | Evidence | Route |
|---|---|---|---|---|
| V1 | `gamepad-slots` never reads buffer data for slot ≥ 4, so the grow-and-reread loop is unverified | medium | Pre-verified: `ids()` reads only `id` (a separate `id()` call) and `index`; with the grow loop deleted, pad 4 had NaN/undefined state and the row still passed | patch |
| V2 | No row reuses a slot before JS reads again (serial changes 0→B without a null in between) | medium | Pre-verified: every slot-freeing row lets JS see the slot empty first; with the serial check mutated, the old object kept A's id with B's state and the suite passed | patch |
| V3 | Zero-duration rumble sent as a stop is untested | medium | Pre-verified: every `playEffect` in `gamepad-rumble` passes a non-zero duration; deleting `if (ms == 0)` leaves SDL rumbling forever and the row passes | patch |
| V4 | `startDelay` branch (delayed start, immediate stop of the preempted effect, clamp) never runs | medium | Pre-verified: no row passes `startDelay`; replacing the branch with `start()` goes unnoticed | patch |
| V5 / B1 / E1 | Destroying one of two routers frees the pad's slot for the other; `connect` with refs > 0 can return a stale index | low | Real in the registry logic, but no host or test creates two `InputRouter`s (`Host.cpp`, `HostMain.mm` each make one); the fix adds a release path. Unlikely in use + more than a direct correction | reject |
| B2 | The claim is process-wide and permanent, with no opt-out | low | The frozen decision says the first `getGamepads()` claims "for the rest of the process"; changing it edits intent | reject |
| B3 / E2 | Keyups released when the claim lands are dropped if the runtime is paused, leaving a key down in the page | medium | `gamepadsClaimed()` runs in `tick` before the pause check and calls `deliver`, which drops while paused, then erases `held_`; breaks the frozen "keys held when the claim lands get their keyup"; a Phaser keyboard `isDown` would stick | patch |
| B3-pre | The same drop exists for keys released by `SDL_EVENT_GAMEPAD_REMOVED` while paused | low | Pre-existing code path in `InputRouter` (disconnect release through `deliver`), not introduced here | defer |
| B4a / E3 | A pad leaving mid-effect settles the `playEffect` promise only when its timer fires (≤ 5 s, or 10 s with `startDelay`) | low | `gamepadLeft()` never touches the actuator; README says `'preempted'` when the pad leaving "comes first". Fix is one call in `gamepadLeft` | patch |
| B4b | Motors keep running until SDL's expiry when the runtime pauses | low | Real (no pause hook stops rumble), but bounded at 5 s, and the fix needs a native pause hook. Unlikely + more than a direct correction | reject |
| B5 | Events queued across a pause carry an index a newer pad may now hold; Phaser's `getPad(index)` would name the newer pad | low | Needs a connect/leave plus another pad taking the slot before JS runs; Phaser still ends in the right state after the queue drains. Coalescing is more than a direct correction | reject |
| B6 | Listeners added after async boot miss `gamepadconnected` for pads present at launch | low | Frozen intent: "Pads present at startup fire `gamepadconnected` once each" and no user-gesture gating; Phaser polls every frame regardless. Changing it edits intent | reject |
| B7 / E7 | Test hints hide physical controllers only on macOS; on Linux a real pad breaks the rows | false | `runtime/tests` builds only `APPLE AND NOT tvOS`, and HIDAPI/MFi/IOKit are the macOS backends, so no configuration that runs the rows is exposed | reject |
| B8 | No test of a stick-held key released by the claim, or a claim first seen in `handleEvent` | low | Both paths call the same `gamepadsClaimed()` release loop that `gamepad-claim` exercises; a test gap with no defect | reject |
| B9 | README: "a poll that finds nothing changed allocates nothing" and "`tick` polls once a frame" are inaccurate | low | `getGamepads()` builds a new array each call; `Host.cpp:727` calls `tick` every loop iteration (~1 ms while busy). Doc wording fix | patch |
| B10 | Buffer layout is duplicated in JS with nothing tying it to C++ | false | The `gamepad-*` rows read every offset (serial, timestamp, rumble flag, 17 buttons, 4 axes) through the shim, so drift fails them | reject |
| B11 | A second runtime without a router gets no connection events and leaks `gamepadsBySerial` entries | low | No path creates a second runtime sharing a router today; fix adds branches. Unlikely + more than a direct correction | reject |
| B12 | Rumble clamps parameters where browsers reject with `TypeError`; `startDelay` + `duration` can total 10 s | low | Clamping is the frozen decision; the difference is undocumented, which a one-line Known limits entry fixes | patch |
| B13 | Docs name the spec by bare filename | false | Repo convention: `tests/CMakeLists.txt:243` and `js/README.md:67` cite other specs the same way | reject |
| E4 | `playEffect` throws synchronously for a Symbol type or a throwing params getter | low | Real (`String(Symbol())` throws) but no app passes those; the fix wraps the body. Unlikely + more than a direct correction | reject |
| E5 | `SDL_GetCurrentTime` failing makes `timestamp` ≈ −TIME_ORIGIN | low | It reads `clock_gettime`/`gettimeofday`, which do not fail on macOS or Linux in practice; the fix adds a fallback clock | reject |
| E6 | `SDL_EVENT_GAMEPAD_REMAPPED` does not refresh `id` or the rumble flag | low | Remapping a connected pad at runtime is rare; handling it adds a registry path | reject |
| E8 | Tasks said "the first read claims"; only `getGamepads()` claims | false | The frozen decision is "the app's first `navigator.getGamepads()` call claims"; the implementation matches it (deviation recorded in Implementation Notes) | reject |

## Design Notes

**Polling, not only events.** Browsers poll pads at about 60 Hz. `tick()` runs once per frame on the main thread before `tickFrame`, so a `getGamepads()` inside rAF sees state at most one frame old. A tap shorter than a frame can be missed, which is the same as in a browser.

**Clock.** `performance.now()` is `Date.now() - TIME_ORIGIN` in the shim. The registry stamps changes with wall-clock milliseconds, and the shim subtracts `TIME_ORIGIN`, so Phaser's `timestamp` comparison holds.

## Verification

**Commands:**
- `cmake --build runtime/build/macos && ctest --test-dir runtime/build/macos --output-on-failure`: all pass, with the new gamepad rows passing, not skipped.
- `node runtime/tests/check-shim-names.mjs runtime/js/dom-shim.js`: passes (also ctest `dom-shim-names`).
- `sh tools/batocera/pi.sh build`: the linux-arm64 host builds.

**Manual checks (if no CLI):**
- Connect a controller to the Mac, run a package with `--window` and `SCREENKIT_LOG_INPUT=1`, and log `navigator.getGamepads()` from rAF. Buttons, triggers and sticks should match the physical controls.
