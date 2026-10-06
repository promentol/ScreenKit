// Copyright (c) ScreenKit contributors. MIT.
#include "IoQueue.h"

#include <utility>

#include <screenkit/Log.h>

namespace screenkit::net {
namespace {
constexpr const char* kTag = "screenkit.net";
}  // namespace

ThreadIoQueue::ThreadIoQueue(std::string label)
    : label_(std::move(label)), thread_([this] { run(); }) {}

ThreadIoQueue::~ThreadIoQueue() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  thread_.join();
  // ...and only then does what was still queued run -- on the *destroying*
  // thread, not the queue's own, which has already gone. What is left here is
  // exactly the work that cancels tasks and releases references: dropping it
  // leaks a connection or parks a callback for good.
  drain();
}

void ThreadIoQueue::post(std::function<void()> work) { schedule(std::move(work)); }

void ThreadIoQueue::sync(std::function<void()> work) {
  std::mutex m;
  std::condition_variable done;
  bool finished = false;
  const bool accepted = schedule([&] {
    work();
    std::lock_guard<std::mutex> lock(m);
    finished = true;
    done.notify_all();
  });
  // A stopping queue will never run it, and waiting on `finished` would then be
  // an unconditional hang rather than a slow shutdown.
  if (!accepted) return;
  std::unique_lock<std::mutex> lock(m);
  done.wait(lock, [&] { return finished; });
}

bool ThreadIoQueue::onQueueThread() const {
  return std::this_thread::get_id() == thread_.get_id();
}

/// False when the queue is stopping and the work will never run, which the
/// caller has to know: `sync` would otherwise wait for it for ever.
bool ThreadIoQueue::schedule(std::function<void()> work) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    work_.push_back(std::move(work));
  }
  cv_.notify_all();
  return true;
}

/// A job that throws must not take the process with it -- and must not unwind
/// out of `run`, which holds `mutex_` around the loop.
void ThreadIoQueue::runJob(const std::function<void()>& job) {
  try {
    job();
  } catch (const std::exception& e) {
    log(LogLevel::Error, kTag, std::string("a network I/O job threw: ") + e.what());
  } catch (...) {
    log(LogLevel::Error, kTag, "a network I/O job threw");
  }
}

/// Everything still queued, in order. Called once the queue's own thread has
/// joined, so nothing else is looking at `work_`.
void ThreadIoQueue::drain() {
  std::deque<std::function<void()>> left;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    left.swap(work_);
  }
  for (auto& job : left) runJob(job);
}

void ThreadIoQueue::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    if (work_.empty()) {
      cv_.wait(lock);
      continue;
    }
    auto job = std::move(work_.front());
    work_.pop_front();
    lock.unlock();
    runJob(job);
    lock.lock();
  }
}

std::shared_ptr<IoQueue> makeIoQueue(const std::string& label) {
  return std::make_shared<ThreadIoQueue>(label);
}

}  // namespace screenkit::net
