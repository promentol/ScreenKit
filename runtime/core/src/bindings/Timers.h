// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {

class EventLoop;

/// Install the timer and frame globals: `setTimeout`, `setInterval`,
/// `clearTimeout`, `clearInterval`, `queueMicrotask`,
/// `requestAnimationFrame` and `cancelAnimationFrame`.
///
/// Scheduling belongs to `EventLoop`, which sits on SDL3's timers; this file is
/// only the JS-facing edge, so it deals in argument coercion and web semantics
/// and nothing else.
///
/// Must be called on the thread that owns `runtime`.
void installTimerBindings(facebook::jsi::Runtime& runtime, std::shared_ptr<EventLoop> loop);

}  // namespace screenkit
