// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {
namespace gfx {

class GlSurface;
class LayerList;

/// A context id, as the vendored tree spells it: `gl.contextId`, and the key
/// `gl::ContextGet` answers. 0 is "no context".
using GlContextId = unsigned;

/// Install a vendored expo-gl rendering context for `surface` on `runtime`.
///
/// This is the seam the vendoring spec deliberately left open. The vendored tree
/// looks a context up by id through `screenkit::gl::ContextGet`, because upstream
/// owns contexts in a registry it also owns the *drawable* lifetime of. We do not:
/// `GlSurface` owns the EGL display, context and surface. So the registry here
/// holds only the vendored bookkeeping object and answers the lookup; it never
/// creates or destroys anything GL.
///
/// Returns the context's id -- the handle everything else here is keyed by -- or
/// 0 when it could not be installed. The context object itself is parked in
/// `__SKGLContexts[id]`, where `takeContextObject` collects it.
///
/// `asGlobal` binds it as the `gl` global as well, which is what the page's own
/// drawable wants and what the DOM shim reads: the first canvas of a page is the
/// frame (M6). A second canvas's context is installed with `asGlobal` false, so
/// nothing about the page's own `gl` changes when one appears.
///
/// Must be called on the thread that owns `runtime`, with `surface` current.
GlContextId installVendoredWebGL(facebook::jsi::Runtime& runtime,
                                 const std::shared_ptr<GlSurface>& surface,
                                 bool asGlobal = true);

/// Take `__SKGLContexts[id]` -- the JS object every vendored GL method hangs
/// off -- out of that map and hand it back. Undefined when the id is unknown.
///
/// Upstream parks every context there for the life of the runtime. For the
/// page's own `gl` that is harmless; for a canvas layer it is a root the
/// collector can never get past, and the whole point of a canvas layer is that
/// dropping the canvas frees it. Nothing reads the map -- every vendored method
/// resolves `this.contextId` through `gl::ContextGet` -- so taking a context out
/// of it costs nothing and makes the canvas, its context and its layer one
/// collectable group. Owning thread.
facebook::jsi::Value takeContextObject(facebook::jsi::Runtime& runtime, GlContextId id);

/// The id of the context that draws into the page's own drawable -- the first
/// one installed on this runtime, the one bound as `gl` -- or 0.
GlContextId primaryContext(facebook::jsi::Runtime& runtime);

/// Make `id`'s context current on the calling thread, unless it already is.
///
/// The one place a page that draws on several canvases pays anything: every
/// vendored GL call resolves its context through `gl::ContextGet`, which does
/// this comparison and switches only when the page has moved between canvases.
/// Everything here that touches GL outside a vendored call goes through this
/// too, so the "which context is current" bookkeeping has exactly one owner.
bool makeContextCurrent(GlContextId id);

/// Forget what this thread has current, so the next `makeContextCurrent` binds
/// for real rather than believing its own bookkeeping.
///
/// For the one caller that binds a context behind this module's back:
/// `GlSurface::adopt` makes its own context current itself (it has to -- it is
/// below this layer), so a surface destroyed between that and the next install
/// leaves the driver with nothing current while the bookkeeping still names the
/// page. Every failure path in `bindings/Canvas.cpp` after `adopt` calls this.
void forgetCurrentContext();

/// The surface `runtime`'s `gl` draws into, or null when none was installed.
/// How the instance binding reaches the host's context to make a child's in its
/// share group, without the host having to hand the surface around. Any thread;
/// the surface itself is still the owning thread's to touch.
///
/// This is the *page's frame* surface, not whichever context happens to be
/// current: a page with three canvases has three surfaces and only one of them
/// is the window.
std::shared_ptr<GlSurface> surfaceFor(facebook::jsi::Runtime& runtime);

/// The surface behind one context id, or null.
std::shared_ptr<GlSurface> surfaceFor(facebook::jsi::Runtime& runtime, GlContextId id);

/// The one layer list this runtime composites. A canvas's layer and an
/// `<iframe>` instance's layer go in the same list and sort together, so a page
/// may put a HUD canvas over a game it embeds (Architecture.md 3.1, 5). Created
/// on first use; released with the runtime's contexts.
std::shared_ptr<LayerList> layersFor(facebook::jsi::Runtime& runtime);

/// Turn `surface` into a compositing one and give it this runtime's layer list.
/// Idempotent, and never done by default -- a page with no second canvas and no
/// `<iframe>` keeps exactly the present path, and the cost, it had. False with
/// `error` when the platform has no composite path. Owning thread, between
/// frames.
bool enableCompositing(facebook::jsi::Runtime& runtime, GlSurface& surface, std::string& error);

/// How many GL contexts the registry holds, across every runtime in the
/// process. The suite's way of seeing that a canvas the page dropped took its
/// context with it, and that teardown took every one of them.
std::size_t contextCount();

/// Drop one context: its registry entry, its `__SKGLContexts` slot and, with
/// them, the surface nothing else holds. Call on the owning thread -- an EGL
/// context is thread-affine and the surface's destructor unbinds it.
void releaseContext(facebook::jsi::Runtime& runtime, GlContextId id);

/// The surface's default framebuffer changed -- it just became a compositing one
/// -- so WebGL's `null` binding has to mean the new one. Call on the owning
/// thread, between frames.
void refreshDefaultFramebuffer(facebook::jsi::Runtime& runtime, const GlSurface& surface);

/// The end of a frame: run the queued GL calls, then present if the frame painted.
///
/// Two things a browser does between a rAF callback and the screen, and both had
/// to be learned the hard way here:
///
///  1. **Run the queued calls.** The vendored context batches non-blocking calls
///     -- `clear`, `drawElements`, `uniform*` -- and executes them only when
///     something blocks (`getError`, `readPixels`) or JS calls `endFrameEXP()`.
///     A WebGL app does neither, so a frame of Lightning was still queued when
///     the host swapped and every presented image was an empty back buffer. The
///     M4 triangle hid this by calling `getError()` each frame.
///  2. **Present only a frame that painted.** WebGL composites a canvas only when
///     its drawing buffer was modified; an idle canvas keeps its last image.
///     Lightning draws only when its scene is dirty, so swapping every frame
///     presented undefined back buffers between its draws. The DOM shim sets
///     `__screenkitPainted` when a draw or clear targets the default framebuffer
///     (runtime/js/dom-shim.js, "Present only frames that painted"); this reads
///     and resets it. Without the shim the flag is absent and every frame
///     presents, which is what a bare GL bundle like the triangle expects.
///
/// Both are **per canvas**. Every other context this runtime owns is one
/// canvas's own layer: its frame is finished into its own texture and published
/// for the compositor here, and only when that canvas painted
/// (`__screenkitLayerPainted[id]`), so an idle canvas keeps its last image and
/// costs nothing.
///
/// Returns whether it swapped. Call on the thread that owns `runtime` -- that
/// thread is the GL thread.
///
/// `beforePresent`, when given, runs after the frame's GL work and before the
/// swap, and only for a frame that painted (FrameCapture reads the frame there).
bool presentFrame(facebook::jsi::Runtime& runtime, GlSurface& surface,
                  const std::function<void()>& beforePresent = nullptr);

/// The window resized: present and let go of any drawable taken before it, so
/// the page's next paint -- usually its answer to `resize` -- takes one of the
/// new size instead of drawing into the old one and staying there once the page
/// goes idle. Swapping with no drawable held presents nothing (ANGLE's Metal
/// surface presents only a drawable it holds). Call on the thread that owns
/// `runtime`, before the page hears about the resize.
void releaseStaleDrawables(facebook::jsi::Runtime& runtime);

/// Drop every context installed on `runtime`, destroying a surface nothing else
/// holds. Host teardown calls it on the JS thread while the runtime is still
/// alive: EGL contexts are thread-affine, and left to the runtime's destructor
/// the release ran inside a HostObject destructor on Hermes' background GC
/// thread, unbinding and destroying a context that was current on another
/// thread. Safe to call twice.
void releaseVendoredWebGL(facebook::jsi::Runtime& runtime);

}  // namespace gfx
}  // namespace screenkit
