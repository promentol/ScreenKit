// Copyright (c) ScreenKit contributors. MIT.
//
// The Linux video plane: see WaylandVideo.h. Written against libwayland-client
// directly, with the protocol code generated at build time from the pinned
// wayland-protocols XMLs (runtime/CMakeLists.txt, [linux-media]).
//
// Two things here would take the whole app down if they went wrong, because a
// Wayland protocol error kills the client's connection -- SDL's window with it:
// a request the compositor must refuse (a zero-sized viewport, a dma-buf it
// cannot import sent through create_immed), and dispatching SDL's own queue
// from another thread. So every size is checked before it is sent, buffers are
// made with the non-immediate `create` whose failure is an event, and
// everything this file owns sits on one private event queue, dispatched under
// this surface's lock by the player's surface thread (and by VLC's vout thread
// only in the round trips that make buffers).
#include "WaylandVideo.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <wayland-client.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

#include <linux/dma-buf.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <screenkit/Log.h>

namespace screenkit::media::wl {

constexpr const char* kTag = "screenkit.video";
/// Frames in flight: one on screen, one queued behind it, and room for the
/// compositor to hold another while the next is copied.
constexpr int kBufferCount = 4;

/// SCREENKIT_MEDIA_TRACE=1, as the player's own trace (MediaPlayerLinux.cpp).
bool traceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("SCREENKIT_MEDIA_TRACE");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

// ---- globals, bound on the surface's own queue ------------------------------------

struct Globals {
  struct wl_display* wrapper = nullptr;
  struct wl_registry* registry = nullptr;
  struct wl_compositor* compositor = nullptr;
  struct wl_subcompositor* subcompositor = nullptr;
  struct zwp_linux_dmabuf_v1* dmabuf = nullptr;
  std::uint32_t dmabufVersion = 0;
  struct wp_viewporter* viewporter = nullptr;
  struct wp_presentation* presentation = nullptr;

  bool nv12Linear = false;
  bool yu12Linear = false;
  bool haveMainDevice = false;
  dev_t mainDevice = 0;
  bool feedbackDone = false;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> formatTable;

  void noteFormat(std::uint32_t format, std::uint64_t modifier) {
    if (modifier != DRM_FORMAT_MOD_LINEAR) return;
    if (format == DRM_FORMAT_NV12) nv12Linear = true;
    if (format == DRM_FORMAT_YUV420) yu12Linear = true;
  }

  ~Globals() {
    if (presentation != nullptr) wp_presentation_destroy(presentation);
    if (viewporter != nullptr) wp_viewporter_destroy(viewporter);
    if (dmabuf != nullptr) zwp_linux_dmabuf_v1_destroy(dmabuf);
    if (subcompositor != nullptr) wl_subcompositor_destroy(subcompositor);
    if (compositor != nullptr) wl_compositor_destroy(compositor);
    if (registry != nullptr) wl_registry_destroy(registry);
    if (wrapper != nullptr) wl_proxy_wrapper_destroy(wrapper);
  }
};

struct Buffer {
  VideoSurface* owner = nullptr;
  struct gbm_bo* bo = nullptr;
  int fd = -1;
  struct wl_buffer* buffer = nullptr;
  /// The whole buffer object, mapped for writing once: VLC copies each picture
  /// straight into it. Null for the scratch buffer, whose bytes are `scratch`.
  std::uint8_t* map = nullptr;
  size_t mapSize = 0;
  std::vector<std::uint8_t> scratch;
  /// Held by the compositor: attached and not yet released.
  bool busy = false;
  /// Handed to VLC by `lock` and not yet displayed or dropped.
  bool locked = false;
  std::uint32_t offsets[3] = {0, 0, 0};
  std::uint32_t strides[3] = {0, 0, 0};

  std::uint8_t* base() { return map != nullptr ? map : scratch.data(); }

  ~Buffer() {
    if (buffer != nullptr) wl_buffer_destroy(buffer);
    if (map != nullptr) munmap(map, mapSize);
    if (fd >= 0) close(fd);
    if (bo != nullptr) gbm_bo_destroy(bo);
  }
};

namespace {

int alignUp(int value, int to) { return (value + to - 1) / to * to; }

// ---- the parent's opaque region -----------------------------------------------------
//
// SDL declares its window fully opaque (SetSurfaceOpaqueRegion), and a
// compositor is free to skip whatever sits beneath an opaque surface -- this
// subsurface included. While a plane is visible the region is the window minus
// every visible plane, so the compositor blends exactly there; with none it is
// the whole window again, as SDL set it, and the canvas-only path is unchanged.
//
// The region and a subsurface's position are the parent's double-buffered
// state, applied on the parent's commit. The app commits only when it paints,
// and a UI idle while the video plays would never apply them, so the parent is
// committed here too -- with no buffer attached, which re-applies its current
// contents and changes nothing else.

struct ParentRegion {
  std::mutex mutex;
  std::map<const void*, std::array<int, 4>> planes;
};

ParentRegion& parentRegion() {
  static ParentRegion region;
  return region;
}

void updateParentRegion(const void* owner, bool visible, const std::array<int, 4>& rect, SDL_Window* window,
                        struct wl_compositor* compositor, struct wl_surface* parent, struct wl_display* display,
                        bool commitAnyway) {
  ParentRegion& state = parentRegion();
  std::lock_guard<std::mutex> lock(state.mutex);
  bool changed = false;
  if (visible) {
    auto it = state.planes.find(owner);
    changed = it == state.planes.end() || it->second != rect;
    state.planes[owner] = rect;
  } else {
    changed = state.planes.erase(owner) != 0;
  }
  if (changed) {
    int width = 0, height = 0;
    if (window != nullptr && SDL_GetWindowSize(window, &width, &height) && width > 0 && height > 0) {
      struct wl_region* region = wl_compositor_create_region(compositor);
      wl_region_add(region, 0, 0, width, height);
      for (const auto& plane : state.planes) {
        wl_region_subtract(region, plane.second[0], plane.second[1], plane.second[2], plane.second[3]);
      }
      wl_surface_set_opaque_region(parent, region);
      wl_region_destroy(region);
    }
  }
  if (changed || commitAnyway) {
    wl_surface_commit(parent);
    wl_display_flush(display);
  }
}

bool sdlWaylandWindow(SDL_Window*& window, struct wl_display*& display, struct wl_surface*& surface,
                      std::string& why) {
  const VideoHost host = videoHost();
  window = host.window;
  if (window == nullptr) {
    why = "video on Linux needs Wayland, and there is no Wayland window to show it under";
    return false;
  }
  const char* driver = SDL_GetCurrentVideoDriver();
  if (driver == nullptr || std::strcmp(driver, "wayland") != 0) {
    why = std::string("video on Linux needs Wayland, and SDL's video driver is ") +
          (driver == nullptr ? "none" : driver) + " (X11 and KMS/DRM have no video plane)";
    return false;
  }
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  display = static_cast<struct wl_display*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  surface = static_cast<struct wl_surface*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  if (display == nullptr || surface == nullptr) {
    why = "video on Linux needs Wayland, and the window has no Wayland surface";
    return false;
  }
  return true;
}

// ---- listeners -------------------------------------------------------------------------

void registryGlobal(void* data, struct wl_registry* registry, std::uint32_t name, const char* interface,
                    std::uint32_t version) {
  auto* g = static_cast<Globals*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0 && g->compositor == nullptr) {
    g->compositor = static_cast<struct wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, std::min<std::uint32_t>(version, 4)));
  } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0 && g->subcompositor == nullptr) {
    g->subcompositor =
        static_cast<struct wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3 &&
             g->dmabuf == nullptr) {
    g->dmabufVersion = std::min<std::uint32_t>(version, 4);
    g->dmabuf = static_cast<struct zwp_linux_dmabuf_v1*>(
        wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, g->dmabufVersion));
  } else if (std::strcmp(interface, wp_viewporter_interface.name) == 0 && g->viewporter == nullptr) {
    g->viewporter =
        static_cast<struct wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  } else if (std::strcmp(interface, wp_presentation_interface.name) == 0 && g->presentation == nullptr) {
    g->presentation =
        static_cast<struct wp_presentation*>(wl_registry_bind(registry, name, &wp_presentation_interface, 1));
  }
}
void registryGlobalRemove(void*, struct wl_registry*, std::uint32_t) {}
const struct wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// linux-dmabuf v3: the formats arrive one modifier at a time.
void dmabufFormat(void*, struct zwp_linux_dmabuf_v1*, std::uint32_t) {}
void dmabufModifier(void* data, struct zwp_linux_dmabuf_v1*, std::uint32_t format, std::uint32_t hi,
                    std::uint32_t lo) {
  static_cast<Globals*>(data)->noteFormat(format, (static_cast<std::uint64_t>(hi) << 32) | lo);
}
const struct zwp_linux_dmabuf_v1_listener kDmabufListener = {dmabufFormat, dmabufModifier};

// linux-dmabuf v4: a format table, the device the compositor composites with,
// and tranches of indices into the table.
void feedbackDone(void* data, struct zwp_linux_dmabuf_feedback_v1*) { static_cast<Globals*>(data)->feedbackDone = true; }
void feedbackFormatTable(void* data, struct zwp_linux_dmabuf_feedback_v1*, std::int32_t fd, std::uint32_t size) {
  auto* g = static_cast<Globals*>(data);
  void* table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (table != MAP_FAILED) {
    struct Entry {
      std::uint32_t format;
      std::uint32_t pad;
      std::uint64_t modifier;
    };
    const auto* entries = static_cast<const Entry*>(table);
    g->formatTable.clear();
    for (std::uint32_t i = 0; i < size / sizeof(Entry); ++i) {
      g->formatTable.emplace_back(entries[i].format, entries[i].modifier);
    }
    munmap(table, size);
  }
  close(fd);
}
void feedbackMainDevice(void* data, struct zwp_linux_dmabuf_feedback_v1*, struct wl_array* device) {
  auto* g = static_cast<Globals*>(data);
  if (device->size == sizeof(dev_t)) {
    std::memcpy(&g->mainDevice, device->data, sizeof(dev_t));
    g->haveMainDevice = true;
  }
}
void feedbackTrancheDone(void*, struct zwp_linux_dmabuf_feedback_v1*) {}
void feedbackTrancheTarget(void*, struct zwp_linux_dmabuf_feedback_v1*, struct wl_array*) {}
void feedbackTrancheFormats(void* data, struct zwp_linux_dmabuf_feedback_v1*, struct wl_array* indices) {
  auto* g = static_cast<Globals*>(data);
  const auto* index = static_cast<const std::uint16_t*>(indices->data);
  for (size_t i = 0; i < indices->size / sizeof(std::uint16_t); ++i) {
    if (index[i] < g->formatTable.size()) {
      g->noteFormat(g->formatTable[index[i]].first, g->formatTable[index[i]].second);
    }
  }
}
void feedbackTrancheFlags(void*, struct zwp_linux_dmabuf_feedback_v1*, std::uint32_t) {}
const struct zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
    feedbackDone,          feedbackFormatTable,    feedbackMainDevice,  feedbackTrancheDone,
    feedbackTrancheTarget, feedbackTrancheFormats, feedbackTrancheFlags};

/// The render node of the device the compositor composites with, so a buffer is
/// one it imports as it is; the first render node when it does not say.
int openRenderNode(const Globals& g) {
  if (g.haveMainDevice) {
    drmDevicePtr device = nullptr;
    if (drmGetDeviceFromDevId(g.mainDevice, 0, &device) == 0 && device != nullptr) {
      int fd = -1;
      if ((device->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
        fd = open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
      }
      if (fd < 0 && (device->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0) {
        fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      }
      drmFreeDevice(&device);
      if (fd >= 0) return fd;
    }
  }
  for (int minor = 128; minor < 136; ++minor) {
    const std::string path = "/dev/dri/renderD" + std::to_string(minor);
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) return fd;
  }
  return -1;
}

struct ParamsResult {
  struct wl_buffer* buffer = nullptr;
  bool done = false;
};
void paramsCreated(void* data, struct zwp_linux_buffer_params_v1*, struct wl_buffer* buffer) {
  auto* result = static_cast<ParamsResult*>(data);
  result->buffer = buffer;
  result->done = true;
}
void paramsFailed(void* data, struct zwp_linux_buffer_params_v1*) { static_cast<ParamsResult*>(data)->done = true; }
const struct zwp_linux_buffer_params_v1_listener kParamsListener = {paramsCreated, paramsFailed};

void bufferRelease(void* data, struct wl_buffer*) {
  auto* buffer = static_cast<Buffer*>(data);
  buffer->busy = false;
  buffer->owner->noteReleased();
}
const struct wl_buffer_listener kBufferListener = {bufferRelease};

void feedbackSyncOutput(void*, struct wp_presentation_feedback*, struct wl_output*) {}
void feedbackPresented(void* data, struct wp_presentation_feedback* feedback, std::uint32_t, std::uint32_t,
                       std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
  static_cast<VideoSurface*>(data)->notePresented();
  wp_presentation_feedback_destroy(feedback);
}
void feedbackDiscarded(void*, struct wp_presentation_feedback* feedback) { wp_presentation_feedback_destroy(feedback); }
const struct wp_presentation_feedback_listener kPresentationListener = {feedbackSyncOutput, feedbackPresented,
                                                                        feedbackDiscarded};

/// CPU access to a mapped dma-buf, bracketed as the kernel asks.
void syncDmabuf(int fd, std::uint64_t flags) {
  struct dma_buf_sync sync = {flags};
  while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && (errno == EINTR || errno == EAGAIN)) {
  }
}

}  // namespace

bool waylandAvailable(std::string& why) {
  SDL_Window* window = nullptr;
  struct wl_display* display = nullptr;
  struct wl_surface* surface = nullptr;
  return sdlWaylandWindow(window, display, surface, why);
}

void declareWindowOpaque(SDL_Window* window) {
  // The window is created with SDL_WINDOW_TRANSPARENT (runtime/linux/HostMain.cpp)
  // so that a video plane beneath it can be seen -- which also stops SDL from
  // declaring the surface opaque, and an entirely un-declared surface is one the
  // compositor must blend, and cannot scan out, even for a page with no video at
  // all. So the window says it is opaque here, at startup; a visible plane cuts
  // that region open and closes it again (updateParentRegion).
  if (window == nullptr) return;
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  auto* display = static_cast<struct wl_display*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  auto* surface = static_cast<struct wl_surface*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  int width = 0, height = 0;
  if (display == nullptr || surface == nullptr || !SDL_GetWindowSize(window, &width, &height) || width <= 0 ||
      height <= 0) {
    return;
  }
  // On a queue of this call's own, so nothing here is dispatched from SDL's.
  struct wl_event_queue* queue = wl_display_create_queue(display);
  if (queue == nullptr) return;
  auto* wrapper = static_cast<struct wl_display*>(wl_proxy_create_wrapper(display));
  wl_proxy_set_queue(reinterpret_cast<struct wl_proxy*>(wrapper), queue);
  {
    // Every proxy goes before the queue it was made on does.
    Globals globals;
    globals.wrapper = wrapper;
    globals.registry = wl_display_get_registry(wrapper);
    wl_registry_add_listener(globals.registry, &kRegistryListener, &globals);
    if (wl_display_roundtrip_queue(display, queue) >= 0 && globals.compositor != nullptr) {
      struct wl_region* region = wl_compositor_create_region(globals.compositor);
      wl_region_add(region, 0, 0, width, height);
      wl_surface_set_opaque_region(surface, region);
      wl_region_destroy(region);
      wl_surface_commit(surface);
      wl_display_flush(display);
    }
  }
  wl_event_queue_destroy(queue);
}

// ---- the surface ---------------------------------------------------------------------

std::unique_ptr<VideoSurface> VideoSurface::create(std::string& error) {
  std::unique_ptr<VideoSurface> self(new VideoSurface());
  if (!sdlWaylandWindow(self->window_, self->display_, self->parent_, error)) return nullptr;

  self->queue_ = wl_display_create_queue(self->display_);
  if (self->queue_ == nullptr) {
    error = "could not create a Wayland event queue";
    return nullptr;
  }
  self->globals_ = std::make_unique<Globals>();
  Globals& g = *self->globals_;
  // A wrapper of the display on this queue: the registry, and so every global
  // bound through it and every object those make, deliver events here only.
  g.wrapper = static_cast<struct wl_display*>(wl_proxy_create_wrapper(self->display_));
  wl_proxy_set_queue(reinterpret_cast<struct wl_proxy*>(g.wrapper), self->queue_);
  g.registry = wl_display_get_registry(g.wrapper);
  wl_registry_add_listener(g.registry, &kRegistryListener, &g);
  if (wl_display_roundtrip_queue(self->display_, self->queue_) < 0) {
    error = "the Wayland connection failed while listing globals";
    return nullptr;
  }
  if (g.compositor == nullptr || g.subcompositor == nullptr || g.dmabuf == nullptr || g.viewporter == nullptr) {
    error = std::string("the compositor lacks what video needs:") + (g.compositor ? "" : " wl_compositor") +
            (g.subcompositor ? "" : " wl_subcompositor") + (g.dmabuf ? "" : " zwp_linux_dmabuf_v1 (v3+)") +
            (g.viewporter ? "" : " wp_viewporter");
    return nullptr;
  }
  if (g.dmabufVersion >= 4) {
    struct zwp_linux_dmabuf_feedback_v1* feedback = zwp_linux_dmabuf_v1_get_default_feedback(g.dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedbackListener, &g);
    for (int i = 0; i < 10 && !g.feedbackDone; ++i) {
      if (wl_display_roundtrip_queue(self->display_, self->queue_) < 0) break;
    }
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
  } else {
    zwp_linux_dmabuf_v1_add_listener(g.dmabuf, &kDmabufListener, &g);
    wl_display_roundtrip_queue(self->display_, self->queue_);
  }
  if (!g.nv12Linear && !g.yu12Linear) {
    error = "the compositor imports no linear NV12 or YU12 dma-buf";
    return nullptr;
  }

  self->drmFd_ = openRenderNode(g);
  if (self->drmFd_ < 0) {
    error = std::string("no DRM render node to allocate video buffers on: ") + std::strerror(errno);
    return nullptr;
  }
  self->gbm_ = gbm_create_device(self->drmFd_);
  if (self->gbm_ == nullptr) {
    error = "gbm_create_device failed on the compositor's device";
    return nullptr;
  }

  self->surface_ = wl_compositor_create_surface(g.compositor);
  // Video never takes input: the app's window, above it, does.
  struct wl_region* empty = wl_compositor_create_region(g.compositor);
  wl_surface_set_input_region(self->surface_, empty);
  wl_region_destroy(empty);
  self->subsurface_ = wl_subcompositor_get_subsurface(g.subcompositor, self->surface_, self->parent_);
  wl_subsurface_set_desync(self->subsurface_);
  wl_subsurface_place_below(self->subsurface_, self->parent_);
  self->viewport_ = wp_viewporter_get_viewport(g.viewporter, self->surface_);
  wl_surface_commit(self->surface_);
  // The subsurface exists, below the window, from the parent's next commit.
  updateParentRegion(self.get(), false, {0, 0, 0, 0}, self->window_, g.compositor, self->parent_, self->display_,
                     true);
  log(LogLevel::Log, kTag,
      std::string("video plane: a subsurface below the window, dma-bufs in ") +
          (g.yu12Linear ? "YU12" : "") + (g.yu12Linear && g.nv12Linear ? "/" : "") + (g.nv12Linear ? "NV12" : "") +
          " linear" + (g.presentation ? ", presentation feedback" : ""));
  return self;
}

VideoSurface::~VideoSurface() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (globals_ && globals_->compositor != nullptr && parent_ != nullptr) {
    // The window's opaque region whole again, before the plane goes.
    updateParentRegion(this, false, {0, 0, 0, 0}, window_, globals_->compositor, parent_, display_, false);
  }
  releaseBuffers();
  if (viewport_ != nullptr) wp_viewport_destroy(viewport_);
  if (subsurface_ != nullptr) wl_subsurface_destroy(subsurface_);
  if (surface_ != nullptr) wl_surface_destroy(surface_);
  if (display_ != nullptr && surface_ != nullptr) {
    // The subsurface's removal is immediate; the requests have to leave.
    wl_display_flush(display_);
  }
  globals_.reset();
  if (gbm_ != nullptr) gbm_device_destroy(gbm_);
  if (drmFd_ >= 0) close(drmFd_);
  if (queue_ != nullptr) wl_event_queue_destroy(queue_);
}

void VideoSurface::setPlane(const PlaneRect& rect, bool visible) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  plane_ = rect;
  planeVisible_ = visible;
  planeDirty_ = true;
}

void VideoSurface::setPicture(int width, int height, double sampleAspect) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (width <= 0 || height <= 0) return;
  const double aspect = sampleAspect > 0 ? sampleAspect : 1.0;
  if (width == pictureWidth_ && height == pictureHeight_ && aspect == aspect_) return;
  pictureWidth_ = width;
  pictureHeight_ = height;
  aspect_ = aspect;
  planeDirty_ = true;  // the fit depends on the picture's shape
}

void VideoSurface::releaseBuffers() {
  buffers_.clear();
  scratch_.reset();
  last_ = nullptr;
  attached_ = false;
  width_ = height_ = stride_ = 0;
}

bool VideoSurface::makeWlBuffer(Buffer& buffer) {
  Globals& g = *globals_;
  struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(g.dmabuf);
  const int planes = nv12_ ? 2 : 3;
  for (int p = 0; p < planes; ++p) {
    zwp_linux_buffer_params_v1_add(params, buffer.fd, static_cast<std::uint32_t>(p), buffer.offsets[p],
                                   buffer.strides[p], static_cast<std::uint32_t>(DRM_FORMAT_MOD_LINEAR >> 32),
                                   static_cast<std::uint32_t>(DRM_FORMAT_MOD_LINEAR & 0xffffffff));
  }
  ParamsResult result;
  zwp_linux_buffer_params_v1_add_listener(params, &kParamsListener, &result);
  zwp_linux_buffer_params_v1_create(params, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_),
                                    nv12_ ? DRM_FORMAT_NV12 : DRM_FORMAT_YUV420, 0);
  for (int tries = 0; tries < 20 && !result.done; ++tries) {
    if (wl_display_roundtrip_queue(display_, queue_) < 0) break;
  }
  zwp_linux_buffer_params_v1_destroy(params);
  if (result.buffer == nullptr) {
    log(LogLevel::Error, kTag,
        std::string("the compositor refused a ") + (nv12_ ? "NV12" : "YU12") + " dma-buf of " +
            std::to_string(width_) + "x" + std::to_string(height_));
    return false;
  }
  buffer.buffer = result.buffer;
  wl_buffer_add_listener(buffer.buffer, &kBufferListener, &buffer);
  return true;
}

bool VideoSurface::configure(bool nv12, unsigned width, unsigned height, unsigned pitches[3], unsigned lines[3]) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (width == 0 || height == 0 || width > 8192 || height > 8192) return false;
  Globals& g = *globals_;
  // What VLC asked for when the compositor takes it, else the other.
  const bool useNv12 = nv12 ? g.nv12Linear : !g.yu12Linear;
  const unsigned chromaRows = (height + 1) / 2;
  auto answer = [&] {
    pitches[0] = stride_;
    lines[0] = height_;
    pitches[1] = nv12_ ? stride_ : stride_ / 2;
    lines[1] = chromaRows;
    pitches[2] = nv12_ ? 0 : stride_ / 2;
    lines[2] = nv12_ ? 0 : chromaRows;
  };
  if (!buffers_.empty() && useNv12 == nv12_ && width == width_ && height == height_) {
    answer();  // the same layout: the buffers are reused, the last picture stays up
    return true;
  }
  releaseBuffers();
  if (attached_ || appliedVisible_) {
    // The old picture's buffer goes: take it off the plane first.
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    attached_ = false;
  }
  nv12_ = useNv12;
  width_ = width;
  height_ = height;
  // One linear R8 buffer object holds every plane: GBM on a Raspberry Pi's vc4
  // cannot make an NV12 or YU12 object, but an R8 one is the same bytes, and the
  // dma-buf protocol describes each plane as an offset and a stride in it.
  const unsigned allocWidth = std::max(64u, static_cast<unsigned>(alignUp(static_cast<int>(width), 64)));
  const unsigned rows = height + chromaRows;
  for (int i = 0; i < kBufferCount; ++i) {
    auto buffer = std::make_unique<Buffer>();
    buffer->owner = this;
    buffer->bo = gbm_bo_create(gbm_, allocWidth, rows, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (buffer->bo == nullptr) {
      log(LogLevel::Error, kTag, "gbm_bo_create for a video buffer failed");
      releaseBuffers();
      return false;
    }
    // Exported read-write: gbm_bo_get_fd's fd is read-only, and VLC writes the
    // picture through a mapping of it.
    int fd = -1;
    if (drmPrimeHandleToFD(drmFd_, gbm_bo_get_handle(buffer->bo).u32, DRM_CLOEXEC | DRM_RDWR, &fd) != 0) fd = -1;
    buffer->fd = fd;
    const std::uint32_t s = gbm_bo_get_stride(buffer->bo);
    if (i == 0) stride_ = s;
    if (buffer->fd < 0 || s != stride_) {
      log(LogLevel::Error, kTag, "a video buffer has no dma-buf, or a stride unlike its siblings'");
      releaseBuffers();
      return false;
    }
    buffer->mapSize = static_cast<size_t>(s) * rows;
    void* map = mmap(nullptr, buffer->mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (map == MAP_FAILED) {
      log(LogLevel::Error, kTag, std::string("mapping a video buffer failed: ") + std::strerror(errno));
      releaseBuffers();
      return false;
    }
    buffer->map = static_cast<std::uint8_t*>(map);
    buffer->offsets[0] = 0;
    buffer->strides[0] = s;
    buffer->offsets[1] = s * height;
    buffer->strides[1] = nv12_ ? s : s / 2;
    buffer->offsets[2] = nv12_ ? 0 : s * height + (s / 2) * chromaRows;
    buffer->strides[2] = nv12_ ? 0 : s / 2;
    if (!makeWlBuffer(*buffer)) {
      releaseBuffers();
      return false;
    }
    buffers_.push_back(std::move(buffer));
  }
  // Where a picture goes when the compositor still holds every buffer: copied,
  // never shown.
  scratch_ = std::make_unique<Buffer>();
  scratch_->owner = this;
  scratch_->scratch.resize(static_cast<size_t>(stride_) * rows);
  std::copy(std::begin(buffers_[0]->offsets), std::end(buffers_[0]->offsets), scratch_->offsets);
  std::copy(std::begin(buffers_[0]->strides), std::end(buffers_[0]->strides), scratch_->strides);
  if (pictureWidth_ <= 0) {
    pictureWidth_ = static_cast<int>(width);
    pictureHeight_ = static_cast<int>(height);
  }
  planeDirty_ = true;
  answer();
  log(LogLevel::Log, kTag,
      std::string("video buffers: ") + std::to_string(kBufferCount) + " x " + (nv12_ ? "NV12 " : "YU12 ") +
          std::to_string(width) + "x" + std::to_string(height) + ", stride " + std::to_string(stride_));
  return true;
}

void VideoSurface::unconfigure() {
  // VLC is done with this layout, not necessarily with the picture on screen: the
  // buffers stay, so the last picture does, until a new layout replaces them.
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& buffer : buffers_) buffer->locked = false;
  if (scratch_) scratch_->locked = false;
}

void* VideoSurface::lock(void* planes[3]) {
  std::unique_lock<std::recursive_mutex> lock(mutex_);
  auto pick = [&]() -> Buffer* {
    for (auto& buffer : buffers_) {
      if (!buffer->busy && !buffer->locked && buffer.get() != last_) return buffer.get();
    }
    return nullptr;
  };
  Buffer* buffer = pick();
  if (buffer == nullptr && !buffers_.empty()) {
    // The compositor releases a buffer once the next one is on screen: wait a
    // little for that, as a frame's worth of time is still ahead of this one.
    released_.wait_for(lock, std::chrono::milliseconds(20), [&] { return pick() != nullptr; });
    buffer = pick();
  }
  if (buffer == nullptr) buffer = scratch_.get();
  if (buffer == nullptr) {
    planes[0] = planes[1] = planes[2] = nullptr;
    return nullptr;
  }
  buffer->locked = true;
  std::uint8_t* base = buffer->base();
  planes[0] = base + buffer->offsets[0];
  planes[1] = base + buffer->offsets[1];
  planes[2] = nv12_ ? nullptr : base + buffer->offsets[2];
  if (buffer->map != nullptr) syncDmabuf(buffer->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
  return buffer;
}

void VideoSurface::unlock(void* picture) {
  auto* buffer = static_cast<Buffer*>(picture);
  if (buffer == nullptr) return;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (buffer->map != nullptr) syncDmabuf(buffer->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

void VideoSurface::display(void* picture) {
  auto* buffer = static_cast<Buffer*>(picture);
  if (buffer == nullptr) return;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  buffer->locked = false;
  if (buffer == scratch_.get()) {
    dropped_.fetch_add(1);
    return;
  }
  last_ = buffer;
  applyPlane();
  if (!appliedVisible_) return;  // shown when the plane is
  showLast();
}

void VideoSurface::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (attached_) {
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    wl_display_flush(display_);
  }
  releaseBuffers();
  pictureWidth_ = pictureHeight_ = 0;
  aspect_ = 1.0;
  planeDirty_ = true;
}

void VideoSurface::showLast() {
  if (last_ == nullptr || last_->buffer == nullptr) return;
  wl_surface_attach(surface_, last_->buffer, 0, 0);
  wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
  if (globals_->presentation != nullptr) {
    struct wp_presentation_feedback* feedback = wp_presentation_feedback(globals_->presentation, surface_);
    wp_presentation_feedback_add_listener(feedback, &kPresentationListener, this);
  }
  wl_surface_commit(surface_);
  wl_display_flush(display_);
  last_->busy = true;
  attached_ = true;
  const std::uint64_t committed = committed_.fetch_add(1) + 1;
  // SCREENKIT_MEDIA_TRACE: what the compositor did with them. `presented` counts
  // wp_presentation's own answer, so a plane that is committed and never
  // presented -- the picture decoded and nothing on screen -- says so here
  // rather than only on the television.
  if (traceEnabled() && committed % 120 == 0) {
    log(LogLevel::Log, kTag,
        "video plane: " + std::to_string(committed) + " committed, " + std::to_string(presented_.load()) +
            " presented, " + std::to_string(dropped_.load()) + " dropped, " + std::to_string(appliedWidth_) + "x" +
            std::to_string(appliedHeight_) + " at " + std::to_string(appliedX_) + "," + std::to_string(appliedY_));
  }
}

void VideoSurface::dispatch(int timeoutMs) {
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    applyPlane();
  }
  // The multi-queue read: prepare on this queue, and whichever thread reads the
  // socket sorts the events into their queues; only this queue is dispatched
  // here, under the lock the vout thread takes too.
  if (wl_display_prepare_read_queue(display_, queue_) != 0) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    wl_display_dispatch_queue_pending(display_, queue_);
    return;
  }
  wl_display_flush(display_);
  struct pollfd fd = {wl_display_get_fd(display_), POLLIN, 0};
  const int ready = poll(&fd, 1, timeoutMs);
  if (ready > 0 && (fd.revents & POLLIN) != 0) {
    wl_display_read_events(display_);
  } else {
    wl_display_cancel_read(display_);
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  wl_display_dispatch_queue_pending(display_, queue_);
}

void VideoSurface::applyPlane() {
  if (!planeDirty_) return;
  planeDirty_ = false;
  Globals& g = *globals_;
  bool visible = planeVisible_;
  const VideoHost host = videoHost();
  int pixelWidth = 0, pixelHeight = 0, logicalWidth = 0, logicalHeight = 0;
  if (host.window != window_ || !SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight) ||
      !SDL_GetWindowSize(window_, &logicalWidth, &logicalHeight) || pixelWidth <= 0 || logicalWidth <= 0) {
    visible = false;
  }
  if (visible) {
    PlaneRect box = planeInWindow(plane_, pixelWidth, pixelHeight, host.drawableWidth, host.drawableHeight);
    if (pictureWidth_ > 0 && pictureHeight_ > 0) {
      box = fitContain(box, static_cast<int>(std::lround(pictureWidth_ * aspect_)), pictureHeight_);
    }
    const double sx = static_cast<double>(logicalWidth) / pixelWidth;
    const double sy = static_cast<double>(logicalHeight) / pixelHeight;
    const int x = static_cast<int>(std::floor(box.x * sx));
    const int y = static_cast<int>(std::floor(box.y * sy));
    const int width = static_cast<int>(std::ceil((box.x + box.width) * sx)) - x;
    const int height = static_cast<int>(std::ceil((box.y + box.height) * sy)) - y;
    if (width > 0 && height > 0) {
      const bool moved = x != appliedX_ || y != appliedY_ || !appliedVisible_;
      if (moved) wl_subsurface_set_position(subsurface_, x, y);
      // A viewport destination of zero is a protocol error: checked above. The
      // source is the picture, when the buffers are padded beyond it.
      if (width_ > 0 && pictureWidth_ > 0 && (static_cast<unsigned>(pictureWidth_) < width_ ||
                                              static_cast<unsigned>(pictureHeight_) < height_)) {
        wp_viewport_set_source(viewport_, 0, 0, wl_fixed_from_int(pictureWidth_), wl_fixed_from_int(pictureHeight_));
      } else {
        wp_viewport_set_source(viewport_, wl_fixed_from_int(-1), wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1));
      }
      wp_viewport_set_destination(viewport_, width, height);
      const bool wasVisible = appliedVisible_;
      appliedX_ = x;
      appliedY_ = y;
      appliedWidth_ = width;
      appliedHeight_ = height;
      appliedVisible_ = true;
      if (!wasVisible && last_ != nullptr) {
        showLast();  // shown again with no new picture: the last one back
      } else {
        wl_surface_commit(surface_);
      }
      updateParentRegion(this, true, {x, y, width, height}, window_, g.compositor, parent_, display_, moved);
      return;
    }
  }
  if (appliedVisible_ || attached_) {
    // Hidden: no buffer, so the subsurface is unmapped until shown again.
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    attached_ = false;
  }
  appliedVisible_ = false;
  updateParentRegion(this, false, {0, 0, 0, 0}, window_, g.compositor, parent_, display_, false);
}

}  // namespace screenkit::media::wl
