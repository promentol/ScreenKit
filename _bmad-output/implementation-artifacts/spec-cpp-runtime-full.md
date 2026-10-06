---
title: 'C++ runtime: event loop and JSI object model'
type: 'feature'
created: '2026-09-16'
status: 'done'
baseline_commit: 'NO_VCS'
route: 'dispatch'
review_loop_iteration: 0
context: ['{project-root}/Architecture.md', '{project-root}/_bmad-output/implementation-artifacts/spec-cpp-runtime-jsi-hermes.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** The runtime boots Hermes and runs a bundle, but JS has no notion of time and no way to
reach native code beyond `console`. M3's exit criterion is "`console.log` **and timers** from an
`.hbc`" — only the first half exists. Both halves were split off at a token gate; this spec
reunites them.

**Approach:** Two layers on top of the existing `HermesHost`:
1. **Event loop** — timers, microtask checkpoint, `requestAnimationFrame`, and the freeze/thaw gate
   behind `Architecture.md` §5.1's `Paused` state.
2. **JSI object model** — `SharedObject`, `SharedRef`, `EventEmitter`, `NativeModule`, `LazyObject`,
   so arbitrary native objects can be handed to JS with correct lifetimes.

Together these complete M3 and provide the frame callback M4's `requestAnimationFrame` needs.

## Boundaries & Constraints

**Always:**
- Preserve every invariant of the existing runtime: one thread owns `jsi::Runtime`, access only via
  the executor, no `invokeSync`, weak-ref teardown discipline.
- Timer identity is stable across freeze/thaw: a thaw re-arms pending timers without reissuing
  handles and without firing a burst of backdated callbacks.
- Microtasks drain to exhaustion after every macrotask, bounded so a self-requeueing microtask
  cannot wedge the loop forever.
- **The loop is SDL3, not hand-rolled.** SDL3 already provides every primitive this needs, so use
  them rather than the C++ standard library equivalents:
  | Need | Use | Not |
  |---|---|---|
  | JS thread | `SDL_CreateThread` / `SDL_WaitThread` | `std::thread` |
  | Mutual exclusion | `SDL_CreateMutex` / `SDL_LockMutex` | `std::mutex` |
  | Blocking wait | `SDL_WaitConditionTimeout` / `SDL_WaitSemaphoreTimeout` | `std::condition_variable` |
  | Work queue | SDL event queue: `SDL_RegisterEvents` + `SDL_PushEvent` + `SDL_PeepEvents` | a `std::deque` of `std::function` |
  | Timers | `SDL_AddTimerNS` / `SDL_RemoveTimer` | a deadline heap |
  | Time | `SDL_GetTicksNS` | `std::chrono::steady_clock` |
  This means reworking the existing `HermesHost` internals. The 16 passing tests are the safety net
  for that rework — they must still pass unchanged.
- SDL3 comes from `tools/prebuilts`, not a source build.
- **When SDL3 and the C++ standard library both offer something, use SDL3.** That is the whole rule.
  Otherwise write normal, idiomatic C++ — do not contort the design for portability.
- `SharedObject` binds lifetime with `jsi::NativeState` + an id registry. `jsi::HostObject` is used
  **only** by `LazyObject`.
- Every listener in `EventEmitter` is isolated: one throwing listener must not prevent the rest from
  running, nor propagate out of `emit`.

**Never:**
- **The host uses SDL3 main callbacks.** `SDL_MAIN_USE_CALLBACKS` with `SDL_AppInit` /
  `SDL_AppIterate` / `SDL_AppEvent` / `SDL_AppQuit` replaces the bespoke `UIApplicationMain`
  delegate in `runtime/apple/HostMain.mm`. `SDL_AppIterate` **is** the frame tick, so
  `requestAnimationFrame` gets a real source instead of an injected clock, and `SDL_AppEvent`
  becomes the input path for later specs. `poc/src/main.cpp` already proves this model runs on
  Apple TV at 3840x2160.
- Do not call `SDL_PollEvent` / `SDL_PumpEvents` off the main thread — SDL documents them as
  main-thread only. The JS thread never pumps SDL events.
- No WebGL, DOM shim, `<video>`, `<iframe>`, or multi-instance.
- No change to the public `Runtime::create/evaluateBundle/evaluateSource/shutdown` signatures.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|---|---|---|---|
| Timer ordering | `setTimeout(a,10)`, `setTimeout(b,0)` | `b` then `a`, regardless of insertion order | N/A |
| Microtask priority | `setTimeout(t,0)` + resolved promise | Promise callback runs **before** the timer | N/A |
| Microtask starvation | Microtask that re-queues itself forever | Loop bounded; a diagnostic is logged and the loop proceeds | Bounded drain, not a hang |
| Interval repeat | `setInterval(f,5)` then `clearInterval` | Fires repeatedly, then stops; handle never reused | Unknown/negative handles ignored |
| Cancel from inside | `clearTimeout(self)` within its own callback | No further fire; no use-after-free | N/A |
| Freeze | `pause()` with pending timers | Zero callbacks, flat CPU, handles retained | N/A |
| Queue full | `SDL_PushEvent` rejected | Caller sees an explicit failure | Never a silently dropped task |
| Teardown drain | Tasks queued as SDL events at shutdown | All drained and freed | No leak under ASan |
| Thaw | `resume()` after a long pause | Pending timers re-armed; **no backdated burst** | N/A |
| rAF | `requestAnimationFrame(cb)` + frame tick | `cb` receives a monotonic timestamp in ms | Cancelled ids never fire |
| SharedObject GC | Native object dropped by JS | Registry entry released; native destructor runs | N/A |
| SharedObject explicit | `obj.release()` then reuse | Second use throws a clear JS error | Not a native crash |
| Listener throws | Two listeners, first throws | Second still runs; `emit` does not throw | Error logged per listener |
| LazyObject | Module never touched by JS | Backing object never constructed | N/A |

</frozen-after-approval>

## Code Map

**Build on, do not re-derive:**
- `runtime/core/src/hermes/HermesHost.{h,cpp}` — the thread, queue, `stopping_` flag and the
  guarded dispatch loop already exist. The loop is where the freeze gate goes.
- `runtime/core/include/screenkit/Runtime.h` — `JsExecutor` is the thread contract; extend the
  runtime surface, do not reshape it.
- `runtime/core/src/bindings/Console.cpp` — the working pattern for
  `jsi::Function::createFromHostFunction` and for installing onto a global.
- `runtime/tests/RuntimeTests.cpp` — hand-rolled harness, one CTest row per matrix row, gated-thread
  pattern at the teardown-race case. `make-fixtures.mjs` compiles `.js` fixtures with the pinned
  `hermesc`.

**Verified design facts — do not re-investigate:**
- Microtasks: `jsi::Runtime::drainMicrotasks(int maxMicrotasksHint = -1)`. React Native bounds its
  checkpoint to 255 retries (`RuntimeScheduler_Modern::performMicrotaskCheckpoint`).
- RN's tick shape, worth copying: select task → execute → **microtask checkpoint** → frame work.
- **SDL3 timer API, verified against `poc/build/sdl3-src`:**
  `SDL_TimerID SDL_AddTimerNS(Uint64 interval, SDL_NSTimerCallback cb, void *userdata)` and
  `bool SDL_RemoveTimer(SDL_TimerID)`. The callback is
  `Uint64 (*)(void *userdata, SDL_TimerID, Uint64 interval)` and **its return value is the next
  interval — return 0 to stop.** So `setInterval` returns the interval and `setTimeout` returns 0;
  no repeat bookkeeping is needed.
- SDL documents that callback as running **on a background thread**: it must only enqueue work onto
  the JS thread, never touch `jsi::Runtime`.
- Timers need no `SDL_Init`: `SDL_InitTimers` self-initializes through `SDL_ShouldInit`
  (`src/timer/SDL_timer.c:209`), so the JS thread can use them without main-thread SDL setup.
- **Event queue, verified in `include/SDL3/SDL_events.h`:** `SDL_PushEvent` and `SDL_PeepEvents` are
  both documented "safe to call from any thread"; `SDL_PollEvent` / `SDL_PumpEvents` are main-thread
  only. `SDL_PeepEvents` does **not** pump — it only reads the queue — which is exactly what lets the
  JS thread drain its own work without touching OS event handling or stealing the main thread's input.
- `SDL_RegisterEvents(n)` reserves a private event-type range. Give each instance its own range and
  drain with `SDL_PeepEvents(..., SDL_GETEVENT, minType, maxType)` so instances never see each
  other's work. `SDL_HasEvents(min, max)` is the cheap "anything pending for me?" check.
- `SDL_UserEvent` carries `Sint32 code` plus `void *data1` / `void *data2`, and a `timestamp` in
  nanoseconds already filled from `SDL_GetTicksNS`. Put the heap-allocated task in `data1` and the
  instance id in `code`.
- **Never `SDL_FlushEvents` on our range.** It discards events without handing back `data1`, which
  leaks every queued task. Drain with a `SDL_PeepEvents` loop, or `SDL_FilterEvents`, so each
  pointer is recovered and freed.
- **Do not deliver tasks via `SDL_AddEventWatch`.** The watch callback runs on whichever thread
  pushed or pumped the event, so it would execute runtime work on the wrong thread — the one
  invariant this design exists to protect.
- Full sync set is available: `SDL_CreateThread`, `SDL_WaitThread`, `SDL_CreateMutex`,
  `SDL_CreateCondition`, `SDL_WaitCondition`, `SDL_WaitConditionTimeout`, `SDL_CreateSemaphore`,
  `SDL_WaitSemaphore`, `SDL_WaitSemaphoreTimeout`, `SDL_SignalSemaphore`.
- SDL3 ships an official `SDL3.xcframework` in `SDL3-<ver>.dmg` with **`tvos-arm64` and
  `tvos-arm64_x86_64-simulator`** slices, plus an Android devel zip — so it needs none of ANGLE's
  retagging. No Linux binary is published.
- RN's `TimerManager` keys timers by a monotonic int handle; keep that handle scheme, but let SDL
  own the scheduling behind it.
- **RN has no freeze primitive.** `BufferedRuntimeExecutor` is a one-shot startup gate;
  iOS `RCTTiming` *throttles* backgrounded timers onto a coalesced `NSTimer` rather than stopping
  them. The gate in task selection and handle-stable re-arming are ours to design.
- `SharedObject` (expo-modules-core): `jsi::NativeState` + `ObjectId` into a registry, **not**
  `HostObject`. Released two ways — the `NativeState` destructor on GC, and an explicit
  `release()` host function that also clears the native state.
- `EventEmitter` (expo): listeners live in a `NativeState` on the emitter object, methods on the
  prototype; multi-listener emit snapshots the list before calling, so mutation during dispatch is
  safe. Not thread-aware — it assumes the JS thread.
- `LazyObject` (expo): the only `HostObject`; `get/set/getPropertyNames` construct the backing
  object on first touch. Wrapper objects have no `NativeState` of their own, so any method reached
  through one must unwrap first.

## Tasks & Acceptance

**Execution:**
- [ ] `tools/prebuilts/manifest.json`, `tools/prebuilts/fetch.mjs` -- add an `sdl3` dep: the
      official `SDL3-<ver>.dmg` xcframework for Apple, Android devel zip; keeps the cold-clone rule.
- [ ] `runtime/core/src/loop/TimerRegistry.{h,cpp}` -- monotonic JS handles mapped to
      `SDL_TimerID`; `SDL_AddTimerNS` schedules, `SDL_RemoveTimer` cancels, the callback's return
      value drives repeat. Freeze removes SDL timers and banks the remaining interval.
- [ ] `runtime/core/src/loop/EventLoop.{h,cpp}` -- own the tick: select task → run → drain
      microtasks (bounded) → frame callbacks; `pause()`/`resume()` gate checked at selection.
- [ ] `runtime/core/src/hermes/HermesHost.{h,cpp}` -- port the thread, mutex, condition and work
      queue to the SDL3 primitives above; the queue becomes a registered SDL event range. Honour the
      pause gate at drain time, and keep the existing guarded dispatch and teardown semantics. All 16
      existing tests must still pass.
- [ ] `runtime/core/src/bindings/Timers.cpp` -- `setTimeout/setInterval/clearTimeout/clearInterval`,
      `queueMicrotask`, `requestAnimationFrame/cancelAnimationFrame`.
- [ ] `runtime/core/src/jsi/JSIUtils.{h,cpp}` -- `getCoreObject` (`global.screenkit`), `createClass`,
      `createInheritingClass`, `defineProperty`, `ObjectDeallocator`.
- [ ] `runtime/core/src/jsi/SharedObject.{h,cpp}` -- `NativeState` + id registry; GC and explicit
      release; use-after-release throws a JS error.
- [ ] `runtime/core/src/jsi/SharedRef.{h,cpp}` -- subclass wrapping an existing native reference.
- [ ] `runtime/core/src/jsi/EventEmitter.{h,cpp}` -- prototype methods, subscription objects with
      `.remove()`, snapshot-before-dispatch, per-listener error isolation.
- [ ] `runtime/core/src/jsi/NativeModule.{h,cpp}` + `LazyObject.{h,cpp}` -- module base class
      inheriting EventEmitter; `LazyObject` as the only `HostObject`.
- [ ] `runtime/core/include/screenkit/Runtime.h` -- expose `pause()`, `resume()`, `tickFrame(ms)`,
      and module registration; no change to existing signatures.
- [ ] `runtime/apple/HostMain.mm` -- port to `SDL_MAIN_USE_CALLBACKS`: `SDL_AppInit` creates the
      runtime and loads the bundle, `SDL_AppIterate` drives the frame tick that services
      `requestAnimationFrame`, `SDL_AppQuit` shuts down. Preserve the existing exit-code contract and
      the tvOS bundle-resource lookup. `poc/src/main.cpp` is the working reference.
- [ ] `runtime/tests/` -- one CTest row per matrix row above, plus a freeze CPU-quiescence check.

**Acceptance Criteria:**
- Given a `.hbc` using `setTimeout`, `setInterval` and Promises, when run on macOS **and** the tvOS
  simulator, then callbacks fire in the matrix's order on both.
- Given a paused runtime with pending timers, when it stays paused for 2s, then zero callbacks run
  and no timer fires late in a burst on resume.
- Given a native object exposed via `SharedObject`, when JS drops its last reference and GC runs,
  then the native destructor runs exactly once.
- Given three listeners where the middle one throws, when the event is emitted, then the other two
  still run and `emit` does not throw.
- Given `runtime/tests` under ASan and UBSan, when the suite completes, then it passes clean with
  `UBSAN_OPTIONS=halt_on_error=1`.

## Implementation Notes

- 2026-09-16: the **event-loop half shipped** during the M3 push, implemented inline rather than by an
  agent. SDL3 timers, SDL event-queue work queue, SDL mutex/condition, freeze/thaw, and the
  `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`/`queueMicrotask`/rAF bindings are in and
  covered by `timers-ordering`, `timers-cancel` and `timers-freeze`. The tvOS host moved to
  `SDL_MAIN_USE_CALLBACKS`. Two findings are recorded in `Architecture.md` §8: Hermes needs
  `withMicrotaskQueue(true)` or `Promise` throws on a missing `setImmediate`, and SDL refuses
  `SDL_INIT_EVENTS` under a hand-written UIKit delegate.
- The **JSI object-model half compiles but is untested** — `SharedObject`, `SharedRef`,
  `EventEmitter`, `NativeModule`, `LazyObject` and `JSIUtils` are wired into `screenkit-core` and
  `installObjectModel` runs at startup, but no test exercises any of it. That is the remaining work
  in this spec.
- 2026-09-17: the **object-model half is tested** -- `object-model-gc` (the native destructor runs
  exactly once after the last reference is dropped and the collector runs; not while JS holds it; not
  again after `release()`), `object-model-release` (release, then every use is a clear JS error),
  `object-model-listener-isolation` (three listeners, the middle throws, the others run and `emit` does
  not throw), `object-model-lazy-module` (never built until touched). Each fails when the behaviour it
  checks is broken. Hades finalizes concurrently, so the GC row collects until the registry reaches the
  expected count. The ASan/UBSan criterion holds (`ctest --preset macos-asan`, 135/135), and leaks(1)
  covers what LeakSanitizer cannot on Darwin (`leaks-*` rows).

## Design Notes

**The freeze gate is original work.** Check it where the loop *selects* a task, not where it runs
one — a gate at execution time still burns the wakeup. Pausing must also stop the timer wait from
firing, so the condition variable predicate becomes `stopping_ || (!paused_ && workReady())`.

**Freeze by removing SDL timers, not by ignoring them.** `SDL_RemoveTimer` each pending timer on
pause and bank how much of its interval was left; re-add on resume with that remainder. Because
SDL's timers are interval-based rather than absolute-deadline, this avoids the backdated burst for
free — a paused timer simply has not started counting again. Leaving the SDL timers armed and
dropping their callbacks would still wake the CPU on every tick, which is exactly what `Paused`
promises not to do. JS handles stay stable across the cycle; only the `SDL_TimerID` behind them
changes.

**On the eventual Carbon migration.** The long-term intent is to move this layer to Carbon once it
has a stable release; it is experimental today. Carbon's defining feature is *bidirectional C++
interop* — it is a successor language, not a foreign one — so ordinary idiomatic C++ migrates better
than C-style contortion would. Write clean C++ now. The SDL3 preference above earns its keep on its
own merits: fewer hand-rolled primitives, one threading model, and behaviour already proven on every
target platform.

**`SDL_Event` carries no ownership.** It is a fixed-size POD, so a queued task travels as a raw
pointer in `event.user.data1` and the drain side owns freeing it. Two hazards follow, and both are
tested by the matrix: anything still queued at teardown must be drained and freed rather than
leaked, and `SDL_PushEvent` can *fail* when the queue is full — a dropped push must surface as an
error, never as a task that silently never runs.

**Unwrap before touching NativeState.** A `LazyObject` wrapper has no `NativeState` of its own, so
every host function reached through one must resolve to the backing object first. Expo hit this and
added `unwrapObjectIfNecessary` for exactly this reason.

## Verification

**Commands:**
- `cmake -S runtime -B runtime/build/macos -G Ninja && cmake --build runtime/build/macos` --
  expected: builds clean, zero third-party sources compiled.
- `ctest --test-dir runtime/build/macos --output-on-failure` -- expected: all matrix rows pass.
- `cmake -S runtime -B runtime/build/macos-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DSCREENKIT_SANITIZE=address,undefined && ctest --test-dir runtime/build/macos-asan` -- clean.
- `runtime/scripts/build-tvos-simulator.sh && runtime/scripts/run-tvos-simulator.sh` -- expected:
  the timers fixture's ordered output reaches the log on tvOS.
