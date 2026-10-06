// Copyright (c) ScreenKit contributors. MIT.
#include "EventLoop.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <screenkit/Log.h>

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// How many `queueMicrotask` callbacks one checkpoint will run before it starts
/// banking them for the next one. Any finite number bounds the loop; this one is
/// large enough that no honest program reaches it and small enough that a
/// runaway chain is throttled within a millisecond.
constexpr int kMicrotaskBudget = 1024;

/// Hermes drains its whole job queue in one call, so this only ever runs once in
/// practice. It is here because `drainMicrotasks` is specified to be resumable
/// and a future engine may actually suspend -- React Native bounds the same loop
/// at 255.
constexpr int kMaxCheckpointAttempts = 255;

std::string describe(const jsi::JSError& error) {
  std::string message = error.getMessage();
  const std::string& stack = error.getStack();
  if (!stack.empty()) message += "\n" + stack;
  return message;
}

}  // namespace

std::shared_ptr<EventLoop> EventLoop::create(Delegate delegate, std::string tag) {
  std::shared_ptr<EventLoop> loop(new EventLoop(std::move(delegate), std::move(tag)));
  loop->start();
  return loop;
}

EventLoop::EventLoop(Delegate delegate, std::string tag)
    : delegate_(std::move(delegate)), tag_(std::move(tag)) {}

EventLoop::~EventLoop() = default;

void EventLoop::start() {
  std::weak_ptr<EventLoop> weak = weak_from_this();
  timers_ = TimerRegistry::create([weak](TimerHandle handle, std::uint64_t generation) {
    // SDL's timer thread. Enqueue and get out -- jsi::Runtime is not ours to
    // touch from here.
    auto self = weak.lock();
    if (!self) return;
    const bool queued = self->delegate_.post([weak, handle, generation](jsi::Runtime& runtime) {
      auto loop = weak.lock();
      if (!loop) return;
      loop->runTimer(runtime, handle, generation);
    });
    if (!queued) {
      // The host stopped, or SDL's queue is full. Either way this callback will
      // not run, and saying so beats a timer that silently never fires.
      log(LogLevel::Error, self->tag_,
          "timer " + std::to_string(handle) + " could not be queued onto the JS thread");
    }
  });
}

// --- timers ------------------------------------------------------------------

TimerHandle EventLoop::setTimer(std::shared_ptr<jsi::Function> callback,
                                std::vector<jsi::Value> args, double delayMs, bool repeating) {
  const TimerHandle handle = timers_->add(delayMs, repeating);
  if (handle == 0) return 0;
  TimerCallback entry;
  entry.function = std::move(callback);
  entry.args = std::move(args);
  entry.repeating = repeating;
  timerCallbacks_.emplace(handle, std::move(entry));
  return handle;
}

void EventLoop::clearTimer(TimerHandle handle) {
  timers_->cancel(handle);
  timerCallbacks_.erase(handle);
}

void EventLoop::runTimer(jsi::Runtime& runtime, TimerHandle handle, std::uint64_t generation) {
  // Stale delivery: cancelled, or frozen after SDL handed it over. Dropping it
  // here is what keeps a thaw from replaying a callback the freeze already
  // banked.
  if (!timers_->claim(handle, generation)) return;

  const auto it = timerCallbacks_.find(handle);
  if (it == timerCallbacks_.end()) return;

  // Move the entry out for the duration of the call. `clearTimeout(self)` from
  // inside the callback is explicitly supported, and it erases from this very
  // map -- which would otherwise free the argument vector the call is reading.
  TimerCallback entry = std::move(it->second);
  timerCallbacks_.erase(it);

  try {
    // const Value* and size_t exactly: with a plain `data()` the variadic
    // call(Runtime&, Args&&...) overload wins resolution and fails to compile.
    const jsi::Value* argv = entry.args.data();
    entry.function->call(runtime, argv, entry.args.size());
  } catch (const jsi::JSError& error) {
    log(LogLevel::Error, tag_, "uncaught error in a timer callback: " + describe(error));
  } catch (const std::exception& error) {
    reportError("timer callback", error);
  }

  // Put a repeating timer's callback back, unless the callback itself cleared
  // it (in which case the registry no longer knows the handle).
  if (entry.repeating && timers_->has(handle)) {
    timerCallbacks_.emplace(handle, std::move(entry));
  }
}

// --- microtasks --------------------------------------------------------------

void EventLoop::queueMicrotask(jsi::Runtime& runtime, const jsi::Function& callback) {
  enqueueMicrotask(runtime, std::make_shared<jsi::Function>(callback.getFunction(runtime)));
}

void EventLoop::enqueueMicrotask(jsi::Runtime& runtime,
                                 std::shared_ptr<jsi::Function> callback) {
  std::weak_ptr<EventLoop> weak = weak_from_this();
  // The user's callback is never handed to the VM directly: the wrapper is where
  // the budget is enforced and where a throwing microtask is contained, so that
  // neither can interrupt the drain.
  jsi::Function wrapper = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "microtask"), 0,
      [weak, callback](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*,
                       size_t) -> jsi::Value {
        auto loop = weak.lock();
        if (loop) loop->runMicrotask(rt, callback);
        return jsi::Value::undefined();
      });
  runtime.queueMicrotask(wrapper);
}

void EventLoop::runMicrotask(jsi::Runtime& runtime,
                             const std::shared_ptr<jsi::Function>& callback) {
  if (microtaskBudget_ <= 0) {
    // Over budget. Bank it rather than running it: not running it is what lets
    // the VM's queue empty, and banking it is what keeps a legitimate deep chain
    // from being silently dropped.
    microtaskOverflow_ = true;
    bankedMicrotasks_.push_back(callback);
    return;
  }
  --microtaskBudget_;
  try {
    callback->call(runtime);
  } catch (const jsi::JSError& error) {
    log(LogLevel::Error, tag_, "uncaught error in a microtask: " + describe(error));
  } catch (const std::exception& error) {
    reportError("microtask", error);
  }
}

void EventLoop::performMicrotaskCheckpoint(jsi::Runtime& runtime) {
  microtaskBudget_ = kMicrotaskBudget;
  microtaskOverflow_ = false;

  for (int attempt = 0; attempt < kMaxCheckpointAttempts; ++attempt) {
    try {
      if (runtime.drainMicrotasks()) break;
    } catch (const jsi::JSError& error) {
      log(LogLevel::Error, tag_, "uncaught error draining microtasks: " + describe(error));
    } catch (const std::exception& error) {
      reportError("microtask checkpoint", error);
      break;
    }
  }

  if (!microtaskOverflow_) return;

  const std::size_t banked = bankedMicrotasks_.size();
  log(LogLevel::Warn, tag_,
      "microtask budget of " + std::to_string(kMicrotaskBudget) +
          " exhausted in one checkpoint; " + std::to_string(banked) +
          " deferred to the next one -- a microtask is probably re-queueing itself");

  std::vector<std::shared_ptr<jsi::Function>> deferred;
  deferred.swap(bankedMicrotasks_);
  for (auto& callback : deferred) enqueueMicrotask(runtime, std::move(callback));

  // The VM queue is now non-empty again but this checkpoint is over, so post an
  // empty macrotask: the loop makes a full tick, other work gets its turn, and
  // the next checkpoint drains the next budget's worth. That is what "the loop
  // proceeds" means -- the runaway chain is throttled, not a hang and not a
  // silent drop.
  if (!delegate_.post([](jsi::Runtime&) {})) {
    log(LogLevel::Error, tag_,
        "could not queue the microtask overflow wake -- " + std::to_string(banked) +
            " deferred microtasks will not run until something else ticks the loop");
  }
}

// --- frames ------------------------------------------------------------------

FrameHandle EventLoop::requestAnimationFrame(std::shared_ptr<jsi::Function> callback) {
  const FrameHandle handle = nextFrameHandle_++;
  frameCallbacks_.emplace(handle, std::move(callback));
  frameCallbackCount_.store(frameCallbacks_.size());
  return handle;
}

void EventLoop::cancelAnimationFrame(FrameHandle handle) {
  frameCallbacks_.erase(handle);
  frameCallbackCount_.store(frameCallbacks_.size());
}

void EventLoop::setFrameFinished(std::function<void(jsi::Runtime&)> callback) {
  frameFinished_ = std::move(callback);
}

void EventLoop::tickFrame(double timestampMs) {
  {
    SdlLock lock(mutex_);
    // Paused: rAF stopped (Architecture.md 5.1). A compositing runtime still
    // takes the tick, and serves nothing but the present with it.
    if (paused_ && !presentWhilePaused_) return;
    framePending_ = true;
    frameTimestampMs_ = timestampMs;
  }
  // The wake, not the work: a frame is not a queued task, and the host folds
  // frameReady() into its wait predicate. Frame callbacks run in the tick's
  // frame phase, after the current task and its microtask checkpoint.
  if (delegate_.wake) delegate_.wake();
}

bool EventLoop::frameReady() const {
  SdlLock lock(mutex_);
  return framePending_ && !paused_;
}

void EventLoop::setPresentWhilePaused(bool present) {
  {
    SdlLock lock(mutex_);
    if (presentWhilePaused_ == present) return;
    presentWhilePaused_ = present;
  }
  if (delegate_.wake) delegate_.wake();
}

bool EventLoop::presentReady() const {
  SdlLock lock(mutex_);
  return framePending_ && paused_ && presentWhilePaused_;
}

void EventLoop::runFrameCallbacks(jsi::Runtime& runtime, double timestampMs) {
  // Snapshot and clear before dispatching. rAF is one-shot per registration, and
  // a callback that re-requests must land in the *next* frame, not this one.
  std::unordered_map<FrameHandle, std::shared_ptr<jsi::Function>> due;
  due.swap(frameCallbacks_);
  frameCallbackCount_.store(0);

  // Ordered by handle, so callbacks run in registration order the way a browser
  // runs them.
  std::vector<FrameHandle> handles;
  handles.reserve(due.size());
  for (const auto& entry : due) handles.push_back(entry.first);
  std::sort(handles.begin(), handles.end());

  const jsi::Value timestamp(timestampMs);
  for (const FrameHandle handle : handles) {
    const auto& callback = due[handle];
    if (!callback) continue;
    try {
      callback->call(runtime, &timestamp, std::size_t{1});
    } catch (const jsi::JSError& error) {
      log(LogLevel::Error, tag_,
          "uncaught error in a requestAnimationFrame callback: " + describe(error));
    } catch (const std::exception& error) {
      reportError("requestAnimationFrame callback", error);
    }
  }
}

// --- the tick ----------------------------------------------------------------

void EventLoop::afterTask(jsi::Runtime& runtime) {
  bool hasFrame = false;
  bool presentOnly = false;
  double timestampMs = 0.0;
  {
    SdlLock lock(mutex_);
    presentOnly = paused_;
    if (framePending_ && (!paused_ || presentWhilePaused_)) {
      hasFrame = true;
      timestampMs = frameTimestampMs_;
      framePending_ = false;
    }
  }
  // A frozen loop runs *nothing* of the page: not a microtask, not an rAF
  // callback. What it still owes is the present, because the window is not the
  // page's (setPresentWhilePaused).
  if (!presentOnly) performMicrotaskCheckpoint(runtime);
  if (!hasFrame) return;

  if (!presentOnly) {
    runFrameCallbacks(runtime, timestampMs);
    performMicrotaskCheckpoint(runtime);
  }

  // The frame is finished, so present it. A throw here is contained for the
  // same reason a throwing rAF callback is: the loop has to keep running, and
  // a failed present is a diagnostic rather than a reason to stop ticking.
  if (frameFinished_) {
    try {
      frameFinished_(runtime);
    } catch (const jsi::JSError& error) {
      log(LogLevel::Error, tag_, "uncaught error finishing a frame: " + describe(error));
    } catch (const std::exception& error) {
      reportError("frame-finished callback", error);
    }
  }
}

// --- freeze ------------------------------------------------------------------

void EventLoop::pause() {
  {
    SdlLock lock(mutex_);
    if (paused_) return;
    paused_ = true;
    pauseEpoch_.fetch_add(1);
    // A frame tick that arrived but has not been served is dropped, not banked:
    // `Paused` stops rAF, and serving a stale frame on thaw would be exactly the
    // backdated callback the lifecycle table forbids.
    framePending_ = false;
  }
  timers_->pause();
  // Outside the lock, and last: the gate reaches into the host's mutex, and lock
  // order here is always loop-then-host.
  if (delegate_.setPaused) delegate_.setPaused(true);
}

void EventLoop::resume() {
  {
    SdlLock lock(mutex_);
    if (!paused_) return;
    paused_ = false;
  }
  timers_->resume();
  if (delegate_.setPaused) delegate_.setPaused(false);
}

bool EventLoop::paused() const {
  SdlLock lock(mutex_);
  return paused_;
}

bool EventLoop::idle() const {
  if (timers_->pending() != 0) return false;
  if (frameCallbackCount_.load() != 0) return false;
  if (heldWork_.load() > 0) return false;
  SdlLock lock(mutex_);
  return !framePending_;
}

void EventLoop::stopTimers() { timers_->clear(); }

void EventLoop::shutdown() {
  timers_->clear();
  timerCallbacks_.clear();
  frameCallbacks_.clear();
  frameCallbackCount_.store(0);
  // Runs on the JS thread, which is what makes whatever the callback captured
  // -- notably the GL surface -- release its EGL objects there rather than on
  // whichever thread happened to drop the last reference.
  frameFinished_ = nullptr;
  bankedMicrotasks_.clear();
}

void EventLoop::reportError(const char* what, const std::exception& error) {
  log(LogLevel::Error, tag_,
      std::string("uncaught exception in a ") + what + ": " + error.what());
}

}  // namespace screenkit
