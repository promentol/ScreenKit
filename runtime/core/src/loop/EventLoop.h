// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <jsi/jsi.h>

#include "SdlSync.h"
#include "TimerRegistry.h"

namespace screenkit {

/// `requestAnimationFrame` id space. Separate from `TimerHandle`, as on the web.
using FrameHandle = std::int64_t;

/// The tick, and everything JS-visible that hangs off it.
///
/// React Native's shape, copied deliberately: **select task -> execute ->
/// microtask checkpoint -> frame work.** The host owns selection and execution
/// (it owns the thread and the SDL event queue the work travels in); this owns
/// everything after the arrow, plus the JS-side state that scheduling needs --
/// the timer callbacks, the frame callbacks, and the freeze gate.
///
/// Scheduling itself is SDL's: see `TimerRegistry`. This class never waits on a
/// deadline and never blocks.
///
/// Threading: the `// JS thread` methods touch `jsi::Runtime` and must run on
/// the thread that owns it. The `// any thread` methods take `mutex_`, release
/// it, and only then call out to the host -- lock order is always
/// loop-then-host, never the reverse, which is what keeps the freeze gate from
/// deadlocking against the dispatch loop.
class EventLoop : public std::enable_shared_from_this<EventLoop> {
 public:
  /// What the loop needs from whoever owns the thread.
  struct Delegate {
    /// Enqueue onto the JS thread. False means dropped -- the host has stopped,
    /// or SDL's event queue refused the push.
    std::function<bool(std::function<void(facebook::jsi::Runtime&)>)> post;
    /// Re-evaluate the dispatch loop's wait predicate. Used for work that is not
    /// a queued task, notably an arriving frame.
    std::function<void()> wake;
    /// Move the freeze gate. The host checks it where it *selects* a task.
    std::function<void(bool paused)> setPaused;
  };

  static std::shared_ptr<EventLoop> create(Delegate delegate, std::string tag);

  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // --- JS thread ------------------------------------------------------------

  /// `setTimeout` / `setInterval`. Returns 0 if scheduling failed.
  TimerHandle setTimer(std::shared_ptr<facebook::jsi::Function> callback,
                       std::vector<facebook::jsi::Value> args, double delayMs, bool repeating);

  /// `clearTimeout` / `clearInterval`. Unknown handles are ignored.
  void clearTimer(TimerHandle handle);

  /// `queueMicrotask`. Goes into the VM's own microtask queue so it interleaves
  /// with promise jobs in the right order, wrapped so the checkpoint can bound
  /// it -- see `performMicrotaskCheckpoint`.
  void queueMicrotask(facebook::jsi::Runtime& runtime, const facebook::jsi::Function& callback);

  FrameHandle requestAnimationFrame(std::shared_ptr<facebook::jsi::Function> callback);
  void cancelAnimationFrame(FrameHandle handle);

  /// Runs at the very end of a frame: after the rAF callbacks and after their
  /// microtask checkpoint, so the frame is genuinely finished rather than
  /// merely dispatched.
  ///
  /// This is where a present belongs. There is exactly one frame source --
  /// `SDL_AppIterate` -> `tickFrame` -> rAF -- and hooking its trailing edge
  /// adds no second clock, where a render thread or a second timer would give
  /// two clocks racing over one GL context.
  ///
  /// JS thread only, and cleared by `shutdown()`: whatever the callback holds
  /// is therefore released on the JS thread too, which is what lets a GL
  /// surface be owned here at all.
  void setFrameFinished(std::function<void(facebook::jsi::Runtime&)> callback);

  /// Everything that happens after a macrotask: the microtask checkpoint, then
  /// any frame work a `tickFrame` has queued up (followed by its own
  /// checkpoint, because rAF callbacks resolve promises too).
  void afterTask(facebook::jsi::Runtime& runtime);

  /// Drain the VM's microtask queue to exhaustion, bounded.
  ///
  /// Hermes ignores `maxMicrotasksHint` and drains its job queue completely, so
  /// React Native's "retry the checkpoint 255 times" bound is a no-op here and a
  /// microtask that re-queues itself would spin inside the VM forever. The bound
  /// is therefore applied one level down, in the wrapper each `queueMicrotask`
  /// callback is queued behind: past the budget it banks the callback instead of
  /// running it, the VM queue empties, a diagnostic is logged, and the banked
  /// callbacks are handed back at the next checkpoint. The loop proceeds and
  /// timers keep firing; the runaway chain is throttled, not dropped.
  ///
  /// A promise chain that re-queues itself forever is *not* bounded by this --
  /// those jobs never pass through the wrapper, and Hermes offers no way to
  /// interrupt its own drain.
  void performMicrotaskCheckpoint(facebook::jsi::Runtime& runtime);

  /// Drop every JS reference this holds. Must run on the JS thread, before the
  /// runtime is destroyed.
  void shutdown();

  /// Stop SDL from delivering any further timer dispatch. Called first at
  /// teardown, before the host drains its queue, so nothing can be enqueued
  /// behind the drain.
  void stopTimers();

  // --- any thread -----------------------------------------------------------

  /// `Architecture.md` 5.1 `Paused`: timers frozen, rAF stopped, task queue
  /// gated at selection. Idempotent.
  void pause();
  void resume();
  bool paused() const;
  /// Incremented by every pause that took effect.
  std::uint64_t pauseEpoch() const { return pauseEpoch_.load(); }

  /// The frame clock. `SDL_AppIterate` is the real source; nothing here waits on
  /// vsync. Ignored while paused, and several ticks that arrive before the loop
  /// reaches its frame phase coalesce into one.
  void tickFrame(double timestampMs);

  /// True when the loop owes the tick a frame phase. The host folds this into
  /// its wait predicate, which is why an arriving frame needs only a wake and
  /// not a queue slot.
  bool frameReady() const;

  /// Keep presenting while paused.
  ///
  /// `Paused` stops the *page* -- rAF, timers, input -- but the window it draws
  /// into is not the page's. A launcher that freezes itself to give an
  /// `<iframe>` the remote still has to composite the instance that now has it,
  /// or the game runs and nothing reaches the screen (Architecture.md 5: "the
  /// launcher pauses, the game runs"). So a runtime that owns a compositor
  /// keeps serving the frame-finished hook while frozen -- and nothing else: no
  /// microtask checkpoint, no rAF callback, no queued task.
  ///
  /// Off by default, and set only on the runtime that owns layers, so an
  /// instance's own pause still burns no wakeup at all. Any thread.
  void setPresentWhilePaused(bool present);

  /// A present owed by a frozen loop: a frame tick has arrived and this loop
  /// presents while paused. The host folds it into its wait predicate beside
  /// `frameReady()`.
  bool presentReady() const;

  /// Nothing left to do: no timers, no frame callbacks, no frame tick waiting,
  /// no held work. Does not consider the host's task queue, which the host
  /// answers for -- which is also how banked microtasks are covered, since
  /// banking one always posts a wake task.
  bool idle() const;

  /// Work that lives outside the loop but will come back to it as tasks: an
  /// in-flight network request, an open WebSocket. Each hold keeps `idle()`
  /// false until its release. Any thread.
  void holdWork() { heldWork_.fetch_add(1); }
  void releaseWork() { heldWork_.fetch_sub(1); }

  std::size_t pendingTimers() const { return timers_->pending(); }

 private:
  struct TimerCallback {
    std::shared_ptr<facebook::jsi::Function> function;
    std::vector<facebook::jsi::Value> args;
    bool repeating = false;
  };

  EventLoop(Delegate delegate, std::string tag);
  void start();

  void runTimer(facebook::jsi::Runtime& runtime, TimerHandle handle, std::uint64_t generation);
  void runFrameCallbacks(facebook::jsi::Runtime& runtime, double timestampMs);
  void enqueueMicrotask(facebook::jsi::Runtime& runtime,
                        std::shared_ptr<facebook::jsi::Function> callback);
  void runMicrotask(facebook::jsi::Runtime& runtime,
                    const std::shared_ptr<facebook::jsi::Function>& callback);
  void reportError(const char* what, const std::exception& error);

  Delegate delegate_;
  std::string tag_;
  std::shared_ptr<TimerRegistry> timers_;

  // JS thread only.
  std::unordered_map<TimerHandle, TimerCallback> timerCallbacks_;
  std::unordered_map<FrameHandle, std::shared_ptr<facebook::jsi::Function>> frameCallbacks_;
  std::function<void(facebook::jsi::Runtime&)> frameFinished_;
  std::vector<std::shared_ptr<facebook::jsi::Function>> bankedMicrotasks_;
  FrameHandle nextFrameHandle_ = 1;
  int microtaskBudget_ = 0;
  bool microtaskOverflow_ = false;

  /// Mirrors frameCallbacks_.size() so idle() can be asked from any thread
  /// without reaching into JS-thread-only state.
  std::atomic<std::size_t> frameCallbackCount_{0};
  std::atomic<std::int64_t> heldWork_{0};
  std::atomic<std::uint64_t> pauseEpoch_{0};

  mutable SdlMutex mutex_;
  bool paused_ = false;
  bool presentWhilePaused_ = false;
  bool framePending_ = false;
  double frameTimestampMs_ = 0.0;
};

}  // namespace screenkit
