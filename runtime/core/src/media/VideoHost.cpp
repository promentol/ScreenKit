// Copyright (c) ScreenKit contributors. MIT.
//
// Where video goes, shared by every platform's player: the host's window, set
// from the main thread and read from whatever thread a player places its plane
// on, and the arithmetic that puts a plane under its CSS rect.
#include <algorithm>
#include <mutex>

#include "MediaPlayer.h"

namespace screenkit::media {
namespace {

std::mutex& hostMutex() {
  static std::mutex m;
  return m;
}

VideoHost& hostSlot() {
  static VideoHost host;
  return host;
}

}  // namespace

void setVideoHost(const VideoHost& host) {
  std::lock_guard<std::mutex> lock(hostMutex());
  hostSlot() = host;
}

void clearVideoHost() {
  std::lock_guard<std::mutex> lock(hostMutex());
  hostSlot() = VideoHost();
}

VideoHost videoHost() {
  std::lock_guard<std::mutex> lock(hostMutex());
  return hostSlot();
}

PlaneRect planeInWindow(const PlaneRect& plane, int windowWidth, int windowHeight, int drawableWidth,
                        int drawableHeight) {
  if (drawableWidth <= 0 || drawableHeight <= 0 || windowWidth <= 0 || windowHeight <= 0 ||
      (drawableWidth == windowWidth && drawableHeight == windowHeight)) {
    return plane;
  }
  // GlSurfaceSdl's present: the frame scaled by the smaller ratio, centred.
  const double scale = std::min(static_cast<double>(windowWidth) / drawableWidth,
                                static_cast<double>(windowHeight) / drawableHeight);
  const double offsetX = (windowWidth - drawableWidth * scale) / 2.0;
  const double offsetY = (windowHeight - drawableHeight * scale) / 2.0;
  PlaneRect out = plane;
  out.x = offsetX + plane.x * scale;
  out.y = offsetY + plane.y * scale;
  out.width = plane.width * scale;
  out.height = plane.height * scale;
  return out;
}

PlaneRect fitContain(const PlaneRect& box, int videoWidth, int videoHeight) {
  if (videoWidth <= 0 || videoHeight <= 0 || box.width <= 0 || box.height <= 0) return box;
  const double scale = std::min(box.width / videoWidth, box.height / videoHeight);
  PlaneRect out = box;
  out.width = videoWidth * scale;
  out.height = videoHeight * scale;
  out.x = box.x + (box.width - out.width) / 2.0;
  out.y = box.y + (box.height - out.height) / 2.0;
  return out;
}

}  // namespace screenkit::media
