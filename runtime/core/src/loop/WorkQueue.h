// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <SDL3/SDL_events.h>

#include "SdlSync.h"

namespace screenkit {

/// The JS thread's work queue -- SDL's own event queue, not a container of ours.
///
/// `SDL_RegisterEvents` reserves this queue a private event type, `SDL_PushEvent`
/// enqueues and `SDL_PeepEvents` drains it. `SDL_PeepEvents` reads the queue
/// without pumping, which is exactly what lets the JS thread take its own work
/// without touching OS event handling or stealing the main thread's input --
/// `SDL_PollEvent` and `SDL_PumpEvents` are main-thread only and are never
/// called from here.
///
/// A private type per instance is what keeps instances out of each other's work:
/// the drain is scoped to `[type, type]`, so a second runtime's tasks are
/// invisible here even though the underlying queue is process-wide.
///
/// **`SDL_Event` carries no ownership.** It is a fixed-size POD, so a task
/// travels as a raw pointer in `event.user.data1` and whoever takes it out owns
/// freeing it. Two consequences, both of them contracts this class keeps:
/// anything still queued at teardown is drained and freed rather than leaked
/// (never `SDL_FlushEvents`, which discards events without handing `data1`
/// back), and a rejected `SDL_PushEvent` -- a full queue -- is reported to the
/// caller instead of becoming a task that silently never runs.
class WorkQueue {
 public:
  struct Task {
    std::function<void()> run;
    /// Runs instead of `run` when the queue is drained with this task still in
    /// it. Exists so a blocked caller learns the work was dropped rather than
    /// waiting forever for a thread that has gone.
    std::function<void()> cancel;
  };

  /// Returns nullptr when SDL has no user event types left; `SDL_GetError()`
  /// says so. Registrations are never recycled, so this is a real if distant
  /// ceiling (SDL publishes 32768 of them) and a silent failure here would be a
  /// queue that drops every task.
  static std::unique_ptr<WorkQueue> create();

  ~WorkQueue();

  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;

  /// False means dropped, never deferred: the queue is closed, SDL refused the
  /// push, or the allocation failed. The task is destroyed either way.
  bool push(Task task);

  /// Take the next task. False when there is nothing for this queue.
  bool pop(Task& out);

  bool empty() const;

  /// Stop accepting pushes. Idempotent.
  void close();

  /// Cancel and free everything still queued. Idempotent.
  void drain();

  std::uint32_t eventType() const { return eventType_; }

  /// Wake the owner when a reclaimed task arrives (see `reclaim`). The owner's
  /// own pushes need no hook -- it signals its condition itself.
  void setWakeCallback(std::function<void()> wake);

  /// **Main thread only.** SDL's main-callback loop drains the whole shared
  /// event queue with `SDL_PollEvent`, every registered range included, so a
  /// work event can be taken by the main thread before the owning JS thread
  /// sees it. Hand it back here from `SDL_AppEvent`.
  ///
  /// True means the event belonged to a live queue and is now that queue's
  /// again; the caller must not touch it afterwards.
  static bool reclaim(const SDL_Event& event);

 private:
  explicit WorkQueue(std::uint32_t eventType);

  SdlMutex mutex_;
  std::uint32_t eventType_ = 0;
  bool closed_ = false;
  /// Tasks the main thread took out of the shared queue and handed back. Not a
  /// second work queue: SDL will not let an event be put back at the front of
  /// its own, and re-pushing it would just let the main thread take it again.
  std::vector<Task*> reclaimed_;
  std::function<void()> wake_;
};

}  // namespace screenkit
