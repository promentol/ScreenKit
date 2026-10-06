// Copyright (c) ScreenKit contributors. MIT.
#include "EventEmitter.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "JSIUtils.h"
#include "LazyObject.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// The log tag emitters report listener errors under -- **per runtime**, keyed
/// by the runtime pointer as the vendored GL registry keys its contexts.
///
/// It used to be one process-wide string assigned by whichever JS thread
/// installed the class last, which was a real unsynchronised cross-thread write
/// the moment a second instance existed (`<iframe>`, Architecture.md 5). Only an
/// error path reads it, so the lock is never on anyone's hot path. An entry is
/// dropped at the runtime's teardown; a pointer that is reused before then is
/// overwritten at install, so a stale entry can only ever be replaced, not read.
std::mutex& emitterTagMutex() {
  static std::mutex m;
  return m;
}

std::unordered_map<jsi::Runtime*, std::string>& emitterTags() {
  // Deliberately never destroyed: a listener can throw while the process is
  // unwinding, and a lookup then must find an empty table rather than a
  // destroyed one (WorkQueue's QueueTable takes the same care).
  static auto* tags = new std::unordered_map<jsi::Runtime*, std::string>();
  return *tags;
}

std::string emitterTag(jsi::Runtime& runtime) {
  std::lock_guard<std::mutex> lock(emitterTagMutex());
  const auto it = emitterTags().find(&runtime);
  return it == emitterTags().end() ? std::string("screenkit") : it->second;
}

class EmitterState final : public jsi::NativeState {
 public:
  std::unordered_map<std::string, std::vector<std::shared_ptr<jsi::Function>>> listeners;
};

std::shared_ptr<EmitterState> stateOf(jsi::Runtime& runtime, const jsi::Value& thisValue,
                                      bool required) {
  // A LazyObject wrapper carries no native state, so resolve through it first.
  jsi::Value resolved = unwrapObjectIfNecessary(runtime, thisValue);
  if (!resolved.isObject()) {
    if (!required) return nullptr;
    throw jsi::JSError(runtime, "EventEmitter method called on a non-object");
  }
  jsi::Object object = resolved.getObject(runtime);
  if (!object.hasNativeState<EmitterState>(runtime)) {
    if (!required) return nullptr;
    throw jsi::JSError(runtime, "EventEmitter method called on an object that is not one");
  }
  return object.getNativeState<EmitterState>(runtime);
}

std::string eventName(jsi::Runtime& runtime, const jsi::Value* args, size_t count) {
  if (count < 1 || !args[0].isString()) {
    throw jsi::JSError(runtime, "the first argument must be the event name");
  }
  return args[0].getString(runtime).utf8(runtime);
}

std::string describe(const jsi::JSError& error) {
  std::string message = error.getMessage();
  const std::string& stack = error.getStack();
  if (!stack.empty()) message += "\n" + stack;
  return message;
}

/// Runs one listener with its own boundary. Returns without rethrowing whatever
/// happened, because the contract is that the next listener still runs.
void callIsolated(jsi::Runtime& runtime, const jsi::Function& listener, const std::string& name,
                  std::size_t index, const jsi::Value* args, std::size_t count) {
  try {
    listener.call(runtime, args, count);
  } catch (const jsi::JSError& error) {
    log(LogLevel::Error, emitterTag(runtime),
        "listener " + std::to_string(index) + " for \"" + name + "\" threw: " + describe(error));
  } catch (const std::exception& error) {
    log(LogLevel::Error, emitterTag(runtime), "listener " + std::to_string(index) + " for \"" + name +
                                                  "\" threw: " + error.what());
  } catch (...) {
    log(LogLevel::Error, emitterTag(runtime),
        "listener " + std::to_string(index) + " for \"" + name + "\" threw an unknown exception");
  }
}

void dispatch(jsi::Runtime& runtime, const std::shared_ptr<EmitterState>& state,
              const std::string& name, const jsi::Value* args, std::size_t count) {
  if (!state) return;
  const auto it = state->listeners.find(name);
  if (it == state->listeners.end()) return;

  // Snapshot before dispatching: a listener is allowed to add or remove
  // listeners, and without the copy that mutation would invalidate the iteration
  // underneath the call.
  const std::vector<std::shared_ptr<jsi::Function>> snapshot = it->second;
  for (std::size_t i = 0; i < snapshot.size(); ++i) {
    if (!snapshot[i]) continue;
    callIsolated(runtime, *snapshot[i], name, i, args, count);
  }
}

}  // namespace

void attachEventEmitterState(jsi::Runtime& runtime, const jsi::Object& object) {
  if (object.hasNativeState<EmitterState>(runtime)) return;
  object.setNativeState(runtime, std::make_shared<EmitterState>());
}

void shutdownEventEmitters(jsi::Runtime& runtime) {
  std::lock_guard<std::mutex> lock(emitterTagMutex());
  emitterTags().erase(&runtime);
}

void installEventEmitterClass(jsi::Runtime& runtime, std::string tag) {
  {
    std::lock_guard<std::mutex> lock(emitterTagMutex());
    emitterTags()[&runtime] = std::move(tag);
  }

  jsi::Function klass = jsiutils::createClass(
      runtime, "EventEmitter",
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value*, size_t) {
        if (!thisValue.isObject()) {
          throw jsi::JSError(rt, "EventEmitter must be constructed with new");
        }
        attachEventEmitterState(rt, thisValue.getObject(rt));
      });

  jsi::Object prototype = klass.getPropertyAsObject(runtime, "prototype");

  jsiutils::defineMethod(
      runtime, prototype, "addListener", 2,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args,
         size_t count) -> jsi::Value {
        auto state = stateOf(rt, thisValue, /*required=*/true);
        const std::string name = eventName(rt, args, count);
        if (count < 2 || !args[1].isObject() || !args[1].getObject(rt).isFunction(rt)) {
          throw jsi::JSError(rt, "the second argument must be the listener function");
        }
        auto listener =
            std::make_shared<jsi::Function>(args[1].getObject(rt).getFunction(rt));
        state->listeners[name].push_back(listener);

        // A subscription object, not a bare handle: `.remove()` is what web and
        // React Native code reaches for, and it keeps the listener identity out
        // of the caller's hands.
        jsi::Object subscription(rt);
        jsiutils::defineMethod(
            rt, subscription, "remove", 0,
            [state, name, listener](jsi::Runtime& rt2, const jsi::Value&, const jsi::Value*,
                                    size_t) -> jsi::Value {
              const auto it = state->listeners.find(name);
              if (it != state->listeners.end()) {
                auto& list = it->second;
                list.erase(std::remove(list.begin(), list.end(), listener), list.end());
                if (list.empty()) state->listeners.erase(it);
              }
              (void)rt2;
              return jsi::Value::undefined();
            });
        return subscription;
      });

  jsiutils::defineMethod(
      runtime, prototype, "removeListener", 2,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args,
         size_t count) -> jsi::Value {
        auto state = stateOf(rt, thisValue, /*required=*/true);
        const std::string name = eventName(rt, args, count);
        if (count < 2 || !args[1].isObject() || !args[1].getObject(rt).isFunction(rt)) {
          throw jsi::JSError(rt, "the second argument must be the listener function");
        }
        jsi::Function target = args[1].getObject(rt).getFunction(rt);
        const auto it = state->listeners.find(name);
        if (it == state->listeners.end()) return jsi::Value::undefined();

        auto& list = it->second;
        // Identity is the JS function's, not the shared_ptr's: JS hands back the
        // same function object it registered, wrapped in a different jsi handle.
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const std::shared_ptr<jsi::Function>& candidate) {
                                    return candidate &&
                                           jsi::Object::strictEquals(rt, *candidate, target);
                                  }),
                   list.end());
        if (list.empty()) state->listeners.erase(it);
        return jsi::Value::undefined();
      });

  jsiutils::defineMethod(
      runtime, prototype, "removeAllListeners", 1,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args,
         size_t count) -> jsi::Value {
        auto state = stateOf(rt, thisValue, /*required=*/true);
        if (count >= 1 && args[0].isString()) {
          state->listeners.erase(args[0].getString(rt).utf8(rt));
        } else {
          state->listeners.clear();
        }
        return jsi::Value::undefined();
      });

  jsiutils::defineMethod(
      runtime, prototype, "listenerCount", 1,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args,
         size_t count) -> jsi::Value {
        auto state = stateOf(rt, thisValue, /*required=*/true);
        const std::string name = eventName(rt, args, count);
        const auto it = state->listeners.find(name);
        return jsi::Value(static_cast<double>(it == state->listeners.end() ? 0 : it->second.size()));
      });

  jsiutils::defineMethod(
      runtime, prototype, "emit", 1,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args,
         size_t count) -> jsi::Value {
        auto state = stateOf(rt, thisValue, /*required=*/true);
        const std::string name = eventName(rt, args, count);
        // emit itself never throws on a listener's behalf -- that is the whole
        // isolation contract -- so the payload is forwarded and the boundary is
        // per listener, inside dispatch().
        dispatch(rt, state, name, count > 1 ? args + 1 : nullptr, count > 1 ? count - 1 : 0);
        return jsi::Value::undefined();
      });

  jsiutils::defineProperty(runtime, jsiutils::coreObject(runtime), "EventEmitter",
                           jsi::Value(runtime, klass), jsiutils::kMethod);
}

jsi::Object createEventEmitter(jsi::Runtime& runtime) {
  jsi::Value klassValue = jsiutils::coreObject(runtime).getProperty(runtime, "EventEmitter");
  if (!klassValue.isObject() || !klassValue.getObject(runtime).isFunction(runtime)) {
    throw jsi::JSError(runtime, "screenkit.EventEmitter is not installed on this runtime");
  }
  jsi::Object emitter =
      jsiutils::createInstance(runtime, klassValue.getObject(runtime).getFunction(runtime));
  attachEventEmitterState(runtime, emitter);
  return emitter;
}

void emitEvent(jsi::Runtime& runtime, const jsi::Object& emitter, const std::string& name,
               const jsi::Value* args, std::size_t count) {
  if (!emitter.hasNativeState<EmitterState>(runtime)) return;
  dispatch(runtime, emitter.getNativeState<EmitterState>(runtime), name, args, count);
}

}  // namespace screenkit
