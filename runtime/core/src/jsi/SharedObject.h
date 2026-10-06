// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <jsi/jsi.h>

#include "../loop/SdlSync.h"

namespace screenkit {

using SharedObjectId = std::uint64_t;

/// A native object with a JS-visible lifetime.
///
/// Lifetime is bound with `jsi::NativeState` plus an id registry, **not**
/// `jsi::HostObject`. The distinction matters: a `HostObject` intercepts every
/// property access, which is a per-access cost and a per-access chance to get
/// the semantics subtly wrong. A native-state binding leaves the JS object an
/// ordinary object with an ordinary prototype, and the native side is reached
/// only by the methods that actually need it. `LazyObject` is the one
/// `HostObject` in this runtime, because interception is the entire point of it.
///
/// Two things release the native object, and both must work: the `NativeState`
/// destructor when JS drops its last reference and the collector runs, and an
/// explicit `release()` for callers who will not wait for GC. After either, the
/// JS object is inert -- any further use throws rather than reaching freed
/// memory.
class SharedObject {
 public:
  virtual ~SharedObject();

  SharedObject(const SharedObject&) = delete;
  SharedObject& operator=(const SharedObject&) = delete;

  SharedObjectId sharedObjectId() const { return id_; }

  /// Names the concrete type in diagnostics and in `SharedRef`'s JS-visible
  /// `nativeRefType`.
  virtual std::string nativeTypeName() const { return "SharedObject"; }

 protected:
  SharedObject() = default;

 private:
  friend class SharedObjectRegistry;
  SharedObjectId id_ = 0;
};

/// The id -> object table a `SharedObject`'s native state indexes into.
///
/// One per runtime. Guarded by an SDL mutex even though the JS thread owns it,
/// because a `NativeState` destructor can run from wherever the collector
/// happens to be and because teardown clears it from the same thread the loop
/// ran on.
class SharedObjectRegistry {
 public:
  SharedObjectId add(std::shared_ptr<SharedObject> object);
  std::shared_ptr<SharedObject> get(SharedObjectId id) const;
  /// True when the id was live. The native destructor runs here unless someone
  /// else still holds the object.
  bool remove(SharedObjectId id);
  std::size_t size() const;
  void clear();

 private:
  mutable SdlMutex mutex_;
  std::unordered_map<SharedObjectId, std::shared_ptr<SharedObject>> objects_;
  /// Monotonic, never reused, so a stale id is "gone" rather than somebody
  /// else's object.
  SharedObjectId next_ = 1;
};

/// Install `screenkit.SharedObject`. Idempotent per runtime.
void installSharedObjectClass(facebook::jsi::Runtime& runtime,
                              std::shared_ptr<SharedObjectRegistry> registry);

/// The registry this runtime was installed with, or nullptr.
std::shared_ptr<SharedObjectRegistry> sharedObjectRegistry(facebook::jsi::Runtime& runtime);

/// Hand a native object to JS as an instance of `screenkit.<className>`.
facebook::jsi::Object wrapSharedObject(facebook::jsi::Runtime& runtime,
                                       std::shared_ptr<SharedObject> object,
                                       const char* className = "SharedObject");

/// Resolve a JS value to the native object behind it, unwrapping a `LazyObject`
/// on the way. Throws a `jsi::JSError` when the value is not a `SharedObject` or
/// has already been released -- a clear JS error, never a native crash.
std::shared_ptr<SharedObject> unwrapSharedObject(facebook::jsi::Runtime& runtime,
                                                 const facebook::jsi::Value& value);

}  // namespace screenkit
