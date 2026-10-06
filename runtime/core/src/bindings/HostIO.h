// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {

/// A failure JS has declared fatal, for the host to act on. Written on the JS
/// thread through `__screenkit.reportFailure`, read by the host from its own
/// loop -- hence the lock. The first report wins: it is the cause, and what
/// follows it is usually fallout.
class FailureReport {
 public:
  void report(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failure_) failure_ = std::move(message);
  }

  std::optional<std::string> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<std::string> failure_;
};

/// Install `__screenkit`: the native capabilities the DOM shim cannot provide
/// in plain JS -- reading an asset's bytes, reading an image's dimensions
/// without decoding it, decoding an image already in memory
/// (`decodeImage(bytes)` -> `{width, height, data}` RGBA) -- and
/// `reportFailure(message)`, which records a failure in `failures` for the host
/// to exit on. The network handle API, `__screenkit.net`, is Net.h's.
///
/// **Confinement is the point of this file.** Architecture.md 9 says untrusted JS
/// must never reach a raw native call, and an unconfined readFile is a sandbox
/// escape. Every path is resolved against an asset root and rejected if it
/// leaves it. The root is set once by the host, before app code runs, through
/// `__screenkit.setAssetRoot`; a second call throws, so app code cannot widen it.
///
/// **The root is per runtime, not per process.** An `<iframe>` instance is a
/// second app with a package of its own (Architecture.md 5), and each confines
/// its own reads: write-once within one instance, and never shared between two.
///
/// Must be called on the thread that owns `runtime`.
void installHostIO(facebook::jsi::Runtime& runtime, std::shared_ptr<FailureReport> failures);

/// Forget this runtime's asset root. JS thread, at teardown, while the runtime
/// is still alive.
void shutdownHostIO(facebook::jsi::Runtime& runtime);

/// A package asset's path on disk, confined exactly as `readFile` confines it
/// for `runtime`: false, with `why`, for a path outside that runtime's asset
/// root or no root set. How a `<video src>` naming a package asset reaches a
/// platform player, which reads files itself (bindings/Media.cpp). JS thread.
bool resolveAssetPath(facebook::jsi::Runtime& runtime, const std::string& candidate,
                      std::string& resolved, std::string& why);

}  // namespace screenkit
