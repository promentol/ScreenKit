// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

class CanvasBinding;

/// Install `__screenkit.canvas`: the narrow handle API behind a `<canvas>` that
/// is **not** the page's frame (runtime/js/README.md, "Canvases").
///
///   create(element, width, height) -> {id, gl} | null
///                                   one real GL context in the page's share
///                                   group, drawing into a layer of its own
///   setSize(id, width, height)      the drawing buffer follows `canvas.width`
///   setPlane(id, x, y, w, h, visible, order, opacity)
///                                   where the layer composites, in drawable px
///
/// The first canvas of a page is the drawable itself and never appears here: it
/// gets the context the graphics bootstrap made, the window's framebuffer and
/// the present path a page has had since M4, so a page with one canvas pays
/// nothing for this binding existing (Architecture.md 3.1).
///
/// Every canvas after it gets a **real GL context of its own** -- its own bound
/// program, textures and blend state, as WebGL says -- and an FBO-backed layer
/// the page's own present composites at the element's CSS rect, through the same
/// `LayerList` an `<iframe>` instance's layer goes through. Both produce on this
/// thread, so a canvas layer publishes with no fence.
///
/// `create` answers **null** when this page cannot composite one: a headless
/// runtime with no drawable, a platform whose compositor will not build, or a
/// driver that will not give another context. `getContext` then returns null, as
/// the web allows, rather than throwing.
///
/// A context is released when the element is collected -- native state on the
/// element, as an `<iframe>`'s instance is -- or at shutdown with every other
/// context of the runtime (`gfx::releaseVendoredWebGL`). There is deliberately
/// no `release(id)` for JS to call: `__screenkit` is reachable from page script,
/// and a page able to name another canvas's id could pull the context out from
/// under a live handle. Dropping the element is the only way, as on the web.
///
/// `__screenkit` must already exist (installHostIO). JS thread only.
std::shared_ptr<CanvasBinding> installCanvas(facebook::jsi::Runtime& runtime,
                                             std::shared_ptr<JsExecutor> executor);

/// Drop every canvas layer this page made, while the runtime is still alive and
/// on the thread its contexts are current on. JS thread only; idempotent.
void shutdownCanvas(CanvasBinding& binding);

}  // namespace screenkit
