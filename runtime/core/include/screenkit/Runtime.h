// Copyright (c) ScreenKit contributors. MIT.
//
// The embedder API. This is the only header a platform shell includes.
//
// Scope is deliberately small: create a runtime, run a bundle on it, drive its
// frame tick, freeze and thaw it, register native modules, shut it down. The
// object model (SharedObject / EventEmitter / NativeModule) and the loop
// (timers, microtask checkpoint, rAF) are reached from JS, not from here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <jsi/jsi.h>

#include <screenkit/Instance.h>
#include <screenkit/Log.h>

namespace screenkit {

/// The one rule of this runtime: a single thread owns the `jsi::Runtime`, and
/// every touch of it goes through this executor. No mutex guards the runtime and
/// no `jsi::Runtime&` may outlive the callback it was handed to.
///
/// `invokeSync` is deliberately absent. A synchronous native->JS call is an
/// error, not a missing feature -- Expo's `BridgelessJSCallInvoker::invokeSync`
/// throws outright, and React Native's `RuntimeExecutor` never offered one. Not
/// declaring it is the same answer, enforced at compile time.
///
/// Work handed to a runtime that has already been torn down is dropped, not
/// queued and not run: the executor holds the host weakly and re-locks it inside
/// the callback, so a teardown that races a caller loses the callback rather
/// than touching freed memory.
class JsExecutor {
 public:
  virtual void invokeAsync(std::function<void(facebook::jsi::Runtime&)>&& work) = 0;

  /// HTML's "perform a microtask checkpoint", for queued work that calls into
  /// JS more than once. Every task already ends with one; this is for the points
  /// in between. A browser runs one after each listener callback of an event it
  /// generated itself (user input), so a promise the first listener resolves has
  /// settled before the second listener runs -- native dispatch has to match.
  /// JS thread only: pass the runtime `invokeAsync` handed the work.
  virtual void performMicrotaskCheckpoint(facebook::jsi::Runtime& runtime) = 0;

  /// How many times the runtime has been paused. Work that must not outlive a
  /// pause -- user input, which the lifecycle table says a `Paused` context
  /// never receives -- reads it when queued and drops itself if it has changed
  /// by the time it runs, rather than landing on resume. Safe from any thread.
  virtual std::uint64_t pauseEpoch() const = 0;
  virtual ~JsExecutor() = default;
};

struct RuntimeConfig {
  /// Shows up as the log tag and the JS thread name.
  std::string name = "screenkit";

  /// Hermes GC ceiling. Not a containment boundary -- see Architecture.md 5.2 --
  /// but it does turn a runaway allocation into a JS error instead of an OOM
  /// kill that takes every other instance with it.
  std::size_t maxHeapBytes = 512ull << 20;

  /// **Test only.** DER certificates the TLS verifier accepts as extra trust
  /// anchors, beside the system store -- how the test suite's generated CA is
  /// trusted without installing it. Nothing reachable from JS can add to this,
  /// and a host never sets it: production trust is the OS's alone.
  std::vector<std::vector<std::uint8_t>> testTlsAnchors;

  /// `<iframe sandbox>`: what this runtime may reach (Instance.h). Enforced
  /// where each binding is installed, exactly as `testTlsAnchors` is -- a gated
  /// binding is not installed at all rather than installed half way, so nothing
  /// reachable from this runtime's JS can widen it.
  ///
  /// Default: not sandboxed. A top-level app is the whole app.
  SandboxPolicy sandbox;

  /// The `<iframe>` instances this runtime may make, reachable from any thread
  /// so the host loop can tick them. Null means this runtime makes none, which
  /// is what an instance itself is given: one level of nesting, and an
  /// `<iframe>` inside an instance is refused with a clear error rather than
  /// left silently inert (a recorded divergence from Architecture.md 5, which
  /// allows a tree).
  std::shared_ptr<InstanceRegistry> instances;

  /// How this runtime reaches whoever embeds it: `window.parent.postMessage`.
  /// Null in a top-level runtime, which has no parent -- and that is exactly
  /// what makes `window.parent === window` true there, as it is in a browser.
  std::shared_ptr<InstanceChannel> parent;
};

/// Builds a native module's JS value. Runs on the JS thread, and only when JS
/// first touches the module -- see `Runtime::registerModule`.
using ModuleFactory = std::function<facebook::jsi::Value(facebook::jsi::Runtime&)>;

struct EvalResult {
  bool ok = false;

  /// On failure: the reason. For a JS throw this is the `jsi::JSError` message
  /// followed by the JS stack.
  std::string error;

  /// On success: the completion value, String()-ed. Empty when it has no useful
  /// string form.
  std::string value;

  explicit operator bool() const { return ok; }
};

class Runtime {
 public:
  /// Starts the JS thread and creates the Hermes runtime on it. Returns nullptr
  /// if the runtime could not be created; the reason is logged at error level.
  static std::shared_ptr<Runtime> create(RuntimeConfig config = {});

  virtual ~Runtime() = default;

  /// Load and run a bundle from disk. A `.hbc` is validated against this
  /// engine's bytecode version and sanity-checked *before* it reaches the VM;
  /// anything else is treated as source (the dev path, no bytecode caching).
  ///
  /// Posts to the JS thread and blocks until it answers. That is a bootstrap
  /// convenience for the embedder thread and is not the same thing as a
  /// synchronous native->JS call from JS-adjacent code -- which is why
  /// `JsExecutor` still has no `invokeSync`. Calling it from the JS thread is a
  /// deadlock and is reported as an error instead.
  virtual EvalResult evaluateBundle(const std::string& path) = 0;

  /// Evaluate source text. Used by the dev module runner; release builds ship a
  /// single precompiled `.hbc` and never evaluate source.
  virtual EvalResult evaluateSource(std::string source, std::string url) = 0;

  /// The only legitimate way to reach the runtime after bootstrap. Safe to keep
  /// past `shutdown()`: later work is dropped.
  virtual const std::shared_ptr<JsExecutor>& executor() const = 0;

  /// `Architecture.md` 5.1 `Paused`: timers frozen, `requestAnimationFrame`
  /// stopped, the task queue gated where the loop selects work rather than
  /// where it runs it. A paused runtime burns no CPU and fires no callbacks,
  /// and a thaw re-arms pending timers without reissuing their handles and
  /// without a burst of backdated callbacks. Idempotent; safe from any thread.
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual bool paused() const = 0;

  /// One frame. `SDL_AppIterate` is the real source, which is what makes
  /// `requestAnimationFrame` a real clock rather than an injected one.
  /// `timestampMs` is monotonic milliseconds and is what rAF callbacks receive.
  /// Ignored while paused; ticks that arrive faster than the loop serves them
  /// coalesce. Safe from any thread.
  virtual void tickFrame(double timestampMs) = 0;

  /// Run `callback` on the JS thread at the trailing edge of every frame --
  /// after the `requestAnimationFrame` callbacks and after their microtask
  /// checkpoint, so the frame is finished rather than merely dispatched.
  ///
  /// This exists for the present. The GL work belongs *inside* the rAF
  /// callback and `eglSwapBuffers` belongs after it, and there is exactly one
  /// frame source, so this is a trailing edge on the existing clock rather than
  /// a second one. A render thread or a second timer would give two clocks
  /// racing over one GL context.
  ///
  /// Installed asynchronously; false means the runtime has shut down. Whatever
  /// the callback captures is released on the JS thread at teardown, which is
  /// what lets a GL context be owned by it.
  virtual bool setFrameFinishedCallback(
      std::function<void(facebook::jsi::Runtime&)> callback) = 0;

  /// Nothing queued, nothing running, no timers armed, no frame work
  /// outstanding, and no network request, WebSocket or EventSource open. A host
  /// that runs a bundle and exits uses this to know when the bundle has
  /// genuinely finished rather than guessing at a delay.
  virtual bool idle() const = 0;

  /// The first failure the app declared fatal through
  /// `__screenkit.reportFailure(message)` -- a package whose entry module
  /// rejected, for one. Empty while there is none. A host exits with a failure
  /// at its next loop iteration once this has a value, rather than waiting for
  /// an idle that a failed app may never reach. Safe from any thread.
  virtual std::optional<std::string> failure() const = 0;

  /// Make a native module reachable from JS as `screenkit.modules.<name>`.
  ///
  /// `factory` is wrapped in a `LazyObject`, so a module JS never touches is
  /// never built. False means the runtime has shut down.
  virtual bool registerModule(std::string name, ModuleFactory factory) = 0;

  /// Stops the JS thread, cancels queued work and destroys the runtime on its
  /// owning thread. Idempotent; the destructor calls it.
  virtual void shutdown() = 0;

  /// Bytecode version this Hermes accepts. `.skpkg` manifests record it so a
  /// mismatch is a load-time error rather than a fault inside the VM.
  static std::uint32_t hermesBytecodeVersion();
};

/// **Main thread only.** The JS thread's work queue is a private range of SDL's
/// event queue, and SDL's own main-callback loop drains that queue wholesale
/// with `SDL_PollEvent` -- so a work event can be taken by the main thread
/// before the runtime that owns it sees it. Hand it back from `SDL_AppEvent`:
///
///     SDL_AppResult SDL_AppEvent(void*, SDL_Event* event) {
///       if (screenkit::reclaimRuntimeEvent(*event)) return SDL_APP_CONTINUE;
///       ...
///     }
///
/// True means the event belonged to a live runtime and has been returned to it;
/// the caller must not touch it afterwards. False means it was somebody else's.
bool reclaimRuntimeEvent(const SDL_Event& event);

}  // namespace screenkit
