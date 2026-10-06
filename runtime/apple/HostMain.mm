// Copyright (c) ScreenKit contributors. MIT.
//
// screenkit-host on Apple platforms: the platform half of the shell. The
// portable half -- the graphics bootstrap, the DOM shim, the .skpkg gate and the
// run loops -- is host/Host.cpp; this file supplies what only Apple has.
//
// SDL3 gives us the window and the lifecycle, ANGLE gives us the GLES context,
// and a CAMetalLayer is the seam. SDL deliberately does not create the context:
// its UIKit backend has no EGL path at all and SDL_EGL reaches EGL by dlopening
// libEGL.dylib, while our ANGLE is a static archive -- see GlSurface.h.
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <screenkit/Input.h>
#include <screenkit/Instance.h>
#include <screenkit/Runtime.h>
#include <screenkit/Viewport.h>

#include "../host/Host.h"
#include "media/MediaPlayer.h"
#include "PlatformLog.h"

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif

#if TARGET_OS_TV
#import <UIKit/UIKit.h>

#include "RemotePresses.h"
#endif

using namespace screenkit::host;

namespace {

/// Where the prelude lives. NSBundle answers for both shapes this host takes:
/// for the tvOS .app it is a bundled resource, and for the macOS command-line
/// tool the main bundle's resource path is simply the directory holding the
/// executable -- which is where CMake puts dom-shim.hbc.
std::string domShimPath() {
  NSString* bundled = [[NSBundle mainBundle] pathForResource:@"dom-shim" ofType:@"hbc"];
  if (bundled != nil) return std::string(bundled.UTF8String);

  // Fallback for a relocated binary: next to the executable itself. Cheap, and
  // it keeps "the host cannot find its own prelude" from depending on how
  // NSBundle classifies the process.
  NSString* executable = [[NSBundle mainBundle] executablePath];
  if (executable != nil) {
    NSString* beside =
        [[executable stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"dom-shim.hbc"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:beside]) {
      return std::string(beside.UTF8String);
    }
  }
  return std::string();
}

/// A window's CAMetalLayer, the EGLNativeWindowType ANGLE wants -- which is the
/// whole reason SDL and ANGLE can share a window without SDL having an EGL
/// backend. `SDL_Metal_CreateView` returns a view whose backing layer is one.
class MetalDrawable {
 public:
  void* attach(SDL_Window* window, std::string& error) {
    view_ = SDL_Metal_CreateView(window);
    if (view_ == nullptr) {
      error = std::string("SDL_Metal_CreateView failed: ") + SDL_GetError();
      return nullptr;
    }
    void* layer = SDL_Metal_GetLayer(view_);
    if (layer == nullptr) error = std::string("SDL_Metal_GetLayer returned no CAMetalLayer: ") + SDL_GetError();
    return layer;
  }

  void detach() {
    if (view_ != nullptr) SDL_Metal_DestroyView(view_);
    view_ = nullptr;
  }

  /// The metal view itself -- an NSView on macOS, a UIView on tvOS -- which is
  /// what a <video>'s plane goes beneath (media::VideoHost::view).
  void* view() const { return view_; }

 private:
  SDL_MetalView view_ = nullptr;
};

#if TARGET_OS_TV
/// On tvOS there is no argv worth reading, so what runs travels inside the app.
std::string bundledLaunchPath() {
  // app.skpkg is a real application, embedded with SCREENKIT_APP_PKG; it wins
  // over every fixture when present. Otherwise triangle.hbc, the M4 bundle, which
  // exercises the whole stack -- console, timers, rAF and GL -- with timers.hbc
  // and hello.hbc as the fallbacks.
  NSString* package = [[NSBundle mainBundle] pathForResource:@"app" ofType:@"skpkg"];
  if (package != nil) return std::string(package.UTF8String);
  for (NSString* name in @[ @"triangle", @"timers", @"hello" ]) {
    NSString* path = [[NSBundle mainBundle] pathForResource:name ofType:@"hbc"];
    if (path != nil) return std::string(path.UTF8String);
  }
  return std::string();
}
#endif

}  // namespace

#if TARGET_OS_TV

// SDL owns the app lifecycle here. That is not a stylistic choice: SDL's event
// subsystem refuses to initialise on UIKit unless SDL set the app up itself
// ("did you include SDL_main.h in the file containing your main() function?"),
// and the work queue is built on SDL's event queue. SDL_AppIterate doubles as
// the frame tick, so requestAnimationFrame has a real clock rather than an
// injected one.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

namespace {

struct HostState {
  std::shared_ptr<screenkit::Runtime> runtime;
  // The `<iframe>` instances this page has embedded (screenkit/Instance.h):
  // ticked with the page, and terminated before the app returns.
  std::shared_ptr<screenkit::InstanceRegistry> instances;
  // Remote, keyboard and gamepad input. Destroyed before the runtime shuts
  // down, so no key event is queued to a JS thread that is going away.
  std::unique_ptr<screenkit::InputRouter> input;
  // Window size changes, as `resize` at `window`. Destroyed with `input`.
  std::unique_ptr<screenkit::ViewportEvents> viewport;
  SDL_Window* window = nullptr;
  MetalDrawable drawable;
  int exitCode = kOk;
  // The app bundle evaluates on this thread, not in SDL_AppInit: evaluation
  // blocks its caller until the JS thread answers, and a slow bundle blocking the
  // main thread before the first frame is what the launch watchdog kills. The
  // result is read by SDL_AppIterate once `bundleDone` is set.
  std::thread bundleThread;
  std::atomic<bool> bundleDone{false};
  screenkit::EvalResult bundleResult;
};

/// Window + Metal layer + GL surface + `gl` global, in that order.
bool openWindowAndGraphics(HostState& state, std::string& error) {
  // The window is full-screen on tvOS whatever is asked for; the numbers only
  // set the logical size SDL reports before the real one arrives.
  state.window = SDL_CreateWindow("ScreenKit", 1920, 1080, 0);
  if (state.window == nullptr) {
    error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }
  void* layer = state.drawable.attach(state.window, error);
  if (layer == nullptr) return false;
  int pixelWidth = 1920;
  int pixelHeight = 1080;
  SDL_GetWindowSizeInPixels(state.window, &pixelWidth, &pixelHeight);
  if (!startGraphics(state.runtime, layer, pixelWidth, pixelHeight, 0, 0, error)) return false;
  // Where a <video>'s plane goes: beneath the metal view, in its pixels.
  screenkit::media::VideoHost host;
  host.window = state.window;
  host.view = state.drawable.view();
  screenkit::media::setVideoHost(host);
  return true;
}

}  // namespace
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  screenkit::installPlatformLogSink();

  auto state = std::make_unique<HostState>();

  // SDL_INIT_VIDEO implies SDL_INIT_EVENTS, and tvOS terminates an app that
  // never presents a window. SDL's window is a real UIWindow, so this also
  // satisfies the watchdog that the old UIKit delegate was handling by hand.
  // The Siri Remote as arrow / select / menu keys has to be asked for before
  // SDL initialises; gamepads need their own subsystem.
  screenkit::InputRouter::configureHints();
  if (!SDL_Init(SDL_INIT_VIDEO | screenkit::InputRouter::requiredSubsystems())) {
    screenkit::log(screenkit::LogLevel::Error, kTag,
                   std::string("SDL_Init failed: ") + SDL_GetError());
    return SDL_APP_FAILURE;
  }
  screenkit::installRemotePressForwarding();

  const std::string path = bundledLaunchPath();
  if (path.empty()) {
    screenkit::log(screenkit::LogLevel::Error, kTag, "no app.skpkg and no .hbc in the app bundle");
    return SDL_APP_FAILURE;
  }

  screenkit::RuntimeConfig config;
  config.name = kTag;
  // This page may embed `<iframe>` instances; each of those is given no registry
  // of its own, which is what makes nesting one level deep.
  state->instances = screenkit::makeInstanceRegistry();
  screenkit::setInstanceEnvironment(*state->instances, domShimPath());
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
  LaunchTarget target;
  std::string error;
  if (!resolveLaunch(state->runtime, path, target, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  if (!openWindowAndGraphics(*state, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  // After the graphics bootstrap, before the app bundle. Fatal on failure.
  if (!evaluateDomShim(state->runtime, domShimPath(), error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }
  if (!setAssetRoot(state->runtime, target.assetRoot, error)) {
    screenkit::log(screenkit::LogLevel::Error, kTag, error);
    return SDL_APP_FAILURE;
  }

  screenkit::log(screenkit::LogLevel::Log, kTag,
                 "hermes bytecode version " +
                     std::to_string(screenkit::Runtime::hermesBytecodeVersion()) +
                     ", loading " + target.bundle);

  HostState* launched = state.get();
  launched->bundleThread = std::thread([launched, bundle = target.bundle] {
    launched->bundleResult = launched->runtime->evaluateBundle(bundle);
    if (launched->bundleResult.ok) announceDocumentLoaded(launched->runtime);
    launched->bundleDone.store(true);
  });

  *appstate = state.release();
  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
  auto* state = static_cast<HostState*>(appstate);
  if (state == nullptr || !state->runtime) return SDL_APP_FAILURE;

  // A bundle that threw while evaluating, or a failure the app reported later,
  // ends the app, whatever it still has scheduled. Read once, when it lands.
  if (state->bundleDone.load() && state->bundleThread.joinable()) {
    state->bundleThread.join();
    if (!state->bundleResult.ok) {
      screenkit::log(screenkit::LogLevel::Error, kTag, state->bundleResult.error);
      state->exitCode = kBundleFailed;
    }
  }
  if (state->exitCode != kOk) return SDL_APP_FAILURE;
  if (appFailed(state->runtime)) {
    state->exitCode = kBundleFailed;
    return SDL_APP_FAILURE;
  }

  // The frame tick that services requestAnimationFrame. The present happens on
  // the JS thread at the end of that frame -- see startGraphics.
  //
  // An app with nothing scheduled is not finished: a TV UI that has drawn its
  // screen sits idle until the remote is pressed. It runs until the system ends
  // it, as a tvOS app must -- it never quits on its own.
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
  // thread's work queue is a private range of it -- so a task can be handed to
  // the main thread before the runtime that owns it sees it. Give it back
  // before doing anything else, or the work is silently dropped.
  if (screenkit::reclaimRuntimeEvent(*event)) return SDL_APP_CONTINUE;
  if (state != nullptr && state->input && state->input->handleEvent(*event)) return SDL_APP_CONTINUE;
  if (state != nullptr && state->viewport && state->viewport->handleEvent(*event)) return SDL_APP_CONTINUE;
  if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
  (void)result;
  auto* state = static_cast<HostState*>(appstate);
  if (state == nullptr) return;
  // shutdown() joins the JS thread, so the EGL objects are gone by the time the
  // layer they were created over is destroyed.
  state->input.reset();
  state->viewport.reset();
  // Every instance is terminated -- thread joined, heap and GL freed -- before
  // this page's own runtime goes: a child's context is in this one's share group.
  if (state->instances) screenkit::terminateInstances(*state->instances);
  // shutdown() cancels an evaluation still in flight, which releases the
  // bundle thread.
  if (state->runtime) state->runtime->shutdown();
  if (state->bundleThread.joinable()) state->bundleThread.join();
  // No plane is placed in a view that is going away; the players went with the
  // runtime.
  screenkit::media::clearVideoHost();
  // The EGL objects are gone by now, so the layer they were created over can go.
  state->drawable.detach();
  if (state->window != nullptr) SDL_DestroyWindow(state->window);
  delete state;
}

#else  // macOS

namespace {

/// SCREENKIT_WINDOW_CAPTURE=<file.png>: save the window as the compositor shows
/// it -- the app's canvas *and* whatever is beneath it, a video plane included --
/// after SCREENKIT_WINDOW_CAPTURE_DELAY_MS (default 5000). SCREENKIT_CAPTURE
/// reads back the GL frame alone, which is not where video is. A process may
/// capture its own windows without the Screen Recording permission, so this
/// works where `screencapture` does not. CGWindowListCreateImage is reached
/// through dlsym: the macOS 15 SDK marks it obsolete in favour of
/// ScreenCaptureKit, which needs that permission for any window.
void scheduleWindowCapture(std::shared_ptr<SDL_Window*> window) {
  const char* path = std::getenv("SCREENKIT_WINDOW_CAPTURE");
  if (path == nullptr || *path == '\0') return;
  const char* delayText = std::getenv("SCREENKIT_WINDOW_CAPTURE_DELAY_MS");
  const long delayMs = delayText != nullptr ? std::strtol(delayText, nullptr, 10) : 5000;
  std::string file = path;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delayMs) * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
    using CreateImage = CGImageRef (*)(CGRect, uint32_t, uint32_t, uint32_t);
    auto create = reinterpret_cast<CreateImage>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
    NSWindow* ns = *window == nullptr ? nil
                                      : (__bridge NSWindow*)SDL_GetPointerProperty(
                                            SDL_GetWindowProperties(*window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    CGImageRef image = create != nullptr && ns != nil
                           ? create(CGRectNull, 8 /* IncludingWindow */, static_cast<uint32_t>(ns.windowNumber),
                                    1 /* BoundsIgnoreFraming */)
                           : nullptr;
    if (image == nullptr) {
      screenkit::log(screenkit::LogLevel::Error, kTag, "SCREENKIT_WINDOW_CAPTURE: could not capture the window");
      return;
    }
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease(image);
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const bool ok = [png writeToFile:[NSString stringWithUTF8String:file.c_str()] atomically:YES];
    screenkit::log(ok ? screenkit::LogLevel::Log : screenkit::LogLevel::Error, kTag,
                   std::string(ok ? "window captured: " : "could not write the window capture: ") + file);
  });
}

}  // namespace

int main(int argc, char* argv[]) {
  @autoreleasepool {
    screenkit::installPlatformLogSink();

    if (argc == 2 && std::strcmp(argv[1], "--bytecode-version") == 0) {
      std::printf("%u\n", screenkit::Runtime::hermesBytecodeVersion());
      return kOk;
    }
    // Headless by default: the CLI runs a bundle and exits, which is what the
    // test suite drives. A window is opt-in because opening one turns the
    // process into an app that has to be closed.
    const bool sized = argc == 5 && std::strcmp(argv[1], "--window") == 0 && std::strcmp(argv[2], "--size") == 0;
    if ((argc == 3 && std::strcmp(argv[1], "--window") == 0) || sized) {
      WindowOptions options;
      if (sized && !parseSize(argv[3], options.width, options.height)) {
        std::fprintf(stderr, "--size takes WxH, such as 640x480\n");
        return kUsage;
      }
      options.fixedSize = sized;
      options.domShimPath = domShimPath();
      auto drawable = std::make_shared<MetalDrawable>();
      auto window = std::make_shared<SDL_Window*>(nullptr);
      scheduleWindowCapture(window);
      WindowPlatform platform;
      platform.attach = [drawable, window](SDL_Window* sdlWindow, std::string& error) {
        *window = sdlWindow;
        return drawable->attach(sdlWindow, error);
      };
      platform.detach = [drawable, window] {
        *window = nullptr;
        drawable->detach();
      };
      platform.videoView = [drawable] { return drawable->view(); };
      return runWindowed(argv[argc - 1], options, platform);
    }
    if (argc != 2) {
      std::fprintf(stderr,
                   "usage: screenkit-host <app.skpkg|bundle.hbc|bundle.js>\n"
                   "       screenkit-host --window [--size WxH] <app.skpkg|bundle.hbc|bundle.js>\n"
                   "       screenkit-host --bytecode-version\n");
      return kUsage;
    }
    return runBundle(argv[1]);
  }
}

#endif
