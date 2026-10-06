// Copyright (c) ScreenKit contributors. MIT.
//
// One serial queue: work posted to it runs in order, one item at a time, on a
// thread of its own. The platform network clients use one per runtime to keep
// their bookkeeping lock-free, and `bindings/Net.cpp` uses one as a
// general-purpose off-thread queue for image decode.
//
// Portable on purpose: it carries no bytes and touches no socket, so there is
// nothing here for a platform to do better, and every target gets the same
// ordering and the same shutdown rules.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace screenkit::net {

class IoQueue {
 public:
  virtual ~IoQueue() = default;
  /// Run `work` on the queue, after everything already posted.
  virtual void post(std::function<void()> work) = 0;
  /// Run `work` on the queue and wait for it. Never call it from the queue.
  virtual void sync(std::function<void()> work) = 0;
  /// True when the caller *is* the queue's thread, where `sync` would deadlock.
  /// A teardown path that can be reached both from outside and from a job has
  /// no other way to tell, and hanging is the failure it would otherwise have.
  virtual bool onQueueThread() const = 0;
};

/// A serial queue on its own thread. The destructor stops the thread, joins it,
/// and then runs whatever was still queued on the *destroying* thread -- a job
/// that closes a task or releases a reference has to run, or teardown leaks.
class ThreadIoQueue final : public IoQueue {
 public:
  explicit ThreadIoQueue(std::string label);
  ~ThreadIoQueue() override;

  ThreadIoQueue(const ThreadIoQueue&) = delete;
  ThreadIoQueue& operator=(const ThreadIoQueue&) = delete;

  void post(std::function<void()> work) override;
  void sync(std::function<void()> work) override;
  bool onQueueThread() const override;

 private:
  bool schedule(std::function<void()> work);
  static void runJob(const std::function<void()>& job);
  void drain();
  void run();

  std::string label_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> work_;
  bool stopping_ = false;
  std::thread thread_;
};

std::shared_ptr<IoQueue> makeIoQueue(const std::string& label);

}  // namespace screenkit::net
