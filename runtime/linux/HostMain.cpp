// Copyright (c) ScreenKit contributors. MIT.
//
// screenkit-host on Linux: the platform half of the shell (host/Host.h has the
// rest). SDL opens the window -- Wayland, X11 or KMS/DRM, whichever the system
// runs -- and creates the GLES context on it, so the drawable GlSurface takes is
// the SDL window itself.
//
//   screenkit-host <app.skpkg|bundle.hbc|bundle.js>                headless
//   screenkit-host --window <app.skpkg|bundle.hbc|bundle.js>       a 1280x720 window
//   screenkit-host --fullscreen <app.skpkg|bundle.hbc|bundle.js>   the whole display -- a TV
//   --size 640x480, with either: a drawable of exactly that size (host/Host.h, fixedSize)
//
// The prelude (dom-shim.hbc) sits beside the executable, and libhermesvm.so in
// ../lib, which is how the Batocera port lays them out (tools/batocera).
#include <unistd.h>

#include <climits>  // PATH_MAX
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL_video.h>

#include <screenkit/Runtime.h>

#include "../core/src/gfx/GlSurface.h"
#include "../host/Host.h"
#include "engine/Engine.h"

using namespace screenkit::host;

namespace {

std::string executableDirectory() {
  char buffer[PATH_MAX];
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::string();
  std::string path(buffer, static_cast<std::size_t>(length));
  return path.substr(0, path.find_last_of('/'));
}

std::string domShimPath() {
  // dom-shim.hbc for Hermes, dom-shim.js for SpiderMonkey (engine/Engine.h) --
  // or, for SpiderMonkey, dom-shim.stencil when screenkit-smc compiled one for
  // exactly this engine build (tools/batocera/pi.sh does, beside the host).
  const std::string stencil = executableDirectory() + "/dom-shim.stencil";
  if (screenkit::engine::canLoadPrecompiled(stencil)) return stencil;
  const std::string path = executableDirectory() + "/" + screenkit::engine::domShimFileName();
  return ::access(path.c_str(), R_OK) == 0 ? path : std::string();
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto usage = [] {
    std::fprintf(stderr,
                 "usage: screenkit-host <app.skpkg|bundle.hbc|bundle.js>\n"
                 "       screenkit-host --window [--size WxH] <app.skpkg|bundle.hbc|bundle.js>\n"
                 "       screenkit-host --fullscreen [--size WxH] <app.skpkg|bundle.hbc|bundle.js>\n"
                 "       screenkit-host --bytecode-version\n");
    return kUsage;
  };
  if (argc == 2 && std::strcmp(argv[1], "--bytecode-version") == 0) {
    std::printf("%u\n", screenkit::Runtime::hermesBytecodeVersion());
    return kOk;
  }

  WindowOptions options;
  bool window = false;
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--window") == 0) {
      window = true;
    } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
      window = options.fullscreen = true;
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc &&
               parseSize(argv[i + 1], options.width, options.height)) {
      options.fixedSize = true;
      ++i;
    } else if (argv[i][0] != '-' && path == nullptr) {
      path = argv[i];
    } else {
      return usage();
    }
  }
  if (path == nullptr || (options.fixedSize && !window)) return usage();
  if (!window) return runBundle(path);

  options.quitOnGuide = true;
  options.domShimPath = domShimPath();
  WindowPlatform platform;
  platform.flags = SDL_WINDOW_OPENGL;
  platform.beforeWindow = [] { screenkit::gfx::GlSurface::prepareWindowAttributes(); };
  platform.attach = [](SDL_Window* sdlWindow, std::string&) -> void* { return sdlWindow; };
  return runWindowed(path, options, platform);
}
