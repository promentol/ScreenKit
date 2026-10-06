// Copyright (c) ScreenKit contributors. MIT.
#include <screenkit/Viewport.h>

#include <string>
#include <utility>

#include <jsi/jsi.h>

#include <screenkit/Instance.h>
#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

#include "gfx/VendoredWebGL.h"
#include "input/UserAgentDispatch.h"

namespace jsi = facebook::jsi;

namespace screenkit {

ViewportEvents::ViewportEvents(std::shared_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {}

bool ViewportEvents::handleEvent(const SDL_Event& event) {
  if (event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) return false;
  // The window's own context hears it, focused or not: an instance draws into a
  // fixed-size layer that a window resize does not change, while the launcher is
  // the one that owns the window, lays out every plane in drawable pixels
  // (dom-shim's `planeFor`) and holds the drawable that has to be let go of
  // below. Sending this to a focused instance instead lost it for everyone.
  const std::shared_ptr<Runtime>& runtime = runtime_;
  if (!runtime) return true;
  std::shared_ptr<JsExecutor> executor = runtime->executor();
  if (!executor) return true;
  // One queued task answers every change before it runs: it reads the size when
  // it runs, not the size this event carried.
  if (queued_->exchange(true)) return true;

  std::weak_ptr<JsExecutor> weakExecutor = executor;
  auto queued = queued_;
  // Not dropped while paused, unlike input: a resize is a fact about the
  // window, and a context that thaws into a new size needs to hear about it.
  executor->invokeAsync([weakExecutor, queued](jsi::Runtime& rt) {
    // Cleared before JS runs, so a change made during the dispatch queues again.
    queued->store(false);
    auto executor = weakExecutor.lock();
    if (!executor) return;
    // Before anyone can paint at the new size, let go of a drawable of the old one.
    gfx::releaseStaleDrawables(rt);
    // The DOM shim defines the hook; without it there is no window to resize.
    jsi::Value hook = rt.global().getProperty(rt, "__screenkitResize");
    if (!hook.isObject() || !hook.getObject(rt).isFunction(rt)) return;
    try {
      runUserAgentDispatch(*executor, rt, hook.getObject(rt).getFunction(rt).call(rt));
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, "screenkit.viewport", std::string("resize event failed: ") + e.what());
    }
  });
  return true;
}

}  // namespace screenkit
