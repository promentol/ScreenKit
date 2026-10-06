// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {

/// The JS-facing event surface: `addListener`, `removeListener`,
/// `removeAllListeners`, `emit`, `listenerCount`.
///
/// Listeners live in a `jsi::NativeState` on the emitter object and the methods
/// live on the prototype, so an emitter is an ordinary JS object with an
/// ordinary shape -- not a `HostObject` intercepting every read.
///
/// **Every listener is isolated.** `emit` snapshots the list before it starts,
/// so a listener that adds or removes listeners cannot change what this
/// dispatch runs, and it calls each one inside its own try/catch, so one
/// throwing listener neither stops the others nor propagates out of `emit`. The
/// error is logged against the listener that raised it.
///
/// Not thread-aware: like expo's, it assumes the JS thread.

/// Install `screenkit.EventEmitter`. Idempotent per runtime. `tag` is recorded
/// **per runtime**, so a second instance (`<iframe>`, Architecture.md 5) reports
/// under its own name rather than overwriting the first's.
void installEventEmitterClass(facebook::jsi::Runtime& runtime, std::string tag);

/// Forget this runtime's log tag. JS thread, at teardown, while the runtime is
/// still alive.
void shutdownEventEmitters(facebook::jsi::Runtime& runtime);

/// A fresh emitter: an instance of `screenkit.EventEmitter` with an empty
/// listener table already attached.
facebook::jsi::Object createEventEmitter(facebook::jsi::Runtime& runtime);

/// Give `object` an emitter's listener table, so a subclass instance built
/// elsewhere can emit. Safe to call twice; the second call is a no-op.
void attachEventEmitterState(facebook::jsi::Runtime& runtime,
                             const facebook::jsi::Object& object);

/// Emit from native. Never throws: a listener that throws is logged and the rest
/// still run.
void emitEvent(facebook::jsi::Runtime& runtime, const facebook::jsi::Object& emitter,
               const std::string& name, const facebook::jsi::Value* args, std::size_t count);

}  // namespace screenkit
