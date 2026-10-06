// Copyright (c) ScreenKit contributors. MIT.
#include "SharedObject.h"

#include <utility>

#include "JSIUtils.h"
#include "LazyObject.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// Where the per-runtime registry is parked so `sharedObjectRegistry()` can find
/// it again. Non-enumerable and namespaced, so it is not something a bundle
/// stumbles over.
constexpr const char* kRegistrySlot = "__sharedObjects";

class RegistryHolder final : public jsi::NativeState {
 public:
  explicit RegistryHolder(std::shared_ptr<SharedObjectRegistry> registry)
      : registry(std::move(registry)) {}
  std::shared_ptr<SharedObjectRegistry> registry;
};

/// The native state a wrapped object carries: an id, and the registry to give it
/// back to.
///
/// The registry is held weakly. A JS object can outlive its runtime's teardown
/// in the collector's own time, and a strong reference here would mean the
/// finalizer reaching into a registry that no longer has an owner.
class SharedObjectState final : public jsi::NativeState {
 public:
  SharedObjectState(std::weak_ptr<SharedObjectRegistry> registry, SharedObjectId id)
      : registry_(std::move(registry)), id_(id) {}

  ~SharedObjectState() override { releaseNow(); }

  SharedObjectId id() const { return released_ ? 0 : id_; }
  bool released() const { return released_; }

  void releaseNow() {
    if (released_) return;
    released_ = true;
    if (auto registry = registry_.lock()) registry->remove(id_);
  }

 private:
  std::weak_ptr<SharedObjectRegistry> registry_;
  SharedObjectId id_ = 0;
  bool released_ = false;
};

[[noreturn]] void throwReleased(jsi::Runtime& runtime) {
  throw jsi::JSError(runtime,
                     "this SharedObject has been released; its native object is gone and the "
                     "wrapper cannot be used again");
}

/// The state behind `thisValue`, or a JS error explaining why there is none.
std::shared_ptr<SharedObjectState> stateOf(jsi::Runtime& runtime, const jsi::Value& thisValue) {
  // A LazyObject wrapper has no native state of its own, so resolve to the
  // backing object before looking. Expo hit exactly this and added
  // `unwrapObjectIfNecessary` for it.
  jsi::Value resolved = unwrapObjectIfNecessary(runtime, thisValue);
  if (!resolved.isObject()) {
    throw jsi::JSError(runtime, "SharedObject method called on a non-object");
  }
  jsi::Object object = resolved.getObject(runtime);
  if (!object.hasNativeState<SharedObjectState>(runtime)) {
    throw jsi::JSError(runtime, "SharedObject method called on an object that is not one");
  }
  auto state = object.getNativeState<SharedObjectState>(runtime);
  if (!state || state->released()) throwReleased(runtime);
  return state;
}

}  // namespace

SharedObject::~SharedObject() = default;

// --- registry ----------------------------------------------------------------

SharedObjectId SharedObjectRegistry::add(std::shared_ptr<SharedObject> object) {
  if (!object) return 0;
  SdlLock lock(mutex_);
  const SharedObjectId id = next_++;
  object->id_ = id;
  objects_.emplace(id, std::move(object));
  return id;
}

std::shared_ptr<SharedObject> SharedObjectRegistry::get(SharedObjectId id) const {
  SdlLock lock(mutex_);
  const auto it = objects_.find(id);
  return it == objects_.end() ? nullptr : it->second;
}

bool SharedObjectRegistry::remove(SharedObjectId id) {
  std::shared_ptr<SharedObject> doomed;
  {
    SdlLock lock(mutex_);
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    doomed = std::move(it->second);
    objects_.erase(it);
  }
  // The native destructor runs here, outside the lock: it is arbitrary code and
  // has no business being able to re-enter the registry under its own lock.
  return true;
}

std::size_t SharedObjectRegistry::size() const {
  SdlLock lock(mutex_);
  return objects_.size();
}

void SharedObjectRegistry::clear() {
  std::unordered_map<SharedObjectId, std::shared_ptr<SharedObject>> doomed;
  {
    SdlLock lock(mutex_);
    doomed.swap(objects_);
  }
}

// --- the JS class ------------------------------------------------------------

void installSharedObjectClass(jsi::Runtime& runtime,
                              std::shared_ptr<SharedObjectRegistry> registry) {
  jsi::Object core = jsiutils::coreObject(runtime);

  jsi::Object holder(runtime);
  holder.setNativeState(runtime, std::make_shared<RegistryHolder>(std::move(registry)));
  jsiutils::defineProperty(runtime, core, kRegistrySlot, jsi::Value(runtime, holder),
                           jsiutils::kNone);

  jsi::Function klass = jsiutils::createClass(
      runtime, "SharedObject", [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
        // Constructing one from JS would produce a wrapper with nothing behind
        // it -- an object that throws on every method. Refuse it outright.
        throw jsi::JSError(rt, "SharedObject is not constructible from JavaScript");
      });

  jsi::Object prototype = klass.getPropertyAsObject(runtime, "prototype");

  // Explicit release, for callers who will not wait for the collector. Calling
  // it twice throws, on the same rule as every other use: a released wrapper is
  // inert.
  jsiutils::defineMethod(
      runtime, prototype, "release", 0,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value*, size_t) -> jsi::Value {
        stateOf(rt, thisValue)->releaseNow();
        return jsi::Value::undefined();
      });

  // The id is the testable, inspectable proof that a wrapper still resolves to
  // something -- and the natural thing to call to prove that a released one no
  // longer does.
  jsiutils::defineMethod(
      runtime, prototype, "nativeId", 0,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value*, size_t) -> jsi::Value {
        return jsi::Value(static_cast<double>(stateOf(rt, thisValue)->id()));
      });

  jsiutils::defineMethod(
      runtime, prototype, "nativeTypeName", 0,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value*, size_t) -> jsi::Value {
        auto object = unwrapSharedObject(rt, thisValue);
        return jsi::String::createFromUtf8(rt, object->nativeTypeName());
      });

  jsiutils::defineProperty(runtime, core, "SharedObject", jsi::Value(runtime, klass),
                           jsiutils::kMethod);
}

std::shared_ptr<SharedObjectRegistry> sharedObjectRegistry(jsi::Runtime& runtime) {
  jsi::Value slot = jsiutils::coreObject(runtime).getProperty(runtime, kRegistrySlot);
  if (!slot.isObject()) return nullptr;
  jsi::Object holder = slot.getObject(runtime);
  if (!holder.hasNativeState<RegistryHolder>(runtime)) return nullptr;
  auto state = holder.getNativeState<RegistryHolder>(runtime);
  return state ? state->registry : nullptr;
}

jsi::Object wrapSharedObject(jsi::Runtime& runtime, std::shared_ptr<SharedObject> object,
                             const char* className) {
  auto registry = sharedObjectRegistry(runtime);
  if (!registry) {
    throw jsi::JSError(runtime, "the SharedObject registry is not installed on this runtime");
  }

  jsi::Value klassValue = jsiutils::coreObject(runtime).getProperty(runtime, className);
  if (!klassValue.isObject() || !klassValue.getObject(runtime).isFunction(runtime)) {
    throw jsi::JSError(runtime,
                       std::string("screenkit.") + className + " is not installed on this runtime");
  }
  jsi::Function klass = klassValue.getObject(runtime).getFunction(runtime);

  const SharedObjectId id = registry->add(std::move(object));
  jsi::Object instance = jsiutils::createInstance(runtime, klass);
  instance.setNativeState(runtime, std::make_shared<SharedObjectState>(
                                       std::weak_ptr<SharedObjectRegistry>(registry), id));
  return instance;
}

std::shared_ptr<SharedObject> unwrapSharedObject(jsi::Runtime& runtime, const jsi::Value& value) {
  auto state = stateOf(runtime, value);
  auto registry = sharedObjectRegistry(runtime);
  std::shared_ptr<SharedObject> object = registry ? registry->get(state->id()) : nullptr;
  if (!object) throwReleased(runtime);
  return object;
}

}  // namespace screenkit
