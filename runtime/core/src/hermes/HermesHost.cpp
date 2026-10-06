// Copyright (c) ScreenKit contributors. MIT.
#include "HermesHost.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include "../bindings/Console.h"
#include "../bindings/Gamepads.h"
#include "../bindings/HostIO.h"
#include "../bindings/Canvas.h"
#include "../bindings/Instance.h"
#include "../bindings/Media.h"
#include "../bindings/Net.h"
#include "../bindings/Text.h"
#include "../bindings/Timers.h"
#include "../engine/Engine.h"
#include "../gfx/VendoredWebGL.h"
#include "../instance/InstanceImpl.h"
#include "../jsi/EventEmitter.h"
#include "../jsi/NativeModule.h"
#include "../jsi/SharedObject.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// `focus()` freezing and thawing the runtime it runs on. The `Runtime` wrapper
/// does not exist yet when bindings are installed -- it is what wraps this host
/// -- so the instance binding reaches its own runtime through the host instead.
class HermesRuntimeControl final : public RuntimeControl {
 public:
  explicit HermesRuntimeControl(std::weak_ptr<HermesHost> host) : host_(std::move(host)) {}

  void pause() override {
    if (auto host = host_.lock()) host->pause();
  }
  void resume() override {
    if (auto host = host_.lock()) host->resume();
  }
  bool paused() const override {
    auto host = host_.lock();
    return host && host->paused();
  }
  std::shared_ptr<JsExecutor> executor() const override {
    if (host_.expired()) return nullptr;
    return std::make_shared<HermesExecutor>(host_);
  }

 private:
  std::weak_ptr<HermesHost> host_;
};

}  // namespace

HermesHost::HermesHost(RuntimeConfig config) : config_(std::move(config)) {}

std::shared_ptr<HermesHost> HermesHost::start(RuntimeConfig config) {
  // The event subsystem backs the work queue. SDL refcounts this, so several
  // runtimes -- or a runtime inside a host that has already called SDL_Init --
  // nest without anyone tearing the queue out from under anyone else. Timers
  // need no such call: SDL_AddTimerNS self-initialises through SDL_ShouldInit.
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
    log(LogLevel::Error, config.name,
        std::string("SDL_InitSubSystem(SDL_INIT_EVENTS) failed: ") + SDL_GetError());
    return nullptr;
  }

  // Not make_shared: the constructor is private and there is nothing to gain.
  std::shared_ptr<HermesHost> host(new HermesHost(std::move(config)));
  std::weak_ptr<HermesHost> weak = host;

  host->queue_ = WorkQueue::create();
  if (!host->queue_) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    return nullptr;
  }
  host->queue_->setWakeCallback([weak] {
    if (auto self = weak.lock()) self->wake();
  });

  // The loop exists before the thread does, so pause(), resume() and tickFrame()
  // are usable the moment start() returns rather than only once JS has run.
  EventLoop::Delegate delegate;
  delegate.post = [weak](std::function<void(jsi::Runtime&)> work) -> bool {
    auto self = weak.lock();
    if (!self) return false;  // host already gone: dropped
    Task task;
    task.run = std::move(work);
    return self->post(std::move(task));
  };
  delegate.wake = [weak] {
    if (auto self = weak.lock()) self->wake();
  };
  delegate.setPaused = [weak](bool paused) {
    if (auto self = weak.lock()) self->setPaused(paused);
  };
  host->loop_ = EventLoop::create(std::move(delegate), host->config_.name);
  host->objects_ = std::make_shared<SharedObjectRegistry>();
  host->mediaRegistry_ = makeMediaRegistry();

  // SDL names the thread for us -- no pthread_setname_np, and it works the same
  // way on every platform SDL supports.
  const std::string threadName = host->config_.name + ".js";
  // A raw pointer, deliberately: capturing the shared_ptr would make the thread
  // keep the host alive forever, so the destructor -- the thing that joins the
  // thread -- could never run.
  host->thread_ = SDL_CreateThread(&HermesHost::threadEntry, threadName.c_str(), host.get());
  if (host->thread_ == nullptr) {
    log(LogLevel::Error, host->config_.name,
        std::string("SDL_CreateThread failed: ") + SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    return nullptr;
  }

  bool ok = false;
  std::string error;
  {
    SdlLock lock(host->startMutex_);
    host->startCv_.wait(lock, [&] { return host->started_; });
    ok = host->startOk_;
    error = host->startError_;
  }

  if (!ok) {
    log(LogLevel::Error, host->config_.name, "Hermes runtime creation failed: " + error);
    host->stop();
    return nullptr;
  }
  return host;
}

HermesHost::~HermesHost() {
  stop();
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

int SDLCALL HermesHost::threadEntry(void* self) {
  static_cast<HermesHost*>(self)->threadMain();
  return 0;
}

void HermesHost::threadMain() {
  std::string error;
  try {
    // The engine is this build's (engine/Engine.h): Hermes, or SpiderMonkey on
    // Linux. What it is configured with lives beside it.
    runtime_ = engine::createRuntime(config_, error);
    if (runtime_) {
      installConsole(*runtime_, config_.name);
      installRejectionTracker(*runtime_, config_.name);
      installTimerBindings(*runtime_, loop_);
      installHostIO(*runtime_, failures_);
      // Every `<canvas>` after the page's own frame: its own GL context and its
      // own compositor layer (Architecture.md 3.1). Installed unconditionally --
      // `create` answers null where there is nothing to composite over, which is
      // what a headless runtime and an instance both are.
      canvas_ = installCanvas(*runtime_, std::make_shared<HermesExecutor>(weak_from_this()));
      // `<iframe sandbox>` is enforced *here*, where the binding is installed,
      // exactly as `testTlsAnchors` is: a gated capability is not installed at
      // all rather than installed half way, so nothing reachable from this
      // runtime's JS can widen it. The shim already has a documented shape for
      // each absence -- `noNetwork()`, and `mediaCaps().available === false` --
      // so a sandboxed page gets the same answer a platform without the thing
      // gives, rather than a crash.
      if (config_.sandbox.network()) {
        // Network completions come back as ordinary tasks, through the same
        // executor the embedder gets -- so they queue behind the freeze gate.
        net_ = installNet(*runtime_, std::make_shared<HermesExecutor>(weak_from_this()), loop_, config_);
      } else {
        log(LogLevel::Log, config_.name,
            "sandboxed without allow-network: __screenkit.net is not installed, and fetch, "
            "XMLHttpRequest, WebSocket and EventSource fail as they do on a platform with no client");
      }
      if (config_.sandbox.media()) {
        // Player events too, and after installNet: a licence request the Shaka
        // layer answers goes out through __screenkit.net.
        media_ = installMedia(*runtime_, std::make_shared<HermesExecutor>(weak_from_this()), loop_,
                              mediaRegistry_, config_);
      } else {
        log(LogLevel::Log, config_.name,
            "sandboxed without allow-media: __screenkit.media is not installed, and <video> reports "
            "no player as it does on a platform without one");
      }
      text_ = installText(*runtime_);
      // Reads the process's gamepad snapshot; holds nothing to shut down.
      installGamepads(*runtime_);
      // `<iframe>`: always installed, because even an instance that may not make
      // instances of its own still needs `parentPost` and `capabilities()`.
      instances_ = installInstances(*runtime_, std::make_shared<HermesExecutor>(weak_from_this()),
                                    loop_, std::make_shared<HermesRuntimeControl>(weak_from_this()),
                                    config_);
      installObjectModel(*runtime_, objects_, config_.name);
    } else if (error.empty()) {
      error = "the " + std::string(engine::name()) + " runtime could not be created";
    }
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "unknown exception";
  }

  {
    // Publishing threadId_ under the same lock start() waits on is what makes
    // onJsThread() safe to call from anywhere afterwards.
    SdlLock lock(startMutex_);
    threadId_ = SDL_GetCurrentThreadID();
    started_ = true;
    startOk_ = runtime_ != nullptr;
    startError_ = error;
  }
  startCv_.broadcast();

  if (!runtime_) {
    {
      SdlLock lock(mutex_);
      stopping_ = true;
    }
    queue_->close();
    return;
  }

  for (;;) {
    WorkQueue::Task task;
    bool haveTask = false;
    {
      SdlLock lock(mutex_);
      // The freeze gate lives here, in *selection*. Gating at execution time
      // would still burn the wakeup, which is exactly what `Paused` promises
      // not to do.
      cv_.wait(lock, [this] { return stopping_ || (!paused_ && workReady()) || presentReady(); });
      // stopping_ wins over pending work: once stop() has been seen under the
      // lock no further task is dispatched, which is what makes the drop
      // deterministic rather than a race the tests could only observe by luck.
      if (stopping_) break;
      // Nothing of the page runs while the gate is closed: a frozen runtime
      // that woke did so for the present alone (EventLoop::presentReady).
      haveTask = !paused_ && queue_->pop(task);
      busy_ = true;
    }

    if (haveTask && task.run) {
      // An invokeAsync callback that throws is a bug in that callback, not a
      // reason to abort the process: threadMain is a noexcept-in-practice
      // boundary, so an escaping JSError would call std::terminate.
      try {
        task.run();
      } catch (const jsi::JSError& e) {
        std::string message = e.getMessage();
        const std::string& stack = e.getStack();
        if (!stack.empty()) message += "\n" + stack;
        log(LogLevel::Error, config_.name, "uncaught JS error in queued work: " + message);
      } catch (const std::exception& e) {
        log(LogLevel::Error, config_.name,
            std::string("uncaught exception in queued work: ") + e.what());
      } catch (...) {
        log(LogLevel::Error, config_.name, "uncaught unknown exception in queued work");
      }
    }

    // React Native's tick, and the reason the checkpoint is here rather than
    // inside any one callback: select task -> execute -> microtask checkpoint
    // -> frame work.
    loop_->afterTask(*runtime_);

    {
      SdlLock lock(mutex_);
      busy_ = false;
    }
  }

  // Teardown, in the one order that cannot leak or fault:
  // 1. stop SDL delivering timer dispatches, so nothing can be enqueued behind
  //    the drain (clear() waits out a callback that is already in flight), and
  //    close every request and socket -- synchronously on the network queue, so
  //    no completion is still being produced -- dropping their JS callbacks
  //    while the runtime is alive. Players first: each shuts down
  //    synchronously, so none is still producing events (or waiting on a
  //    licence request) when the network goes;
  loop_->stopTimers();
  // Instances first, and synchronously: each one's thread is joined and its GL
  // freed while this runtime -- whose share group its context belongs to, and
  // whose compositor was sampling its texture -- is still alive. Nothing of an
  // instance is delivered after this returns.
  if (instances_) shutdownInstances(*instances_);
  // Every canvas layer goes before the contexts behind them do, so nothing is
  // still in the compositor's list when its texture is deleted below.
  if (canvas_) shutdownCanvas(*canvas_);
  if (media_) shutdownMedia(*media_);
  if (net_) shutdownNet(*net_);
  // 2. refuse new work and hand every queued task to its cancel;
  queue_->close();
  queue_->drain();
  // 3. drop JS references while the runtime is still alive, and release the GL
  //    context here on the thread it is current on -- the frame hook's surface
  //    reference went with the loop -- and close the fonts on it too;
  loop_->shutdown();
  gfx::releaseVendoredWebGL(*runtime_);
  if (text_) shutdownText(*text_);
  objects_->clear();
  // 3b. drop the per-runtime registries keyed by this runtime's address, so a
  //     later instance that is handed the same address starts clean rather than
  //     inheriting a dead one's asset root or log tag.
  shutdownEventEmitters(*runtime_);
  shutdownHostIO(*runtime_);
  // 4. destroy the runtime on its owning thread, as the invariant requires.
  runtime_.reset();
}

bool HermesHost::workReady() const {
  if (!queue_->empty()) return true;
  return loop_ && loop_->frameReady();
}

bool HermesHost::presentReady() const { return loop_ && loop_->presentReady(); }

bool HermesHost::post(Task task) {
  {
    SdlLock lock(mutex_);
    if (stopping_) return false;
    WorkQueue::Task queued;
    // The runtime reference is supplied here rather than travelling with the
    // task: it only exists on the JS thread, which is the only place this runs.
    queued.run = [this, run = std::move(task.run)] {
      if (run && runtime_) run(*runtime_);
    };
    queued.cancel = std::move(task.cancel);
    if (!queue_->push(std::move(queued))) return false;
  }
  cv_.broadcast();
  return true;
}

void HermesHost::wake() {
  SdlLock lock(mutex_);
  cv_.broadcast();
}

void HermesHost::setPaused(bool paused) {
  {
    SdlLock lock(mutex_);
    if (paused_ == paused) return;
    paused_ = paused;
  }
  cv_.broadcast();
  // Architecture.md 5.1: a Paused context's video is stopped too. The players
  // are told directly, from this thread -- a task would wait behind the very
  // gate that was just closed.
  if (mediaRegistry_) setMediaRuntimePaused(*mediaRegistry_, paused);
}

void HermesHost::pause() {
  if (loop_) loop_->pause();
}

void HermesHost::resume() {
  if (loop_) loop_->resume();
}

bool HermesHost::paused() const { return loop_ && loop_->paused(); }

std::uint64_t HermesHost::pauseEpoch() const { return loop_ ? loop_->pauseEpoch() : 0; }

void HermesHost::tickFrame(double timestampMs) {
  if (loop_) loop_->tickFrame(timestampMs);
}

bool HermesHost::setFrameFinishedCallback(std::function<void(jsi::Runtime&)> callback) {
  if (!loop_) return false;
  std::weak_ptr<EventLoop> weak = loop_;
  Task task;
  task.run = [weak, callback = std::move(callback)](jsi::Runtime&) mutable {
    if (auto loop = weak.lock()) loop->setFrameFinished(std::move(callback));
  };
  return post(std::move(task));
}

bool HermesHost::idle() const {
  // The loop first. Work leaves the loop's books -- a timer claimed, a frame
  // served, held network work released -- only while the JS thread is busy
  // running it, so a loop that reads idle and then a thread that is not busy is
  // a runtime with nothing left. The other order read "not busy" and then, once
  // a timer's task had started and claimed it, "no timers": idle, mid-callback.
  if (loop_ && !loop_->idle()) return false;
  SdlLock lock(mutex_);
  return !busy_ && queue_->empty();
}

bool HermesHost::registerModule(std::string name, ModuleFactory factory) {
  Task task;
  task.run = [name = std::move(name),
              factory = std::move(factory)](jsi::Runtime& runtime) mutable {
    installModule(runtime, std::move(name), std::move(factory));
  };
  return post(std::move(task));
}

void HermesHost::stop() {
  SdlLock lifecycle(lifecycleMutex_);
  {
    SdlLock lock(mutex_);
    stopping_ = true;
  }
  queue_->close();
  cv_.broadcast();

  SDL_Thread* thread = thread_;
  if (thread == nullptr) {
    // The thread never started, so nobody will drain what was queued before it
    // was asked to stop.
    queue_->drain();
    return;
  }
  thread_ = nullptr;

  if (SDL_GetCurrentThreadID() == threadId_) {
    // Self-join is undefined, but leaving the thread attached is worse: SDL
    // would leak its handle. Detach and let it unwind. Runtime::shutdown()
    // rejects this call before it gets here, so reaching it means the host was
    // destroyed from its own thread.
    SDL_DetachThread(thread);
    return;
  }
  SDL_WaitThread(thread, nullptr);
}

bool HermesHost::stopped() const {
  SdlLock lock(mutex_);
  return stopping_;
}

bool HermesHost::onJsThread() const { return SDL_GetCurrentThreadID() == threadId_; }

void HermesExecutor::invokeAsync(std::function<void(jsi::Runtime&)>&& work) {
  auto host = host_.lock();
  if (!host) return;  // runtime already gone: drop

  std::weak_ptr<HermesHost> weak = host_;
  HermesHost::Task task;
  task.run = [weak, work = std::move(work)](jsi::Runtime& runtime) {
    auto alive = weak.lock();  // re-locked inside the lambda
    if (!alive) return;        // torn down between queueing and running: drop
    work(runtime);
  };
  host->post(std::move(task));  // false here also means dropped
}

void HermesExecutor::performMicrotaskCheckpoint(jsi::Runtime& runtime) {
  if (auto host = host_.lock()) host->performMicrotaskCheckpoint(runtime);
}

std::uint64_t HermesExecutor::pauseEpoch() const {
  auto host = host_.lock();
  return host ? host->pauseEpoch() : 0;
}

void HermesHost::performMicrotaskCheckpoint(jsi::Runtime& runtime) {
  // Off the JS thread this would drain another thread's VM mid-task; refuse
  // loudly rather than corrupt it.
  if (!onJsThread()) {
    log(LogLevel::Error, config_.name,
        "performMicrotaskCheckpoint called off the JS thread; ignored");
    return;
  }
  loop_->performMicrotaskCheckpoint(runtime);
}

}  // namespace screenkit
