// Copyright (c) ScreenKit contributors. MIT.
//
// The small set of things every JSI binding in this runtime needs and JSI
// itself does not provide: where the runtime's own namespace lives, how to make
// a JS class whose construction runs native code, how to define a property with
// attributes, and how to learn that a JS object has been collected.
#pragma once

#include <functional>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {
namespace jsiutils {

/// `global.screenkit`, created on first use. Everything this runtime exposes to
/// JS that is not a web standard lives under it, so the global namespace picks
/// up exactly one name.
facebook::jsi::Object coreObject(facebook::jsi::Runtime& runtime);

/// Runs when JS calls `new Klass(...)`. `thisValue` is the instance being
/// constructed; throw a `jsi::JSError` to reject the construction.
using ClassConstructor = std::function<void(facebook::jsi::Runtime& runtime,
                                            const facebook::jsi::Value& thisValue,
                                            const facebook::jsi::Value* args, size_t count)>;

/// A JS class with a native constructor body.
///
/// The class itself is a real JS function rather than a host function, because
/// a host function is not a constructor in every engine and because `class`
/// semantics -- `instanceof`, a live `prototype`, a usable `name` -- are what
/// make these objects look ordinary from JS. The one-line function it evaluates
/// only forwards to a non-enumerable host function on the prototype.
facebook::jsi::Function createClass(facebook::jsi::Runtime& runtime, const char* name,
                                    ClassConstructor constructor);

/// `class Name extends Base`. Both the prototype chain and the static chain are
/// linked, so `Name.prototype instanceof Base` and inherited statics both work.
facebook::jsi::Function createInheritingClass(facebook::jsi::Runtime& runtime, const char* name,
                                              const facebook::jsi::Function& base,
                                              ClassConstructor constructor);

/// A fresh object whose prototype is `klass.prototype`, without running the
/// constructor -- how a native wrapper is handed to JS already typed.
facebook::jsi::Object createInstance(facebook::jsi::Runtime& runtime,
                                     const facebook::jsi::Function& klass);

enum PropertyAttributes {
  kNone = 0,
  kWritable = 1 << 0,
  kEnumerable = 1 << 1,
  kConfigurable = 1 << 2,
  /// What a prototype method wants: reachable and replaceable, but not listed
  /// by `Object.keys` or spread into a copy.
  kMethod = kWritable | kConfigurable,
};

void defineProperty(facebook::jsi::Runtime& runtime, const facebook::jsi::Object& target,
                    const char* name, const facebook::jsi::Value& value, int attributes);

/// Convenience for the overwhelmingly common case: a non-enumerable method.
void defineMethod(facebook::jsi::Runtime& runtime, const facebook::jsi::Object& target,
                  const char* name, unsigned argCount,
                  facebook::jsi::HostFunctionType function);

/// Runs a callback when the JS object it is attached to is collected.
///
/// `jsi::NativeState`'s destructor is the only GC signal JSI offers, so this is
/// that destructor wearing a name. Attaching one costs the object its native
/// state slot, which is why `SharedObject` carries its own rather than using
/// this.
class ObjectDeallocator final : public facebook::jsi::NativeState {
 public:
  explicit ObjectDeallocator(std::function<void()> deallocate)
      : deallocate_(std::move(deallocate)) {}
  ~ObjectDeallocator() override;

 private:
  std::function<void()> deallocate_;
};

void setDeallocator(facebook::jsi::Runtime& runtime, const facebook::jsi::Object& object,
                    std::function<void()> deallocate);

}  // namespace jsiutils
}  // namespace screenkit
