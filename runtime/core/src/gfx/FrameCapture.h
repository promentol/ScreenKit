// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace screenkit {
namespace gfx {

class GlSurface;

/// A developer aid: save one presented frame as a PNG.
///
/// `SCREENKIT_CAPTURE=<file.png>` asks for it, and `SCREENKIT_CAPTURE_DELAY_MS`
/// (default 2000) says how long after the first frame to wait, so an app has
/// loaded and started animating. The frame is read back from the default
/// framebuffer after its GL work has run and before it is presented -- what the
/// screen is about to show, with no screen-recording permission and no simulator
/// involved. Captured once; alpha is written as opaque, as the page composites
/// over black.
class FrameCapture {
 public:
  /// Null when SCREENKIT_CAPTURE is unset.
  static std::unique_ptr<FrameCapture> fromEnvironment();

  FrameCapture(std::string path, double delayMs) : path_(std::move(path)), delayMs_(delayMs) {}

  /// GL thread, with the frame flushed and the surface current, before the swap.
  void beforePresent(GlSurface& surface, double nowMs);

 private:
  std::string path_;
  double delayMs_;
  double firstFrameMs_ = -1;
  bool done_ = false;
};

/// RGBA8 rows, top row first, to a PNG file. False with `error` on failure.
bool writePng(const std::string& path, int width, int height, const std::vector<std::uint8_t>& rgba,
              std::string& error);

}  // namespace gfx
}  // namespace screenkit
