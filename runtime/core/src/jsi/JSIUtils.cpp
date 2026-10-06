// Copyright (c) ScreenKit contributors. MIT.
#include "JSIUtils.h"

#include <memory>
#include <utility>

namespace jsi = facebook::jsi;

namespace screenkit {
namespace jsiutils {
namespace {

/// The prototype slot the evaluated constructor forwards to. Non-enumerable, so
/// it never shows up in `Object.keys` or a spread of an instance.
constexpr const char* kNativeConstructor = "__nativeConstructor__";

jsi::Object objectConstructor(jsi::Runtime& runtime) {
  return runtime.global().getPropertyAsObject(runtime, "Object");
}

jsi::Function classFromSource(jsi::Runtime& runtime, const char* name) {
  // A real JS function, not a host function: host functions are not reliably
  // constructible, and `instanceof` against one is engine-dependent.
  std::string source = "(function ";
  source += name;
  source += "(...args) { this.";
  source += kNativeConstructor;
  source += "(...args); return this; })";
  auto buffer = std::make_shared<const jsi::StringBuffer>(std::move(source));
  return runtime.evaluateJavaScript(buffer, "screenkit://class")
      .asObject(runtime)
      .asFunction(runtime);
}

void attachConstructor(jsi::Runtime& runtime, const jsi::Function& klass, const char* name,
                       ClassConstructor constructor) {
  jsi::Object prototype = klass.getPropertyAsObject(runtime, "prototype");
  jsi::Function body = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, kNativeConstructor), 0,
      [constructor = std::move(constructor)](jsi::Runtime& rt, const jsi::Value& thisValue,
                                             const jsi::Value* args, size_t count) -> jsi::Value {
        if (constructor) constructor(rt, thisValue, args, count);
        return jsi::Value::undefined();
      });
  defineProperty(runtime, prototype, kNativeConstructor, jsi::Value(runtime, body), kMethod);
  (void)name;
}

}  // namespace

jsi::Object coreObject(jsi::Runtime& runtime) {
  jsi::Object global = runtime.global();
  jsi::Value existing = global.getProperty(runtime, "screenkit");
  if (existing.isObject()) return existing.getObject(runtime);

  jsi::Object core(runtime);
  global.setProperty(runtime, "screenkit", core);
  return core;
}

jsi::Function createClass(jsi::Runtime& runtime, const char* name, ClassConstructor constructor) {
  jsi::Function klass = classFromSource(runtime, name);
  attachConstructor(runtime, klass, name, std::move(constructor));
  return klass;
}

jsi::Function createInheritingClass(jsi::Runtime& runtime, const char* name,
                                    const jsi::Function& base, ClassConstructor constructor) {
  jsi::Function klass = classFromSource(runtime, name);

  // Both chains: the instance chain so methods are inherited, and the static
  // chain so `Derived.someStatic` resolves the way `class ... extends` gives
  // you for free.
  jsi::Object prototype = klass.getPropertyAsObject(runtime, "prototype");
  jsi::Value basePrototype = base.getProperty(runtime, "prototype");
  prototype.setPrototype(runtime, basePrototype);
  klass.setPrototype(runtime, jsi::Value(runtime, base));

  attachConstructor(runtime, klass, name, std::move(constructor));
  return klass;
}

jsi::Object createInstance(jsi::Runtime& runtime, const jsi::Function& klass) {
  jsi::Object instance(runtime);
  instance.setPrototype(runtime, klass.getProperty(runtime, "prototype"));
  return instance;
}

void defineProperty(jsi::Runtime& runtime, const jsi::Object& target, const char* name,
                    const jsi::Value& value, int attributes) {
  jsi::Object descriptor(runtime);
  descriptor.setProperty(runtime, "value", value);
  descriptor.setProperty(runtime, "writable", (attributes & kWritable) != 0);
  descriptor.setProperty(runtime, "enumerable", (attributes & kEnumerable) != 0);
  descriptor.setProperty(runtime, "configurable", (attributes & kConfigurable) != 0);

  jsi::Function define = objectConstructor(runtime).getPropertyAsFunction(runtime, "defineProperty");
  define.call(runtime, target, jsi::String::createFromAscii(runtime, name), descriptor);
}

void defineMethod(jsi::Runtime& runtime, const jsi::Object& target, const char* name,
                  unsigned argCount, jsi::HostFunctionType function) {
  jsi::Function method = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, name), argCount, std::move(function));
  defineProperty(runtime, target, name, jsi::Value(runtime, method), kMethod);
}

ObjectDeallocator::~ObjectDeallocator() {
  if (deallocate_) deallocate_();
}

void setDeallocator(jsi::Runtime& runtime, const jsi::Object& object,
                    std::function<void()> deallocate) {
  object.setNativeState(runtime, std::make_shared<ObjectDeallocator>(std::move(deallocate)));
}

}  // namespace jsiutils
}  // namespace screenkit
