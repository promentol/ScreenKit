// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>
#include <string>
#include <utility>

#include <jsi/jsi.h>

#include "SharedObject.h"

namespace screenkit {

/// A `SharedObject` that wraps a reference somebody else already owns.
///
/// `SharedObject` is for native objects whose whole life is the JS wrapper.
/// `SharedRef` is the other half of the pattern: a texture, a decoded image, a
/// player handle -- something with its own native identity that JS needs a
/// handle to. The distinction is visible from JS as `nativeRefType`, which is
/// what lets a binding refuse the wrong kind of handle with a real message
/// instead of a cast that happens to work.
class SharedRef : public SharedObject {
 public:
  SharedRef(std::shared_ptr<void> ref, std::string typeName)
      : ref_(std::move(ref)), typeName_(std::move(typeName)) {}

  const std::shared_ptr<void>& nativeRef() const { return ref_; }
  std::string nativeTypeName() const override { return typeName_; }

 private:
  std::shared_ptr<void> ref_;
  std::string typeName_;
};

/// Install `screenkit.SharedRef`, inheriting from `screenkit.SharedObject`.
/// `installSharedObjectClass` must have run first.
void installSharedRefClass(facebook::jsi::Runtime& runtime);

/// Hand a native reference to JS as a `SharedRef`.
facebook::jsi::Object wrapSharedRef(facebook::jsi::Runtime& runtime,
                                    std::shared_ptr<SharedRef> ref);

}  // namespace screenkit
