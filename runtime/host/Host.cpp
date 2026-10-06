// Copyright (c) ScreenKit contributors. MIT.
//
// The platform shell's portable half (Host.h). The SDL window, the frame tick
// and the drawable are shared by every platform; only how a window becomes a
// GL drawable, and where the prelude and storage live, differ.
#include "Host.h"

#include "FramePacing.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include <screenkit/Input.h>
#include <screenkit/Instance.h>
#include <screenkit/Viewport.h>

#include "bundle/Package.h"
#include "engine/Engine.h"
#include "gfx/FrameCapture.h"
#include "gfx/FrameStats.h"
#include "gfx/GlSurface.h"
#include "gfx/VendoredWebGL.h"
#include "media/MediaPlayer.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include "media/linux/WaylandVideo.h"
#endif

namespace screenkit::host {

// ---------------------------------------------------------------------------
// Graphics bootstrap
//
// The EGL context is created on, and made current on, the JS thread: that is
// where every GL call from JS lands, and an EGL context is current on one
// thread at a time. So the drawable is taken on the main thread (SDL owns the
// window) and handed across, and everything after that happens over there.
//
// Nothing here keeps a strong reference to the surface afterwards. The `gl`
// host functions and the loop's frame-finished hook own it, both of which the
// runtime destroys on its own thread -- which is what makes teardown happen on
// the JS thread without anybody having to remember to do it.
// ---------------------------------------------------------------------------

namespace {

struct GlBootstrap {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string error;
  std::shared_ptr<gfx::GlSurface> surface;

  void settle(bool succeeded, std::string reason,
              std::shared_ptr<gfx::GlSurface> created) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
      ok = succeeded;
      error = std::move(reason);
      surface = std::move(created);
    }
    cv.notify_all();
  }
};

/// Create the GL surface on the JS thread, install `gl`, and hang the present
/// off the end of the frame. Blocks the caller until the JS thread has
/// answered, so a failure is reported before a bundle that needs `gl` runs.
}  // namespace

bool startGraphics(const std::shared_ptr<Runtime>& runtime, void* nativeDrawable, int width, int height,
                   int fixedWidth, int fixedHeight, std::string& error,
                   std::weak_ptr<gfx::GlSurface>* surfaceOut,
                   std::shared_ptr<std::atomic<bool>> painted) {
  auto bootstrap = std::make_shared<GlBootstrap>();

  runtime->executor()->invokeAsync(
      [bootstrap, nativeDrawable, width, height, fixedWidth, fixedHeight](facebook::jsi::Runtime& js) {
        gfx::GlSurface::Desc desc;
        desc.nativeLayer = nativeDrawable;
        desc.width = width;
        desc.height = height;
        desc.fixedWidth = fixedWidth;
        desc.fixedHeight = fixedHeight;

        std::string reason;
        std::shared_ptr<gfx::GlSurface> surface =
            gfx::GlSurface::create(desc, reason);
        if (!surface) {
          bootstrap->settle(false, std::move(reason), nullptr);
          return;
        }
        // The vendored expo-gl WebGLRenderingContext is the GL path now; the
        // hand-rolled `gl` global from M4 is gone. Installed on the JS thread
        // with the surface current, which is what prepareContext requires.
        gfx::installVendoredWebGL(js, surface);
        bootstrap->settle(true, std::string(), std::move(surface));
      });

  std::shared_ptr<gfx::GlSurface> surface;
  {
    std::unique_lock<std::mutex> lock(bootstrap->mutex);
    if (!bootstrap->cv.wait_for(lock, std::chrono::seconds(20), [&] { return bootstrap->done; })) {
      error = "the JS thread did not answer the graphics bootstrap within 20s";
      return false;
    }
    if (!bootstrap->ok) {
      error = bootstrap->error;
      return false;
    }
    surface = std::move(bootstrap->surface);
  }

  const std::string renderer = surface->renderer();
  if (surfaceOut != nullptr) *surfaceOut = surface;

  // The present, and the frame-time measurement that goes with it. This is the
  // trailing edge of the *existing* frame -- after the loop has serviced
  // requestAnimationFrame and its microtasks -- not a second clock. A render
  // thread or a second timer would give two clocks racing over one context.
  auto stats = std::make_shared<gfx::FrameStats>("screenkit.gl");
  // SCREENKIT_CAPTURE=<file.png>: save one frame (gfx/FrameCapture.h).
  std::shared_ptr<gfx::FrameCapture> capture = gfx::FrameCapture::fromEnvironment();
  const bool installed = runtime->setFrameFinishedCallback(
      [surface, stats, capture, painted](facebook::jsi::Runtime& rt) {
        const double nowMs = static_cast<double>(SDL_GetTicksNS()) / 1.0e6;
        // Flush the frame's queued GL, and swap only if it painted -- see
        // presentFrame in gfx/VendoredWebGL.h for why both halves exist.
        bool swapped;
        if (capture) {
          swapped = gfx::presentFrame(rt, *surface, [&] { capture->beforePresent(*surface, nowMs); });
        } else {
          swapped = gfx::presentFrame(rt, *surface);
        }
        // Whether the swap happened is what tells a host loop if the display
        // already paced this frame (runWindowed). Written from the JS thread,
        // read from the loop's, so it is one frame stale there -- which is the
        // right answer anyway, painting being stable frame to frame.
        if (painted) painted->store(swapped, std::memory_order_relaxed);
        stats->frame(nowMs);
      });

  // Drop this thread's reference. From here the only owners are the `gl` host
  // functions and the frame hook, and the runtime releases both on the JS
  // thread at teardown -- which is what makes "destroyed on the JS thread" true
  // without anybody having to remember it.
  surface.reset();

  if (!installed) {
    error = "the runtime shut down before the present could be installed";
    return false;
  }

  log(LogLevel::Log, kTag, "drawable ready on " + renderer);
  return true;
}

// ---------------------------------------------------------------------------
// The DOM shim prelude
//
// `runtime/js/dom-shim.js`, compiled to bytecode by the pinned hermesc. It is
// what turns the `gl` global the bootstrap just installed into something an
// unmodified web build can reach: document.createElement("canvas").getContext.
//
// Order is load-bearing in both directions. Evaluated before startGraphics the
// shim would have no context to hand out; evaluated after the app bundle the
// bundle has already asked.
// ---------------------------------------------------------------------------

/// Confine asset reads to `dir` -- the package directory, or the directory a
/// plain bundle file sits in.
///
/// This must run after the prelude and **before** the app bundle: the root is
/// write-once, so whoever sets it first wins, and it has to be the host rather
/// than app code.
bool setAssetRoot(const std::shared_ptr<Runtime>& runtime, const std::string& dir, std::string& error) {
  std::string escaped;
  for (char c : dir) {
    if (c == '\\' || c == '\'') escaped += '\\';
    escaped += c;
  }
  const EvalResult result =
      runtime->evaluateSource("__screenkit.setAssetRoot('" + escaped + "')", "asset-root.js");
  if (!result.ok) {
    error = "could not set the asset root to " + dir + ": " + result.error;
    return false;
  }
  log(LogLevel::Log, kTag, "asset root: " + result.value);
  return true;
}

/// Evaluate the prelude. A failure is fatal and says which half failed: a
/// missing document does not degrade, it throws somewhere unrelated the first
/// time an app calls createElement, and by then the message names the app
/// rather than the runtime.
/// The app's code has run: the DOM shim moves the document to 'interactive' and
/// then 'complete', firing DOMContentLoaded and load. A packed entry that
/// resolves later calls the same hook; it fires once. Harmless without a shim.
void announceDocumentLoaded(const std::shared_ptr<Runtime>& runtime) {
  const EvalResult result = runtime->evaluateSource(
      "typeof __screenkitDocumentLoaded === 'function' && __screenkitDocumentLoaded();",
      "screenkit://document-loaded");
  if (!result.ok) log(LogLevel::Error, kTag, "document load events failed: " + result.error);
}

bool evaluateDomShim(const std::shared_ptr<Runtime>& runtime, const std::string& path, std::string& error) {
  if (path.empty()) {
    error =
        "the DOM shim prelude (dom-shim.hbc) is not beside this binary. It is built from "
        "runtime/js/dom-shim.js by the screenkit-dom-shim target; a host without it cannot "
        "give an app a document.";
    return false;
  }

  const EvalResult result = runtime->evaluateBundle(path);
  if (!result.ok) {
    error = "the DOM shim prelude failed to evaluate (" + path + "): " + result.error;
    return false;
  }

  log(LogLevel::Log, kTag, "DOM shim ready: " + result.value);
  return true;
}

// ---------------------------------------------------------------------------
// .skpkg packages
//
// A package is a directory: manifest.json, the entry bytecode it names, and the
// app's assets. The manifest is read and gated before anything from the package
// is evaluated -- a bytecode version this engine does not speak, or a package
// built for a newer runtime, is a logged error and a failed launch rather than a
// fault somewhere inside the VM. The package directory then becomes the asset
// root, so HostIO's confinement applies to it unchanged.
//
// A plain .hbc or .js path still runs as it always has: its directory is the
// asset root and there is no manifest to gate on.
// ---------------------------------------------------------------------------

/// The package gate itself lives in core (`bundle/Package.h`): an `<iframe>`
/// instance gates the package its `src` names with exactly the same code and the
/// same messages, and one gate is what keeps the host rows meaningful for both.
bool resolveLaunch(const std::shared_ptr<Runtime>& runtime, const std::string& path, LaunchTarget& target,
                   std::string& error) {
  bundle::LaunchTarget resolved;
  if (!bundle::resolveLaunch(runtime, path, kTag, resolved, error)) return false;
  target.bundle = std::move(resolved.bundle);
  target.assetRoot = std::move(resolved.assetRoot);
  target.package = resolved.package;
  return true;
}

/// True once the app has declared a failure fatal through
/// `__screenkit.reportFailure` -- a package whose entry module rejected, say.
/// Every run mode checks it once per loop iteration: headless and --window exit
/// kBundleFailed (65), and tvOS ends the app with SDL_APP_FAILURE, which SDL's
/// UIKit main turns into process status 1. A failed app may never go idle, so
/// waiting for idle would hang a CI run or leave a blank screen up. Logs the
/// failure as it says so.
bool appFailed(const std::shared_ptr<Runtime>& runtime) {
  const std::optional<std::string> failure = runtime->failure();
  if (!failure) return false;
  log(LogLevel::Error, kTag,
      "the app reported a fatal failure; exiting with " + std::to_string(kBundleFailed) +
          ": " + *failure);
  return true;
}

/// The headless path: run a bundle, wait for what it scheduled, exit. No window
/// and no GL -- this is what the test suite drives, and it is what makes
/// `screenkit-host <bundle>` usable as a CI assertion.
int runBundle(const std::string& path) {
  RuntimeConfig config;
  config.name = kTag;

  auto runtime = Runtime::create(config);
  if (!runtime) return kRuntimeFailed;

  // A package is gated before anything in it runs, and is its own asset root.
  // A plain bundle keeps the M3 behaviour: no root, since there is no prelude to
  // read assets through.
  LaunchTarget target;
  std::string error;
  if (!resolveLaunch(runtime, path, target, error)) {
    log(LogLevel::Error, kTag, error);
    runtime->shutdown();
    return kBundleFailed;
  }
  if (target.package && !setAssetRoot(runtime, target.assetRoot, error)) {
    log(LogLevel::Error, kTag, error);
    runtime->shutdown();
    return kRuntimeFailed;
  }

  log(LogLevel::Log, kTag, engine::description() + ", loading " + target.bundle);

  const EvalResult result = runtime->evaluateBundle(target.bundle);
  bool failed = !result.ok;
  if (failed) {
    log(LogLevel::Error, kTag, result.error);
  } else {
    // Evaluating a bundle only runs its synchronous body. Anything it scheduled
    // -- timers, promise continuations -- still has to be served, so a host that
    // exits here reports success for a bundle whose work never ran. Wait for the
    // runtime's own idle(), bounded so a runaway setInterval cannot hang a CLI,
    // and stop at once if the app reports a failure along the way.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!(failed = appFailed(runtime)) && !runtime->idle() &&
           std::chrono::steady_clock::now() < deadline) {
      runtime->tickFrame(SDL_GetTicksNS() / 1.0e6);
      SDL_Delay(2);
    }
    // The last task before idle can be the one that reports.
    if (!failed) failed = appFailed(runtime);
    if (!failed && !runtime->idle()) {
      log(LogLevel::Warn, kTag,
          "bundle still had work pending after 30s; exiting anyway");
    }
  }
  runtime->shutdown();
  return failed ? kBundleFailed : kOk;
}
namespace {

struct WindowState {
  std::shared_ptr<Runtime> runtime;
  // The `<iframe>` instances this page has embedded (screenkit/Instance.h).
  // Reachable from here because the loop ticks them, while the binding that
  // makes them lives on the JS thread. Every one is terminated before the host
  // returns.
  std::shared_ptr<InstanceRegistry> instances;
  // Remote, keyboard and gamepad input. Destroyed before the runtime shuts
  // down, so no key event is queued to a JS thread that is going away.
  std::unique_ptr<InputRouter> input;
  // Window size changes, as `resize` at `window`. Destroyed with `input`.
  std::unique_ptr<ViewportEvents> viewport;
  SDL_Window* window = nullptr;
  int exitCode = kOk;
};

/// How close to the deadline still counts as "the frame is due", in
/// milliseconds. SDL_WaitEventTimeout takes whole milliseconds, so without a
/// little slack the last fraction of an interval is spent waking, finding the
/// frame not quite due, and waiting again.
constexpr double kFrameDueSlackMs = 0.25;

/// The window's display refresh rate, as a frame interval. SDL reports 0 when it
/// does not know one -- some Wayland compositors, and every headless display.
double displayFrameIntervalMs(SDL_Window* window) {
  if (window == nullptr) return frameIntervalMsForRefreshRate(0.0);
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
  return frameIntervalMsForRefreshRate(mode == nullptr ? 0.0
                                                       : static_cast<double>(mode->refresh_rate));
}

/// Window + the platform's drawable + GL surface + `gl` global, in that order.
bool openWindowAndGraphics(WindowState& state, const WindowPlatform& platform, const WindowOptions& options,
                           std::string& error, std::shared_ptr<std::atomic<bool>> painted) {
  // A fixed size does not change the window: fullscreen stays the display's own
  // mode, and the GL surface scales the app's frame into it (GlSurface.h).
  SDL_WindowFlags flags = platform.flags;
  if (options.fullscreen) {
    flags |= SDL_WINDOW_FULLSCREEN;
  } else if (!options.fixedSize) {
    flags |= SDL_WINDOW_RESIZABLE;
  }
  if (platform.beforeWindow) platform.beforeWindow();
#if defined(__linux__) && !defined(__ANDROID__)
  // SDL_WINDOW_TRANSPARENT, so a <video>'s plane beneath this window can be
  // seen: without it SDL declares the whole surface opaque on every configure
  // (SDL_waylandwindow.c, SetSurfaceOpaqueRegion), overwriting the region the
  // plane cuts open, and the compositor skips the subsurface under it -- every
  // picture decoded and none shown. Wayland only, and asked after SDL_Init so
  // the driver is known: X11 has no video plane at all, and there the flag
  // would only make a compositing window manager blend the desktop through
  // wherever the page cleared transparent.
  const char* videoDriver = SDL_GetCurrentVideoDriver();
  if (videoDriver != nullptr && std::strcmp(videoDriver, "wayland") == 0) flags |= SDL_WINDOW_TRANSPARENT;
#endif
  state.window = SDL_CreateWindow(options.title, options.width, options.height, flags);
  if (state.window == nullptr) {
    error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }
  void* drawable = platform.attach(state.window, error);
  if (drawable == nullptr) return false;

  int pixelWidth = options.width;
  int pixelHeight = options.height;
  SDL_GetWindowSizeInPixels(state.window, &pixelWidth, &pixelHeight);
  const int fixedWidth = options.fixedSize ? options.width : 0;
  const int fixedHeight = options.fixedSize ? options.height : 0;
  if (!startGraphics(state.runtime, drawable, pixelWidth, pixelHeight, fixedWidth, fixedHeight, error, nullptr,
                     std::move(painted))) {
    return false;
  }
  // Where a <video>'s plane goes: beneath this window's drawable, in the same
  // pixels the app draws in (media/MediaPlayer.h).
  media::VideoHost host;
  host.window = state.window;
  host.view = platform.videoView ? platform.videoView() : nullptr;
  host.drawableWidth = fixedWidth;
  host.drawableHeight = fixedHeight;
  // The display's mode, read here because SDL asks for these on the main thread
  // and the player that wants them (a ladder's ceiling, MediaPlayerLinux) runs
  // on its own.
  SDL_DisplayID display = SDL_GetDisplayForWindow(state.window);
  if (display == 0) display = SDL_GetPrimaryDisplay();
  if (const SDL_DisplayMode* mode = display != 0 ? SDL_GetCurrentDisplayMode(display) : nullptr) {
    host.displayWidth = mode->w;
    host.displayHeight = mode->h;
  }
  media::setVideoHost(host);
#if defined(__linux__) && !defined(__ANDROID__)
  // The window is transparent so a video plane below it shows (linux/HostMain.cpp);
  // this puts back the opaque region SDL would otherwise have declared, until a
  // plane cuts it open.
  media::wl::declareWindowOpaque(state.window);
#endif
  return true;
}

void closeWindow(WindowState& state, const WindowPlatform& platform) {
  // No plane is placed in a window that is going away. The players went with
  // the runtime, which has shut down by now.
  media::clearVideoHost();
  // Order matters: the GL surface was created over the drawable, and the
  // runtime destroys it on the JS thread during shutdown(). Releasing the
  // drawable before that would leave the context holding a dead one.
  if (platform.detach) platform.detach();
  if (state.window != nullptr) {
    SDL_DestroyWindow(state.window);
    state.window = nullptr;
  }
}

}  // namespace

bool parseSize(const char* text, int& width, int& height) {
  unsigned w = 0;
  unsigned h = 0;
  char trailing = 0;
  if (std::sscanf(text, "%ux%u%c", &w, &h, &trailing) != 2 || w == 0 || h == 0 || w > 16384 || h > 16384) {
    return false;
  }
  width = static_cast<int>(w);
  height = static_cast<int>(h);
  return true;
}

/// The windowed path: same frame source as tvOS -- one tick per iteration of the
/// loop, servicing requestAnimationFrame -- with SDL's event pump on the main
/// thread, where AppKit requires it and Wayland expects it.
int runWindowed(const std::string& path, const WindowOptions& options, const WindowPlatform& platform) {
  InputRouter::configureHints();
  if (!SDL_Init(SDL_INIT_VIDEO | InputRouter::requiredSubsystems())) {
    // No video means no window server: the same "cannot show a window" as a
    // failed window or GL context, and the status the test rows skip on.
    log(LogLevel::Error, kTag,
        std::string("SDL_Init failed: ") + SDL_GetError());
    return kGraphicsFailed;
  }

  WindowState state;
  RuntimeConfig config;
  config.name = kTag;
  // This page may embed `<iframe>` instances; each of those is given no registry
  // of its own, which is what makes nesting one level deep.
  state.instances = makeInstanceRegistry();
  setInstanceEnvironment(*state.instances, options.domShimPath);
  config.instances = state.instances;
  state.runtime = Runtime::create(config);
  if (!state.runtime) return kRuntimeFailed;
  state.input = std::make_unique<InputRouter>(state.runtime);
  state.viewport = std::make_unique<ViewportEvents>(state.runtime);
  // Input and `resize` follow focus: the focused browsing context is the one
  // that hears from the remote and from the window.
  state.input->setFocusTarget(focusTarget(*state.instances));

  // The package gate, before a window opens and before anything is evaluated.
  LaunchTarget target;
  std::string error;
  if (!resolveLaunch(state.runtime, path, target, error)) {
    log(LogLevel::Error, kTag, error);
    state.runtime->shutdown();
    SDL_Quit();
    return kBundleFailed;
  }

  // Set after every present to whether the frame swapped; see the frame
  // deadline in the loop below for what reads it.
  auto painted = std::make_shared<std::atomic<bool>>(false);
  if (!openWindowAndGraphics(state, platform, options, error, painted)) {
    log(LogLevel::Error, kTag, error);
    state.runtime->shutdown();
    closeWindow(state, platform);
    return kGraphicsFailed;
  }

  // After the graphics bootstrap, before the app bundle. Fatal on failure: a
  // runtime with `gl` but no `document` is one an ordinary web build cannot use.
  if (!evaluateDomShim(state.runtime, options.domShimPath, error)) {
    log(LogLevel::Error, kTag, error);
    state.runtime->shutdown();
    closeWindow(state, platform);
    return kRuntimeFailed;
  }
  if (!setAssetRoot(state.runtime, target.assetRoot, error)) {
    log(LogLevel::Error, kTag, error);
    state.runtime->shutdown();
    closeWindow(state, platform);
    return kRuntimeFailed;
  }

  log(LogLevel::Log, kTag, engine::description() + ", loading " + target.bundle);

  const EvalResult result = state.runtime->evaluateBundle(target.bundle);
  if (!result.ok) {
    log(LogLevel::Error, kTag, result.error);
    state.exitCode = kBundleFailed;
  } else {
    announceDocumentLoaded(state.runtime);
  }

  // Runs until the window closes (or the process is asked to quit). An app with
  // nothing scheduled is not finished -- a TV UI that has drawn its screen waits
  // for a key -- so idle is not an exit here, unlike the headless path. While
  // idle the loop sleeps in the event queue instead of spinning; input, a resize
  // and the runtime's own queued work all wake it.
  constexpr Sint32 kIdleWaitMs = 16;
  // `requestAnimationFrame` belongs to the display, not to how fast this loop
  // can spin. Every tickFrame runs the frame's rAF callbacks, and for a
  // Lightning app one of those is a whole scene-graph update -- so a loop that
  // ticks faster than the screen refreshes does that work several times over to
  // show one frame. Measured on a Pi 3 before this deadline existed: ~800 ticks
  // a second against a 60 Hz panel, ~30% of a core with the UI holding still.
  //
  // vsync does not pace it. `swap()` blocks on the refresh, but presentFrame
  // only swaps a frame that painted (gfx/VendoredWebGL.h), and a UI that is
  // holding still paints rarely -- so on exactly the frames where the spinning
  // is pure waste there is nothing to block on.
  // SCREENKIT_FRAME_PACING=0 turns the deadline off, leaving the loop ticking as
  // fast as it can as it did before. An escape hatch for measuring the pacing
  // against itself on one binary, and for a display whose reported rate lies.
  const char* pacingEnv = std::getenv("SCREENKIT_FRAME_PACING");
  const bool pacingOff = pacingEnv != nullptr && pacingEnv[0] == '0' && pacingEnv[1] == '\0';
  const double frameIntervalMs = pacingOff ? 0.0 : displayFrameIntervalMs(state.window);
  if (pacingOff) log(LogLevel::Log, kTag, "frame pacing off (SCREENKIT_FRAME_PACING=0)");
  double nextFrameMs = static_cast<double>(SDL_GetTicksNS()) / 1.0e6;
  bool running = result.ok;
  while (running) {
    SDL_Event event;
    // A frame that painted is already paced by the display, so it keeps the loop
    // exactly as it was -- poll, tick, yield a millisecond. Only a frame that
    // painted nothing is held to the deadline. The distinction matters both ways:
    // waiting on a painting frame pays the swap's vsync block *and* a deadline,
    // and SDL_WaitEventTimeout's millisecond is a scheduler's millisecond, not
    // SDL_Delay's, so spending one per frame is not free either.
    // `painted` is set by whichever frame actually reached the screen, and with
    // an `<iframe>` on the page that includes a frame only an instance drew --
    // a focused game while the launcher is frozen. So pacing already follows
    // the context that is running, without the loop having to know which one
    // that is.
    const bool pacing =
        !pacingOff && !state.runtime->idle() && !painted->load(std::memory_order_relaxed);
    bool have;
    if (pacing) {
      // Sleep in the event queue until the frame is due, rather than spinning:
      // input, a resize and the runtime's own queued work all wake it early.
      // Rounded up, so a wait that truncates to zero does not become a spin.
      const double remaining = std::ceil(nextFrameMs - static_cast<double>(SDL_GetTicksNS()) / 1.0e6);
      const Sint32 waitMs = remaining > 0.0 ? static_cast<Sint32>(remaining) : 0;
      have = waitMs > 0 ? SDL_WaitEventTimeout(&event, waitMs) : SDL_PollEvent(&event);
    } else if (state.runtime->idle()) {
      have = SDL_WaitEventTimeout(&event, kIdleWaitMs);
    } else {
      have = SDL_PollEvent(&event);
    }
    for (; have; have = SDL_PollEvent(&event)) {
      if (reclaimRuntimeEvent(event)) continue;
      if (options.quitOnGuide && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
          event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
        running = false;
        continue;
      }
      if (state.input->handleEvent(event)) continue;
      if (state.viewport->handleEvent(event)) continue;
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        running = false;
      }
    }
    if (!running) break;

    // A failure the app reported ends the run, whatever it still has scheduled.
    if (appFailed(state.runtime)) {
      state.exitCode = kBundleFailed;
      break;
    }
    const Uint64 tickNs = SDL_GetTicksNS();
    const double tickMs = static_cast<double>(tickNs) / 1.0e6;
    // Woken by an event with the frame not due yet: the event is handled (it
    // went through handleEvent above), but it does not buy an extra frame. An
    // idle runtime ticks anyway -- that is what services its timers.
    if (pacing && tickMs + kFrameDueSlackMs < nextFrameMs) continue;

    state.input->tick(tickNs);
    // Every live instance, then this page. A paused instance ignores the tick
    // itself (EventLoop::tickFrame), so a launcher with ten frozen tiles pays
    // for one loop over a vector and nothing else.
    for (const std::shared_ptr<Instance>& instance : liveInstances(*state.instances)) {
      instance->tickFrame(tickMs);
    }
    state.runtime->tickFrame(tickMs);

    const bool swapped = painted->load(std::memory_order_relaxed);
    nextFrameMs = nextFrameDeadlineMs(nextFrameMs, tickMs,
                                      static_cast<double>(SDL_GetTicksNS()) / 1.0e6,
                                      frameIntervalMs, swapped);
    // The yield this loop has always taken between painting frames: tickFrame
    // only posts to the JS thread, so without it the loop spins posting frames
    // to a thread still finishing the last one.
    if (swapped || pacingOff) SDL_Delay(1);
  }
  // A quit or close polled in the same iteration the report landed breaks the
  // loop before the check above; the failure still decides the exit status.
  if (state.exitCode == kOk && appFailed(state.runtime)) state.exitCode = kBundleFailed;

  state.input.reset();
  state.viewport.reset();
  // Every instance is terminated -- thread joined, heap and GL freed -- before
  // the host returns, and before this page's own runtime goes: a child's context
  // is in this one's share group.
  terminateInstances(*state.instances);
  state.runtime->shutdown();
  closeWindow(state, platform);
  SDL_Quit();
  return state.exitCode;
}

}  // namespace screenkit::host
