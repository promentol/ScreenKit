// Copyright (c) ScreenKit contributors. MIT.
//
// Window size changes, delivered as the DOM's `resize` event at `window`.
//
// Every size JS reads -- `innerWidth`, `canvas.width`, `gl.drawingBufferWidth`
// -- is live, so nothing has to be pushed for the numbers to be right. What a
// page cannot do is notice: a UI that sized its stage at launch waits for
// `resize`. SDL reports a pixel-size change on the main thread; this turns it
// into one JS task that asks the DOM shim's `__screenkitResize` hook to fire
// the event if the size JS sees really changed. The burst of events a window
// drag produces collapses into at most one queued task.
//
// A host's whole integration:
//
//   ViewportEvents viewport(runtime);
//   ... for each event:  if (viewport.handleEvent(event)) continue;
#pragma once

#include <atomic>
#include <memory>

#include <SDL3/SDL_events.h>

namespace screenkit {

class FocusTarget;
class Runtime;

/// Routes SDL window pixel-size changes to a runtime's JS thread. Main thread
/// only.
class ViewportEvents {
 public:
  explicit ViewportEvents(std::shared_ptr<Runtime> runtime);

  ViewportEvents(const ViewportEvents&) = delete;
  ViewportEvents& operator=(const ViewportEvents&) = delete;

  /// Handles SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, which SDL sends for a resize
  /// and for a move to a display with a different scale. True when the event
  /// was one.
  bool handleEvent(const SDL_Event& event);

  /// `resize` follows focus, as input does: the focused browsing context is the
  /// one that hears the window changed size. Null in the target means the
  /// runtime this was built with. Main thread.

 private:
  std::shared_ptr<Runtime> runtime_;
  /// A resize task is queued and has not started yet. Shared with that task.
  std::shared_ptr<std::atomic<bool>> queued_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace screenkit
