// Copyright (c) ScreenKit contributors. MIT.
#include "GlSurface.h"

#include "GlSurfaceTarget.h"
#include "MetalLayerSize.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
// EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE lives here and NOT in eglext.h, so this
// include is load-bearing rather than defensive.
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <SDL3/SDL_thread.h>

#include <screenkit/Log.h>

namespace screenkit {
namespace gfx {
namespace {

/// EGL has no refcount of its own: `eglTerminate` invalidates every context and
/// surface on a display, no matter who else is still using it. ANGLE hands back
/// the *same* display handle for the same attributes, so two live GlSurfaces
/// share one -- and the first destructor to run would pull it out from under the
/// second. Counting here is what makes a second surface in one process safe,
/// which `gl-teardown` exercises directly.
std::mutex& displayMutex() {
  static std::mutex m;
  return m;
}

std::map<EGLDisplay, int>& displayRefs() {
  static std::map<EGLDisplay, int> refs;
  return refs;
}

void retainDisplay(EGLDisplay display) {
  std::lock_guard<std::mutex> lock(displayMutex());
  ++displayRefs()[display];
}

/// True when this was the last user and the caller should terminate.
bool releaseDisplay(EGLDisplay display) {
  std::lock_guard<std::mutex> lock(displayMutex());
  auto it = displayRefs().find(display);
  if (it == displayRefs().end()) return true;
  if (--it->second > 0) return false;
  displayRefs().erase(it);
  return true;
}


constexpr const char* kTag = "screenkit.gl";

std::string eglErrorString() {
  const EGLint code = eglGetError();
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%04x", static_cast<unsigned>(code));
  return buffer;
}

std::string glStringOrEmpty(GLenum name) {
  const GLubyte* value = glGetString(name);
  return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
}

}  // namespace

std::unique_ptr<GlSurface> GlSurface::create(const Desc& desc, std::string& error) {
  std::unique_ptr<GlSurface> self(new GlSurface());
  self->offscreen_ = desc.nativeLayer == nullptr;
  self->layer_ = desc.nativeLayer;
  self->owner_ = SDL_GetCurrentThreadID();

  // Ask ANGLE for the Metal backend by name rather than taking the default, so
  // a fallback to some other backend shows up as a failure instead of a silent
  // pass. This is the same discipline the M1 spike used to prove the gate.
  const EGLint displayAttribs[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
      EGL_NONE,
  };

  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (getPlatformDisplay == nullptr) {
    error = "eglGetPlatformDisplayEXT is missing -- this ANGLE cannot be asked for Metal by name";
    return nullptr;
  }
  self->display_ = getPlatformDisplay(
      EGL_PLATFORM_ANGLE_ANGLE,
      reinterpret_cast<void*>(static_cast<intptr_t>(EGL_DEFAULT_DISPLAY)), displayAttribs);
  if (self->display_ == EGL_NO_DISPLAY) {
    error = "eglGetPlatformDisplayEXT returned no display for the Metal backend";
    return nullptr;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(self->display_, &major, &minor)) {
    error = "eglInitialize failed: " + eglErrorString();
    self->display_ = nullptr;  // nothing to terminate
    return nullptr;
  }
  retainDisplay(self->display_);

  const EGLint configAttribs[] = {
      EGL_SURFACE_TYPE, desc.nativeLayer != nullptr ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      // A depth and a stencil buffer, because the context says it has them
      // (getContextAttributes reports depth and stencil), and because WebGL's
      // default is a depth buffer: a 3D scene -- three.js -- draws overlapping
      // geometry in submission order without one.
      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint configCount = 0;
  if (!eglChooseConfig(self->display_, configAttribs, &config, 1, &configCount) ||
      configCount < 1) {
    error = "eglChooseConfig found no ES3 config: " + eglErrorString();
    return nullptr;
  }
  self->config_ = config;

  if (desc.nativeLayer != nullptr) {
    // On Apple platforms ANGLE takes a CALayer as the native window type.
    self->surface_ = eglCreateWindowSurface(
        self->display_, config, reinterpret_cast<EGLNativeWindowType>(desc.nativeLayer), nullptr);
    if (self->surface_ == EGL_NO_SURFACE) {
      error = "eglCreateWindowSurface over the CAMetalLayer failed: " + eglErrorString();
      self->surface_ = nullptr;
      return nullptr;
    }
  } else {
    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, desc.width > 0 ? desc.width : 1,
        EGL_HEIGHT, desc.height > 0 ? desc.height : 1,
        EGL_NONE,
    };
    self->surface_ = eglCreatePbufferSurface(self->display_, config, pbufferAttribs);
    if (self->surface_ == EGL_NO_SURFACE) {
      error = "eglCreatePbufferSurface failed: " + eglErrorString();
      self->surface_ = nullptr;
      return nullptr;
    }
  }

  // ES 3.0 exactly, and no probing upwards. ANGLE's Metal backend tops out at
  // 3.0 on Apple -- 3.1 and 3.2 both fail with EGL_BAD_MATCH (Architecture.md
  // 9) -- so a walk down from 3.2 would only buy two guaranteed failures and
  // two confusing log lines. 3.0 is also exactly what WebGL2 is defined
  // against, so nothing above it is reachable from the layer above anyway.
  const EGLint contextAttribs[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 0,
      EGL_NONE,
  };
  // Sharing needs the same display. ANGLE returns one per attribute set, so a
  // mismatch here means the other surface was made some other way.
  if (desc.shareWith != nullptr && desc.shareWith->display_ != self->display_) {
    error = "shareWith is a surface on a different EGL display; contexts can only share within one";
    return nullptr;
  }
  const EGLContext share = desc.shareWith != nullptr ? desc.shareWith->context_ : EGL_NO_CONTEXT;
  self->context_ = eglCreateContext(self->display_, config, share, contextAttribs);
  if (self->context_ == EGL_NO_CONTEXT) {
    error = "eglCreateContext for ES 3.0 failed: " + eglErrorString();
    self->context_ = nullptr;
    return nullptr;
  }

  if (!eglMakeCurrent(self->display_, self->surface_, self->surface_, self->context_)) {
    error = "eglMakeCurrent failed: " + eglErrorString();
    return nullptr;
  }

  self->vendor_ = glStringOrEmpty(GL_VENDOR);
  self->renderer_ = glStringOrEmpty(GL_RENDERER);
  self->version_ = glStringOrEmpty(GL_VERSION);

  log(LogLevel::Log, kTag,
      "EGL " + std::to_string(major) + "." + std::to_string(minor) + " / ES 3.0 / " +
          (self->offscreen_ ? "pbuffer " : "window ") + std::to_string(self->width()) + "x" +
          std::to_string(self->height()));
  log(LogLevel::Log, kTag, "GL_VENDOR = " + self->vendor_);
  log(LogLevel::Log, kTag, "GL_RENDERER = " + self->renderer_);
  log(LogLevel::Log, kTag, "GL_VERSION = " + self->version_);

  self->presentedWidth_ = self->width();
  self->presentedHeight_ = self->height();
  return self;
}

void GlSurface::prepareWindowAttributes() {}

// ---- instances: one offscreen layer surface each -----------------------------

std::unique_ptr<GlSurface> GlSurface::createShared(const GlSurface& parent, int width, int height,
                                                   std::string& error) {
  if (parent.display_ == nullptr || parent.context_ == nullptr) {
    error = "the parent surface has no context to share";
    return nullptr;
  }
  if (width <= 0 || height <= 0) {
    error = "an instance's layer needs a positive size, not " + std::to_string(width) + "x" +
            std::to_string(height);
    return nullptr;
  }

  std::unique_ptr<GlSurface> self(new GlSurface());
  self->display_ = parent.display_;
  self->config_ = parent.config_;
  self->offscreen_ = true;
  self->layerSurface_ = true;
  self->pendingAdopt_ = true;
  self->fixedWidth_ = width;
  self->fixedHeight_ = height;
  self->frameWidth_ = width;
  self->frameHeight_ = height;
  // The thread that adopts it is the one that owns it; until then nobody does.
  self->owner_ = 0;
  retainDisplay(self->display_);

  // A 1x1 pbuffer: the context needs a surface to be made current on, and every
  // pixel this instance draws goes into its own framebuffer rather than here.
  const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  self->surface_ = eglCreatePbufferSurface(self->display_, self->config_, pbufferAttribs);
  if (self->surface_ == EGL_NO_SURFACE) {
    error = "eglCreatePbufferSurface for an instance layer failed: " + eglErrorString();
    self->surface_ = nullptr;
    return nullptr;
  }

  const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
  self->context_ = eglCreateContext(self->display_, self->config_, parent.context_, contextAttribs);
  if (self->context_ == EGL_NO_CONTEXT) {
    error = "eglCreateContext in the parent's share group failed: " + eglErrorString();
    self->context_ = nullptr;
    return nullptr;
  }
  // Deliberately not made current here: it belongs to the instance's JS thread,
  // which calls adopt().
  return self;
}

bool GlSurface::adopt(std::string& error) {
  if (context_ == nullptr) {
    error = "this surface has no context to adopt";
    return false;
  }
  owner_ = SDL_GetCurrentThreadID();
  if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
    error = "eglMakeCurrent for an instance layer failed: " + eglErrorString();
    return false;
  }
  if (!pendingAdopt_) return true;
  pendingAdopt_ = false;

  if (!detail::makeFrameTarget(fixedWidth_, fixedHeight_, framebuffer_, colorTexture_, depthStencil_,
                               error)) {
    return false;
  }
  // A fresh texture's contents are undefined, and the host composites this one
  // the moment the instance's first frame says it may. Start it transparent.
  glViewport(0, 0, fixedWidth_, fixedHeight_);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  vendor_ = glStringOrEmpty(GL_VENDOR);
  renderer_ = glStringOrEmpty(GL_RENDERER);
  version_ = glStringOrEmpty(GL_VERSION);
  presentedWidth_ = fixedWidth_;
  presentedHeight_ = fixedHeight_;
  log(LogLevel::Log, kTag,
      "instance layer " + std::to_string(fixedWidth_) + "x" + std::to_string(fixedHeight_) + " on " +
          renderer_);
  return true;
}

// ---- the host: compositing over its own frame --------------------------------

bool GlSurface::enableCompositing(std::string& error) {
  if (layerPass_ != nullptr) return true;
  if (display_ == nullptr || context_ == nullptr) {
    error = "this surface has no context to composite in";
    return false;
  }
  if (layerSurface_) {
    error = "an instance cannot composite instances of its own: one level of nesting";
    return false;
  }

  const int w = width();
  const int h = height();
  // The page's frame moves into a texture. After a swap the window's back buffer
  // is undefined, so a frame in which only a child painted would otherwise have
  // nothing of the page left to put back.
  if (framebuffer_ == 0 &&
      !detail::makeFrameTarget(w, h, framebuffer_, colorTexture_, depthStencil_, error)) {
    return false;
  }
  // 1:1 with the window rather than a letterboxed fixed size; the frame size is
  // what the texture is allocated at, so a resize is noticed at the next present.
  frameWidth_ = w;
  frameHeight_ = h;

  if (presentContext_ == nullptr) {
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,
                                     EGL_NONE};
    EGLContext present = eglCreateContext(display_, config_, context_, contextAttribs);
    if (present == EGL_NO_CONTEXT) {
      error = "the present context could not be created: " + eglErrorString();
      return false;
    }
    presentContext_ = present;
  }

  if (!eglMakeCurrent(display_, surface_, surface_, presentContext_)) {
    error = "eglMakeCurrent on the present context failed: " + eglErrorString();
    return false;
  }
  bool ok = presentProgram_ != 0 || detail::makePresentProgram(presentProgram_, presentBuffer_, error);
  if (ok) {
    layerPass_ = CompositePass::create(error);
    ok = layerPass_ != nullptr;
  }
  // Back to the app's context whatever happened: the JS thread draws there.
  eglMakeCurrent(display_, surface_, surface_, context_);
  if (!ok) return false;
  // The page draws into the framebuffer from here on, and WebGL's `null`
  // binding is refreshed by the caller (gfx::refreshDefaultFramebuffer).
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  // A fresh texture's contents are undefined, and the very next present may
  // composite this one -- a page that adds a canvas without painting that frame
  // would otherwise show whatever the driver left in it. `adopt` does the same
  // for an instance's layer, and for the same reason.
  //
  // Unlike `adopt`, this runs on a context the page is already drawing with: the
  // page set its clear colour and its viewport before it ever added a canvas, so
  // everything this touches goes back the way it was. `resizeLayer` does the
  // same dance for the same reason.
  {
    GLint viewport[4] = {0, 0, 0, 0};
    GLfloat clearColor[4] = {0, 0, 0, 0};
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depthMask = GL_TRUE;
    GLint stencilMask = ~0;
    GLint stencilBackMask = ~0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &stencilBackMask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (scissor) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(~0u);
    glViewport(0, 0, frameWidth_, frameHeight_);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    if (scissor) glEnable(GL_SCISSOR_TEST);
    glStencilMaskSeparate(GL_FRONT, static_cast<GLuint>(stencilMask));
    glStencilMaskSeparate(GL_BACK, static_cast<GLuint>(stencilBackMask));
    glDepthMask(depthMask);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  }
  log(LogLevel::Log, kTag,
      "compositing " + std::to_string(w) + "x" + std::to_string(h) + ": the page's frame is a texture now");
  return true;
}

GlSurface::~GlSurface() {
  if (display_ == nullptr) return;
  // A context is current on the thread that made it. Torn down from another,
  // the unbind below is a no-op there and the destroy is deferred until the
  // owning thread releases a context it no longer has a handle to. An instance
  // layer that was never adopted has no owner yet, and nothing to warn about.
  if (owner_ != 0 && SDL_GetCurrentThreadID() != owner_) {
    log(LogLevel::Warn, kTag,
        "GL surface destroyed off the thread that created it; its context may outlive it");
  }
  // The present context's objects go first, in the context that made them.
  if (presentContext_ != nullptr && eglMakeCurrent(display_, surface_, surface_, presentContext_)) {
    layerPass_.reset();
    if (presentBuffer_ != 0) glDeleteBuffers(1, &presentBuffer_);
    if (presentProgram_ != 0) glDeleteProgram(presentProgram_);
  }
  // Then the frame target, in the app's -- an instance's layer texture included,
  // which is why an instance frees its GL with its heap rather than leaking one
  // texture per launch.
  if (context_ != nullptr && (framebuffer_ != 0 || colorTexture_ != 0) &&
      eglMakeCurrent(display_, surface_, surface_, context_)) {
    if (framebuffer_ != 0) glDeleteFramebuffers(1, &framebuffer_);
    if (depthStencil_ != 0) glDeleteRenderbuffers(1, &depthStencil_);
    if (colorTexture_ != 0) glDeleteTextures(1, &colorTexture_);
  }
  // Unbind before destroying: EGL keeps a bound context alive until it is
  // released, so destroying it while current leaks it rather than failing. Only
  // when one of *these* contexts is the one bound, though: an instance's layer
  // surface can be destroyed on the launcher's own JS thread (a bootstrap that
  // failed before the instance ever adopted it), and this display is the
  // launcher's too -- unbinding there would take the launcher's context away
  // from under it and every later GL call with it.
  const EGLContext current = eglGetCurrentContext();
  const bool mine = current != EGL_NO_CONTEXT && (current == context_ || current == presentContext_);
  if (mine) eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (presentContext_ != nullptr) eglDestroyContext(display_, presentContext_);
  if (context_ != nullptr) eglDestroyContext(display_, context_);
  if (surface_ != nullptr) eglDestroySurface(display_, surface_);
  // Terminate only when nothing else holds this display. Leaving it initialised
  // keeps ANGLE's whole backend allocated, so the last one out still turns off
  // the lights -- but doing it unconditionally invalidates a sibling surface's
  // context, which shows up as glGetString returning null and a strlen(NULL)
  // crash inside the GL bindings rather than anywhere near here.
  if (releaseDisplay(display_)) {
    eglTerminate(display_);
  }
  // Only this thread's own EGL state, for the same reason.
  if (mine) eglReleaseThread();
  display_ = nullptr;
  surface_ = nullptr;
  context_ = nullptr;
  presentContext_ = nullptr;
}

bool GlSurface::makeCurrent() {
  if (display_ == nullptr) return false;
  return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}

bool GlSurface::swap() {
  if (display_ == nullptr || surface_ == nullptr) return false;
  // A layer is never presented. Its frame is finished into its own texture and
  // published for the compositor: a fence where the platform has one, a flush
  // where it does not -- never glFinish in the frame path. A canvas of the page
  // that composites it produces on that very thread, so there is nothing to
  // synchronise and the flush alone orders it.
  if (layerSurface_) {
    void* fence = nullptr;
    if (layerSameThread_) {
      glFlush();
    } else {
      fence = insertFenceOrFlush();
    }
    if (layerSource_) {
      layerSource_->produced(colorTexture_, fence);
    } else {
      waitAndDestroyFence(fence);
    }
    return true;
  }
  const int w = width();
  const int h = height();
  // A pbuffer with no compositing has nothing to do at all. Saying so here keeps
  // the caller's frame loop identical between the windowed host and the headless
  // tests.
  if (offscreen_ && presentContext_ == nullptr) return true;

  if (presentContext_ != nullptr) {
    // The window resized: the frame texture follows it, keeping the framebuffer
    // name WebGL's `null` binding resolves to.
    if ((w != frameWidth_ || h != frameHeight_) && w > 0 && h > 0) {
      std::string reason;
      if (detail::makeFrameTarget(w, h, framebuffer_, colorTexture_, depthStencil_, reason)) {
        frameWidth_ = w;
        frameHeight_ = h;
      } else {
        log(LogLevel::Error, kTag, "resizing the composited frame: " + reason);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    }
    if (!eglMakeCurrent(display_, surface_, surface_, presentContext_)) {
      log(LogLevel::Error, kTag, "the present context could not be bound: " + eglErrorString());
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    detail::drawFrameQuad(presentProgram_, presentBuffer_, colorTexture_);
    if (layers_ && layerPass_) {
      layerPass_->draw(layers_->paintOrder(), w, h);
      layers_->clearDirty();
    }
  }

  // A pbuffer has no swap, but it does have the composited frame in its default
  // framebuffer -- which is what makes the whole compositor path readable from a
  // headless row, exactly as `gl-triangle` reads a drawn frame.
  const bool swapped = offscreen_ ? true : eglSwapBuffers(display_, surface_) == EGL_TRUE;
  if (presentContext_ != nullptr) eglMakeCurrent(display_, surface_, surface_, context_);
  if (swapped) {
    presentedWidth_ = width();
    presentedHeight_ = height();
    return true;
  }
  log(LogLevel::Error, kTag, "eglSwapBuffers failed: " + eglErrorString());
  return false;
}

bool GlSurface::resizedSincePresent() const {
  return !offscreen_ && (width() != presentedWidth_ || height() != presentedHeight_);
}

int GlSurface::width() const {
  if (display_ == nullptr || surface_ == nullptr) return 0;
  // An instance draws at its layer's size, not at the 1x1 pbuffer its context
  // happens to be bound to.
  if (layerSurface_) return fixedWidth_;
  int layerWidth = 0;
  int layerHeight = 0;
  if (!offscreen_ && metalLayerPixelSize(layer_, layerWidth, layerHeight)) return layerWidth;
  EGLint value = 0;
  eglQuerySurface(display_, surface_, EGL_WIDTH, &value);
  return static_cast<int>(value);
}

int GlSurface::height() const {
  if (display_ == nullptr || surface_ == nullptr) return 0;
  if (layerSurface_) return fixedHeight_;
  int layerWidth = 0;
  int layerHeight = 0;
  if (!offscreen_ && metalLayerPixelSize(layer_, layerWidth, layerHeight)) return layerHeight;
  EGLint value = 0;
  eglQuerySurface(display_, surface_, EGL_HEIGHT, &value);
  return static_cast<int>(value);
}

}  // namespace gfx
}  // namespace screenkit
