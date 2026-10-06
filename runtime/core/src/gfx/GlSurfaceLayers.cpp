// Copyright (c) ScreenKit contributors. MIT.
//
// The parts of `GlSurface` that are the same on every backend -- the offscreen
// frame target, the present quad, and the layer bookkeeping a compositing host
// and an instance's layer both need. What differs per platform (how a context is
// made, shared and presented) stays in GlSurface.cpp and GlSurfaceSdl.cpp.
#include "GlSurface.h"
#include "GlSurfaceTarget.h"

#include <GLES3/gl3.h>

#include <cstdio>
#include <string>
#include <utility>

namespace screenkit {
namespace gfx {

namespace {

// GLES 2 names the packed depth-stencil format through
// OES_packed_depth_stencil; GLES 3 has it in core under the same value.
constexpr GLenum kDepth24Stencil8 = 0x88F0;

// GLSL ES 1.00, so one program compiles on a GLES 2 context as well as a
// GLES 3 one -- a Raspberry Pi 3's VideoCore IV offers only the former.
constexpr const char* kPresentVertexSource = R"(
attribute vec2 position;
varying vec2 uv;
void main() {
  uv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
})";

constexpr const char* kPresentFragmentSource = R"(
precision mediump float;
varying vec2 uv;
uniform sampler2D frame;
void main() {
  gl_FragColor = texture2D(frame, uv);
})";

GLuint compileOne(GLenum type, const char* source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok && error.empty()) {
    char info[512] = {};
    glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
    error = std::string("present shader: ") + info;
  }
  return shader;
}

std::string hex16(unsigned value) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%04x", value);
  return buffer;
}

}  // namespace

namespace detail {

bool makeFrameTarget(int width, int height, unsigned& framebuffer, unsigned& colorTexture,
                     unsigned& depthStencil, std::string& error) {
  if (width <= 0 || height <= 0) {
    error = "an offscreen frame needs a positive size, not " + std::to_string(width) + "x" +
            std::to_string(height);
    return false;
  }

  // The page's own bindings survive this: a resize runs it inside `swap()` on
  // the app's context, and a framework that caches what it bound (three.js does)
  // would otherwise draw its next frame against texture 0.
  GLint boundTexture = 0, boundRenderbuffer = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &boundRenderbuffer);
  struct Restore {
    GLint texture;
    GLint renderbuffer;
    ~Restore() {
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
      glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
    }
  } restore{boundTexture, boundRenderbuffer};

  if (colorTexture == 0) glGenTextures(1, &colorTexture);
  glBindTexture(GL_TEXTURE_2D, colorTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (depthStencil == 0) glGenRenderbuffers(1, &depthStencil);
  glBindRenderbuffer(GL_RENDERBUFFER, depthStencil);
  glRenderbufferStorage(GL_RENDERBUFFER, kDepth24Stencil8, width, height);

  const bool fresh = framebuffer == 0;
  if (fresh) glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthStencil);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthStencil);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    error = "the " + std::to_string(width) + "x" + std::to_string(height) +
            " framebuffer is incomplete (0x" + hex16(status) + ")";
    return false;
  }
  return true;
}

bool makePresentProgram(unsigned& program, unsigned& buffer, std::string& error) {
  const GLuint vertex = compileOne(GL_VERTEX_SHADER, kPresentVertexSource, error);
  const GLuint fragment = compileOne(GL_FRAGMENT_SHADER, kPresentFragmentSource, error);
  const GLuint linked = glCreateProgram();
  glAttachShader(linked, vertex);
  glAttachShader(linked, fragment);
  glBindAttribLocation(linked, 0, "position");
  glLinkProgram(linked);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  program = linked;
  GLint ok = 0;
  glGetProgramiv(linked, GL_LINK_STATUS, &ok);
  if (!ok && error.empty()) error = "the present program did not link";
  glUseProgram(linked);
  glUniform1i(glGetUniformLocation(linked, "frame"), 0);

  static constexpr GLfloat kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  GLuint made = 0;
  glGenBuffers(1, &made);
  glBindBuffer(GL_ARRAY_BUFFER, made);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  buffer = made;
  return error.empty();
}

void drawFrameQuad(unsigned program, unsigned buffer, unsigned texture) {
  glUseProgram(program);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}  // namespace detail

// ---- layer bookkeeping -------------------------------------------------------

void GlSurface::setLayers(std::shared_ptr<LayerList> layers) { layers_ = std::move(layers); }

bool GlSurface::layersDirty() const { return layers_ && layers_->dirty(); }

/// The texture is published by the first `swap()`, not here: a layer whose
/// producer has not drawn yet has nothing but the undefined contents of a fresh
/// texture, and `LayerList::paintOrder` skips a source with no texture rather
/// than compositing that.
void GlSurface::setLayerSource(std::shared_ptr<LayerSource> source, bool sameThread) {
  layerSource_ = std::move(source);
  layerSameThread_ = sameThread;
}

bool GlSurface::resizeLayer(int width, int height, std::string& error) {
  if (!layerSurface_) {
    error = "only a layer surface has a drawing buffer of its own to resize";
    return false;
  }
  if (width <= 0 || height <= 0) {
    error = "a canvas layer needs a positive size, not " + std::to_string(width) + "x" +
            std::to_string(height);
    return false;
  }
  if (width == fixedWidth_ && height == fixedHeight_) return true;

  // Resizing a canvas gives it a new drawing buffer; it does not reset the GL
  // state the page set, and in a browser it does not even reset the viewport.
  // So everything this touches goes back the way it was -- including the
  // framebuffer binding, which the DOM shim's paint tracker is also following.
  GLint viewport[4] = {0, 0, 0, 0};
  GLfloat clearColor[4] = {0, 0, 0, 0};
  GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLint boundFramebuffer = 0;
  GLboolean depthMask = GL_TRUE;
  GLint stencilMask = ~0;
  GLint stencilBackMask = ~0;
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
  glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFramebuffer);
  // A 3D engine routinely leaves these off, and a masked-off buffer swallows
  // the clear below -- which would leave exactly the undefined depth/stencil
  // the clear is here to prevent.
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
  glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &stencilBackMask);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

  // The ids go in and come back out, so the framebuffer keeps the name WebGL's
  // `null` binding resolves to and the texture keeps the one the compositor is
  // already holding.
  if (!detail::makeFrameTarget(width, height, framebuffer_, colorTexture_, depthStencil_, error)) {
    // The storage was already reallocated at the new size before whatever
    // failed, so leaving now would have the surface describing a texture it no
    // longer has and the compositor sampling it. Put the old size back; if even
    // that fails there is no drawing buffer left, and the error says which.
    std::string restoreError;
    if (!detail::makeFrameTarget(fixedWidth_, fixedHeight_, framebuffer_, colorTexture_,
                                 depthStencil_, restoreError)) {
      error += " (and the previous " + std::to_string(fixedWidth_) + "x" +
               std::to_string(fixedHeight_) + " buffer could not be put back: " + restoreError + ")";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(boundFramebuffer));
    return false;
  }
  fixedWidth_ = width;
  fixedHeight_ = height;
  frameWidth_ = width;
  frameHeight_ = height;
  presentedWidth_ = width;
  presentedHeight_ = height;

  // A fresh texture's contents are undefined and the compositor may sample this
  // one before the page draws into it again, so clear it -- past whatever
  // scissor or colour mask the page left set.
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glViewport(0, 0, width, height);
  if (scissor) glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glStencilMask(~0u);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  if (scissor) glEnable(GL_SCISSOR_TEST);
  glStencilMaskSeparate(GL_FRONT, static_cast<GLuint>(stencilMask));
  glStencilMaskSeparate(GL_BACK, static_cast<GLuint>(stencilBackMask));
  glDepthMask(depthMask);
  glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
  glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(boundFramebuffer));
  return true;
}

}  // namespace gfx
}  // namespace screenkit
