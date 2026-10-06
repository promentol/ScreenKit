// Copyright (c) ScreenKit contributors. MIT.
//
// Hermes behind engine/Engine.h -- every platform's engine, and Linux's default.
#include "../engine/Engine.h"

#include <hermes/Public/GCConfig.h>
#include <hermes/Public/RuntimeConfig.h>
#include <hermes/hermes.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

#include <screenkit/Log.h>

#include "BytecodeLoader.h"

namespace screenkit {
namespace engine {

const char* name() { return "hermes"; }

std::string description() { return "Hermes (bytecode " + std::to_string(supportedBytecodeVersion()) + ")"; }

std::unique_ptr<facebook::jsi::Runtime> createRuntime(const RuntimeConfig& config, std::string& error) {
  // gcheapsize_t is 32-bit, so a 64-bit byte count above 4 GiB would wrap to a
  // tiny heap rather than a large one. Clamp instead.
  using gcheapsize_t = ::hermes::vm::gcheapsize_t;
  const auto kCeiling = static_cast<std::size_t>(std::numeric_limits<gcheapsize_t>::max());
  const auto maxHeap = static_cast<gcheapsize_t>(std::min(config.maxHeapBytes, kCeiling));
  auto gc = ::hermes::vm::GCConfig::Builder().withMaxHeapSize(maxHeap).build();
  // withMicrotaskQueue: without it Hermes falls back to a JS Promise polyfill
  // in InternalBytecode.js that schedules through `setImmediate`, so Promise
  // is broken unless the host supplies one -- and supplying one is the wrong
  // fix, because it turns Promise callbacks into macrotasks and loses the
  // "microtasks drain before the next timer" ordering. The native queue is
  // what makes jsi::Runtime::drainMicrotasks() mean anything.
  //
  // withES6BlockScoping: off by default in this Hermes, and off means `let` and
  // `const` are function-scoped like `var`. The visible casualty is a closure
  // created in `for (let i ...)`: every iteration shares one `i`, so each one
  // sees the final value. Blits builds its reactive effects exactly that way,
  // and the first state change after the initial render called `effects[length]`
  // -- "undefined is not a function". This covers source evaluation; bytecode
  // gets the same semantics from `hermesc -Xes6-block-scoping`, which every
  // compile in the tree passes.
  // SCREENKIT_HERMES_JIT: Hermes' JIT, off unless asked for. It exists only in a
  // library built with HERMESVM_ALLOW_JIT (the shipping prebuilts are built
  // without it, so this does nothing there) and `EnableJIT` is false by
  // default even then. "1" turns it on, "force" also compiles every function
  // rather than waiting for the call threshold -- for measuring an engine
  // against the interpreter on a device (EMBEDDED_LINUX_EXPERIMENTS.md: the
  // Raspberry Pi's stress numbers are CPU-bound because Hermes interprets).
  auto builder = ::hermes::vm::RuntimeConfig::Builder()
                     .withGCConfig(gc)
                     .withMicrotaskQueue(true)
                     .withES6BlockScoping(true);
  const char* jit = std::getenv("SCREENKIT_HERMES_JIT");
  if (jit != nullptr && jit[0] != '\0' && jit[0] != '0') {
    builder.withEnableJIT(true);
    if (std::string(jit) == "force") builder.withForceJIT(true);
    log(LogLevel::Log, config.name,
        std::string("Hermes JIT requested (SCREENKIT_HERMES_JIT=") + jit +
            "); a library built without HERMESVM_ALLOW_JIT ignores it");
  }
  auto runtime = facebook::hermes::makeHermesRuntime(builder.build());
  if (!runtime) error = "makeHermesRuntime returned null";
  return runtime;
}

const char* domShimFileName() { return "dom-shim.hbc"; }

bool canLoadPrecompiled(const std::string&) { return false; }

}  // namespace engine
}  // namespace screenkit
