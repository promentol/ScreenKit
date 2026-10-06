// Copyright (c) ScreenKit contributors. MIT.
//
// `<iframe>` instances: the embedder's half (Architecture.md 5).
//
// **One iframe = one Instance** = one `Runtime` on its own thread, with its own
// GL context in the host's share group, drawing into an FBO-backed texture the
// host composites at the element's CSS rect. Everything an instance is made of
// already existed separately -- a runtime, a shared context, the freeze gate,
// a layer rect -- and this header is where a host reaches the set of them.
//
// **This is not an isolation boundary.** Architecture.md 5.2 stands: JS is
// isolated, memory is not, a native crash or an OOM in any instance takes every
// instance with it including the launcher, and `sandbox` gates capabilities
// only. Nothing here may be presented to a third-party developer as containment.
//
// A host's whole integration:
//
//   RuntimeConfig config;
//   config.instances = makeInstanceRegistry();
//   ... setInstanceEnvironment(*config.instances, domShimPath);
//   ... for each frame:   for (auto& i : liveInstances(*config.instances)) i->tickFrame(ms);
//   ... before returning: terminateInstances(*config.instances);
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace screenkit {

class Runtime;

/// `<iframe sandbox>`: what one instance is allowed to reach.
///
/// An element with no `sandbox` attribute is not sandboxed and reaches
/// everything, as on the web. An element *with* the attribute starts from
/// nothing and is given back only what its tokens name, and the gate is applied
/// where the binding is installed -- as `testTlsAnchors` is -- so nothing
/// reachable from the child's JS can widen it.
///
/// Which tokens mean something here is recorded rather than inherited:
/// `allow-network` gates `__screenkit.net` (added to the web's set, because
/// networking is the capability an embedded app most obviously needs withheld)
/// and `allow-media` gates `__screenkit.media`. `allow-background-audio` and
/// `allow-background-timers` are accepted and reported but gate nothing yet:
/// the freeze gate is all-or-nothing, so a `Paused` instance keeps neither.
/// `allow-storage` gates nothing today -- this runtime has no storage API at all
/// -- so it is accepted and reserved, and says so once. An unknown token is
/// ignored with a one-time warning, as a browser ignores one.
struct SandboxPolicy {
  /// False for a top-level runtime and for an `<iframe>` with no `sandbox`
  /// attribute: every capability, and none of the fields below is consulted.
  bool sandboxed = false;
  bool allowNetwork = false;
  bool allowMedia = false;
  bool allowStorage = false;
  bool allowBackgroundAudio = false;
  bool allowBackgroundTimers = false;

  bool network() const { return !sandboxed || allowNetwork; }
  bool media() const { return !sandboxed || allowMedia; }
  bool backgroundAudio() const { return !sandboxed || allowBackgroundAudio; }
  bool backgroundTimers() const { return !sandboxed || allowBackgroundTimers; }
};

/// A `postMessage` payload: the structured-clone subset, already copied out of
/// the sending runtime. Opaque here -- `instance/StructuredClone.h` defines it --
/// because the two runtimes share no JSI value and this is what crosses instead.
struct CloneValue;

/// The child's end of the channel to whoever embeds it.
///
/// `postMessage` is task-to-task and asynchronous: the sender encodes on its own
/// JS thread, this hands the copy over, and the receiver decodes on its own. No
/// `invokeSync`, no shared JSI value, and no JSON bridge between the two.
class InstanceChannel {
 public:
  virtual ~InstanceChannel() = default;
  /// Deliver a message to the other side. Any thread; never blocks.
  virtual void deliver(std::shared_ptr<const CloneValue> message) = 0;
  /// `window.parent.focus()`: hand the remote back to whoever embeds this. Any
  /// thread -- and deliberately not a task, because the parent it is asking to
  /// thaw is frozen, so a task would wait behind the very gate it means to open.
  virtual void focusParent() = 0;
};

/// One `<iframe>`, from the host's point of view.
class Instance {
 public:
  virtual ~Instance() = default;

  /// This instance's own id, unique in the process. Not the id the binding that
  /// made it uses in its events and `setPlane` calls -- that one is per binding,
  /// and the two part company on the first reload or refused load.
  virtual std::uint64_t id() const = 0;

  /// Its runtime, or null once it has been terminated.
  virtual std::shared_ptr<Runtime> runtime() const = 0;

  /// One frame, exactly as the host ticks its own runtime. Ignored while paused,
  /// which is what makes a `Paused` instance fire zero rAF callbacks without the
  /// host having to know which one is focused.
  virtual void tickFrame(double timestampMs) = 0;

  virtual void setPaused(bool paused) = 0;
  virtual bool paused() const = 0;

  /// Thread joined, heap and GL resources freed, layer gone. Idempotent, and
  /// never called from the instance's own JS thread.
  virtual void terminate() = 0;
};

/// The live instances of one runtime, reachable from any thread -- which is what
/// the host loop needs: it ticks them from the main thread while the binding
/// that owns them lives on the JS thread. Made with the host, before its JS
/// thread exists.
class InstanceRegistry;

std::shared_ptr<InstanceRegistry> makeInstanceRegistry();

/// What an instance needs from the host to boot: the DOM shim prelude, and the
/// drawable size a child with no CSS size of its own falls back to. Set once,
/// before any instance is made.
void setInstanceEnvironment(InstanceRegistry& registry, std::string domShimPath);

/// Every live instance, in creation order. Safe from any thread.
std::vector<std::shared_ptr<Instance>> liveInstances(InstanceRegistry& registry);

/// Terminate every instance and refuse to make more. The host calls this before
/// it returns, so nothing is delivered to a runtime that is going away. Safe
/// from any thread but an instance's own; idempotent.
void terminateInstances(InstanceRegistry& registry);

/// Which runtime input and `resize` go to: the focused browsing context.
///
/// `focus()` is atomic -- the outgoing context is paused before the incoming one
/// resumes -- and it happens on a JS thread, while `InputRouter` and
/// `ViewportEvents` read this from the main thread. The generation counter is
/// how the router notices a switch and releases the keys it was holding, the way
/// it does for a gamepad that disconnects mid-press.
class FocusTarget {
 public:
  /// The focused runtime, or null when it is the host's own -- which is what an
  /// unfocused tree, and a child that has just been terminated, both mean.
  std::shared_ptr<Runtime> current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_.lock();
  }

  void set(std::shared_ptr<Runtime> runtime) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_ = runtime;
    ++generation_;
  }

  std::uint64_t generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

 private:
  mutable std::mutex mutex_;
  /// Weak, so a terminated instance cannot be kept alive by having been focused.
  std::weak_ptr<Runtime> runtime_;
  std::uint64_t generation_ = 0;
};

/// The focus target of this registry's tree. The host hands it to `InputRouter`
/// and `ViewportEvents`; the instance binding moves it on `focus()`.
std::shared_ptr<FocusTarget> focusTarget(InstanceRegistry& registry);

}  // namespace screenkit
