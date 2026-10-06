// Copyright (c) ScreenKit contributors. MIT.
//
// Frame timing, measured and logged -- never gated.
//
// M4's exit criterion is "a triangle from JS at 60fps", but a CI threshold on
// that number would be a flaky test on a simulator whose frame timing is noisy
// enough to fail for reasons that have nothing to do with the renderer, and a
// flaky test gets disabled. So the rate is *published* once a second at
// LogLevel::Log: visible in `log show`, visible in ctest output, and a
// regression is obvious to anyone reading the log. Nothing here fails a build.
#pragma once

#include <cstddef>
#include <string>

namespace screenkit {
namespace gfx {

class FrameStats {
 public:
  /// `tag` is the log tag; `reportIntervalMs` is how often a line is emitted.
  explicit FrameStats(std::string tag, double reportIntervalMs = 1000.0);

  /// One presented frame, at the monotonic timestamp the frame was served with.
  /// Call it once per swap, from the thread that owns the frame -- there is no
  /// locking here because there is only ever one frame producer.
  void frame(double timestampMs);

  /// Frames counted since construction. Lets a test assert that frames really
  /// kept arriving rather than inferring it from a log line.
  std::size_t totalFrames() const { return totalFrames_; }

 private:
  std::string tag_;
  double reportIntervalMs_;

  bool started_ = false;
  double windowStartMs_ = 0.0;
  double lastFrameMs_ = 0.0;
  double worstIntervalMs_ = 0.0;
  std::size_t windowFrames_ = 0;
  std::size_t totalFrames_ = 0;
};

}  // namespace gfx
}  // namespace screenkit
