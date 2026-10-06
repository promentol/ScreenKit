// Copyright (c) ScreenKit contributors. MIT.
#include "FramePacing.h"

namespace screenkit::host {

double frameIntervalMsForRefreshRate(double refreshRateHz) {
  constexpr double kDefaultMs = 1000.0 / 60.0;
  // Only a rate a display could actually mean: a broken EDID should turn into
  // neither a spin (huge) nor a one-frame-a-second app (tiny). Written as a
  // negated range so a NaN takes the default too.
  if (!(refreshRateHz >= 20.0 && refreshRateHz <= 1000.0)) return kDefaultMs;
  return 1000.0 / refreshRateHz;
}

double nextFrameDeadlineMs(double deadlineMs, double tickMs, double nowMs, double intervalMs,
                           bool painted) {
  // The 1 ms the loop has always yielded between painting frames. tickFrame only
  // posts to the JS thread, so without it the loop would spin posting frames to
  // a thread still finishing the last one.
  constexpr double kPaintedYieldMs = 1.0;
  if (painted) return nowMs + kPaintedYieldMs;

  const double advanced = deadlineMs + intervalMs;
  // Already behind -- the frame overran, or the loop sat idle against a stale
  // deadline -- so restart from this frame rather than firing the run of
  // catch-up frames the old deadline is owed.
  return advanced < tickMs ? tickMs + intervalMs : advanced;
}

}  // namespace screenkit::host
