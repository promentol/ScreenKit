// Copyright (c) ScreenKit contributors. MIT.
//
// The layer tree and the composite pass: `<iframe>` instances drawn over the
// host page's own frame (Architecture.md 2, 5).
//
// **Where this runs.** `GlSurface`'s contract is that every GL call happens on
// the thread that owns the context (GlSurface.h), and the main thread only pumps
// SDL -- so this is not a main-thread pass. It runs at the *host* runtime's
// frame end, inside `GlSurface::swap()`, in the present context that already
// exists there: the app's own GL state is never disturbed, because the present
// context is a second context sharing only textures.
//
// **What a layer is.** One instance's colour texture, in the host's share group,
// plus where it goes in drawable pixels and where it sorts. The producer writes
// the texture on its own JS thread and signals; the compositor waits on that
// signal -- a fence where the platform has one, the producer's flush where it
// does not -- and never calls glFinish in the frame path.
//
// **A page with no iframe costs what it cost before.** An empty layer list means
// no program is compiled, no context is made and no extra present happens; the
// whole of this file is inert until the first instance appears.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace screenkit {
namespace gfx {

/// Where a layer goes, in drawable pixels -- the same coordinates
/// `media::PlaneRect` uses, and the same ones `planeFor` computes in the shim.
struct LayerRect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

/// What one instance produces, and the handshake that makes it safe to sample.
///
/// Shared between two JS threads: the instance's, which writes a frame and
/// signals, and the host's, which waits and samples. Everything here is under
/// the lock, and no GL object is created or destroyed by this class -- the
/// texture belongs to the instance's `GlSurface` and the fence to whoever
/// inserted it.
class LayerSource {
 public:
  /// The instance's frame is finished. `fence` is a `GLsync` the host waits on,
  /// or null when the platform has none and the producer flushed instead.
  /// Instance JS thread.
  void produced(unsigned texture, void* fence);

  /// The newest unconsumed fence, or null. Takes ownership: the caller waits on
  /// it and destroys it. Host JS thread.
  void* takeFence();

  /// The texture to sample, 0 until the instance's first frame.
  unsigned texture() const;

  /// A frame has been produced that the host has not presented yet. This is what
  /// makes the host present a frame its own page did not paint.
  bool dirty() const;
  void clearDirty();

 private:
  mutable std::mutex mutex_;
  unsigned texture_ = 0;
  void* fence_ = nullptr;
  bool dirty_ = false;
};

/// One composited layer, as the host's present sees it.
struct Layer {
  std::shared_ptr<LayerSource> source;
  LayerRect rect;
  double opacity = 1.0;
  /// `z-index`. Layers sort by this, then by insertion order -- the same rule
  /// the shim already applies between canvases and video planes.
  int order = 0;
  bool visible = false;
  /// Insertion order, the tie-break for an equal `order`.
  std::uint64_t sequence = 0;
};

/// Where a canvas layer's id starts.
///
/// There is **one** layer list per page (`gfx::layersFor`): a canvas's layer and
/// an `<iframe>` instance's layer sort together, so a page may put a HUD canvas
/// over a game it embeds. The two owners allocate ids independently -- the
/// instance binding counts from 1, a canvas layer is keyed by its GL context id
/// -- so the canvas half is offset into a range the instance half can never
/// reach.
constexpr std::uint64_t kCanvasLayerBase = std::uint64_t{1} << 62;

/// The host's layer list: add, update, remove, and read back in paint order.
///
/// Written from the page's JS thread (both the instance binding and the canvas
/// binding live there) and read from the same thread inside `swap()`. The lock
/// is for the one cross-thread reader that exists: `dirty()`, which the frame
/// boundary asks before deciding to present.
class LayerList {
 public:
  /// Add or replace the layer an instance id owns. A layer keeps the sequence
  /// it was first given, so updating a rect never reorders anything.
  void set(std::uint64_t id, Layer layer);
  void remove(std::uint64_t id);
  bool empty() const;

  /// Visible layers, sorted by `order` then insertion. Never includes a layer
  /// whose instance has not produced a frame yet -- there is nothing to sample.
  std::vector<Layer> paintOrder() const;

  /// Any layer has a frame the host has not presented. False for an empty list,
  /// which is what keeps a page with no iframe on exactly its old present path.
  bool dirty() const;
  void clearDirty();

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::uint64_t, Layer>> layers_;
  std::uint64_t nextSequence_ = 1;
};

/// The GL half: one program and one quad, drawing textured rects into whatever
/// framebuffer is bound.
///
/// Created and used on one thread, with the context it was created in current --
/// the host's present context, so the page's program, buffers and blend state
/// are never touched.
class CompositePass {
 public:
  /// Compiles the program. Null with `error` set when it will not compile, which
  /// is the "no compositor on this platform" case the spec refuses `src` for.
  static std::unique_ptr<CompositePass> create(std::string& error);
  ~CompositePass();

  CompositePass(const CompositePass&) = delete;
  CompositePass& operator=(const CompositePass&) = delete;

  /// Draw `layers` over whatever is already in the bound framebuffer.
  /// `drawableWidth`/`drawableHeight` are the pixel size the rects are in; the
  /// current viewport is where that space lands on screen, so a letterboxed
  /// present scales the layers exactly as it scales the frame.
  void draw(const std::vector<Layer>& layers, int drawableWidth, int drawableHeight);

 private:
  CompositePass() = default;

  unsigned program_ = 0;
  unsigned buffer_ = 0;
  int rectUniform_ = -1;
  int opacityUniform_ = -1;
};

/// A fence in the context that is current, or null when this platform has none
/// -- in which case the producer's work has been flushed instead, which is the
/// weaker ordering the spec allows. Never `glFinish`.
void* insertFenceOrFlush();

/// Make the current context wait for `fence`, then destroy it. A null fence is
/// a no-op: the producer flushed. Server-side; the CPU never blocks.
void waitAndDestroyFence(void* fence);

/// Destroy a fence nobody is going to wait on -- a layer removed before the
/// host sampled its last frame. Same share group, so the host's context may
/// delete what an instance's inserted. Null is a no-op.
void discardFence(void* fence);

}  // namespace gfx
}  // namespace screenkit
