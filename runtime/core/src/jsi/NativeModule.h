// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>
#include <string>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

class SharedObjectRegistry;

/// Install the whole object model on a runtime: `SharedObject`, `SharedRef`,
/// `EventEmitter`, `NativeModule` and the `screenkit.modules` namespace. Called
/// once, on the JS thread, while the runtime is being built.
void installObjectModel(facebook::jsi::Runtime& runtime,
                        std::shared_ptr<SharedObjectRegistry> registry, std::string tag);

/// Install `screenkit.NativeModule`, inheriting from `screenkit.EventEmitter`.
///
/// A native module *is* an event emitter -- that is the whole reason for the
/// inheritance. Native code that has something to say (a player reached the end,
/// a socket closed) has one way to say it, and JS has one way to listen, rather
/// than a callback convention per module.
void installNativeModuleClass(facebook::jsi::Runtime& runtime);

/// A fresh module object: a `NativeModule` instance with a listener table and a
/// non-writable `name`.
facebook::jsi::Object createNativeModule(facebook::jsi::Runtime& runtime,
                                         const std::string& name);

/// Put a module at `screenkit.modules.<name>`, behind a `LazyObject` so
/// `factory` does not run until JS touches it.
void installModule(facebook::jsi::Runtime& runtime, std::string name, ModuleFactory factory);

}  // namespace screenkit
