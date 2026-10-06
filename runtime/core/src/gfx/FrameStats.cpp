// Copyright (c) ScreenKit contributors. MIT.
#include "FrameStats.h"

#include <cstdio>
#include <utility>

#include <screenkit/Log.h>

namespace screenkit {
namespace gfx {

FrameStats::FrameStats(std::string tag, double reportIntervalMs)
    : tag_(std::move(tag)), reportIntervalMs_(reportIntervalMs > 0.0 ? reportIntervalMs : 1000.0) {}

void FrameStats::frame(double timestampMs) {
  ++totalFrames_;

  if (!started_) {
    // The first frame has no predecessor, so it starts the window and nothing
    // else. Counting an interval from zero would report a 20-second frame.
    started_ = true;
    windowStartMs_ = timestampMs;
    lastFrameMs_ = timestampMs;
    return;
  }

  const double interval = timestampMs - lastFrameMs_;
  lastFrameMs_ = timestampMs;
  ++windowFrames_;
  if (interval > worstIntervalMs_) worstIntervalMs_ = interval;

  const double elapsed = timestampMs - windowStartMs_;
  if (elapsed < reportIntervalMs_ || windowFrames_ == 0) return;

  // Mean over the window rather than over the last frame: a single hitch should
  // move `worst`, not the headline number.
  const double meanMs = elapsed / static_cast<double>(windowFrames_);
  const double fps = meanMs > 0.0 ? 1000.0 / meanMs : 0.0;

  char line[160];
  std::snprintf(line, sizeof(line),
                "frame time %.2f ms mean, %.2f ms worst over %zu frames (%.1f fps)", meanMs,
                worstIntervalMs_, windowFrames_, fps);
  log(LogLevel::Log, tag_, line);

  windowStartMs_ = timestampMs;
  windowFrames_ = 0;
  worstIntervalMs_ = 0.0;
}

}  // namespace gfx
}  // namespace screenkit
