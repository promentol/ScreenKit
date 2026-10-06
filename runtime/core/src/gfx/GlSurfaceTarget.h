// Copyright (c) ScreenKit contributors. MIT.
//
// The offscreen frame, shared by both `GlSurface` backends.
//
// Three callers want the same two objects -- a framebuffer whose colour
// attachment is a texture, and a quad program that puts that texture on screen:
// a fixed-size window (Linux, Android), a compositing host (Architecture.md 5),
// and one `<iframe>` instance's layer, which has the framebuffer and no present
// at all. Writing it once keeps the three from drifting apart, and keeps the
// GLSL at ES 1.00 so a GLES 2 context runs it.
//
// Every function here calls GL on the calling thread, with the context it is
// meant for current. Nothing here owns anything: the caller deletes what it made.
#pragma once

#include <string>

namespace screenkit {
namespace gfx {
namespace detail {

/// Create, or resize in place, the colour texture, the depth-stencil
/// renderbuffer and the framebuffer that ties them together. Pass zeros to
/// create; pass the ids back to resize, which keeps the framebuffer's name --
/// what WebGL's `null` binding resolves to -- stable across a window resize.
///
/// False, with `error`, when the framebuffer is incomplete. That is the platform
/// saying it has no offscreen path, which is a refusal rather than a fallback.
bool makeFrameTarget(int width, int height, unsigned& framebuffer, unsigned& colorTexture,
                     unsigned& depthStencil, std::string& error);

/// The program and quad that draw a frame texture over the current viewport.
/// False, with `error`, when it will not compile or link.
bool makePresentProgram(unsigned& program, unsigned& buffer, std::string& error);

/// Draw `texture` over the whole of the current viewport with that program.
/// Leaves blending disabled, which is what a frame -- as opposed to a layer over
/// one -- wants: the app's alpha is copied rather than blended, so a page that
/// cleared transparent still shows a video plane beneath.
void drawFrameQuad(unsigned program, unsigned buffer, unsigned texture);

}  // namespace detail
}  // namespace gfx
}  // namespace screenkit
