// Copyright (c) ScreenKit contributors. MIT.
#include "LazyObject.h"

#include <utility>

namespace jsi = facebook::jsi;

namespace screenkit {

LazyObject::~LazyObject() = default;

const jsi::Object& LazyObject::backing(jsi::Runtime& runtime) {
  if (backing_) return *backing_;

  jsi::Value built = initializer_ ? initializer_(runtime) : jsi::Value::undefined();
  // Released as soon as it has run: an initializer that captures state has no
  // reason to keep it alive for the life of the module.
  initializer_ = nullptr;

  backing_ = std::make_shared<jsi::Object>(built.isObject() ? built.getObject(runtime)
                                                            : jsi::Object(runtime));
  return *backing_;
}

jsi::Value LazyObject::get(jsi::Runtime& runtime, const jsi::PropNameID& name) {
  return backing(runtime).getProperty(runtime, name);
}

void LazyObject::set(jsi::Runtime& runtime, const jsi::PropNameID& name, const jsi::Value& value) {
  backing(runtime).setProperty(runtime, name, value);
}

std::vector<jsi::PropNameID> LazyObject::getPropertyNames(jsi::Runtime& runtime) {
  std::vector<jsi::PropNameID> names;
  jsi::Array keys = backing(runtime).getPropertyNames(runtime);
  const size_t count = keys.size(runtime);
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    names.push_back(
        jsi::PropNameID::forString(runtime, keys.getValueAtIndex(runtime, i).asString(runtime)));
  }
  return names;
}

jsi::Value unwrapObjectIfNecessary(jsi::Runtime& runtime, const jsi::Value& value) {
  if (!value.isObject()) return jsi::Value(runtime, value);
  jsi::Object object = value.getObject(runtime);
  if (!object.isHostObject<LazyObject>(runtime)) return jsi::Value(runtime, object);
  auto lazy = object.getHostObject<LazyObject>(runtime);
  if (!lazy) return jsi::Value(runtime, object);
  return jsi::Value(runtime, lazy->backing(runtime));
}

jsi::Object createLazyObject(jsi::Runtime& runtime, LazyObject::Initializer initializer) {
  return jsi::Object::createFromHostObject(runtime,
                                           std::make_shared<LazyObject>(std::move(initializer)));
}

}  // namespace screenkit
