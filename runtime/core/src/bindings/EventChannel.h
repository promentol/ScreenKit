// Copyright (c) ScreenKit contributors. MIT.
//
// How a native producer's events reach the JS thread, in order, as event-loop
// tasks. Shared by `__screenkit.net` (one channel per request or socket) and
// `__screenkit.media` (one per player): the I/O side appends, the JS side
// delivers, and nothing JSI is ever touched off the JS thread.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <jsi/jsi.h>

#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

namespace screenkit {

/// The JS-thread end of a set of channels: the binding that owns the callbacks
/// an id's events go to.
class ChannelTarget {
 public:
  virtual ~ChannelTarget() = default;
  /// True once the binding has shut down: queued events are dropped, not delivered.
  virtual bool channelStopped() const = 0;
  virtual const std::string& channelTag() const = 0;
  /// One event, on the JS thread. `terminal` is the id's last event.
  virtual void deliver(facebook::jsi::Runtime& rt, std::uint64_t id, const char* type,
                       const facebook::jsi::Value& payload, bool terminal) = 0;
};

/// One producer's events on their way to the JS thread, in order.
///
/// At most one task is queued per channel: the producer appends events here and
/// posts a drain only when none is pending, and the drain delivers what has
/// accumulated with a microtask checkpoint after each event -- what a browser's
/// one-task-per-event gives a page (a promise the first message resolves has
/// settled before the second message is dispatched) without one runtime task per
/// chunk. That matters because the runtime's work queue is SDL's event queue,
/// which is finite: a paused runtime with a busy WebSocket would otherwise fill
/// it, and a full queue drops tasks. Here the backlog waits in memory, behind the
/// freeze gate, as one queued task.
///
/// Holds the binding weakly and re-locks it on the JS thread, where the callbacks
/// live. No JSI value is ever stored here: payloads are built on the JS thread.
class EventChannel : public std::enable_shared_from_this<EventChannel> {
 public:
  using MakePayload = std::function<facebook::jsi::Value(facebook::jsi::Runtime&)>;

  EventChannel(std::weak_ptr<ChannelTarget> binding, std::shared_ptr<JsExecutor> executor, std::uint64_t id)
      : binding_(std::move(binding)), executor_(std::move(executor)), id_(id) {}

  /// Any thread.
  void post(const char* type, bool terminal, MakePayload make) {
    bool schedule = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(Event{type, terminal, std::move(make)});
      schedule = !scheduled_;
      scheduled_ = true;
    }
    if (schedule) scheduleDrain();
  }

 private:
  struct Event {
    const char* type;
    bool terminal;
    MakePayload make;
  };

  /// Enough to keep one drain from holding the JS thread through several frames.
  static constexpr int kEventsPerTask = 64;

  void scheduleDrain() {
    auto self = shared_from_this();
    executor_->invokeAsync([self](facebook::jsi::Runtime& rt) { self->drain(rt); });
  }

  /// JS thread.
  void drain(facebook::jsi::Runtime& rt) {
    // However this returns -- drained, stopped, or an exception escaping --
    // the channel must not be left marked scheduled with nothing queued to run,
    // or it never drains again. Normal exits settle it under the lock that
    // checked the queue; this settles any other exit.
    struct Settle {
      EventChannel* channel;
      bool settled = false;
      ~Settle() {
        if (settled) return;
        bool more = false;
        {
          std::lock_guard<std::mutex> lock(channel->mutex_);
          more = !channel->pending_.empty();
          if (!more) channel->scheduled_ = false;
        }
        if (more) channel->scheduleDrain();
      }
    } settle{this};

    for (int delivered = 0;; ++delivered) {
      Event event;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
          scheduled_ = false;
          settle.settled = true;
          return;
        }
        if (delivered == kEventsPerTask) break;
        event = std::move(pending_.front());
        pending_.pop_front();
      }
      auto binding = binding_.lock();
      if (!binding || binding->channelStopped()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        scheduled_ = false;
        settle.settled = true;
        return;
      }
      if (delivered > 0) executor_->performMicrotaskCheckpoint(rt);
      try {
        binding->deliver(rt, id_, event.type, event.make(rt), event.terminal);
      } catch (const std::exception& e) {
        // Building the payload failed (deliver catches the callback's own).
        log(LogLevel::Error, binding->channelTag(),
            std::string("could not deliver a ") + event.type + " event: " + e.what());
      }
    }
    // More than one task's worth: let other work run, then carry on.
    settle.settled = true;
    scheduleDrain();
  }

  std::weak_ptr<ChannelTarget> binding_;
  std::shared_ptr<JsExecutor> executor_;
  std::uint64_t id_;
  std::mutex mutex_;
  std::deque<Event> pending_;
  bool scheduled_ = false;
};

/// What a sink posts through: a handle on one channel, cheap to copy.
class Poster {
 public:
  Poster(std::weak_ptr<ChannelTarget> binding, std::shared_ptr<JsExecutor> executor, std::uint64_t id)
      : channel_(std::make_shared<EventChannel>(std::move(binding), std::move(executor), id)) {}

  void post(const char* type, bool terminal, EventChannel::MakePayload make) const {
    channel_->post(type, terminal, std::move(make));
  }

 private:
  std::shared_ptr<EventChannel> channel_;
};

}  // namespace screenkit
