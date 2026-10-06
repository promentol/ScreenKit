// Copyright (c) ScreenKit contributors. MIT.
//
// screenkit-smc: compile a script to a SpiderMonkey stencil ahead of time -- the
// parse and the bytecode a device would otherwise do at every launch.
//
//   screenkit-smc [--eager] [--url <name>] <in.js> <out.stencil>
//   screenkit-smc --build-id
//
// The stencil loads only in the SpiderMonkey build that wrote it (the runtime
// checks the build id it carries), so this links the same pinned libmozjs the
// device runs: tools/spidermonkey/smc.sh runs it in that library's container.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "spidermonkey/SpiderMonkeyRuntime.h"

int main(int argc, char** argv) {
  bool eager = false;
  std::string url, in, out;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--eager") eager = true;
    else if (arg == "--url" && i + 1 < argc) url = argv[++i];
    else if (arg == "--build-id") {
      std::printf("%s\n", screenkit::spidermonkey::engineVersion().c_str());
      return 0;
    } else if (in.empty()) in = arg;
    else if (out.empty()) out = arg;
    else {
      std::fprintf(stderr, "screenkit-smc: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (in.empty() || out.empty()) {
    std::fprintf(stderr, "usage: screenkit-smc [--eager] [--url <name>] <in.js> <out.stencil>\n");
    return 2;
  }
  std::ifstream file(in, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "screenkit-smc: cannot read %s\n", in.c_str());
    return 1;
  }
  const std::string source((std::istreambuf_iterator<char>(file)), {});
  try {
    const auto stencil = screenkit::spidermonkey::compileToStencil(source, url.empty() ? in : url, eager);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(stencil.data()),
                                               static_cast<std::streamsize>(stencil.size()));
    std::fprintf(stderr, "screenkit-smc: %s -> %s (%zu KB source, %zu KB stencil%s)\n", in.c_str(), out.c_str(),
                 source.size() / 1024, stencil.size() / 1024, eager ? ", eager" : "");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "screenkit-smc: %s: %s\n", in.c_str(), e.what());
    return 1;
  }
}
