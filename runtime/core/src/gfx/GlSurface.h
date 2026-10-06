// Copyright (c) ScreenKit contributors. MIT.
//
// The drawable. SDL3 owns the window; who owns the context depends on the
// platform, and so does the file that implements this header:
//
//   Apple   GlSurface.cpp     ANGLE over Metal, with a `CAMetalLayer` as the seam
//                             between SDL's window and ANGLE's EGL (Architecture.md 9)
//   Linux   GlSurfaceSdl.cpp  the system's GLES through SDL's own EGL context, on the
//                             window SDL made -- on a Raspberry Pi 3, Mesa's VideoCore
//                             IV driver, which offers GLES 2.0 alone
//   Android GlSurfaceSdl.cpp  the same, on the device's own GLES: ES 3 where there is
//                             one, else ES 2, since no ANGLE exists for Android
//
// On Apple, SDL does not and cannot create this context. Its UIKit video backend has no
// EGL implementation at all -- `src/video/uikit/` contains zero `SDL_EGL`
// references, the backend is EAGL and Metal only -- and even on a backend that
// did, `SDL_EGL` reaches EGL by `SDL_LoadObject`ing `libEGL.dylib`, while our
// ANGLE is a static archive with no dylib to open. Both reasons are
// independently fatal, so the EGL objects are created here against SDL's Metal
// layer and SDL keeps doing what it is good at: the window, the lifecycle and
// the frame tick.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../compositor/Compositor.h"

namespace screenkit {
namespace gfx {

/// One GL context and the surface it draws to: an EGL display, config, context
/// and window surface over ANGLE's Metal backend on Apple; SDL's GL context on
/// its window on Linux.
///
/// **Thread affinity.** An EGL context is current on one thread at a time, and
/// this runtime's rule is that every GL call happens on the JS thread inside the
/// executor discipline. `create()` makes the context current on the thread that
/// calls it, so it must be called *from the JS thread*, and the object must be
/// destroyed there too. Host teardown does that explicitly
/// (`releaseVendoredWebGL`), and `~GlSurface` logs a warning when it runs on any
/// other thread -- ownership alone did not guarantee it: a HostObject destructor
/// runs on Hermes' GC thread.
///
/// **Displays and sharing.** ANGLE hands every surface the same display, which
/// is reference-counted here so the last surface, not the first, terminates it.
/// A surface created with `shareWith` joins that surface's share group: textures,
/// buffers and programs made in either context are usable in both, from their
/// own threads -- what one instance compositing another's output needs.
class GlSurface {
 public:
  struct Desc {
    /// Apple: `CAMetalLayer*`, from `SDL_Metal_GetLayer(SDL_Metal_CreateView(window))`.
    /// Null asks for an offscreen pbuffer instead, which is how the headless
    /// test rows get a context without a window server.
    ///
    /// Linux and Android: the `SDL_Window*` itself, created with
    /// `SDL_WINDOW_OPENGL` after `prepareWindowAttributes()`. There is no
    /// offscreen surface there yet.
    void* nativeLayer = nullptr;

    /// Pbuffer dimensions. Ignored when `nativeLayer` is set -- a window surface
    /// takes its size from the layer.
    int width = 1;
    int height = 1;

    /// A live surface whose share group the new context joins. It must stay
    /// alive until `create()` returns; after that either may go first.
    const GlSurface* shareWith = nullptr;

    /// Linux and Android: draw at exactly this size, whatever the window's. The app draws
    /// into an offscreen framebuffer of this size -- its default framebuffer --
    /// and each present scales it into the window, centred, aspect kept, black
    /// around it. Zero draws at the window's own size. (On Apple a fixed size is
    /// a window of that size instead, and this is ignored.)
    int fixedWidth = 0;
    int fixedHeight = 0;
  };

  /// Creates the display, config, context and surface and makes them current.
  /// Returns nullptr and fills `error` on any failure; there is deliberately no
  /// software fallback, because a silent one would let the ANGLE-Metal
  /// requirement regress unnoticed.
  static std::unique_ptr<GlSurface> create(const Desc& desc, std::string& error);

  /// An **offscreen layer surface**: what one `<iframe>` instance -- or one
  /// `<canvas>` that is not the page's frame -- draws into.
  ///
  /// A context in `parent`'s share group with a colour texture of its own, which
  /// the parent composites at the element's CSS rect. There is no window and no
  /// present: `swap()` here finishes into the texture and signals, rather than
  /// putting anything on screen.
  ///
  /// **Call on `parent`'s own thread**, with its context current -- SDL's
  /// sharing is spelled "share with the context current on this thread", so
  /// there is nowhere else it could be made. The context is left bound to no
  /// thread; the instance's JS thread calls `adopt()` to take it.
  static std::unique_ptr<GlSurface> createShared(const GlSurface& parent, int width, int height,
                                                 std::string& error);

  /// Take a surface `createShared` made: bind its context to the calling thread
  /// -- the instance's JS thread, which owns every GL call it will ever make --
  /// and build the framebuffer it draws into. False, with `error`, when the
  /// framebuffer is incomplete, which is the platform saying it has no
  /// composite path.
  bool adopt(std::string& error);

  /// What a window needs to be asked for before it is created, so the surface
  /// SDL makes with it matches the context `create()` will ask for: the colour,
  /// depth and stencil sizes and the GLES profile. A no-op on Apple, where the
  /// context is not SDL's. Main thread, before `SDL_CreateWindow`.
  static void prepareWindowAttributes();

  ~GlSurface();

  GlSurface(const GlSurface&) = delete;
  GlSurface& operator=(const GlSurface&) = delete;

  /// Re-bind the context to the calling thread. Only needed if something else
  /// made a different context current; `create()` already did it once.
  bool makeCurrent();

  /// Present. One call per frame, after the loop has serviced `rAF`.
  bool swap();

  /// SDL surface only (GlSurfaceSdl.cpp). The window's drawable is about to be
  /// destroyed -- Android takes an app's EGL window surface away when it goes to
  /// the background -- so let go of it: nothing is current on this thread
  /// afterwards. The context and every object JS made in it stay. Call on the
  /// thread the context is current on, with nothing about to draw (the host
  /// pauses the runtime first); `swap()` presents nothing until `resume()`.
  void suspend();

  /// SDL surface only. The window has a drawable again: make the context current
  /// on it, and -- when the frame is drawn offscreen (a fixed size) -- present
  /// the last frame again, so the screen is not blank until the app next paints.
  /// Same thread as `suspend()`. False, logged, when the context cannot be bound.
  bool resume();

  /// A window surface whose layer is not the size it had at the last present.
  /// ANGLE keeps drawing into a drawable it took before a resize until that one
  /// is presented, so a frame drawn now would land in the old size.
  bool resizedSincePresent() const;

  /// Live drawable dimensions, never cached.
  ///
  /// A window surface reports its layer's pixel size, not `eglQuerySurface`.
  /// ANGLE's Metal surface adopts a new layer size only when it takes its next
  /// drawable -- the first draw after a swap -- and EGL reports the old size
  /// until then. So after a resize EGL lagged at least one drawn frame, and never
  /// caught up for an app that was idle: measured, a 320x200 window resized to
  /// 480x300 still reported 320x200 after a presented frame. The layer's size is
  /// what that next drawable will be (gl-drawing-buffer-resize). A pbuffer never
  /// resizes, so it reports EGL's size.
  int width() const;
  int height() const;

  /// `GL_VENDOR` / `GL_RENDERER` / `GL_VERSION`, read once at creation. The
  /// renderer string is the evidence that ANGLE's Metal backend is what we got
  /// rather than some other backend chosen by default.
  const std::string& vendor() const { return vendor_; }
  const std::string& renderer() const { return renderer_; }
  const std::string& version() const { return version_; }

  /// True when this surface draws into a pbuffer rather than a layer. A pbuffer
  /// swap is a no-op, so callers that log frame rates can say so.
  bool offscreen() const { return offscreen_; }

  /// The framebuffer WebGL's `null` binding means: 0, or the offscreen one a
  /// fixed size -- or a compositing host, or an instance's layer -- draws into.
  unsigned defaultFramebuffer() const { return framebuffer_; }

  /// The colour texture behind `defaultFramebuffer()`, 0 when the frame goes
  /// straight to the window. This is what the compositor samples, and it is why
  /// it is public rather than private: an instance's frame *is* this texture.
  unsigned colorTexture() const { return colorTexture_; }

  /// Turn this window surface into a compositing one: the page's frame becomes
  /// an offscreen texture and each present draws that texture and then every
  /// instance layer over it.
  ///
  /// Why the frame has to go offscreen: after a swap the window's back buffer is
  /// undefined, so a frame in which a child painted and the page did not has
  /// nothing of the page left to re-present. With the page in a texture, the
  /// present recomposes both without the page having to redraw.
  ///
  /// Idempotent, and **not** done by default -- a page with no iframe keeps
  /// exactly the present path it had. False, with `error`, when the framebuffer
  /// or the layer program will not build: the platform has no composite path,
  /// which is what makes `src` fail with a documented error rather than a blank
  /// element. Owning thread, between frames.
  bool enableCompositing(std::string& error);
  bool compositing() const { return layerPass_ != nullptr; }

  /// True for a surface that *is* a layer -- an `<iframe>` instance's frame or
  /// a canvas's. One level of nesting, so a layer composites nothing of its own,
  /// and callers say so in their own words rather than passing the refusal on.
  bool isLayer() const { return layerSurface_; }

  /// The layers this surface's present composites. Set once, by the instance
  /// binding that owns the list; shared, because the list is written on this
  /// thread and read inside `swap()`. Owning thread.
  void setLayers(std::shared_ptr<LayerList> layers);

  /// A layer has a frame this surface has not presented. False when there are no
  /// layers at all, which is what keeps `presentFrame`'s decision -- and its
  /// cost -- unchanged for a page with no iframe.
  bool layersDirty() const;

  /// Where a layer's finished frame is published. Set on a surface
  /// `createShared` made, before its runtime draws anything: `swap()` then
  /// signals here instead of presenting. Owning thread.
  ///
  /// `sameThread` is a producer that runs on the thread that composites -- a
  /// canvas of this very page, as opposed to an `<iframe>` instance's own
  /// thread. There is nothing to synchronise between two contexts driven in
  /// order by one thread, so the frame is published with a null fence and a
  /// flush, which `LayerSource` already accepts.
  void setLayerSource(std::shared_ptr<LayerSource> source, bool sameThread = false);

  /// Give a layer surface a new drawing-buffer size, keeping the framebuffer's
  /// name -- what WebGL's `null` binding resolves to -- and its colour texture's,
  /// which the compositor is holding. `canvas.width = 640` on a placed canvas is
  /// this. Owning thread, with this surface's context current; false with
  /// `error` when the framebuffer will not build at that size.
  bool resizeLayer(int width, int height, std::string& error);

 private:
  GlSurface() = default;

  // EGLDisplay, EGLSurface and EGLContext are all `void*`, so the EGL headers
  // stay out of this header and out of everything that includes it.
  void* display_ = nullptr;
  void* surface_ = nullptr;
  void* context_ = nullptr;
  void* config_ = nullptr;
  void* layer_ = nullptr;  // the CAMetalLayer under a window surface; not owned
  bool offscreen_ = false;
  /// Between `suspend()` and `resume()`: the window has no drawable to present to.
  bool suspended_ = false;
  /// The thread that created the context, and so has it current. An `SDL_ThreadID`.
  std::uint64_t owner_ = 0;
  /// The layer's pixel size at the last present (or at creation).
  int presentedWidth_ = 0;
  int presentedHeight_ = 0;

  /// The framebuffer the app draws into, its colour texture and depth-stencil
  /// buffer, and the second context -- sharing the texture -- that puts it in
  /// the window: its own GL state, so presenting never disturbs the app's.
  ///
  /// Three things arrive here. A fixed size (Linux, Android) scales the frame
  /// into the window. `enableCompositing()` on a host draws the frame 1:1 and
  /// then the instance layers over it. `createShared` on an instance makes the
  /// same framebuffer with no window and no present at all -- its texture is
  /// what the host composites.
  ///
  /// `fixedWidth_`/`fixedHeight_` is the *logical* drawable: a letterboxed fixed
  /// size, or an instance layer's own size, and zero for a surface that draws at
  /// the window's. `frameWidth_`/`frameHeight_` is what the texture is actually
  /// allocated at, which is how a window resize is noticed at the next present.
  int fixedWidth_ = 0;
  int fixedHeight_ = 0;
  int frameWidth_ = 0;
  int frameHeight_ = 0;
  unsigned framebuffer_ = 0;
  unsigned colorTexture_ = 0;
  unsigned depthStencil_ = 0;
  void* presentContext_ = nullptr;
  unsigned presentProgram_ = 0;
  unsigned presentBuffer_ = 0;

  /// This surface is one layer -- an instance's, or a canvas's: offscreen, never
  /// presented, and its finished frame is published to `layerSource_` instead.
  bool layerSurface_ = false;
  /// The layer's producer is the thread that composites it, so the publish
  /// needs a flush rather than a fence.
  bool layerSameThread_ = false;
  /// Set between `createShared` and `adopt`: the framebuffer is still to be
  /// made, on the thread that adopts it.
  bool pendingAdopt_ = false;
  std::shared_ptr<LayerSource> layerSource_;

  /// A compositing host: the instance layers its present draws over the frame,
  /// and the program that draws them. Both null until `enableCompositing`.
  std::shared_ptr<LayerList> layers_;
  std::unique_ptr<CompositePass> layerPass_;

  std::string vendor_;
  std::string renderer_;
  std::string version_;
};

}  // namespace gfx
}  // namespace screenkit
