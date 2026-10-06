// Copyright (c) ScreenKit contributors. MIT.
#include "Timers.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../loop/EventLoop.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// HTML says a non-finite or negative delay clamps to zero rather than
/// throwing, and that a missing delay is zero. Anything else -- a string, an
/// object with valueOf -- goes through the usual ToNumber.
double coerceDelay(jsi::Runtime& /*runtime*/, const jsi::Value* args, size_t count) {
  if (count < 2) return 0.0;
  double delay = 0.0;
  if (args[1].isNumber()) {
    delay = args[1].getNumber();
  } else {
    try {
      delay = args[1].asNumber();
    } catch (const jsi::JSIException&) {
      delay = 0.0;  // ToNumber failed: treat as "as soon as possible"
    }
  }
  if (!std::isfinite(delay) || delay < 0.0) return 0.0;
  return delay;
}

/// setTimeout(fn, delay, ...rest) hands `rest` to the callback when it fires.
std::vector<jsi::Value> trailingArgs(jsi::Runtime& runtime, const jsi::Value* args, size_t count) {
  std::vector<jsi::Value> extra;
  if (count <= 2) return extra;
  extra.reserve(count - 2);
  for (size_t i = 2; i < count; ++i) extra.emplace_back(runtime, args[i]);
  return extra;
}

/// clearTimeout/clearInterval must ignore anything they do not recognise --
/// undefined, a string, a stale id -- rather than throwing. Only a real number
/// is worth passing down.
bool coerceHandle(const jsi::Value* args, size_t count, TimerHandle& out) {
  if (count < 1 || !args[0].isNumber()) return false;
  const double raw = args[0].getNumber();
  if (!std::isfinite(raw)) return false;
  out = static_cast<TimerHandle>(raw);
  return true;
}

jsi::Value makeScheduler(jsi::Runtime& runtime, const std::shared_ptr<EventLoop>& loop,
                         const char* name, bool repeating) {
  std::weak_ptr<EventLoop> weak = loop;
  return jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, name), 2,
      [weak, repeating, name](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                              size_t count) -> jsi::Value {
        auto live = weak.lock();
        if (!live) return jsi::Value(0);  // runtime tearing down: accept and drop

        if (count < 1 || !args[0].isObject() || !args[0].getObject(rt).isFunction(rt)) {
          throw jsi::JSError(rt, std::string(name) + " requires a function as its first argument");
        }
        auto callback =
            std::make_shared<jsi::Function>(args[0].getObject(rt).getFunction(rt));
        const TimerHandle handle =
            live->setTimer(std::move(callback), trailingArgs(rt, args, count),
                           coerceDelay(rt, args, count), repeating);
        return jsi::Value(static_cast<double>(handle));
      });
}

jsi::Value makeCanceller(jsi::Runtime& runtime, const std::shared_ptr<EventLoop>& loop,
                         const char* name) {
  std::weak_ptr<EventLoop> weak = loop;
  return jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, name), 1,
      [weak](jsi::Runtime&, const jsi::Value&, const jsi::Value* args,
             size_t count) -> jsi::Value {
        auto live = weak.lock();
        TimerHandle handle = 0;
        if (live && coerceHandle(args, count, handle)) live->clearTimer(handle);
        return jsi::Value::undefined();
      });
}

}  // namespace

void installTimerBindings(jsi::Runtime& runtime, std::shared_ptr<EventLoop> loop) {
  if (!loop) return;
  jsi::Object global = runtime.global();

  global.setProperty(runtime, "setTimeout", makeScheduler(runtime, loop, "setTimeout", false));
  global.setProperty(runtime, "setInterval", makeScheduler(runtime, loop, "setInterval", true));
  global.setProperty(runtime, "clearTimeout", makeCanceller(runtime, loop, "clearTimeout"));
  global.setProperty(runtime, "clearInterval", makeCanceller(runtime, loop, "clearInterval"));

  // queueMicrotask runs before the next timer, which is the whole point of it
  // existing alongside setTimeout(f, 0).
  {
    std::weak_ptr<EventLoop> weak = loop;
    global.setProperty(
        runtime, "queueMicrotask",
        jsi::Function::createFromHostFunction(
            runtime, jsi::PropNameID::forAscii(runtime, "queueMicrotask"), 1,
            [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                   size_t count) -> jsi::Value {
              auto live = weak.lock();
              if (!live) return jsi::Value::undefined();
              if (count < 1 || !args[0].isObject() || !args[0].getObject(rt).isFunction(rt)) {
                throw jsi::JSError(rt, "queueMicrotask requires a function");
              }
              live->queueMicrotask(rt, args[0].getObject(rt).getFunction(rt));
              return jsi::Value::undefined();
            }));
  }

  // rAF callbacks receive a monotonic timestamp in milliseconds, as on the web.
  {
    std::weak_ptr<EventLoop> weak = loop;
    global.setProperty(
        runtime, "requestAnimationFrame",
        jsi::Function::createFromHostFunction(
            runtime, jsi::PropNameID::forAscii(runtime, "requestAnimationFrame"), 1,
            [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                   size_t count) -> jsi::Value {
              auto live = weak.lock();
              if (!live) return jsi::Value(0);
              if (count < 1 || !args[0].isObject() || !args[0].getObject(rt).isFunction(rt)) {
                throw jsi::JSError(rt, "requestAnimationFrame requires a function");
              }
              auto callback =
                  std::make_shared<jsi::Function>(args[0].getObject(rt).getFunction(rt));
              return jsi::Value(
                  static_cast<double>(live->requestAnimationFrame(std::move(callback))));
            }));

    global.setProperty(
        runtime, "cancelAnimationFrame",
        jsi::Function::createFromHostFunction(
            runtime, jsi::PropNameID::forAscii(runtime, "cancelAnimationFrame"), 1,
            [weak](jsi::Runtime&, const jsi::Value&, const jsi::Value* args,
                   size_t count) -> jsi::Value {
              auto live = weak.lock();
              TimerHandle handle = 0;
              if (live && coerceHandle(args, count, handle)) {
                live->cancelAnimationFrame(static_cast<FrameHandle>(handle));
              }
              return jsi::Value::undefined();
            }));
  }
}

}  // namespace screenkit
