// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

namespace screenkit {

namespace gfx {
class GlSurface;
}

/// Install the `gl` global: the raw GLES entry points listed in `gl.def`,
/// expanded into host functions, plus the GL constants they take.
///
/// This is **not** `WebGLRenderingContext`. There are no wrapper objects, no
/// validation layer and no WebGL semantics -- JS sees the same integer handles
/// GL itself uses, and the object model that turns those into `WebGLShader` and
/// friends lives in JS above this (Architecture.md 2). Untrusted JS must never
/// be handed this global directly.
///
/// `surface` must already be current on the calling thread, and the calling
/// thread must be the one that owns `runtime` -- every GL call from JS lands on
/// it. The binding holds the surface alive, which is also what makes teardown
/// happen on the JS thread: the last reference dies when the runtime destroys
/// its host functions, which it does on its own thread.
void installWebGLBindings(facebook::jsi::Runtime& runtime,
                          std::shared_ptr<gfx::GlSurface> surface);

}  // namespace screenkit
