// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <jsi/jsi.h>

namespace screenkit {

/// The one `jsi::HostObject` in this runtime.
///
/// Everything else binds with `jsi::NativeState`, because interception costs
/// something on every property access. Here interception *is* the feature: a
/// module that JS never touches is never built, which is what keeps a runtime
/// carrying twenty native modules from paying for twenty of them at startup.
///
/// The wrapper is not the object. It has no native state of its own and no
/// prototype of its own, so anything reached *through* one -- a `SharedObject`
/// method, an `EventEmitter` listener list -- has to resolve to the backing
/// object first. `unwrapObjectIfNecessary` is that resolution, and every host
/// function that reads native state calls it.
class LazyObject final : public facebook::jsi::HostObject {
 public:
  /// Builds the backing value. Runs at most once, on the JS thread, the first
  /// time JS reads, writes or enumerates the wrapper.
  using Initializer = std::function<facebook::jsi::Value(facebook::jsi::Runtime&)>;

  explicit LazyObject(Initializer initializer) : initializer_(std::move(initializer)) {}
  ~LazyObject() override;

  bool initialized() const { return backing_ != nullptr; }

  facebook::jsi::Value get(facebook::jsi::Runtime& runtime,
                           const facebook::jsi::PropNameID& name) override;
  void set(facebook::jsi::Runtime& runtime, const facebook::jsi::PropNameID& name,
           const facebook::jsi::Value& value) override;
  std::vector<facebook::jsi::PropNameID> getPropertyNames(
      facebook::jsi::Runtime& runtime) override;

  /// The backing object, building it if this is the first touch.
  const facebook::jsi::Object& backing(facebook::jsi::Runtime& runtime);

 private:
  Initializer initializer_;
  std::shared_ptr<facebook::jsi::Object> backing_;
};

/// `value` if it is anything but a `LazyObject` wrapper; the object behind it if
/// it is. Building it in the process, which is the point.
facebook::jsi::Value unwrapObjectIfNecessary(facebook::jsi::Runtime& runtime,
                                             const facebook::jsi::Value& value);

/// A JS object that stands in for `initializer`'s result until first touch.
facebook::jsi::Object createLazyObject(facebook::jsi::Runtime& runtime,
                                       LazyObject::Initializer initializer);

}  // namespace screenkit
