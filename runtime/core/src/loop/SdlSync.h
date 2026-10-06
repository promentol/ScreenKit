// Copyright (c) ScreenKit contributors. MIT.
//
// RAII over SDL3's synchronisation primitives.
//
// SDL3 rather than the standard library, deliberately: threads, mutexes,
// conditions, the work queue, timers and the clock all come from one substrate,
// so the loop has a single well-understood set of guarantees instead of a mix of
// `std::` and SDL semantics -- and so the one place that knows how this runtime
// blocks is `SDL_WaitCondition`, not four different things.
//
// These are thin: `SdlMutex` is a handle plus a destructor, `SdlLock` is
// `std::unique_lock` for it, `SdlCondition` is the pair `SDL_WaitCondition`
// needs. Nothing here has behaviour of its own.
#pragma once

#include <utility>

#include <SDL3/SDL_mutex.h>

namespace screenkit {

class SdlMutex {
 public:
  SdlMutex() : mutex_(SDL_CreateMutex()) {}
  ~SdlMutex() {
    if (mutex_ != nullptr) SDL_DestroyMutex(mutex_);
  }

  SdlMutex(const SdlMutex&) = delete;
  SdlMutex& operator=(const SdlMutex&) = delete;

  void lock() const { SDL_LockMutex(mutex_); }
  void unlock() const { SDL_UnlockMutex(mutex_); }

  SDL_Mutex* raw() const { return mutex_; }
  explicit operator bool() const { return mutex_ != nullptr; }

 private:
  SDL_Mutex* mutex_ = nullptr;
};

/// Scoped lock. `unlock()` / `relock()` exist because the loop has to drop the
/// lock before calling out into JS and take it back afterwards.
class SdlLock {
 public:
  explicit SdlLock(const SdlMutex& mutex) : mutex_(&mutex) { mutex_->lock(); }
  ~SdlLock() {
    if (owns_) mutex_->unlock();
  }

  SdlLock(const SdlLock&) = delete;
  SdlLock& operator=(const SdlLock&) = delete;

  void unlock() {
    if (!owns_) return;
    owns_ = false;
    mutex_->unlock();
  }
  void relock() {
    if (owns_) return;
    mutex_->lock();
    owns_ = true;
  }

  const SdlMutex& mutex() const { return *mutex_; }

 private:
  const SdlMutex* mutex_;
  bool owns_ = true;
};

class SdlCondition {
 public:
  SdlCondition() : cond_(SDL_CreateCondition()) {}
  ~SdlCondition() {
    if (cond_ != nullptr) SDL_DestroyCondition(cond_);
  }

  SdlCondition(const SdlCondition&) = delete;
  SdlCondition& operator=(const SdlCondition&) = delete;

  /// Waits until `predicate()` holds. The mutex is released while blocked and
  /// held on return, exactly as `std::condition_variable::wait` behaves.
  template <typename Predicate>
  void wait(SdlLock& lock, Predicate predicate) const {
    while (!predicate()) SDL_WaitCondition(cond_, lock.mutex().raw());
  }

  /// Bounded form. Returns what `predicate()` evaluated to at the end, so a
  /// false return means the deadline won rather than the condition.
  template <typename Predicate>
  bool waitFor(SdlLock& lock, int timeoutMs, Predicate predicate) const {
    while (!predicate()) {
      if (!SDL_WaitConditionTimeout(cond_, lock.mutex().raw(), timeoutMs)) {
        return predicate();  // one last look: a signal can race the timeout
      }
    }
    return true;
  }

  void signal() const { SDL_SignalCondition(cond_); }
  void broadcast() const { SDL_BroadcastCondition(cond_); }

  explicit operator bool() const { return cond_ != nullptr; }

 private:
  SDL_Condition* cond_ = nullptr;
};

}  // namespace screenkit
