// Copyright (c) ScreenKit contributors. MIT.
//
// SpiderMonkey behind engine/Engine.h (SCREENKIT_ENGINE=spidermonkey, Linux).
//
// Tuning from the environment, as SCREENKIT_HERMES_JIT is for Hermes:
//   SCREENKIT_SM_JIT=off|baseline|on        the tiers that run (default on)
//   SCREENKIT_SM_NURSERY_KB=<n>             the nursery's ceiling (default 64 MB)
//   SCREENKIT_SM_INCREMENTAL_GC=1           sliced major collections
#include "../engine/Engine.h"

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <screenkit/Log.h>

#include "../hermes/BytecodeLoader.h"
#include "SpiderMonkeyRuntime.h"

namespace screenkit {
namespace engine {

namespace {

/// Three quarters of the calling thread's stack: the engine throws "too much
/// recursion" at its quota, and the rest is for the native frames between JS.
std::size_t threadStackQuota() {
  std::size_t size = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstacksize(&attr, &size);
    pthread_attr_destroy(&attr);
  }
  if (size == 0) size = 1024 * 1024;
  return size / 4 * 3;
}

const char* env(const char* name) {
  const char* value = std::getenv(name);
  return value && *value ? value : nullptr;
}

}  // namespace

const char* name() { return "spidermonkey"; }

std::string description() { return spidermonkey::engineVersion(); }

std::unique_ptr<facebook::jsi::Runtime> createRuntime(const RuntimeConfig& config, std::string& error) {
  spidermonkey::Options options;
  if (const char* jit = env("SCREENKIT_SM_JIT")) options.jit = jit;
  if (options.jit != "off" && options.jit != "baseline" && options.jit != "on") {
    error = "SCREENKIT_SM_JIT is \"" + options.jit + "\"; it takes off, baseline or on";
    return nullptr;
  }
  if (const char* kb = env("SCREENKIT_SM_NURSERY_KB")) {
    options.maxNurseryBytes = static_cast<std::uint32_t>(std::strtoul(kb, nullptr, 10) * 1024);
  }
  if (const char* incremental = env("SCREENKIT_SM_INCREMENTAL_GC")) options.incrementalGc = incremental[0] == '1';
  options.stackQuotaBytes = threadStackQuota();
  const std::string tag = config.name;
  options.onUnhandledRejection = [tag](const std::string& reason) {
    log(LogLevel::Error, tag, "Unhandled promise rejection: " + reason);
  };
  const std::string jit = options.jit;
  try {
    auto runtime = spidermonkey::makeSpiderMonkeyRuntime(std::move(options));
    log(LogLevel::Log, config.name, description() + ", JIT " + jit);
    return runtime;
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

const char* domShimFileName() { return "dom-shim.js"; }

bool canLoadPrecompiled(const std::string& path) {
  // The stencil header -- magic, length, build id -- is well inside this.
  std::uint8_t head[512];
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const std::size_t n = std::fread(head, 1, sizeof head, f);
  std::fclose(f);
  return spidermonkey::isLoadableStencil(head, n);
}

}  // namespace engine

// ---- BytecodeLoader.h, for this engine ---------------------------------------
//
// No Hermes bytecode here. A bundle is source, or a stencil, which is checked for
// the engine build it was compiled by before the engine sees it.

std::uint32_t supportedBytecodeVersion() { return 0; }

LoadResult validateBundle(Bundle bundle) {
  LoadResult r;
  if (!bundle.buffer) {
    r.error = "no bundle buffer";
    return r;
  }
  const std::uint8_t* data = bundle.buffer->data();
  const std::size_t size = bundle.buffer->size();
  if (looksLikeHermesBytecode(data, size)) {
    r.error = "\"" + bundle.url +
              "\" is Hermes bytecode, and this runtime is SpiderMonkey: it runs a package's source or "
              "its stencil (engines.spidermonkey in the manifest; `screenkit bundle` writes both)";
    return r;
  }
  if (spidermonkey::looksLikeStencil(data, size) && !spidermonkey::isLoadableStencil(data, size)) {
    r.error = "\"" + bundle.url + "\" is a stencil compiled by another SpiderMonkey build than " +
              spidermonkey::engineVersion() + " -- recompile it with screenkit-smc, or load the source";
    return r;
  }
  bundle.bytecode = false;
  r.ok = true;
  r.bundle = std::move(bundle);
  return r;
}

}  // namespace screenkit
