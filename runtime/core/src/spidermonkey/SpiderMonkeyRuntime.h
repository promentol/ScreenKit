// Copyright (c) ScreenKit contributors. MIT.
//
// JSI over SpiderMonkey (mozjs-128): a `facebook::jsi::Runtime` whose engine is
// Firefox's, JIT included. Every binding in the runtime is written against JSI,
// so on an engine behind this they run unchanged -- the GL, timers, net, text.
//
// What the engine is chosen for (EMBEDDED_LINUX_EXPERIMENTS.md, "Engine bench"):
// on a Raspberry Pi 3 SpiderMonkey's JIT ran Phaser-shaped JavaScript 5-10x
// faster than the Hermes interpreter. What it costs: ~20 MB more RSS, a slower
// start, and source rather than bytecode -- which is what `compileToStencil`
// below is for.
//
// Threading is JSI's: the runtime is created, used and destroyed on one thread.
// JSI handles (Value, Object, ...) may be *destroyed* on any thread; their
// release is deferred to the runtime's thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <jsi/jsi.h>

namespace screenkit {
namespace spidermonkey {

struct Options {
  /// Which of SpiderMonkey's tiers run: "off" (the C++ interpreter only),
  /// "baseline" (+ the Baseline Interpreter and JIT), "on" (+ Ion/Warp, the
  /// optimising JIT -- what Firefox runs).
  std::string jit = "on";
  /// The nursery's ceiling. 0 keeps SpiderMonkey's (64 MB on 64-bit), which
  /// it grows into only under allocation pressure.
  std::uint32_t maxNurseryBytes = 0;
  /// Incremental (sliced) major collections.
  bool incrementalGc = false;
  /// The native stack the engine may use before throwing "too much
  /// recursion". Must be below the thread's real stack size.
  std::size_t stackQuotaBytes = 4 * 1024 * 1024;
  /// A promise rejected with no handler by the end of a microtask drain.
  std::function<void(const std::string& reason)> onUnhandledRejection;
};

/// Create the runtime on the calling thread, which becomes its thread.
/// Throws std::runtime_error when the engine cannot start.
std::unique_ptr<facebook::jsi::Runtime> makeSpiderMonkeyRuntime(Options options = {});

/// "SpiderMonkey 128.14.0" -- the engine this was linked against.
std::string engineVersion();

// ---- precompiled scripts ("stencils") ------------------------------------------
//
// SpiderMonkey's answer to Hermes bytecode: a parsed and compiled script,
// serialised. Loading one skips parsing, which on a Pi is most of an app's start
// (~0.8 s for 1.4 MB of source). The format is private to one build of the
// engine, so a stencil carries the engine's build id and is refused by any
// other: a package keeps its source beside it as the fallback.

/// Compile `source` to a stencil. Runs on a runtime of its own; any thread.
/// Throws std::runtime_error with the syntax error.
///
/// `eager` compiles every function now, so nothing is parsed on the device --
/// by default an inner function is compiled on its first call, from the source
/// the stencil carries. Eager is the bigger file and the faster first run.
///
/// A stencil loaded from a buffer the runtime keeps (any jsi::Buffer handed to
/// prepareJavaScript) is run in place: its bytecode is read straight out of the
/// buffer, so a mapped file costs file-backed pages -- as Hermes' .hbc does --
/// and the buffer is held until the process exits.
std::vector<std::uint8_t> compileToStencil(const std::string& source, const std::string& url,
                                           bool eager = false);

/// True when `data` is a stencil this build of the engine can load.
bool isLoadableStencil(const std::uint8_t* data, std::size_t size);

/// True when `data` looks like a stencil at all, loadable or not -- so a stale
/// one is reported as stale rather than parsed as source.
bool looksLikeStencil(const std::uint8_t* data, std::size_t size);

}  // namespace spidermonkey
}  // namespace screenkit
