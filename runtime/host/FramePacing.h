// Copyright (c) ScreenKit contributors. MIT.
//
// How the windowed loop paces `requestAnimationFrame` (host/Host.cpp).
//
// Two pure functions, kept out of the loop and out of SDL, because getting
// either wrong is invisible in ordinary use: too fast shows up only as CPU burnt
// on a screen that is holding still, too slow only as a halved frame rate under
// load, and both look like a normal-looking app either way.
//
// The rule the pair encodes: a frame that painted is already paced by the
// display, because `swap()` blocks on the refresh. Only a frame that painted
// nothing needs a deadline -- and those are exactly the frames that would
// otherwise spin, since there is no swap to block on.
#pragma once

namespace screenkit::host {

/// The frame interval a refresh rate implies, in milliseconds. A rate SDL does
/// not know (it reports 0 for some Wayland compositors, and for every headless
/// display) or could not plausibly mean falls back to 60 Hz -- the same guess
/// the web makes when it cannot tell either.
double frameIntervalMsForRefreshRate(double refreshRateHz);

/// Where the next frame is due, given the deadline that just fired, when the
/// frame ran (`tickMs`), when it finished (`nowMs`), and whether it painted.
///
/// A painted frame is due again as soon as the loop can yield: waiting a whole
/// interval on top of the swap would pace it twice and halve the rate. An
/// unpainted one gets the deadline, advanced from the deadline rather than from
/// now so the rate does not drift with what the frame happened to cost.
double nextFrameDeadlineMs(double deadlineMs, double tickMs, double nowMs, double intervalMs,
                           bool painted);

}  // namespace screenkit::host
