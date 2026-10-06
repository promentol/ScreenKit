// Copyright (c) ScreenKit contributors. MIT.
#include "TimerRegistry.h"

#include <algorithm>
#include <utility>

#include <SDL3/SDL_timer.h>

#include <screenkit/Log.h>

namespace screenkit {
namespace {

/// What SDL's `userdata` points at -- an integer key, never a pointer.
///
/// `SDL_RemoveTimer` only raises a cancelled flag; it does not wait for a
/// callback that is already running, and it cannot wait, because the callback
/// may be the thing that asked for the removal. So a `TimerRegistry*` handed to
/// SDL could be dangling by the time SDL dereferenced it. A key into a table
/// that outlives every runtime cannot be: a late callback takes the table lock,
/// finds nothing, and tells SDL to stop.
struct TimerSlot {
  std::weak_ptr<TimerRegistry> registry;
  TimerHandle handle = 0;
  std::uint64_t generation = 0;
};

class SlotTable {
 public:
  static SlotTable& instance() {
    // Deliberately never destroyed. SDL's timer thread is not joined by anything
    // we own and outlives static destruction, so a callback that arrives while
    // the process is tearing down must still find a live table to be told that
    // its timer is gone. The pointer stays reachable from here, so this is not a
    // leak a sanitizer will or should complain about.
    static SlotTable* table = new SlotTable();
    return *table;
  }

  std::uint64_t add(TimerSlot slot) {
    SdlLock lock(mutex_);
    const std::uint64_t key = next_++;
    slots_.emplace(key, std::move(slot));
    return key;
  }

  void remove(std::uint64_t key) {
    SdlLock lock(mutex_);
    slots_.erase(key);
  }

  /// SDL timer thread. Returns the interval to repeat with, or 0 to stop.
  std::uint64_t fire(std::uint64_t key) {
    std::shared_ptr<TimerRegistry> registry;
    TimerHandle handle = 0;
    std::uint64_t generation = 0;
    {
      SdlLock lock(mutex_);
      const auto it = slots_.find(key);
      if (it == slots_.end()) return 0;  // cancelled, frozen, or torn down
      registry = it->second.registry.lock();
      handle = it->second.handle;
      generation = it->second.generation;
      if (!registry) {
        slots_.erase(it);
        return 0;
      }
    }
    // Deliberately outside the table lock: onSdlFired takes the registry lock,
    // and every other path takes them registry-then-table. Holding both in the
    // other order here is the one way to deadlock this.
    return registry->onSdlFired(handle, generation, key);
  }

 private:
  SdlMutex mutex_;
  std::unordered_map<std::uint64_t, TimerSlot> slots_;
  std::uint64_t next_ = 1;
};

/// HTML's timer clamp: past a nesting level of five a timer waits at least
/// 4 ms, and every repeat of an interval is one level deeper than the last. It
/// is also what keeps a 0 ms interval alive at all -- SDL reads a callback's 0
/// as "stop", so an interval handed back 0 would fire exactly once.
constexpr std::uint64_t kMinRepeatNs = 4'000'000;

std::uint64_t msToNs(double ms) {
  if (!(ms > 0.0)) return 0;  // also catches NaN
  const double ns = ms * 1e6;
  // A delay past this is indistinguishable from "never" and would overflow.
  constexpr double kMax = 1e18;
  return static_cast<std::uint64_t>(ns > kMax ? kMax : ns);
}

Uint64 SDLCALL screenkitTimerCallback(void* userdata, SDL_TimerID, Uint64) {
  const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(userdata));
  return SlotTable::instance().fire(key);
}

}  // namespace

std::shared_ptr<TimerRegistry> TimerRegistry::create(Dispatch dispatch) {
  return std::shared_ptr<TimerRegistry>(new TimerRegistry(std::move(dispatch)));
}

TimerRegistry::TimerRegistry(Dispatch dispatch) : dispatch_(std::move(dispatch)) {}

TimerRegistry::~TimerRegistry() { clear(); }

void TimerRegistry::armLocked(Timer& timer, std::uint64_t intervalNs) {
  timer.armedIntervalNs = intervalNs;
  timer.armedAtNs = SDL_GetTicksNS();
  timer.slotKey = SlotTable::instance().add(
      TimerSlot{weak_from_this(), timer.handle, timer.generation});
  // No SDL_Init: SDL_AddTimerNS self-initialises the timer subsystem through
  // SDL_ShouldInit, so the JS thread can schedule without any main-thread setup.
  timer.sdlId = SDL_AddTimerNS(intervalNs, screenkitTimerCallback,
                               reinterpret_cast<void*>(static_cast<std::uintptr_t>(timer.slotKey)));
  if (timer.sdlId == 0) {
    SlotTable::instance().remove(timer.slotKey);
    timer.slotKey = 0;
    log(LogLevel::Error, "screenkit",
        std::string("SDL_AddTimerNS failed: ") + (SDL_GetError() ? SDL_GetError() : "unknown"));
  }
}

void TimerRegistry::disarmLocked(Timer& timer, std::uint64_t nowNs) {
  if (timer.sdlId != 0) {
    SDL_RemoveTimer(timer.sdlId);
    const std::uint64_t elapsed = nowNs - timer.armedAtNs;
    timer.remainingNs = elapsed >= timer.armedIntervalNs ? 0 : timer.armedIntervalNs - elapsed;
    timer.sdlId = 0;
  } else {
    // Nothing armed: a one-shot that already fired, or an interval whose
    // dispatch is still in flight. It came due before the freeze, so it is owed
    // one callback -- immediately on thaw, which is a debt paid, not a backdated
    // burst.
    timer.remainingNs = 0;
  }
  if (timer.slotKey != 0) {
    SlotTable::instance().remove(timer.slotKey);
    timer.slotKey = 0;
  }
}

TimerHandle TimerRegistry::add(double delayMs, bool repeating) {
  SdlLock lock(mutex_);
  Timer timer;
  timer.handle = nextHandle_++;
  timer.intervalNs = msToNs(delayMs);
  timer.repeating = repeating;

  const auto it = timers_.emplace(timer.handle, timer).first;
  if (paused_) {
    // Armed by resume(). A timer created while frozen must not start counting,
    // or `Paused` would leak wakeups through the back door.
    it->second.remainingNs = it->second.intervalNs;
    it->second.armedIntervalNs = it->second.intervalNs;
    return it->second.handle;
  }
  armLocked(it->second, it->second.intervalNs);
  if (it->second.sdlId == 0) {
    timers_.erase(it);
    return 0;
  }
  return it->second.handle;
}

bool TimerRegistry::cancel(TimerHandle handle) {
  SdlLock lock(mutex_);
  const auto it = timers_.find(handle);
  if (it == timers_.end()) return false;
  Timer& timer = it->second;
  if (timer.sdlId != 0) SDL_RemoveTimer(timer.sdlId);
  if (timer.slotKey != 0) SlotTable::instance().remove(timer.slotKey);
  timers_.erase(it);
  return true;
}

bool TimerRegistry::has(TimerHandle handle) const {
  SdlLock lock(mutex_);
  return timers_.find(handle) != timers_.end();
}

std::size_t TimerRegistry::pending() const {
  SdlLock lock(mutex_);
  return timers_.size();
}

std::uint64_t TimerRegistry::onSdlFired(TimerHandle handle, std::uint64_t generation,
                                        std::uint64_t slotKey) {
  SdlLock lock(mutex_);
  const auto it = timers_.find(handle);
  if (it == timers_.end()) return 0;
  Timer& timer = it->second;
  // Cancelled, frozen or re-armed between SDL picking this up and getting here.
  if (timer.generation != generation || timer.slotKey != slotKey) return 0;

  if (timer.repeating) {
    // SDL reschedules from the value returned below, so the next round runs at
    // the full requested interval even when this one was a post-thaw remainder.
    timer.armedIntervalNs = std::max(timer.intervalNs, kMinRepeatNs);
    timer.armedAtNs = SDL_GetTicksNS();
  } else {
    timer.sdlId = 0;
    SlotTable::instance().remove(slotKey);
    timer.slotKey = 0;
  }

  // Enqueue only. This is SDL's timer thread; the JS callback runs on the JS
  // thread when the loop selects the task.
  if (dispatch_) dispatch_(handle, generation);
  return timer.repeating ? std::max(timer.intervalNs, kMinRepeatNs) : 0;
}

bool TimerRegistry::claim(TimerHandle handle, std::uint64_t generation) {
  SdlLock lock(mutex_);
  const auto it = timers_.find(handle);
  if (it == timers_.end()) return false;
  Timer& timer = it->second;
  if (timer.generation != generation) return false;  // frozen or re-armed since
  if (!timer.repeating) timers_.erase(it);
  return true;
}

void TimerRegistry::pause() {
  SdlLock lock(mutex_);
  if (paused_) return;
  paused_ = true;
  const std::uint64_t now = SDL_GetTicksNS();
  for (auto& entry : timers_) {
    // Bumping the generation is what makes a dispatch already sitting in the JS
    // queue get dropped rather than run after the freeze.
    ++entry.second.generation;
    disarmLocked(entry.second, now);
  }
}

void TimerRegistry::resume() {
  SdlLock lock(mutex_);
  if (!paused_) return;
  paused_ = false;
  for (auto& entry : timers_) armLocked(entry.second, entry.second.remainingNs);
}

bool TimerRegistry::paused() const {
  SdlLock lock(mutex_);
  return paused_;
}

void TimerRegistry::clear() {
  SdlLock lock(mutex_);
  for (auto& entry : timers_) {
    if (entry.second.sdlId != 0) SDL_RemoveTimer(entry.second.sdlId);
    if (entry.second.slotKey != 0) SlotTable::instance().remove(entry.second.slotKey);
  }
  timers_.clear();
}

}  // namespace screenkit
