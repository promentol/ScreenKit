// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {

struct Bundle {
  std::shared_ptr<const facebook::jsi::Buffer> buffer;
  std::string url;
  /// True once the buffer has been confirmed to be Hermes bytecode this engine
  /// accepts. False means source text, which is the dev path.
  bool bytecode = false;
};

struct LoadResult {
  bool ok = false;
  std::string error;
  Bundle bundle;
};

/// mmap a bundle off disk. Nothing is copied and nothing is parsed.
LoadResult mapBundleFile(const std::string& path);

/// Refuse bad bytecode *before* `prepareJavaScript`.
///
/// `evaluateJavaScript` accepts a raw HBC buffer happily, which makes skipping
/// this tempting. A mismatched or truncated `.hbc` then faults inside the VM
/// instead of raising anything catchable -- the difference between a clear error
/// and a crash. Validation runs through `IHermesRootAPI`; the static
/// `HermesRuntime::isHermesBytecode` no longer exists.
LoadResult validateBundle(Bundle bundle);

/// True when the buffer starts with the Hermes bytecode magic. A buffer shorter
/// than the magic counts if what is there matches -- that is how a badly
/// truncated `.hbc` is reported as truncated bytecode rather than as a syntax
/// error in "source".
bool looksLikeHermesBytecode(const std::uint8_t* data, std::size_t size);

/// Bytecode version this build of Hermes accepts.
std::uint32_t supportedBytecodeVersion();

}  // namespace screenkit
