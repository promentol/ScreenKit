// Copyright (c) ScreenKit contributors. MIT.
//
// One `<iframe>` = one Instance (Architecture.md 5).
//
// The pieces already existed separately: a `Runtime` on its own thread, two
// contexts joined by a share group, a freeze gate that is what `Paused` means,
// and a layer rect. This file is where they become one object with a lifecycle.
#include "InstanceImpl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <SDL3/SDL_thread.h>

#include <jsi/jsi.h>

#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

#include "../bundle/Package.h"
#include "../compositor/Compositor.h"
#include "../gfx/GlSurface.h"
#include "../gfx/VendoredWebGL.h"
#include "StructuredClone.h"

namespace jsi = facebook::jsi;

namespace screenkit {

// ---- the registry ------------------------------------------------------------

class InstanceRegistry : public std::enable_shared_from_this<InstanceRegistry> {
 public:
  std::shared_ptr<FocusTarget> focus = std::make_shared<FocusTarget>();

  void setEnvironment(std::string domShimPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    domShimPath_ = std::move(domShimPath);
  }

  std::string domShimPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return domShimPath_;
  }

  bool add(std::shared_ptr<Instance> instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    instances_.push_back(std::move(instance));
    return true;
  }

  void remove(const Instance* instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
      if (it->get() != instance) continue;
      instances_.erase(it);
      return;
    }
  }

  std::vector<std::shared_ptr<Instance>> live() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instances_;
  }

  std::vector<std::shared_ptr<Instance>> close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    std::vector<std::shared_ptr<Instance>> taken;
    taken.swap(instances_);
    return taken;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Instance>> instances_;
  std::string domShimPath_;
  bool closed_ = false;
};

std::shared_ptr<InstanceRegistry> makeInstanceRegistry() {
  return std::make_shared<InstanceRegistry>();
}

void setInstanceEnvironment(InstanceRegistry& registry, std::string domShimPath) {
  registry.setEnvironment(std::move(domShimPath));
}

std::vector<std::shared_ptr<Instance>> liveInstances(InstanceRegistry& registry) {
  return registry.live();
}

void terminateInstances(InstanceRegistry& registry) {
  // Taken out of the registry first, so nothing can be handed a half-terminated
  // instance while this runs, and terminated outside the lock, because each one
  // joins a thread.
  for (auto& instance : registry.close()) instance->terminate();
}

std::shared_ptr<FocusTarget> focusTarget(InstanceRegistry& registry) { return registry.focus; }

void dispatchLifecycleEvent(jsi::Runtime& rt, const char* type) {
  try {
    jsi::Value hook = rt.global().getProperty(rt, "__screenkitLifecycle");
    if (!hook.isObject() || !hook.getObject(rt).isFunction(rt)) return;
    hook.getObject(rt).getFunction(rt).call(rt, jsi::String::createFromAscii(rt, type));
  } catch (const jsi::JSError& e) {
    log(LogLevel::Error, "screenkit.instance",
        "uncaught error in a lifecycle event: " + e.getMessage() + "\n" + e.getStack());
  } catch (const jsi::JSIException& e) {
    log(LogLevel::Error, "screenkit.instance", std::string("lifecycle event failed: ") + e.what());
  } catch (const std::exception& e) {
    // Nothing may escape: both callers run `pause()` in the same task, right
    // after this, so a throw here would leave the instance running when the
    // launcher believed it frozen.
    log(LogLevel::Error, "screenkit.instance", std::string("lifecycle event failed: ") + e.what());
  }
}

void freezeRuntime(const std::shared_ptr<RuntimeControl>& control) {
  if (!control) return;
  std::shared_ptr<JsExecutor> executor = control->executor();
  if (!executor) {
    control->pause();
    return;
  }
  std::weak_ptr<RuntimeControl> weak = control;
  executor->invokeAsync([weak](jsi::Runtime& rt) {
    dispatchLifecycleEvent(rt, "pause");
    if (auto live = weak.lock()) live->pause();
  });
}

void thawRuntime(const std::shared_ptr<RuntimeControl>& control) {
  if (!control) return;
  // Only a context that was actually frozen hears `resume`. A page that focuses
  // a child and takes focus back in the same turn cancels the freeze before it
  // lands (`freezeSelf` skips itself when the request has moved on), and an
  // unpaired `resume` is a lifecycle event for something that never happened.
  if (!control->paused()) return;
  control->resume();
  std::shared_ptr<JsExecutor> executor = control->executor();
  if (executor) {
    executor->invokeAsync([](jsi::Runtime& rt) { dispatchLifecycleEvent(rt, "resume"); });
  }
}

namespace {

constexpr int kBootstrapWaitSeconds = 20;

/// The child's end of `postMessage`: it hands a copy to the binding that owns
/// the instance, which delivers it on the parent's JS thread.
class ParentChannel final : public InstanceChannel {
 public:
  ParentChannel(std::function<void(std::shared_ptr<const CloneValue>)> deliver,
                std::function<void()> focus)
      : deliver_(std::move(deliver)), focus_(std::move(focus)) {}

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    deliver_ = nullptr;
    focus_ = nullptr;
  }

  void deliver(std::shared_ptr<const CloneValue> message) override {
    std::function<void(std::shared_ptr<const CloneValue>)> deliver;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      deliver = deliver_;
    }
    // A message posted while the parent is tearing the instance down goes
    // nowhere, rather than reaching a binding that has shut down.
    if (deliver) deliver(std::move(message));
  }

  void focusParent() override {
    std::function<void()> focus;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      focus = focus_;
    }
    if (focus) focus();
  }

 private:
  std::mutex mutex_;
  std::function<void(std::shared_ptr<const CloneValue>)> deliver_;
  std::function<void()> focus_;
};

/// Run `work` on `runtime`'s JS thread and wait for it. Used by the bootstrap
/// thread, never by a JS thread: this is the one place an instance's setup is
/// allowed to block, because nothing is on screen yet.
bool onJsThreadBlocking(const std::shared_ptr<Runtime>& runtime,
                        std::function<void(jsi::Runtime&)> work) {
  struct Cell {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };
  auto cell = std::make_shared<Cell>();
  runtime->executor()->invokeAsync([cell, work](jsi::Runtime& rt) {
    work(rt);
    {
      std::lock_guard<std::mutex> lock(cell->mutex);
      cell->done = true;
    }
    cell->cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(cell->mutex);
  return cell->cv.wait_for(lock, std::chrono::seconds(kBootstrapWaitSeconds),
                           [&] { return cell->done; });
}

/// Everything here is public: the class lives in an anonymous namespace, so
/// "public" means "this file", and a friend declaration for `createInstance`
/// would only be a more brittle way of saying the same thing.
class InstanceImpl final : public Instance {
 public:
  std::uint64_t id() const override { return id_; }

  std::shared_ptr<Runtime> runtime() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtime_;
  }

  void tickFrame(double timestampMs) override {
    std::shared_ptr<Runtime> runtime = this->runtime();
    if (!runtime) return;
    // A paused runtime ignores the tick itself (EventLoop::tickFrame), so a host
    // that ticks every instance costs nothing for the frozen ones.
    runtime->tickFrame(timestampMs);
    // ...and the same check the host makes on its own app every frame
    // (host/Host.cpp, `appFailed`): an entry that rejects settles long after
    // `evaluateBundle` returned, so without this a child that failed keeps a
    // layer and the element never hears about it.
    if (!loadReported_.load() || reportedFailure_.load()) return;
    const std::optional<std::string> failure = runtime->failure();
    if (!failure) return;
    if (reportedFailure_.exchange(true)) return;
    if (events_.failed) events_.failed(*failure);
  }

  void setPaused(bool paused) override {
    std::shared_ptr<Runtime> runtime = this->runtime();
    if (!runtime) return;
    if (paused) {
      // The lifecycle event and the freeze are one task on the instance's own
      // thread: dispatching `pause` first and freezing from inside the same
      // callback is the only order in which the page can hear about a pause at
      // all -- a task queued after the gate closed would wait behind it and
      // arrive on resume instead, which is precisely the stale-callback burst
      // `Paused` exists to prevent.
      std::weak_ptr<Runtime> weak = runtime;
      runtime->executor()->invokeAsync([weak](jsi::Runtime& rt) {
        dispatchLifecycleEvent(rt, "pause");
        if (auto live = weak.lock()) live->pause();
      });
    } else {
      // The other way round: thaw, then let the page hear about it.
      runtime->resume();
      runtime->executor()->invokeAsync([](jsi::Runtime& rt) { dispatchLifecycleEvent(rt, "resume"); });
    }
    wantPaused_.store(paused);
  }

  bool paused() const override {
    std::shared_ptr<Runtime> runtime = this->runtime();
    // `wantPaused_` rather than the runtime's own flag: the freeze lands one
    // task later (see setPaused), and a caller asking "is this one frozen?"
    // means the decision, not the moment it took effect.
    return runtime ? wantPaused_.load() : true;
  }

  void terminate() override {
    if (terminating_.exchange(true)) return;
    std::shared_ptr<Runtime> runtime;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      runtime = std::move(runtime_);
    }
    if (channel_) channel_->close();
    // The bootstrap may still be blocked inside evaluateBundle; shutting the
    // runtime down cancels the queued work, which settles it.
    if (runtime) runtime->shutdown();
    joinBootstrap();
    if (auto registry = registry_.lock()) registry->remove(this);
    // The layer's texture went with the runtime's GL teardown, on its own
    // thread. Nothing here may touch GL: this runs on the parent's.
    layer_.reset();
  }

  void post(std::shared_ptr<const CloneValue> message) {
    std::shared_ptr<Runtime> runtime = this->runtime();
    if (!runtime) return;
    runtime->executor()->invokeAsync([message](jsi::Runtime& rt) {
      deliverMessage(rt, *message);
    });
  }

  static void deliverMessage(jsi::Runtime& rt, const CloneValue& message) {
    try {
      jsi::Value api = rt.global().getProperty(rt, "__screenkit");
      if (!api.isObject()) return;
      jsi::Value instances = api.getObject(rt).getProperty(rt, "instances");
      if (!instances.isObject()) return;
      jsi::Value hook = instances.getObject(rt).getProperty(rt, "onparentmessage");
      if (!hook.isObject() || !hook.getObject(rt).isFunction(rt)) return;
      hook.getObject(rt).getFunction(rt).call(rt, cloneIn(rt, message));
    } catch (const jsi::JSError& e) {
      log(LogLevel::Error, "screenkit.instance",
          "uncaught error in a message event: " + e.getMessage() + "\n" + e.getStack());
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, "screenkit.instance", std::string("message event failed: ") + e.what());
    }
  }

  void joinBootstrap() {
    SDL_Thread* thread = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      thread = bootstrap_;
      bootstrap_ = nullptr;
    }
    if (thread != nullptr) SDL_WaitThread(thread, nullptr);
  }

  static int SDLCALL bootstrapEntry(void* self) {
    // The shared_ptr the starter handed over; released when this returns.
    std::shared_ptr<InstanceImpl>* held = static_cast<std::shared_ptr<InstanceImpl>*>(self);
    std::shared_ptr<InstanceImpl> instance = *held;
    delete held;
    instance->bootstrap();
    return 0;
  }

  /// Everything the host does for the app it runs, for one instance, off both
  /// JS threads: gate the package, adopt the GL context, evaluate the prelude,
  /// confine the asset root, run the entry.
  void bootstrap() {
    std::shared_ptr<Runtime> runtime = this->runtime();
    if (!runtime) return;
    std::string error;
    const bool ok = bootstrapSteps(runtime, error);
    InstanceEvents events;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events = events_;
    }
    if (events.loaded) events.loaded(ok, std::move(error));
    // Only now may a failure be reported as `error`: a package whose entry fails
    // at once has already set `failure()` by the time the first frame ticks, and
    // reporting it before this would put `error` in front of the `load` it
    // follows.
    loadReported_.store(true);
  }

  bool bootstrapSteps(const std::shared_ptr<Runtime>& runtime, std::string& error) {
    // 1. The package gate, before anything from the package is evaluated. The
    //    same code, and the same messages, the host gates its own app with.
    bundle::LaunchTarget target;
    if (!bundle::resolveLaunch(runtime, options_.src, options_.name, target, error)) return false;
    if (!target.package) {
      error = "\"" + options_.src +
              "\" is not a package: an <iframe src> names a .skpkg directory inside the parent's "
              "package";
      return false;
    }

    // 2. The GL context, adopted on the instance's own JS thread -- where every
    //    GL call it will ever make happens -- and `gl` installed on it.
    std::shared_ptr<gfx::GlSurface> surface;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      surface = std::move(surface_);
    }
    if (!surface) {
      error = "this instance has no drawable";
      return false;
    }
    // Shared, not captured by reference: `onJsThreadBlocking` gives up after a
    // timeout while the task may still be queued behind a freeze gate, and a
    // reference to this frame's local would be a write into a dead stack when it
    // finally runs.
    auto glError = std::make_shared<std::string>();
    auto layer = layer_;
    if (!onJsThreadBlocking(runtime, [glError, surface, layer](jsi::Runtime& js) {
          if (!surface->adopt(*glError)) return;
          surface->setLayerSource(layer);
          gfx::installVendoredWebGL(js, surface);
        })) {
      error = "the instance's JS thread did not answer the graphics bootstrap";
      return false;
    }
    if (!glError->empty()) {
      error = *glError;
      return false;
    }
    // The frame boundary, exactly as `startGraphics` hangs one off the host's:
    // the trailing edge of this instance's own frame, on its own thread. For a
    // layer surface `swap()` is not a present -- it finishes into the texture
    // and signals -- so this is where a frame becomes something the host may
    // composite.
    if (!runtime->setFrameFinishedCallback([surface](jsi::Runtime& js) {
          gfx::presentFrame(js, *surface);
        })) {
      error = "the instance shut down before its frame boundary could be installed";
      return false;
    }
    // Let go of this thread's reference: from here the GL registry and the frame
    // hook own it, and the instance's own teardown destroys both on the thread
    // the context is current on.
    surface.reset();

    // 3. The prelude, then the asset root, then the bundle -- the host's order,
    //    and load-bearing in both directions.
    const std::string shim = domShimPath_;
    if (shim.empty()) {
      error = "no DOM shim prelude for instances: the host did not call setInstanceEnvironment";
      return false;
    }
    const EvalResult prelude = runtime->evaluateBundle(shim);
    if (!prelude.ok) {
      error = "the DOM shim prelude failed to evaluate in the instance: " + prelude.error;
      return false;
    }
    std::string escaped;
    for (char c : target.assetRoot) {
      if (c == '\\' || c == '\'') escaped += '\\';
      escaped += c;
    }
    const EvalResult root =
        runtime->evaluateSource("__screenkit.setAssetRoot('" + escaped + "')", "asset-root.js");
    if (!root.ok) {
      error = "could not set the instance's asset root to " + target.assetRoot + ": " + root.error;
      return false;
    }
    const EvalResult ran = runtime->evaluateBundle(target.bundle);
    if (!ran.ok) {
      error = ran.error;
      return false;
    }
    runtime->evaluateSource(
        "typeof __screenkitDocumentLoaded === 'function' && __screenkitDocumentLoaded();",
        "screenkit://document-loaded");
    return true;
  }

  mutable std::mutex mutex_;
  std::uint64_t id_ = 0;
  std::shared_ptr<Runtime> runtime_;
  std::shared_ptr<gfx::GlSurface> surface_;
  std::shared_ptr<gfx::LayerSource> layer_;
  std::shared_ptr<ParentChannel> channel_;
  std::weak_ptr<InstanceRegistry> registry_;
  InstanceOptions options_;
  InstanceEvents events_;
  std::string domShimPath_;
  SDL_Thread* bootstrap_ = nullptr;
  std::atomic<bool> wantPaused_{false};
  /// The child's failure is reported to the element once, not every frame, and
  /// never before the load it follows.
  std::atomic<bool> reportedFailure_{false};
  std::atomic<bool> loadReported_{false};
  std::atomic<bool> terminating_{false};
};

std::atomic<std::uint64_t> gNextInstanceId{1};

}  // namespace

std::shared_ptr<Instance> createInstance(InstanceRegistry& registry, const gfx::GlSurface& parent,
                                         InstanceOptions options, InstanceEvents events,
                                         std::shared_ptr<gfx::LayerSource> layer,
                                         std::string& error) {
  auto self = std::make_shared<InstanceImpl>();
  self->id_ = gNextInstanceId.fetch_add(1);
  self->options_ = std::move(options);
  self->events_ = std::move(events);
  self->layer_ = std::move(layer);
  self->domShimPath_ = registry.domShimPath();

  // The layer surface, in the host's share group, made here because SDL shares
  // only with the context current on the calling thread -- which is this one.
  std::unique_ptr<gfx::GlSurface> surface = gfx::GlSurface::createShared(
      parent, self->options_.width, self->options_.height, error);
  if (!surface) return nullptr;
  self->surface_ = std::move(surface);

  self->channel_ = std::make_shared<ParentChannel>(self->events_.message, self->events_.focusParent);

  RuntimeConfig config;
  config.name = self->options_.name;
  if (self->options_.maxHeapBytes > 0) config.maxHeapBytes = self->options_.maxHeapBytes;
  config.testTlsAnchors = self->options_.testTlsAnchors;
  config.sandbox = self->options_.sandbox;
  config.parent = self->channel_;
  // One level of nesting: an instance makes no instances of its own, and its
  // binding refuses an `<iframe>` rather than leaving one silently inert.
  config.instances = nullptr;

  self->runtime_ = Runtime::create(std::move(config));
  if (!self->runtime_) {
    error = "the instance's runtime would not start";
    return nullptr;
  }
  self->registry_ = registry.weak_from_this();
  if (!registry.add(self)) {
    error = "the host is shutting down; no new instances";
    self->terminate();
    return nullptr;
  }

  // The bootstrap runs on a thread of its own so neither JS thread blocks on the
  // other: the package gate and `evaluateBundle` both block their caller, and
  // the caller here is the launcher's own JS thread.
  auto* held = new std::shared_ptr<InstanceImpl>(self);
  const std::string threadName = self->options_.name + ".boot";
  SDL_Thread* thread = SDL_CreateThread(&InstanceImpl::bootstrapEntry, threadName.c_str(), held);
  if (thread == nullptr) {
    delete held;
    error = "the instance's bootstrap thread would not start";
    self->terminate();
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->bootstrap_ = thread;
  }
  return self;
}

void postToInstance(Instance& instance, std::shared_ptr<const CloneValue> message) {
  static_cast<InstanceImpl&>(instance).post(std::move(message));
}

}  // namespace screenkit
