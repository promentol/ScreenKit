// Copyright (c) ScreenKit contributors. MIT.
#include "WorkQueue.h"

#include <algorithm>
#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

namespace screenkit {
namespace {

/// Every live queue, by event type, so the main thread can route an event it
/// took out of the shared queue back to its owner.
///
/// Deliberately never destroyed: `SDL_AppEvent` can run while the process is
/// tearing down, and a lookup that arrives then must find an empty table rather
/// than a destroyed one. The pointer stays reachable from here, so this is not a
/// leak a sanitizer will or should complain about.
struct QueueTable {
  SdlMutex mutex;
  std::vector<std::pair<std::uint32_t, WorkQueue*>> entries;

  static QueueTable& instance() {
    static QueueTable* table = new QueueTable();
    return *table;
  }
};

}  // namespace

std::unique_ptr<WorkQueue> WorkQueue::create() {
  // One type per instance. SDL_RegisterEvents returns 0 when the user-event
  // space is exhausted; a queue built on type 0 would silently swallow work, so
  // refuse instead.
  const std::uint32_t type = SDL_RegisterEvents(1);
  if (type == 0) {
    log(LogLevel::Error, "screenkit",
        std::string("SDL_RegisterEvents failed -- no user event types left: ") + SDL_GetError());
    return nullptr;
  }

  std::unique_ptr<WorkQueue> queue(new WorkQueue(type));
  if (!queue->mutex_) {
    log(LogLevel::Error, "screenkit",
        std::string("SDL_CreateMutex failed: ") + SDL_GetError());
    return nullptr;
  }

  QueueTable& table = QueueTable::instance();
  SdlLock lock(table.mutex);
  table.entries.emplace_back(type, queue.get());
  return queue;
}

WorkQueue::WorkQueue(std::uint32_t eventType) : eventType_(eventType) {}

WorkQueue::~WorkQueue() {
  {
    QueueTable& table = QueueTable::instance();
    SdlLock lock(table.mutex);
    auto& entries = table.entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [this](const std::pair<std::uint32_t, WorkQueue*>& entry) {
                                   return entry.second == this;
                                 }),
                  entries.end());
  }
  close();
  drain();
}

void WorkQueue::setWakeCallback(std::function<void()> wake) {
  SdlLock lock(mutex_);
  wake_ = std::move(wake);
}

bool WorkQueue::push(Task task) {
  auto* heap = new Task(std::move(task));

  {
    // Held across the push so a close() racing this cannot leave the event
    // behind a drain that has already run -- which is the one way a queued task
    // could leak.
    SdlLock lock(mutex_);
    if (closed_) {
      delete heap;
      return false;
    }

    SDL_Event event;
    SDL_zero(event);
    event.type = eventType_;
    event.user.type = eventType_;
    event.user.code = static_cast<Sint32>(eventType_);
    event.user.data1 = heap;
    if (!SDL_PushEvent(&event)) {
      // A full queue, not a dropped task: the caller is told.
      log(LogLevel::Error, "screenkit",
          std::string("SDL_PushEvent rejected a runtime task: ") + SDL_GetError());
      delete heap;
      return false;
    }
  }
  return true;
}

bool WorkQueue::pop(Task& out) {
  Task* heap = nullptr;
  {
    SdlLock lock(mutex_);
    if (!reclaimed_.empty()) {
      // Reclaimed first: they were queued before anything still in SDL's queue.
      heap = reclaimed_.front();
      reclaimed_.erase(reclaimed_.begin());
    } else {
      SDL_Event event;
      if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, eventType_, eventType_) != 1) return false;
      heap = static_cast<Task*>(event.user.data1);
    }
  }
  if (heap == nullptr) return false;
  out = std::move(*heap);
  delete heap;
  return true;
}

bool WorkQueue::empty() const {
  SdlLock lock(mutex_);
  if (!reclaimed_.empty()) return false;
  return !SDL_HasEvents(eventType_, eventType_);
}

void WorkQueue::close() {
  SdlLock lock(mutex_);
  closed_ = true;
}

void WorkQueue::drain() {
  std::vector<Task*> dropped;
  {
    SdlLock lock(mutex_);
    dropped.swap(reclaimed_);
    // A SDL_PeepEvents loop, never SDL_FlushEvents: flushing discards events
    // without handing data1 back, which leaks every task still queued.
    for (;;) {
      SDL_Event event;
      if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, eventType_, eventType_) != 1) break;
      dropped.push_back(static_cast<Task*>(event.user.data1));
    }
  }
  // Cancels run outside the lock: one of them settles a blocked caller's cell,
  // and none of them has any business reaching back into this queue.
  for (Task* task : dropped) {
    if (task == nullptr) continue;
    if (task->cancel) task->cancel();
    delete task;
  }
}

bool WorkQueue::reclaim(const SDL_Event& event) {
  std::function<void()> wake;
  Task* orphan = nullptr;
  bool mine = false;

  {
    // The table lock is held for the whole time `owner` is touched. The
    // destructor takes it before unregistering, so a queue found under it
    // cannot go away underneath this.
    QueueTable& table = QueueTable::instance();
    SdlLock lock(table.mutex);
    WorkQueue* owner = nullptr;
    for (const auto& entry : table.entries) {
      if (entry.first == event.type) {
        owner = entry.second;
        break;
      }
    }
    if (owner == nullptr) return false;
    mine = true;

    auto* task = static_cast<Task*>(event.user.data1);
    SdlLock queueLock(owner->mutex_);
    if (owner->closed_) {
      // Teardown won the race. Cancel it here rather than handing it to a queue
      // whose drain has already run.
      orphan = task;
    } else {
      if (task != nullptr) owner->reclaimed_.push_back(task);
      wake = owner->wake_;
    }
  }

  // Both locks released. The wake reaches into the host's mutex, and the host
  // takes host-then-queue in its dispatch loop -- taking them the other way
  // round here is the one way to deadlock this. (The host outlives this call by
  // construction: reclaim is main-thread-only and so is teardown.)
  if (orphan != nullptr) {
    if (orphan->cancel) orphan->cancel();
    delete orphan;
  }
  if (wake) wake();
  return mine;
}

// The embedder-facing spelling of WorkQueue::reclaim. Declared in Runtime.h so a
// platform shell can hand a work event back from SDL_AppEvent without reaching
// into the runtime's private headers.
bool reclaimRuntimeEvent(const SDL_Event& event) { return WorkQueue::reclaim(event); }

}  // namespace screenkit
