// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <SDL3/SDL_thread.h>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

#include "../bindings/HostIO.h"
#include "../loop/EventLoop.h"
#include "../loop/SdlSync.h"
#include "../loop/WorkQueue.h"

namespace screenkit {

class SharedObjectRegistry;
class CanvasBinding;
class NetBinding;
class InstanceBinding;
class MediaBinding;
class MediaRegistry;
namespace text {
class FontLibrary;
}

/// One thread, one `jsi::Runtime`, one queue.
///
/// React Native (`RuntimeExecutor` over `MessageQueueThread::runOnQueue`) and
/// Expo (`BridgelessJSCallInvoker`) converged independently on one executor
/// owning the runtime. This adopts the same shape for a single instance,
/// because retrofitting thread discipline afterwards is far harder than
/// starting with it.
///
/// The runtime is created on the JS thread and destroyed on it, and never
/// leaves: no mutex guards it and no `jsi::Runtime&` crosses a thread boundary.
///
/// **Every primitive underneath is SDL3's.** The thread is `SDL_CreateThread` /
/// `SDL_WaitThread`, mutual exclusion is `SDL_CreateMutex`, the blocking wait is
/// `SDL_WaitCondition`, the queue is a registered SDL event range (`WorkQueue`)
/// and the clock is `SDL_GetTicksNS` -- not `std::thread`, `std::mutex`,
/// `std::condition_variable`, a `std::deque` or `std::chrono`. One substrate, and
/// the same one the timers and the frame tick already run on.
class HermesHost : public std::enable_shared_from_this<HermesHost> {
 public:
  struct Task {
    /// Runs on the JS thread with the live runtime.
    std::function<void(facebook::jsi::Runtime&)> run;
    /// Runs instead of `run` when the host stops with this task still queued.
    /// Exists so a blocked caller is told the work was dropped rather than
    /// waiting forever for a thread that has gone.
    std::function<void()> cancel;
  };

  /// Starts the thread and creates the runtime on it. Blocks until the runtime
  /// exists or creation has failed; returns nullptr on failure.
  static std::shared_ptr<HermesHost> start(RuntimeConfig config);

  ~HermesHost();

  HermesHost(const HermesHost&) = delete;
  HermesHost& operator=(const HermesHost&) = delete;

  /// Queue work. Returns false -- meaning dropped, not deferred -- once the host
  /// has stopped or when SDL's event queue refuses the push.
  bool post(Task task);

  /// Stop accepting work, cancel what is queued, join the thread and destroy the
  /// runtime on it. Idempotent.
  void stop();

  bool stopped() const;

  /// True when called from the JS thread. Used to turn a would-be deadlock into
  /// an error.
  bool onJsThread() const;

  /// `Architecture.md` 5.1 `Paused`. Safe from any thread; idempotent.
  void pause();
  void resume();
  bool paused() const;
  std::uint64_t pauseEpoch() const;

  /// Hand the loop a frame. `SDL_AppIterate` is the source.
  void tickFrame(double timestampMs);

  /// `JsExecutor::performMicrotaskCheckpoint`. JS thread only.
  void performMicrotaskCheckpoint(facebook::jsi::Runtime& runtime);

  /// Install the loop's frame-finished hook. Posted rather than assigned, so
  /// the callback is only ever touched on the JS thread and needs no lock.
  bool setFrameFinishedCallback(std::function<void(facebook::jsi::Runtime&)> callback);

  /// Nothing queued, nothing running, no timers, no frame work outstanding.
  bool idle() const;

  /// `Runtime::failure`. Safe from any thread.
  std::optional<std::string> failure() const { return failures_->get(); }

  /// Make a native module reachable from JS as `screenkit.modules.<name>`,
  /// behind a `LazyObject` so the factory does not run until JS touches it.
  bool registerModule(std::string name, ModuleFactory factory);

  const RuntimeConfig& config() const { return config_; }

 private:
  explicit HermesHost(RuntimeConfig config);

  static int SDLCALL threadEntry(void* self);
  void threadMain();

  /// The dispatch loop's wait predicate, minus `stopping_`. Called under
  /// `mutex_`; takes the queue's lock and the loop's, never the other way round.
  bool workReady() const;

  /// A present owed by a frozen runtime: the page is stopped, but the window it
  /// composites `<iframe>` instances into is not the page's
  /// (`EventLoop::setPresentWhilePaused`). Same locking rules as `workReady()`.
  bool presentReady() const;

  void setPaused(bool paused);
  void wake();

  RuntimeConfig config_;

  mutable SdlMutex mutex_;
  SdlCondition cv_;
  std::unique_ptr<WorkQueue> queue_;
  bool stopping_ = false;
  /// The freeze gate, checked where the loop *selects* a task. A gate at
  /// execution time would still burn the wakeup, which is exactly what `Paused`
  /// promises not to do.
  bool paused_ = false;
  /// A task is executing right now. `idle()` has to say no while it is, or a
  /// caller polling for quiescence would stop one task too early.
  bool busy_ = false;

  // Startup handshake: the thread signals once the runtime exists or failed.
  SdlMutex startMutex_;
  SdlCondition startCv_;
  bool started_ = false;
  bool startOk_ = false;
  std::string startError_;

  SdlMutex lifecycleMutex_;  // serialises stop() against itself
  SDL_Thread* thread_ = nullptr;
  SDL_ThreadID threadId_ = 0;

  /// Touched only by the JS thread. Whichever engine this build runs
  /// (engine/Engine.h); everything here speaks JSI.
  std::unique_ptr<facebook::jsi::Runtime> runtime_;

  /// Created before the thread starts so `pause()` and `tickFrame()` are usable
  /// the moment `start()` returns.
  std::shared_ptr<EventLoop> loop_;
  std::shared_ptr<SharedObjectRegistry> objects_;

  /// `__screenkit.net`: the I/O queue, open requests and sockets, and their JS
  /// callbacks. Created and shut down on the JS thread.
  std::shared_ptr<NetBinding> net_;

  /// `__screenkit.instances`: every `<iframe>` this runtime embeds, and its
  /// layer. Created and shut down on the JS thread; shutting it down joins each
  /// instance's own thread.
  std::shared_ptr<InstanceBinding> instances_;

  /// `__screenkit.canvas`: every `<canvas>` of this page that is not the frame
  /// -- its own GL context and its own compositor layer. Created and shut down
  /// on the JS thread, before the contexts themselves are released.
  std::shared_ptr<CanvasBinding> canvas_;

  /// `__screenkit.media`: every player and its JS target. Created and shut down
  /// on the JS thread.
  std::shared_ptr<MediaBinding> media_;
  /// The same players, reachable from any thread: a pause (from whichever
  /// thread pauses the runtime) pauses them without waiting for the frozen JS
  /// thread. Made with the host, so it exists before and after the JS thread.
  std::shared_ptr<MediaRegistry> mediaRegistry_;

  /// `__screenkit.text`: the fonts the 2D canvas draws with. Created and
  /// closed on the JS thread.
  std::shared_ptr<text::FontLibrary> text_;

  /// What `__screenkit.reportFailure` recorded. Created with the host, so it
  /// is readable before -- and after -- the JS thread exists.
  std::shared_ptr<FailureReport> failures_ = std::make_shared<FailureReport>();
};

/// The `JsExecutor` the embedder sees. It holds the host weakly and re-locks it
/// inside the queued callback, so work that races teardown is dropped at either
/// point rather than reaching a destroyed runtime.
class HermesExecutor final : public JsExecutor {
 public:
  explicit HermesExecutor(std::weak_ptr<HermesHost> host) : host_(std::move(host)) {}

  void invokeAsync(std::function<void(facebook::jsi::Runtime&)>&& work) override;
  void performMicrotaskCheckpoint(facebook::jsi::Runtime& runtime) override;
  std::uint64_t pauseEpoch() const override;

 private:
  std::weak_ptr<HermesHost> host_;
};

}  // namespace screenkit
