// Copyright (c) ScreenKit contributors. MIT.
//
// The JavaScript engine behind JSI, chosen when the runtime is built
// (SCREENKIT_ENGINE in runtime/CMakeLists.txt). Everything above this line is
// JSI and does not know which engine it runs on; what differs is here:
//
//   hermes        core/src/hermes/HermesEngine.cpp -- every platform; packages run
//                 the bytecode `screenkit bundle` compiles with the pinned hermesc
//   spidermonkey  core/src/spidermonkey/SpiderMonkeyEngine.cpp -- Linux, where it
//                 may JIT; packages run their source, or a stencil precompiled
//                 by screenkit-smc for exactly the library the device loads
#pragma once

#include <memory>
#include <string>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {
namespace engine {

/// "hermes" or "spidermonkey".
const char* name();

/// The engine and its version, for logs: "Hermes (bytecode 99)", "SpiderMonkey 128.14.0".
std::string description();

/// Create the runtime on the calling thread -- the JS thread, which it never
/// leaves. Null with `error` set when the engine cannot start.
std::unique_ptr<facebook::jsi::Runtime> createRuntime(const RuntimeConfig& config, std::string& error);

/// The prelude's file name beside the host executable: the DOM shim compiled
/// for this engine ("dom-shim.hbc"), or its source ("dom-shim.js").
const char* domShimFileName();

/// For a SpiderMonkey package: true when `path` is a stencil this build of the
/// engine can load, so the host takes it over the source. Always false on Hermes.
bool canLoadPrecompiled(const std::string& path);

}  // namespace engine
}  // namespace screenkit
