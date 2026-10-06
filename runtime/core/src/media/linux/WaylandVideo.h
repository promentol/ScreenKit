// Copyright (c) ScreenKit contributors. MIT.
//
// Where a Linux <video>'s frames are shown: a Wayland subsurface of the app's
// window, placed BELOW it, carrying linear YUV dma-bufs the compositor
// composites itself. Video never passes through the runtime's GL -- the app
// clears transparent where the video shows, and the window's opaque region is
// cut open under a visible plane so the compositor blends only there.
//
//   wl_subsurface (desync, below the parent)   where the plane is
//   wp_viewport                                 its size: the picture scaled to the plane
//   zwp_linux_dmabuf_v1                         the pictures: GBM buffers VLC writes into
//   wp_presentation                             which pictures reached the screen
//
// It is fed by libvlc's memory video output (libvlc_video_set_callbacks): VLC
// asks for a buffer (`lock`), copies its picture into it -- the one copy a frame
// takes on its way to the screen -- and when the picture is due, `display`
// commits it. VLC's vout thread makes those calls; the player's surface thread
// runs `dispatch`, serving this surface's private event queue (buffer releases,
// presentation feedback) and moving the plane when the page does. An internal
// lock lets the two share the Wayland objects; SDL's own queue is never
// dispatched from here.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../MediaPlayer.h"

// Declared at global scope: inside the namespace below, `struct wl_display*`
// would declare a new type of the namespace's own.
struct SDL_Window;
struct wl_display;
struct wl_event_queue;
struct wl_surface;
struct wl_subsurface;
struct wp_viewport;
struct gbm_device;

namespace screenkit::media::wl {

struct Globals;
struct Buffer;

/// Whether video can be shown at all: the VideoHost window is on SDL's Wayland
/// driver and has a surface. `why` names the missing Wayland otherwise.
bool waylandAvailable(std::string& why);

/// Says the window covers its whole area, which SDL does not while the window
/// carries SDL_WINDOW_TRANSPARENT -- the flag that lets a plane beneath it show.
/// Called once, when the window is made; a visible plane cuts the region open.
/// Does nothing off Wayland.
void declareWindowOpaque(SDL_Window* window);

class VideoSurface {
 public:
  /// Null, with `error`, when there is no Wayland window to put video under or the
  /// compositor lacks what it needs (subsurfaces, dma-bufs in linear NV12/YU12).
  static std::unique_ptr<VideoSurface> create(std::string& error);
  ~VideoSurface();

  VideoSurface(const VideoSurface&) = delete;
  VideoSurface& operator=(const VideoSurface&) = delete;

  /// The plane, in drawable pixels. Any thread; applied at the next `display` or
  /// `dispatch`.
  void setPlane(const PlaneRect& rect, bool visible);

  /// The picture's visible size and sample aspect, when known better than the
  /// buffers' own size (a decoder that pads to 16 rows). Any thread.
  void setPicture(int width, int height, double sampleAspect);

  // ---- vmem, on VLC's vout thread -------------------------------------------------
  /// The picture's layout: allocate buffers for it and answer VLC's pitches and
  /// lines. `nv12` false is I420. False when the buffers cannot be made.
  bool configure(bool nv12, unsigned width, unsigned height, unsigned pitches[3], unsigned lines[3]);
  /// The buffers go (VLC is done with this layout).
  void unconfigure();
  /// A buffer for the next picture, mapped for writing, its planes in `planes`.
  /// The compositor may still hold every buffer: then a scratch one, whose
  /// picture is never shown. Returns the handle `unlock` and `display` take.
  void* lock(void* planes[3]);
  void unlock(void* picture);
  /// The picture is due: show it.
  void display(void* picture);

  /// Nothing loaded any more: the plane shows nothing and the buffers go. Only
  /// once VLC has stopped writing into them.
  void clear();

  // ---- the player's surface thread -----------------------------------------------
  /// Serve this surface's events and apply a changed plane, waiting up to
  /// `timeoutMs` for something to arrive.
  void dispatch(int timeoutMs);

  /// Pictures committed, shown by the compositor, and dropped because every
  /// buffer was still on screen.
  std::uint64_t committed() const { return committed_.load(); }
  std::uint64_t presented() const { return presented_.load(); }
  std::uint64_t dropped() const { return dropped_.load(); }

  // The compositor's answers, from the listeners (under the lock).
  void notePresented() { presented_.fetch_add(1); }
  void noteReleased() { released_.notify_all(); }

 private:
  VideoSurface() = default;

  bool makeWlBuffer(Buffer& buffer);
  void releaseBuffers();
  void applyPlane();
  void showLast();

  std::recursive_mutex mutex_;
  std::condition_variable_any released_;
  std::unique_ptr<Globals> globals_;
  SDL_Window* window_ = nullptr;
  struct wl_display* display_ = nullptr;
  struct wl_surface* parent_ = nullptr;
  struct wl_event_queue* queue_ = nullptr;
  struct wl_surface* surface_ = nullptr;
  struct wl_subsurface* subsurface_ = nullptr;
  struct wp_viewport* viewport_ = nullptr;
  int drmFd_ = -1;
  struct gbm_device* gbm_ = nullptr;

  std::vector<std::unique_ptr<Buffer>> buffers_;
  std::unique_ptr<Buffer> scratch_;
  bool nv12_ = false;
  unsigned width_ = 0;
  unsigned height_ = 0;
  unsigned stride_ = 0;
  /// What the dma-bufs describe: the visible part of the buffers.
  int pictureWidth_ = 0;
  int pictureHeight_ = 0;
  double aspect_ = 1.0;
  bool attached_ = false;
  /// The buffer holding the picture last displayed: shown again when a hidden
  /// plane comes back with no new picture (a paused video put back in the page).
  Buffer* last_ = nullptr;

  PlaneRect plane_;
  bool planeVisible_ = false;
  bool planeDirty_ = true;
  int appliedX_ = 0, appliedY_ = 0, appliedWidth_ = 0, appliedHeight_ = 0;
  bool appliedVisible_ = false;

  std::atomic<std::uint64_t> committed_{0};
  std::atomic<std::uint64_t> presented_{0};
  std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace screenkit::media::wl
