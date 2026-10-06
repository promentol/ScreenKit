// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "SdlSync.h"

namespace screenkit {

/// A JS-visible timer handle. Monotonic from 1, never reused -- React Native's
/// `TimerManager` scheme, kept because it is what makes `clearTimeout` on a
/// stale handle a no-op instead of cancelling somebody else's timer.
using TimerHandle = std::int64_t;

/// Timer scheduling for the event loop, on top of SDL3.
///
/// **SDL owns the waiting.** There is no deadline heap here and no condition
/// variable armed with a deadline: `SDL_AddTimerNS` schedules, `SDL_RemoveTimer`
/// cancels, and the callback's return value drives repetition (the interval to
/// repeat, or 0 to stop) so there is no repeat bookkeeping either.
///
/// **SDL calls back on its own timer thread.** Nothing in this class may touch
/// `jsi::Runtime`; a timer coming due only hands `(handle, generation)` to the
/// dispatch callback, which is expected to enqueue work onto the JS thread. The
/// JS callbacks themselves live in `EventLoop`, one layer up, so that this layer
/// stays free of JSI entirely.
///
/// **Freeze is why the SDL id and the JS handle are separate.** `pause()`
/// removes every armed SDL timer and banks how much of its interval was left;
/// `resume()` re-adds them with that remainder. SDL's intervals are relative, so
/// a frozen timer has simply not been counting -- there is no absolute deadline
/// to go stale, and therefore no burst of backdated callbacks on thaw. The JS
/// handle survives the cycle untouched; only the `SDL_TimerID` behind it
/// changes.
class TimerRegistry : public std::enable_shared_from_this<TimerRegistry> {
 public:
  /// Runs on SDL's timer thread. Must only enqueue.
  ///
  /// `generation` is how a dispatch that was already in flight when the loop
  /// froze gets dropped rather than run late: `pause()` bumps every timer's
  /// generation, so `claim()` rejects the stale delivery.
  using Dispatch = std::function<void(TimerHandle handle, std::uint64_t generation)>;

  /// shared_ptr, not a plain object: SDL's timer thread reaches this through a
  /// weak reference, which is what makes a callback that arrives during teardown
  /// find nothing instead of freed memory.
  static std::shared_ptr<TimerRegistry> create(Dispatch dispatch);

  ~TimerRegistry();

  TimerRegistry(const TimerRegistry&) = delete;
  TimerRegistry& operator=(const TimerRegistry&) = delete;

  /// Arm a timer. `delayMs` below zero is treated as zero, matching the web.
  /// Returns 0 if SDL refused to schedule it.
  TimerHandle add(double delayMs, bool repeating);

  /// Cancel. Unknown, already-fired and negative handles are ignored, which is
  /// what `clearTimeout` promises.
  bool cancel(TimerHandle handle);

  bool has(TimerHandle handle) const;

  /// Timers that still have somewhere to go: armed, frozen, or with a dispatch
  /// in flight.
  std::size_t pending() const;

  /// Take delivery of a dispatch on the JS thread. False means the delivery is
  /// stale -- the timer was cancelled or frozen after SDL handed it over -- and
  /// the JS callback must not run. Retires one-shot timers.
  bool claim(TimerHandle handle, std::uint64_t generation);

  /// Remove every armed SDL timer and bank its remaining interval. Idempotent.
  void pause();

  /// Re-arm everything with its banked remainder. Idempotent.
  void resume();

  bool paused() const;

  /// Drop everything. Called on the JS thread at teardown.
  void clear();

  /// Called by the SDL timer callback, on SDL's timer thread. Public only
  /// because the callback lives outside the class; not part of the API.
  /// Returns the interval to repeat with, or 0 to stop.
  std::uint64_t onSdlFired(TimerHandle handle, std::uint64_t generation, std::uint64_t slotKey);

 private:
  struct Timer {
    TimerHandle handle = 0;
    /// The interval the caller asked for; what a repeating timer goes back to
    /// after each fire.
    std::uint64_t intervalNs = 0;
    /// What this arming was actually scheduled with -- the same as `intervalNs`
    /// normally, the banked remainder on the first tick after a thaw.
    std::uint64_t armedIntervalNs = 0;
    std::uint64_t armedAtNs = 0;
    std::uint64_t remainingNs = 0;
    std::uint64_t generation = 0;
    /// Key into the process-wide slot table SDL's `userdata` points at. Never a
    /// pointer: `SDL_RemoveTimer` does not wait for an in-flight callback, so a
    /// pointer could be stale by the time SDL dereferenced it.
    std::uint64_t slotKey = 0;
    /// 0 when nothing is armed -- frozen, or a one-shot that has already fired.
    std::uint32_t sdlId = 0;
    bool repeating = false;
  };

  explicit TimerRegistry(Dispatch dispatch);

  void armLocked(Timer& timer, std::uint64_t intervalNs);
  void disarmLocked(Timer& timer, std::uint64_t nowNs);

  Dispatch dispatch_;

  mutable SdlMutex mutex_;
  std::unordered_map<TimerHandle, Timer> timers_;
  TimerHandle nextHandle_ = 1;
  bool paused_ = false;
};

}  // namespace screenkit
