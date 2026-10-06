// Copyright (c) ScreenKit contributors. MIT.
//
// The platform shell's portable half: the graphics bootstrap, the DOM shim
// prelude, the .skpkg gate, and the headless and windowed run loops. Each
// platform's HostMain supplies what differs -- where the prelude and storage
// live, and how a window becomes something GlSurface can draw into -- and its
// own main().
#pragma once

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <string>

#include <SDL3/SDL_video.h>

#include <screenkit/Runtime.h>

namespace screenkit::gfx {
class GlSurface;
}

namespace screenkit::host {

enum ExitCode : int {
  kOk = 0,
  kUsage = 64,          // EX_USAGE
  kBundleFailed = 65,   // EX_DATAERR -- the bundle or package was refused, or threw
  kRuntimeFailed = 70,  // EX_SOFTWARE -- the runtime would not start
  kGraphicsFailed = 71, // EX_OSERR -- no GL context
};

inline constexpr const char* kTag = "screenkit-host";

/// What to run and where its assets live, whichever form the path took.
struct LaunchTarget {
  std::string bundle;     // the file evaluateBundle runs
  std::string assetRoot;  // the directory asset reads are confined to
  bool package = false;
};

/// Create the GL surface on the JS thread over `nativeDrawable` (GlSurface.h
/// says what that is per platform), install `gl`, and hang the present off the
/// end of the frame. Blocks until the JS thread has answered, so a failure is
/// reported before a bundle that needs `gl` runs.
///
/// `surface`, when given, is pointed at the surface without owning it -- for a
/// host that has to reach it later on the JS thread (Android gives up and
/// retakes the window's drawable around the background, GlSurface::suspend).
/// `painted`, when given, is set after every present to whether the frame
/// actually swapped. That is how a loop knows the display has already paced the
/// frame: a swap blocks on the refresh, so a loop that also waits for a frame
/// deadline would pace it twice and halve the rate (runWindowed).
bool startGraphics(const std::shared_ptr<Runtime>& runtime, void* nativeDrawable, int width, int height,
                   int fixedWidth, int fixedHeight, std::string& error,
                   std::weak_ptr<gfx::GlSurface>* surface = nullptr,
                   std::shared_ptr<std::atomic<bool>> painted = nullptr);

/// Evaluate the DOM shim prelude at `path` (empty when the platform found none).
/// Fatal on failure: a runtime with `gl` but no `document` is one an ordinary web
/// build cannot use.
bool evaluateDomShim(const std::shared_ptr<Runtime>& runtime, const std::string& path, std::string& error);

/// Confine asset reads to `dir`. After the prelude and before the app bundle: the
/// root is write-once, so the host has to be the one that sets it.
bool setAssetRoot(const std::shared_ptr<Runtime>& runtime, const std::string& dir, std::string& error);

/// The app's code has run: the DOM shim fires DOMContentLoaded and load.
void announceDocumentLoaded(const std::shared_ptr<Runtime>& runtime);

/// A directory is a package, opened and gated; anything else is a plain bundle
/// file run from the directory it sits in.
bool resolveLaunch(const std::shared_ptr<Runtime>& runtime, const std::string& path, LaunchTarget& target,
                   std::string& error);

/// True once the app has declared a failure fatal through
/// `__screenkit.reportFailure`. Logs the failure as it says so.
bool appFailed(const std::shared_ptr<Runtime>& runtime);

/// The headless path: run a bundle, wait for what it scheduled, exit. No window
/// and no GL -- what the test suite drives.
int runBundle(const std::string& path);

/// How one platform turns an SDL window into a GL drawable.
struct WindowPlatform {
  /// Flags every window on this platform needs (SDL_WINDOW_OPENGL on Linux).
  SDL_WindowFlags flags = 0;
  /// Runs before SDL_CreateWindow.
  std::function<void()> beforeWindow;
  /// The drawable GlSurface takes for this window, or null with `error` set.
  std::function<void*(SDL_Window* window, std::string& error)> attach;
  /// Releases what `attach` made, after the runtime -- and the GL surface on
  /// it -- has shut down.
  std::function<void()> detach;
  /// The native view video planes go beneath (media::VideoHost::view): the
  /// metal view on Apple. Unset elsewhere, where the SDL window is enough.
  std::function<void*()> videoView;
};

struct WindowOptions {
  const char* title = "ScreenKit";
  int width = 1280;
  int height = 720;
  bool fullscreen = false;
  /// The drawable is exactly `width` x `height`, whatever the window: a window
  /// that does not resize, or -- on Linux -- a frame drawn at that size and
  /// scaled into the window, centred with its aspect ratio kept. A TV showing a
  /// 640x480 game at 1080p draws the game's pixels at a sixth of the cost.
  bool fixedSize = false;
  /// Quit when a gamepad's Guide (home) button is pressed: how a TV box with no
  /// keyboard leaves a fullscreen app -- back to Batocera's menu, say.
  bool quitOnGuide = false;
  std::string domShimPath;
};

/// "640x480" as a width and a height, each 1 to 16384. False for anything else.
bool parseSize(const char* text, int& width, int& height);

/// The windowed path: one frame tick per loop iteration, SDL's event pump on the
/// calling (main) thread, until the window closes or the app reports a failure.
int runWindowed(const std::string& path, const WindowOptions& options, const WindowPlatform& platform);

}  // namespace screenkit::host
