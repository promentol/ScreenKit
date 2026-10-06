// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <string>

#include <jsi/jsi.h>

namespace screenkit {

/// Install `console.log` / `.warn` / `.error` on the global object.
///
/// Installed directly, with no object model behind it: no SharedObject, no
/// EventEmitter, no NativeModule registry. Those are a separate spec, and
/// `console` is the one binding this runtime needs in order to prove it ran
/// anything at all.
///
/// Must be called on the thread that owns `runtime`.
void installConsole(facebook::jsi::Runtime& runtime, std::string tag);

/// Log every promise rejection nothing handles, as an error with its stack.
///
/// Without it a rejection simply disappears. That is how a real app failed
/// silently: Blits mounts the app inside `Promise.resolve().then(...)`, so a
/// throw during mount -- a missing DOM method, say -- became a rejected promise,
/// and the screen stayed black with nothing in the log. Uses Hermes' own
/// `HermesInternal.enablePromiseRejectionTracker`; on an engine without it this
/// installs nothing.
///
/// Must be called on the thread that owns `runtime`, after `installConsole`.
void installRejectionTracker(facebook::jsi::Runtime& runtime, std::string tag);

}  // namespace screenkit
