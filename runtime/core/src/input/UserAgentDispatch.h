// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

/// Run a dispatch the DOM shim handed back as a stepper (its `dispatcher`): one
/// listener callback per step and a microtask checkpoint after each, as a
/// browser does for an event it fires itself rather than one a script
/// dispatches. Does nothing when `stepper` is not a function. JS thread only.
inline void runUserAgentDispatch(JsExecutor& executor, facebook::jsi::Runtime& runtime,
                                 const facebook::jsi::Value& stepper) {
  if (!stepper.isObject() || !stepper.getObject(runtime).isFunction(runtime)) return;
  facebook::jsi::Function step = stepper.getObject(runtime).getFunction(runtime);
  while (true) {
    facebook::jsi::Value more = step.call(runtime);
    if (!more.isBool() || !more.getBool()) break;
    executor.performMicrotaskCheckpoint(runtime);
  }
}

}  // namespace screenkit
