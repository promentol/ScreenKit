// Copyright (c) ScreenKit contributors. MIT.
//
// libscreenkit.so's SDL_main: the Android TV / Fire TV host. The portable half --
// the graphics bootstrap, the DOM shim, the .skpkg gate -- is host/Host.cpp;
// this file is what Android adds.
//
// SDL owns the app lifecycle, as on tvOS (apple/HostMain.mm): SDL's Java
// activity starts this library's SDL_main on its own thread, and with
// SDL_MAIN_USE_CALLBACKS that thread runs SDL_AppIterate as the frame tick.
// Host.cpp's runWindowed is not used: its loop blocks, and quits on a gamepad's
// Guide button, where an Android app lives in the activity lifecycle instead.
//
// What reaches here from Java (runtime/android/app, ScreenKitActivity) is argv:
//
//   --package <dir>            the .skpkg to run: extracted from the APK's assets,
//                              or a directory named by the `package` intent extra
//   --dom-shim <file>          the prelude, extracted beside it
//   --size WxH                 draw at that size, scaled to the screen
//   --capture <file.png>       save one frame (gfx/FrameCapture.h)
//   --capture-delay-ms <ms>    how long after the first frame to take it
//
// Graphics are SDL's EGL context on its window (core/src/gfx/GlSurfaceSdl.cpp):
// ES 3 when the device offers it, else ES 2 and the WebGL1 path. The frame is
// always drawn offscreen at a fixed size -- the screen's, unless --size says
// otherwise -- because Android destroys the window's drawable whenever the app
// goes to the background, and an offscreen frame is what can be shown again on
// return without waiting for an idle page to repaint.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <jni.h>

#include <SDL3/SDL.h>

#include <hermes/hermes.h>
#include <jsi/jsi.h>

#include <screenkit/Input.h>
#include <screenkit/Instance.h>
#include <screenkit/Log.h>
#include <screenkit/Runtime.h>
#include <screenkit/Viewport.h>

#include "../../host/Host.h"
#include "gfx/GlSurface.h"
#include "media/MediaPlayer.h"
#include "media/MediaPlayerAndroid.h"
#include "net/NetServiceAndroid.h"

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

using namespace screenkit::host;
namespace jsi = facebook::jsi;

namespace {

struct LaunchOptions {
  std::string package;
  std::string domShim;
  int width = 0;  // --size; 0 is the screen's own
  int height = 0;
  std::string capture;
  std::string captureDelayMs;
};

/// argv as ScreenKitActivity.getArguments() builds it. argv[0] is SDL's
/// "app_process".
bool parseArguments(int argc, char* argv[], LaunchOptions& out, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--package" && hasValue) {
      out.package = argv[++i];
    } else if (arg == "--dom-shim" && hasValue) {
      out.domShim = argv[++i];
    } else if (arg == "--size" && hasValue) {
      if (!parseSize(argv[++i], out.width, out.height)) {
        error = std::string("the size extra takes WxH, such as 640x480, not \"") + argv[i] + "\"";
        return false;
      }
    } else if (arg == "--capture" && hasValue) {
      out.capture = argv[++i];
    } else if (arg == "--capture-delay-ms" && hasValue) {
      out.captureDelayMs = argv[++i];
    } else {
      error = "unknown launch argument \"" + arg + "\"";
      return false;
    }
  }
  if (out.package.empty()) {
    error = "no package to run: the APK bundles none, and no `package` intent extra named one";
    return false;
  }
  return true;
}

/// Hermes' Intl on Android is Java (com.facebook.hermes.intl, reached through
/// fbjni), and fbjni caches each Java class the first time it is used. The JS
/// thread is a native thread attached to the VM, and a class lookup from such a
/// thread goes to the system class loader, which does not have the app's
/// classes: a first use there fails, and `'a'.toLocaleUpperCase()` -- which the
/// DOM shim itself calls -- throws. This is SDL's main thread, started from
/// Java, whose lookups use the app's loader. So every Intl entry point is
/// touched once here, in a throwaway runtime, and the classes stay cached for
/// the process.
void warmIntl() {
  static constexpr const char* kWarm = R"((function () {
    var failed = [];
    function touch(name, f) { try { f(); } catch (e) { failed.push(name + ': ' + e); } }
    touch('toLocaleUpperCase', function () { 'a'.toLocaleUpperCase(); 'A'.toLocaleLowerCase(); });
    touch('localeCompare', function () { 'a'.localeCompare('b'); });
    touch('normalize', function () { 'a'.normalize('NFD'); });
    touch('getCanonicalLocales', function () { Intl.getCanonicalLocales('en-US'); });
    touch('Collator', function () {
      var c = new Intl.Collator('en-US'); c.compare('a', 'b'); c.resolvedOptions();
      Intl.Collator.supportedLocalesOf('en-US');
    });
    touch('DateTimeFormat', function () {
      var f = new Intl.DateTimeFormat('en-US'); f.format(0); f.formatToParts(0); f.resolvedOptions();
      Intl.DateTimeFormat.supportedLocalesOf('en-US');
      new Date(0).toLocaleString(); new Date(0).toLocaleDateString(); new Date(0).toLocaleTimeString();
    });
    touch('NumberFormat', function () {
      var f = new Intl.NumberFormat('en-US'); f.format(1.5); f.formatToParts(1.5); f.resolvedOptions();
      Intl.NumberFormat.supportedLocalesOf('en-US'); (1.5).toLocaleString();
    });
    return failed.join('; ');
  })())";
  try {
    auto runtime = facebook::hermes::makeHermesRuntime();
    const jsi::Value result =
        runtime->evaluateJavaScript(std::make_shared<jsi::StringBuffer>(kWarm), "screenkit://warm-intl.js");
    const std::string failed = result.isString() ? result.getString(*runtime).utf8(*runtime) : std::string();
    if (!failed.empty()) {
      screenkit::log(screenkit::LogLevel::Warn, kTag, "Intl is not fully usable: " + failed);
    }
  } catch (const std::exception& e) {
    screenkit::log(screenkit::LogLevel::Warn, kTag, std::string("could not prepare Intl: ") + e.what());
  }
}

/// The app's place on screen, shared by SDL's thread and the Java UI thread.
///
/// Leaving the screen is driven from Java (ScreenKitActivity's onPause and its
/// surface's surfaceDestroyed), not from SDL's WILL_ENTER_BACKGROUND event,
/// because SDL delivers that event from inside SDL_PushEvent's watcher lock.
/// Anything that waits there deadlocks: the SDL timer thread, and any thread
/// queueing runtime work, holds the runtime's locks while it calls
/// SDL_PushEvent, so a watcher that queues a task or pauses the runtime waits on
/// a thread that waits on the watcher. The UI thread holds none of SDL's locks.
///
/// Returning is picked up by SDL_AppIterate: SDL resumes its loop only once the
/// new window surface exists, which is when it can be bound. The exception is a
/// return during startup, before that loop runs at all -- see `started`.
struct Lifecycle {
  std::weak_ptr<screenkit::Runtime> runtime;
  std::weak_ptr<screenkit::gfx::GlSurface> surface;
  // Between leaving the screen and coming back. Read by the JS-thread task that
  // lets go of the drawable, which does nothing if it only runs after the return.
  std::shared_ptr<std::atomic<bool>> backgrounded = std::make_shared<std::atomic<bool>>(false);
  // Set without locks (Java's onResume, SDL's DID_ENTER_FOREGROUND); acted on by
  // SDL_AppIterate.
  std::atomic<bool> returnPending{false};
  // False until SDL_AppInit is past the evaluations it waits on the JS thread
  // for. Until then SDL_AppIterate has not run, so a return cannot be left to it:
  // SDL_AppInit would wait for a JS thread the pause has stopped, forever.
  std::atomic<bool> started{false};
};

std::mutex gLifecycleMutex;
std::shared_ptr<Lifecycle> gLifecycle;  // guarded by gLifecycleMutex

std::shared_ptr<Lifecycle> currentLifecycle() {
  std::lock_guard<std::mutex> lock(gLifecycleMutex);
  return gLifecycle;
}

struct HostState {
  std::shared_ptr<screenkit::Runtime> runtime;
  // The `<iframe>` instances this page has embedded (screenkit/Instance.h):
  // ticked with the page, and terminated before the activity returns.
  std::shared_ptr<screenkit::InstanceRegistry> instances;
  // Remote, keyboard and gamepad input. Destroyed before the runtime shuts
  // down, so no key event is queued to a JS thread that is going away.
  std::unique_ptr<screenkit::InputRouter> input;
  // Window size changes, as `resize` at `window`. Destroyed with `input`.
  std::unique_ptr<screenkit::ViewportEvents> viewport;
  SDL_Window* window = nullptr;
  // The GL surface, owned by the runtime's `gl` and its frame hook. Reached on
  // the JS thread around the background (leaveScreen, returnToScreen).
  std::weak_ptr<screenkit::gfx::GlSurface> surface;
  std::shared_ptr<Lifecycle> lifecycle = std::make_shared<Lifecycle>();
  // Set on the JS thread by window.close() (runtime/js/dom-shim.js,
  // __screenkitClose); read by SDL_AppIterate.
  std::shared_ptr<std::atomic<bool>> closeRequested = std::make_shared<std::atomic<bool>>(false);
  // The app bundle evaluates on this thread, not in SDL_AppInit, as on tvOS:
  // evaluation blocks its caller until the JS thread answers, and SDL's main
  // thread has to keep serving the activity meanwhile.
  std::thread bundleThread;
  std::atomic<bool> bundleDone{false};
  screenkit::EvalResult bundleResult;

  ~HostState() {
    {
      std::lock_guard<std::mutex> lock(gLifecycleMutex);
      if (gLifecycle == lifecycle) gLifecycle.reset();
    }
    input.reset();
    viewport.reset();
    // Every instance is terminated -- thread joined, heap and GL freed -- before
    // this page's own runtime goes: a child's context is in this one's share
    // group, and its texture is what this one's compositor was sampling.
    if (instances) screenkit::terminateInstances(*instances);
    // shutdown() cancels an evaluation still in flight, which releases the
    // bundle thread, and destroys the GL surface on the JS thread -- so the
    // window it was created over can go after it.
    if (runtime) runtime->shutdown();
    if (bundleThread.joinable()) bundleThread.join();
    // The players went with the runtime; no plane is placed in a window that
    // is going away.
    screenkit::media::clearVideoHost();
    if (window != nullptr) SDL_DestroyWindow(window);
  }
};

/// Window + GL surface + `gl` global, in that order. ES 3 first: WebGL2 needs
/// it, and a device that has it gets it. A device without an ES 3 config fails
/// the window (SDL picks the config when it creates the window's surface), and
/// one whose driver refuses the context fails the surface; either way the next
/// try is ES 2, the WebGL1 path a Raspberry Pi 3 takes. (Asking for ES 2 on an
/// ES 3 device does not get ES 2: EGL answers with the newest compatible
/// version, which is why ES 3 is not simply the only request.)
bool openWindowAndGraphics(HostState& state, const LaunchOptions& options, std::string& error) {
  for (const int major : {3, 2}) {
    screenkit::gfx::GlSurface::prepareWindowAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    state.window = SDL_CreateWindow("ScreenKit", 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (state.window == nullptr) {
      error = "SDL_CreateWindow (GLES " + std::to_string(major) + ") failed: " + SDL_GetError();
    } else {
      int pixelWidth = 1920;
      int pixelHeight = 1080;
      SDL_GetWindowSizeInPixels(state.window, &pixelWidth, &pixelHeight);
      const int fixedWidth = options.width > 0 ? options.width : pixelWidth;
      const int fixedHeight = options.height > 0 ? options.height : pixelHeight;
      if (startGraphics(state.runtime, state.window, pixelWidth, pixelHeight, fixedWidth, fixedHeight, error,
                        &state.surface)) {
        // Where a <video>'s plane goes: beneath SDL's surface, measured in the
        // fixed-size drawable the frame is drawn at (media/MediaPlayer.h). The
        // Java player scales it into the window as the present does.
        screenkit::media::VideoHost host;
        host.window = state.window;
        host.drawableWidth = fixedWidth;
        host.drawableHeight = fixedHeight;
        screenkit::media::setVideoHost(host);
        return true;
      }
      error = "GLES " + std::to_string(major) + ": " + error;
      SDL_DestroyWindow(state.window);
      state.window = nullptr;
    }
    if (major == 3) screenkit::log(screenkit::LogLevel::Warn, kTag, error + " -- trying GLES 2");
  }
  return false;
}

/// `window.close()` ends the app: the activity finishes, as a TV app leaving to
/// the launcher does. What Back means at the app's root is the app's decision --
/// it is delivered as a key and nothing more -- so a page that wants to leave
/// calls close() itself (runtime/js/dom-shim.js). The hook is queued before the
/// app bundle is, so it exists before any app code runs.
void installCloseHook(const std::shared_ptr<screenkit::Runtime>& runtime,
                      std::shared_ptr<std::atomic<bool>> closeRequested) {
  runtime->executor()->invokeAsync([closeRequested](jsi::Runtime& js) {
    js.global().setProperty(
        js, "__screenkitClose",
        jsi::Function::createFromHostFunction(
            js, jsi::PropNameID::forAscii(js, "__screenkitClose"), 0,
            [closeRequested](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
              closeRequested->store(true);
              return jsi::Value::undefined();
            }));
  });
}

/// The app is leaving the screen, and Android is about to destroy the window's
/// drawable while the GL context is current on the JS thread. Pause the runtime
/// and let go of the drawable there, and wait for that -- the UI thread, before
/// SDL hears of the pause, and so before SDL releases the surface. Nothing JS can
/// see is released: the context, the page's GL objects, its timers and its state
/// all stay.
void leaveScreen(Lifecycle& lifecycle) {
  auto runtime = lifecycle.runtime.lock();
  if (!runtime) return;
  // A return SDL_AppIterate has not acted on yet is stale now: acting on it
  // would resume and bind the drawable that is being taken away.
  lifecycle.returnPending.store(false);
  if (lifecycle.backgrounded->exchange(true)) return;

  struct Done {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };
  auto done = std::make_shared<Done>();
  std::weak_ptr<screenkit::Runtime> weakRuntime = runtime;
  // On the JS thread, so it lands between tasks: nothing is drawing. Pausing
  // there, before the drawable goes, means no frame runs after it has.
  runtime->executor()->invokeAsync([done, weakRuntime, surface = lifecycle.surface,
                                    backgrounded = lifecycle.backgrounded](jsi::Runtime&) {
    if (backgrounded->load()) {
      if (auto live = weakRuntime.lock()) live->pause();
      if (auto live = surface.lock()) live->suspend();
    }
    {
      std::lock_guard<std::mutex> lock(done->mutex);
      done->done = true;
    }
    done->cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(done->mutex);
  if (!done->cv.wait_for(lock, std::chrono::seconds(1), [&] { return done->done; })) {
    // A task that runs long. Pause from here; the queued release still runs
    // when the runtime resumes, or does nothing if the app is back by then, and
    // resume() binds the new drawable either way. Android keeps the old one
    // alive for as long as the JS thread has it current.
    runtime->pause();
    screenkit::log(screenkit::LogLevel::Warn, kTag, "backgrounded: the JS thread did not let go of the drawable within 1s");
  }
  screenkit::log(screenkit::LogLevel::Log, kTag, "backgrounded: runtime paused");
}

/// Back on screen, with a new drawable. Binding it is queued before the thaw,
/// and a paused runtime queues no timers, frames or input, so it is the first
/// drawing work the JS thread does; the last frame is presented again at once.
/// Called from SDL_AppIterate, or from the UI thread while the host is still
/// starting up -- never from inside an SDL event watcher. `bindNow` is false for
/// the one caller that cannot know whether SDL has a drawable yet.
void returnToScreen(Lifecycle& lifecycle, bool bindNow = true) {
  auto runtime = lifecycle.runtime.lock();
  if (!runtime) return;
  const bool wasBackgrounded = lifecycle.backgrounded->exchange(false);
  // Bound again even when no suspend ran -- a surface lost during startup, or a
  // release that never got its turn on the JS thread. SDL hands back a new
  // drawable either way, and binding the one recorded as current would leave
  // the app drawing into the dead one: a black screen.
  if (bindNow) {
    runtime->executor()->invokeAsync([surface = lifecycle.surface](jsi::Runtime&) {
      if (auto live = surface.lock()) live->resume();
    });
  }
  if (!wasBackgrounded) return;
  runtime->resume();
  screenkit::log(screenkit::LogLevel::Log, kTag, "foreground: runtime resumed");
}

/// SDL_AppIterate is the frame tick, and SDL's generic main loop calls it as
/// fast as it returns. Pace it at the display's rate, so rAF runs at the rate a
/// browser gives it and an idle app does not spin a core.
void paceFrames(SDL_Window* window) {
  float rate = 60.0f;
  if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
    if (mode->refresh_rate > 0.0f) rate = mode->refresh_rate;
  }
  SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, std::to_string(rate).c_str());
}

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
  auto state = std::make_unique<HostState>();

  // Back goes to the page as a key. Left to SDL's activity, a Back it saw itself
  // would finish the app before JS heard of it; a page that wants Back at its
  // root to leave calls window.close() (see the close hook).
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  screenkit::InputRouter::configureHints();
  if (!SDL_Init(SDL_INIT_VIDEO | screenkit::InputRouter::requiredSubsystems())) {
    screenkit::log(screenkit::LogLevel::Error, kTag, std::string("SDL_Init failed: ") + SDL_GetError());
    return SDL_APP_FAILURE;
  }

  LaunchOptions options;
  std::string error;
  if (!parseArguments(argc, argv, options, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  warmIntl();

  // FrameCapture reads its request from the environment on every platform.
  if (!options.capture.empty()) {
    setenv("SCREENKIT_CAPTURE", options.capture.c_str(), 1);
    if (!options.captureDelayMs.empty()) setenv("SCREENKIT_CAPTURE_DELAY_MS", options.captureDelayMs.c_str(), 1);
  }

  screenkit::RuntimeConfig config;
  config.name = kTag;
  // This page may embed `<iframe>` instances; each of those is given no registry
  // of its own, which is what makes nesting one level deep.
  state->instances = screenkit::makeInstanceRegistry();
  screenkit::setInstanceEnvironment(*state->instances, options.domShim);
  config.instances = state->instances;
  state->runtime = screenkit::Runtime::create(config);
  if (!state->runtime) {
    screenkit::log(screenkit::LogLevel::Error, kTag, "runtime failed to start");
    return SDL_APP_FAILURE;
  }
  state->input = std::make_unique<screenkit::InputRouter>(state->runtime);
  state->viewport = std::make_unique<screenkit::ViewportEvents>(state->runtime);
  // Input and `resize` follow focus: the focused browsing context is the one
  // that hears from the remote and from the window.
  state->input->setFocusTarget(screenkit::focusTarget(*state->instances));

  // The package gate, before a window exists and before anything is evaluated.
  // A refusal ends SDL_main, and SDL's activity finishes when it returns.
  LaunchTarget target;
  if (!resolveLaunch(state->runtime, options.package, target, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  if (!openWindowAndGraphics(*state, options, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  // Published as soon as there is something to pause and a drawable to let go
  // of: the activity can be backgrounded while the shim, the gate and the
  // bundle are still going, and the Java side calls in from there.
  state->lifecycle->runtime = state->runtime;
  state->lifecycle->surface = state->surface;
  {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    gLifecycle = state->lifecycle;
  }

  // After the graphics bootstrap, before the app bundle. Fatal on failure.
  if (!evaluateDomShim(state->runtime, options.domShim, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }
  installCloseHook(state->runtime, state->closeRequested);
  if (!setAssetRoot(state->runtime, target.assetRoot, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  screenkit::log(screenkit::LogLevel::Log, kTag,
                 "hermes bytecode version " + std::to_string(screenkit::Runtime::hermesBytecodeVersion()) +
                     ", loading " + target.bundle);

  HostState* launched = state.get();
  launched->bundleThread = std::thread([launched, bundle = target.bundle] {
    launched->bundleResult = launched->runtime->evaluateBundle(bundle);
    if (launched->bundleResult.ok) announceDocumentLoaded(launched->runtime);
    launched->bundleDone.store(true);
  });

  // Nothing on the main thread waits on the JS thread from here on, so a return
  // can be left to SDL_AppIterate.
  state->lifecycle->started.store(true);

  paceFrames(state->window);
  *appstate = state.release();
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
  auto* state = static_cast<HostState*>(appstate);
  if (state == nullptr || !state->runtime) return SDL_APP_FAILURE;

  // A bundle that threw while evaluating, or a failure the app reported later,
  // ends the app, whatever it still has scheduled.
  if (state->bundleDone.load() && state->bundleThread.joinable()) {
    state->bundleThread.join();
    if (!state->bundleResult.ok) {
      screenkit::log(screenkit::LogLevel::Error, kTag, state->bundleResult.error);
      return SDL_APP_FAILURE;
    }
  }
  if (appFailed(state->runtime)) return SDL_APP_FAILURE;

  // window.close(): the activity finishes, as any TV app leaving does.
  if (state->closeRequested->exchange(false)) {
    screenkit::log(screenkit::LogLevel::Log, kTag, "window.close(): finishing");
    return SDL_APP_SUCCESS;
  }

  if (state->lifecycle->returnPending.exchange(false)) returnToScreen(*state->lifecycle);

  // The frame tick that services requestAnimationFrame. The present happens on
  // the JS thread at the end of that frame -- see startGraphics. An idle app is
  // not finished: a TV UI that has drawn its screen waits for the remote.
  state->input->tick(SDL_GetTicksNS());
  const double tickMs = static_cast<double>(SDL_GetTicksNS()) / 1.0e6;
  // Every live instance, then this page. A paused instance ignores the tick
  // itself, so a launcher with frozen tiles pays for a loop over a vector.
  if (state->instances) {
    for (const std::shared_ptr<screenkit::Instance>& instance :
         screenkit::liveInstances(*state->instances)) {
      instance->tickFrame(tickMs);
    }
  }
  state->runtime->tickFrame(tickMs);
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
  auto* state = static_cast<HostState*>(appstate);
  // SDL's main-callback loop drains the whole shared event queue, and the JS
  // thread's work queue is a private range of it -- give that work back first.
  if (screenkit::reclaimRuntimeEvent(*event)) return SDL_APP_CONTINUE;
  if (state == nullptr) return SDL_APP_CONTINUE;

  switch (event->type) {
    // App events arrive here from inside SDL_PushEvent's watcher lock
    // (SDL_main_callbacks.c dispatches them at once), where nothing may wait or
    // touch the runtime -- see Lifecycle. Leaving the screen came from Java
    // already; the return is flagged for SDL_AppIterate.
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      state->lifecycle->returnPending.store(true);
      return SDL_APP_CONTINUE;
    // A TV that switches mode -- 60 Hz to a 50 Hz broadcast rate, or a new
    // resolution -- changes the rate rAF should run at.
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
      if (state->window != nullptr) paceFrames(state->window);
      return SDL_APP_CONTINUE;
    case SDL_EVENT_LOW_MEMORY:
      // SDL's name for every onTrimMemory, which Android also sends routinely
      // when the app's UI is hidden. Nothing is dropped for it yet.
      screenkit::log(screenkit::LogLevel::Log, kTag, "memory trim requested by the system (onTrimMemory)");
      return SDL_APP_CONTINUE;
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_QUIT:
      return SDL_APP_SUCCESS;
    default:
      break;
  }
  if (state->input && state->input->handleEvent(*event)) return SDL_APP_CONTINUE;
  if (state->viewport && state->viewport->handleEvent(*event)) return SDL_APP_CONTINUE;
  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
  (void)result;
  delete static_cast<HostState*>(appstate);
}

// ---- the VM, and the classes only this thread can see ------------------------
//
// The repo's first JNI_OnLoad, and it exists for one reason: the class-loader
// trap. `System.loadLibrary("screenkit")` calls this on the Java thread that
// loaded it, where the app's class loader is in place; the JS thread and the
// network I/O queue are native threads attached to the VM, whose `FindClass`
// reaches only the system class loader and would never find
// `dev.screenkit.net.HttpClient`. That is exactly how Hermes' Intl failed
// (warmIntl, above), so the network client resolves its classes here instead
// and holds them as global references for the process.
//
// A failure here is not fatal: `net::networkAvailable()` stays false and every
// request fails with `unsupported`, as it does on a platform with no client. The
// media player (dev.screenkit.media.VideoPlayer) is resolved here too, and fails
// the same way: `media::mediaAvailable()` false, every load `unavailable`.

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  screenkit::net::setJavaVm(vm);
  JNIEnv* env = nullptr;
  std::string error;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    screenkit::log(screenkit::LogLevel::Warn, kTag,
                   "no JNIEnv in JNI_OnLoad: the network client stays unavailable");
  } else {
    if (!screenkit::net::prepareAndroidNetwork(env, error)) {
      screenkit::log(screenkit::LogLevel::Warn, kTag, "no network client: " + error);
    }
    // dev.screenkit.media.VideoPlayer, for the same class-loader reason; a
    // failure leaves every <video> load failing `unavailable`.
    error.clear();
    if (!screenkit::media::prepareAndroidMedia(vm, env, error)) {
      screenkit::log(screenkit::LogLevel::Warn, kTag, "no media player: " + error);
    }
  }
  return JNI_VERSION_1_6;
}

// ---- called from ScreenKitActivity (Java), on the UI thread -------------------

extern "C" {

JNIEXPORT void JNICALL Java_dev_screenkit_host_ScreenKitActivity_nativeLeaveScreen(JNIEnv*, jclass) {
  if (auto lifecycle = currentLifecycle()) leaveScreen(*lifecycle);
}

JNIEXPORT void JNICALL Java_dev_screenkit_host_ScreenKitActivity_nativeReturnToScreen(JNIEnv*, jclass) {
  auto lifecycle = currentLifecycle();
  if (!lifecycle) return;
  if (lifecycle->started.load()) {
    // SDL's loop is running: leave it to SDL_AppIterate, which runs once SDL has
    // taken the window's new drawable back.
    lifecycle->returnPending.store(true);
    return;
  }
  // Still inside SDL_AppInit, which is waiting on a JS thread the pause stopped,
  // so nothing would read that flag: thaw from here instead, and let SDL_AppInit
  // finish. Not the drawable, though -- onResume can arrive before SDL has
  // recreated it, and binding nothing is how the app ends up drawing nowhere.
  // That comes from surfaceChanged, which calls this too, and from the flag once
  // SDL's loop starts.
  returnToScreen(*lifecycle, /*bindNow=*/false);
  lifecycle->returnPending.store(true);
}

}  // extern "C"
