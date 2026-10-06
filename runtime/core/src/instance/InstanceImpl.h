// Copyright (c) ScreenKit contributors. MIT.
//
// Making one `<iframe>` instance: the half the binding calls, as opposed to the
// half a host calls (screenkit/Instance.h).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <jsi/jsi.h>

#include <screenkit/Instance.h>

namespace screenkit {

class JsExecutor;
struct CloneValue;

namespace gfx {
class GlSurface;
class LayerSource;
}  // namespace gfx

struct InstanceOptions {
  /// The log tag and the JS thread's name: "screenkit.iframe-3".
  std::string name;
  /// The `.skpkg` directory, already confined to the parent's package by the
  /// caller -- `src` names a local package and nothing else (M12 owns fetching).
  std::string src;
  SandboxPolicy sandbox;
  /// The drawable the instance draws at: the element's CSS rect in drawable
  /// pixels, as `planeFor` computes it.
  int width = 1;
  int height = 1;
  std::size_t maxHeapBytes = 0;  // 0: RuntimeConfig's default
  /// Inherited from the parent, so a test-suite instance trusts the same CA.
  /// Not reachable from JS on either side.
  std::vector<std::vector<std::uint8_t>> testTlsAnchors;
};

/// What an instance tells the binding that owns it. Both are called off the
/// binding's thread -- from the bootstrap thread, or from the instance's JS
/// thread -- so each one hops to the binding's own before touching JSI.
struct InstanceEvents {
  /// The package loaded and its entry ran, or it did not and this says why.
  /// Exactly once per instance.
  std::function<void(bool ok, std::string error)> loaded;
  /// The instance's app failed *after* its entry ran -- a rejected dynamic
  /// import, an unhandled rejection reported through `__screenkit.reportFailure`.
  /// `loaded` has already said the load succeeded by then, so this is the only
  /// way the element hears about it. At most once, and the instance keeps
  /// running: what failed is the child's page, not the launcher.
  std::function<void(std::string)> failed;
  /// `parent.postMessage(v)` in the instance.
  std::function<void(std::shared_ptr<const CloneValue>)> message;
  /// `window.parent.focus()` in the instance: the launcher wants the remote
  /// back. Called from the instance's JS thread, and the parent it is asking to
  /// thaw is frozen -- which is why this is a direct call rather than a task.
  std::function<void()> focusParent;
};

/// Freezing and thawing the runtime a binding runs on.
///
/// `Runtime` does not exist yet when bindings are installed -- it is the wrapper
/// around the host that installs them -- so `focus()` reaches its own runtime
/// through this instead of through a `Runtime` it cannot have.
class RuntimeControl {
 public:
  virtual ~RuntimeControl() = default;
  /// Safe from any thread, as `Runtime::pause` is.
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual bool paused() const = 0;
  virtual std::shared_ptr<JsExecutor> executor() const = 0;
};

/// `window`'s `pause` / `resume`, through the shim's `__screenkitLifecycle` hook
/// -- the `__screenkitKey` family. A runtime without the shim has no window to
/// tell, and says nothing. JS thread.
void dispatchLifecycleEvent(facebook::jsi::Runtime& rt, const char* type);

/// Freeze `control`'s runtime and tell its page, in the one order that works:
/// the event is dispatched and the gate closed inside one task on that
/// runtime's own thread. A task queued after the gate closed would wait behind
/// it and arrive on resume instead -- exactly the stale-callback burst `Paused`
/// exists to prevent. Any thread.
void freezeRuntime(const std::shared_ptr<RuntimeControl>& control);

/// Thaw it, then tell its page. The other order, for the same reason.
void thawRuntime(const std::shared_ptr<RuntimeControl>& control);

/// Make one instance: a `GlSurface` in `parent`'s share group, a `Runtime` on a
/// thread of its own, and a bootstrap that gates the package, evaluates the
/// prelude, confines the asset root and runs the entry.
///
/// **Call on the thread that owns `parent`'s context** -- the host runtime's JS
/// thread, where the binding lives. That is where a shared context has to be
/// made (GlSurface::createShared).
///
/// Null with `error` set when the layer surface or the runtime could not be
/// made. A package that fails its gate is *not* a null return: the instance
/// exists, `loaded` says what went wrong, and the caller tears it down -- which
/// is what makes `error` fire on the element with no instance left behind.
std::shared_ptr<Instance> createInstance(InstanceRegistry& registry, const gfx::GlSurface& parent,
                                         InstanceOptions options, InstanceEvents events,
                                         std::shared_ptr<gfx::LayerSource> layer, std::string& error);

/// Post a message into an instance: it arrives on its JS thread as a `message`
/// event at its `window`, behind its freeze gate like any other task. Any
/// thread; never blocks.
void postToInstance(Instance& instance, std::shared_ptr<const CloneValue> message);

}  // namespace screenkit
