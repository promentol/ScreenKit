// Copyright (c) ScreenKit contributors. MIT.
#include "SharedRef.h"

#include "JSIUtils.h"

namespace jsi = facebook::jsi;

namespace screenkit {

void installSharedRefClass(jsi::Runtime& runtime) {
  jsi::Object core = jsiutils::coreObject(runtime);
  jsi::Value baseValue = core.getProperty(runtime, "SharedObject");
  if (!baseValue.isObject() || !baseValue.getObject(runtime).isFunction(runtime)) {
    throw jsi::JSError(runtime, "installSharedRefClass requires screenkit.SharedObject");
  }
  jsi::Function base = baseValue.getObject(runtime).getFunction(runtime);

  jsi::Function klass = jsiutils::createInheritingClass(
      runtime, "SharedRef", base,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
        throw jsi::JSError(rt, "SharedRef is not constructible from JavaScript");
      });

  jsi::Object prototype = klass.getPropertyAsObject(runtime, "prototype");
  jsiutils::defineMethod(
      runtime, prototype, "nativeRefType", 0,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value*, size_t) -> jsi::Value {
        auto object = unwrapSharedObject(rt, thisValue);
        return jsi::String::createFromUtf8(rt, object->nativeTypeName());
      });

  jsiutils::defineProperty(runtime, core, "SharedRef", jsi::Value(runtime, klass),
                           jsiutils::kMethod);
}

jsi::Object wrapSharedRef(jsi::Runtime& runtime, std::shared_ptr<SharedRef> ref) {
  return wrapSharedObject(runtime, std::move(ref), "SharedRef");
}

}  // namespace screenkit
