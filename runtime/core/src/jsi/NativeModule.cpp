// Copyright (c) ScreenKit contributors. MIT.
#include "NativeModule.h"

#include <utility>

#include "EventEmitter.h"
#include "JSIUtils.h"
#include "LazyObject.h"
#include "SharedObject.h"
#include "SharedRef.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

constexpr const char* kModules = "modules";

jsi::Object modulesObject(jsi::Runtime& runtime) {
  jsi::Object core = jsiutils::coreObject(runtime);
  jsi::Value existing = core.getProperty(runtime, kModules);
  if (existing.isObject()) return existing.getObject(runtime);
  jsi::Object modules(runtime);
  core.setProperty(runtime, kModules, modules);
  return modules;
}

}  // namespace

void installNativeModuleClass(jsi::Runtime& runtime) {
  jsi::Object core = jsiutils::coreObject(runtime);
  jsi::Value baseValue = core.getProperty(runtime, "EventEmitter");
  if (!baseValue.isObject() || !baseValue.getObject(runtime).isFunction(runtime)) {
    throw jsi::JSError(runtime, "installNativeModuleClass requires screenkit.EventEmitter");
  }
  jsi::Function base = baseValue.getObject(runtime).getFunction(runtime);

  jsi::Function klass = jsiutils::createInheritingClass(
      runtime, "NativeModule", base,
      [](jsi::Runtime& rt, const jsi::Value& thisValue, const jsi::Value* args, size_t count) {
        if (!thisValue.isObject()) {
          throw jsi::JSError(rt, "NativeModule must be constructed with new");
        }
        jsi::Object self = thisValue.getObject(rt);
        attachEventEmitterState(rt, self);
        if (count >= 1 && args[0].isString()) {
          jsiutils::defineProperty(rt, self, "name", jsi::Value(rt, args[0]),
                                   jsiutils::kEnumerable | jsiutils::kConfigurable);
        }
      });

  jsiutils::defineProperty(runtime, core, "NativeModule", jsi::Value(runtime, klass),
                           jsiutils::kMethod);
}

jsi::Object createNativeModule(jsi::Runtime& runtime, const std::string& name) {
  jsi::Value klassValue = jsiutils::coreObject(runtime).getProperty(runtime, "NativeModule");
  if (!klassValue.isObject() || !klassValue.getObject(runtime).isFunction(runtime)) {
    throw jsi::JSError(runtime, "screenkit.NativeModule is not installed on this runtime");
  }
  jsi::Object module =
      jsiutils::createInstance(runtime, klassValue.getObject(runtime).getFunction(runtime));
  attachEventEmitterState(runtime, module);
  jsiutils::defineProperty(runtime, module, "name", jsi::String::createFromUtf8(runtime, name),
                           jsiutils::kEnumerable | jsiutils::kConfigurable);
  return module;
}

void installModule(jsi::Runtime& runtime, std::string name, ModuleFactory factory) {
  jsi::Object modules = modulesObject(runtime);
  jsi::Object lazy = createLazyObject(
      runtime, [name, factory = std::move(factory)](jsi::Runtime& rt) -> jsi::Value {
        if (!factory) return createNativeModule(rt, name);
        return factory(rt);
      });
  modules.setProperty(runtime, name.c_str(), lazy);
}

void installObjectModel(jsi::Runtime& runtime, std::shared_ptr<SharedObjectRegistry> registry,
                        std::string tag) {
  installSharedObjectClass(runtime, std::move(registry));
  installSharedRefClass(runtime);
  installEventEmitterClass(runtime, std::move(tag));
  installNativeModuleClass(runtime);
  // Present from the start, so a bundle can feature-detect `screenkit.modules`
  // rather than discovering it only once something registers.
  modulesObject(runtime);
}

}  // namespace screenkit
