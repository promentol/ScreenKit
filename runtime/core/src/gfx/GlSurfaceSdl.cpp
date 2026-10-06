// Copyright (c) ScreenKit contributors. MIT.
//
// GlSurface on Linux and Android: the system's GLES, through the EGL context SDL
// creates on its own window.
//
// No ANGLE here. On a Raspberry Pi 3 the only GPU driver is Mesa's VideoCore IV,
// which offers GLES 2.0 and nothing else -- no Vulkan, no desktop GL, no ES 3 --
// and ANGLE has no backend that sits on that. SDL already speaks EGL on Linux
// (Wayland, X11 and KMS/DRM alike), so it makes the context and this file drives
// it. The vendored WebGL calls the same GLES entry points it calls through ANGLE
// on Apple; they resolve to Mesa's libGLESv2 instead.
//
// A fixed size (Desc::fixedWidth) draws into an offscreen framebuffer of that
// size and scales it into the window at each present. Letting the display mode
// do the scaling does not work everywhere: a Wayland compositor decides where a
// fullscreen surface smaller than the output goes, and Batocera's labwc puts it
// in the corner.
//
// Android has no ANGLE either (Architecture.md 9), and SDL's Android backend is
// EGL too, so the same file serves it. What Android adds is losing the window's
// drawable while the app is in the background: suspend() and resume() let the
// host give it up and take the new one without touching the context.
#include "GlSurface.h"

#include "GlSurfaceTarget.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_video.h>

#include <screenkit/Log.h>

namespace screenkit {
namespace gfx {
namespace {

constexpr const char* kTag = "screenkit.gl";

std::string glStringOrEmpty(GLenum name) {
  const GLubyte* value = glGetString(name);
  return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
}

SDL_Window* windowOf(void* layer) { return static_cast<SDL_Window*>(layer); }

// The present quad and the offscreen frame target are shared with the Apple
// backend (GlSurfaceTarget.h): three callers want the same two objects, and one
// implementation is what keeps them from drifting.

/// The framebuffer the app draws into, and the present program and quad in the
/// context that shares its texture. Called with the app's context current;
/// leaves it current.
bool createFixedTarget(SDL_Window* window, SDL_GLContext appContext, int width, int height,
                       unsigned& framebuffer, unsigned& colorTexture, unsigned& depthStencil,
                       void*& presentContext, unsigned& presentProgram, unsigned& presentBuffer,
                       std::string& error) {
  if (!detail::makeFrameTarget(width, height, framebuffer, colorTexture, depthStencil, error)) {
    return false;
  }

  // The present context shares the app context's objects -- the texture is all it needs.
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  const SDL_GLContext context = SDL_GL_CreateContext(window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  if (context == nullptr) {
    error = std::string("the present context: ") + SDL_GetError();
    SDL_GL_MakeCurrent(window, appContext);
    return false;
  }
  presentContext = context;
  // Presenting happens in this context, so the display's rate is set on it.
  if (!SDL_GL_SetSwapInterval(1)) log(LogLevel::Warn, kTag, std::string("no vsync: ") + SDL_GetError());

  const bool ok = detail::makePresentProgram(presentProgram, presentBuffer, error);
  SDL_GL_MakeCurrent(window, appContext);
  return ok;
}

#if defined(__linux__) && !defined(__ANDROID__)
/// Whether SDL is on Wayland, where a <video>'s plane is a subsurface beneath
/// the window and the window's alpha is what lets it show (media/linux/
/// WaylandVideo.cpp). X11 and KMS/DRM have no plane, so there the window stays
/// opaque: an alpha-0 clear under a compositing window manager would otherwise
/// blend the desktop through the letterbox bars and through any page that
/// cleared transparent.
bool onWayland() {
  static const bool wayland = [] {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr && std::strcmp(driver, "wayland") == 0;
  }();
  return wayland;
}
#endif

}  // namespace

void GlSurface::prepareWindowAttributes() {
  // A GLES context, asked for as 2.0: EGL hands back the newest version that is
  // compatible with it, so a GPU with ES 3 still gets ES 3 (and WebGL2), and the
  // Pi 3 gets the 2.0 it has. Asking for 3.0 first fails to find a config there.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  // No alpha in the window's own buffer: a compositor would blend a transparent
  // clear with whatever is behind the window. WebGL's alpha is about the page,
  // and there is no page behind this one.
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
#if defined(__ANDROID__) || defined(__linux__)
  // Android shows <video> on a SurfaceView beneath SDL's, so the window's buffer
  // carries alpha: where the page cleared transparent, the video shows. SDL's
  // surface stays opaque to the compositor until the page makes its first
  // <video> (dev/screenkit/media/VideoLayer.java), so a page with no video is
  // composited exactly as before -- an RGBA buffer costs what an RGBX one does.
  //
  // Linux the same, with a Wayland subsurface beneath the window: the window's
  // whole area is declared opaque (wl::declareWindowOpaque), and only a visible
  // video plane cuts that region open (media/linux/WaylandVideo.cpp), so the
  // compositor blends nothing for a page without video. Wayland only -- X11 and
  // KMS/DRM have no video plane, and an alpha window there only risks showing
  // the desktop through.
#if defined(__ANDROID__)
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#else
  if (onWayland()) SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#endif
#endif
  // A depth and a stencil buffer, as the Apple surface has: WebGL's default
  // context has a depth buffer, and getContextAttributes says so.
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

std::unique_ptr<GlSurface> GlSurface::create(const Desc& desc, std::string& error) {
  if (desc.nativeLayer == nullptr) {
    error = "no offscreen GL surface over SDL: pass the SDL window created with SDL_WINDOW_OPENGL";
    return nullptr;
  }

  std::unique_ptr<GlSurface> self(new GlSurface());
  self->layer_ = desc.nativeLayer;
  self->owner_ = SDL_GetCurrentThreadID();

  // Sharing over SDL is spelled "share with the context current on *this*
  // thread" (SDL_GL_SHARE_WITH_CURRENT_CONTEXT), which is the only sharing SDL
  // offers -- there is no "share with that one". So the caller has to be on the
  // thread that owns the other context, and saying so is better than the old
  // blanket refusal, which made an instance impossible on Linux and Android.
  const bool share = desc.shareWith != nullptr;
  if (share) {
    if (SDL_GL_GetCurrentContext() != static_cast<SDL_GLContext>(desc.shareWith->context_)) {
      error =
          "shareWith over SDL needs that surface's context current on this thread: SDL only offers "
          "SDL_GL_SHARE_WITH_CURRENT_CONTEXT. Create the shared surface from the thread that owns "
          "the one it shares with (GlSurface::createShared does).";
      return nullptr;
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  }
  SDL_GLContext context = SDL_GL_CreateContext(windowOf(self->layer_));
  if (share) SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  if (context == nullptr) {
    error = std::string("SDL_GL_CreateContext failed: ") + SDL_GetError();
    return nullptr;
  }
  self->context_ = context;
  // SDL_GL_CreateContext leaves the new context current on this thread, which
  // is the JS thread -- where every GL call happens from here on.
  if (!SDL_GL_MakeCurrent(windowOf(self->layer_), context)) {
    error = std::string("SDL_GL_MakeCurrent failed: ") + SDL_GetError();
    return nullptr;
  }
  // Present at the display's rate, as eglSwapBuffers does by default on Apple.
  if (!SDL_GL_SetSwapInterval(1)) {
    log(LogLevel::Warn, kTag, std::string("no vsync: ") + SDL_GetError());
  }

  if (desc.fixedWidth > 0 && desc.fixedHeight > 0) {
    self->fixedWidth_ = desc.fixedWidth;
    self->fixedHeight_ = desc.fixedHeight;
    self->frameWidth_ = desc.fixedWidth;
    self->frameHeight_ = desc.fixedHeight;
    if (!createFixedTarget(windowOf(self->layer_), context, desc.fixedWidth, desc.fixedHeight, self->framebuffer_,
                           self->colorTexture_, self->depthStencil_, self->presentContext_, self->presentProgram_,
                           self->presentBuffer_, error)) {
      return nullptr;
    }
  }

  self->vendor_ = glStringOrEmpty(GL_VENDOR);
  self->renderer_ = glStringOrEmpty(GL_RENDERER);
  self->version_ = glStringOrEmpty(GL_VERSION);

  log(LogLevel::Log, kTag,
      std::string("SDL ") + SDL_GetCurrentVideoDriver() + " / " + self->version_ + " / " +
          (self->framebuffer_ != 0 ? "fixed " : "window ") + std::to_string(self->width()) + "x" +
          std::to_string(self->height()));
  log(LogLevel::Log, kTag, "GL_VENDOR = " + self->vendor_);
  log(LogLevel::Log, kTag, "GL_RENDERER = " + self->renderer_);
  log(LogLevel::Log, kTag, "GL_VERSION = " + self->version_);

  self->presentedWidth_ = self->width();
  self->presentedHeight_ = self->height();
  return self;
}

// ---- instances: one offscreen layer surface each -----------------------------

std::unique_ptr<GlSurface> GlSurface::createShared(const GlSurface& parent, int width, int height,
                                                   std::string& error) {
  if (parent.context_ == nullptr || parent.layer_ == nullptr) {
    error = "the parent surface has no context to share";
    return nullptr;
  }
  if (width <= 0 || height <= 0) {
    error = "an instance's layer needs a positive size, not " + std::to_string(width) + "x" +
            std::to_string(height);
    return nullptr;
  }
  SDL_Window* window = windowOf(parent.layer_);
  if (SDL_GL_GetCurrentContext() != static_cast<SDL_GLContext>(parent.context_)) {
    error =
        "an instance's layer must be created on the thread that owns the parent's context: SDL "
        "shares only with the context current on the calling thread";
    return nullptr;
  }

  std::unique_ptr<GlSurface> self(new GlSurface());
  self->layer_ = parent.layer_;  // the same window; nothing is ever presented to it
  self->offscreen_ = true;
  self->layerSurface_ = true;
  self->pendingAdopt_ = true;
  self->fixedWidth_ = width;
  self->fixedHeight_ = height;
  self->frameWidth_ = width;
  self->frameHeight_ = height;
  self->owner_ = 0;  // the thread that adopts it owns it

  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  SDL_GLContext context = SDL_GL_CreateContext(window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  // SDL_GL_CreateContext makes the new context current here; the parent's
  // thread must keep its own.
  SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(parent.context_));
  if (context == nullptr) {
    error = std::string("the instance's context: ") + SDL_GetError();
    return nullptr;
  }
  self->context_ = context;
  return self;
}

bool GlSurface::adopt(std::string& error) {
  if (context_ == nullptr) {
    error = "this surface has no context to adopt";
    return false;
  }
  owner_ = SDL_GetCurrentThreadID();
  if (!SDL_GL_MakeCurrent(windowOf(layer_), static_cast<SDL_GLContext>(context_))) {
    error = std::string("binding an instance's context: ") + SDL_GetError();
    return false;
  }
  if (!pendingAdopt_) return true;
  pendingAdopt_ = false;
  // Nothing of this context is ever presented, so it must not block on vsync.
  SDL_GL_SetSwapInterval(0);

  if (!detail::makeFrameTarget(fixedWidth_, fixedHeight_, framebuffer_, colorTexture_, depthStencil_,
                               error)) {
    return false;
  }
  // A fresh texture's contents are undefined and the host composites this one as
  // soon as the instance's first frame says it may. Start it transparent.
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
  if (context_ == nullptr) {
    error = "this surface has no context to composite in";
    return false;
  }
  if (layerSurface_) {
    error = "an instance cannot composite instances of its own: one level of nesting";
    return false;
  }
  SDL_Window* window = windowOf(layer_);

  if (presentContext_ == nullptr) {
    // No fixed size, so the frame has nowhere offscreen to live yet. After a
    // swap the window's back buffer is undefined, so a frame in which only a
    // child painted would have nothing of the page left to put back.
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!createFixedTarget(window, static_cast<SDL_GLContext>(context_), w, h, framebuffer_,
                           colorTexture_, depthStencil_, presentContext_, presentProgram_,
                           presentBuffer_, error)) {
      return false;
    }
    frameWidth_ = w;
    frameHeight_ = h;
  }

  if (!SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(presentContext_))) {
    error = std::string("binding the present context: ") + SDL_GetError();
    return false;
  }
  layerPass_ = CompositePass::create(error);
  SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(context_));
  if (!layerPass_) return false;
  // The page draws into the framebuffer from here on; WebGL's `null` binding is
  // refreshed by the caller (gfx::refreshDefaultFramebuffer).
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
      "compositing " + std::to_string(frameWidth_) + "x" + std::to_string(frameHeight_) +
          ": the page's frame is a texture now");
  return true;
}

GlSurface::~GlSurface() {
  if (context_ == nullptr) return;
  if (owner_ != 0 && SDL_GetCurrentThreadID() != owner_) {
    log(LogLevel::Warn, kTag,
        "GL surface destroyed off the thread that created it; its context may outlive it");
  }
  if (presentContext_ != nullptr) {
    if (SDL_GL_MakeCurrent(windowOf(layer_), static_cast<SDL_GLContext>(presentContext_))) {
      layerPass_.reset();
      glDeleteBuffers(1, &presentBuffer_);
      glDeleteProgram(presentProgram_);
    }
  }
  // The frame target, in the app's context -- an instance's layer texture
  // included, which is why an instance frees its GL with its heap rather than
  // leaving one texture per launch behind.
  if ((framebuffer_ != 0 || colorTexture_ != 0) &&
      SDL_GL_MakeCurrent(windowOf(layer_), static_cast<SDL_GLContext>(context_))) {
    if (framebuffer_ != 0) glDeleteFramebuffers(1, &framebuffer_);
    if (depthStencil_ != 0) glDeleteRenderbuffers(1, &depthStencil_);
    if (colorTexture_ != 0) glDeleteTextures(1, &colorTexture_);
  }
  // Only when one of these contexts is the one bound on this thread: an
  // instance's layer surface can be destroyed on the launcher's JS thread (a
  // bootstrap that failed before it was ever adopted) and shares the launcher's
  // window, so an unconditional unbind would take the launcher's own context
  // away and every later GL call with it.
  const SDL_GLContext current = SDL_GL_GetCurrentContext();
  if (current != nullptr && (current == static_cast<SDL_GLContext>(context_) ||
                             current == static_cast<SDL_GLContext>(presentContext_))) {
    SDL_GL_MakeCurrent(windowOf(layer_), nullptr);
  }
  if (presentContext_ != nullptr) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(presentContext_));
  SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
  presentContext_ = nullptr;
  context_ = nullptr;
}

bool GlSurface::makeCurrent() {
  return context_ != nullptr && SDL_GL_MakeCurrent(windowOf(layer_), static_cast<SDL_GLContext>(context_));
}

void GlSurface::suspend() {
  if (context_ == nullptr || suspended_) return;
  // Releases whichever of the two contexts is current here. SDL tracks what is
  // current per thread, so resume() binds for real rather than finding the
  // (now dead) surface still recorded as current.
  if (!SDL_GL_MakeCurrent(windowOf(layer_), nullptr)) {
    log(LogLevel::Warn, kTag, std::string("releasing the window's drawable: ") + SDL_GetError());
  }
  suspended_ = true;
}

bool GlSurface::resume() {
  if (context_ == nullptr) return false;
  suspended_ = false;
  // Release first: if suspend() never ran -- the host could not reach this
  // thread in time -- SDL still records the old drawable as current, and would
  // skip binding the new one.
  SDL_GL_MakeCurrent(windowOf(layer_), nullptr);
  if (!SDL_GL_MakeCurrent(windowOf(layer_), static_cast<SDL_GLContext>(context_))) {
    log(LogLevel::Error, kTag, std::string("binding the new drawable: ") + SDL_GetError());
    return false;
  }
  // An offscreen frame survived the old drawable; show it again now. A window
  // frame did not, and the app's next paint is the first thing shown.
  return presentContext_ == nullptr || swap();
}

bool GlSurface::swap() {
  if (context_ == nullptr || suspended_) return false;
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
  SDL_Window* window = windowOf(layer_);
  if (presentContext_ != nullptr) {
    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSizeInPixels(window, &windowWidth, &windowHeight);
    // A fixed size scales the app's frame into the window: centred, aspect kept,
    // black around it. A compositing window draws it 1:1, and follows a resize.
    const bool letterbox = fixedWidth_ > 0 && fixedHeight_ > 0;
    if (!letterbox && (windowWidth != frameWidth_ || windowHeight != frameHeight_) &&
        windowWidth > 0 && windowHeight > 0) {
      std::string reason;
      if (detail::makeFrameTarget(windowWidth, windowHeight, framebuffer_, colorTexture_,
                                  depthStencil_, reason)) {
        frameWidth_ = windowWidth;
        frameHeight_ = windowHeight;
      } else {
        log(LogLevel::Error, kTag, "resizing the composited frame: " + reason);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    }
    const double scale = letterbox ? std::min(static_cast<double>(windowWidth) / fixedWidth_,
                                              static_cast<double>(windowHeight) / fixedHeight_)
                                   : 1.0;
    const int width = letterbox ? static_cast<int>(fixedWidth_ * scale + 0.5) : windowWidth;
    const int height = letterbox ? static_cast<int>(fixedHeight_ * scale + 0.5) : windowHeight;
    SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(presentContext_));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
#if defined(__ANDROID__)
    // Transparent, not black: around the frame, and -- through the quad below,
    // drawn without blending so the app's alpha is copied -- wherever the page
    // cleared transparent, a video plane beneath shows. Until the page makes a
    // <video>, SDL's surface is opaque to the compositor and this reads as black.
    glClearColor(0, 0, 0, 0);
#elif defined(__linux__)
    // The same on Wayland, where the plane is a subsurface below the window;
    // opaque on X11 and KMS/DRM, which have no plane.
    glClearColor(0, 0, 0, onWayland() ? 0.0f : 1.0f);
#else
    glClearColor(0, 0, 0, 1);
#endif
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((windowWidth - width) / 2, (windowHeight - height) / 2, width, height);
    detail::drawFrameQuad(presentProgram_, presentBuffer_, colorTexture_);
    // The instance layers go over the frame, in the same viewport, so a
    // letterboxed present scales them exactly as it scales the page.
    if (layers_ && layerPass_) {
      layerPass_->draw(layers_->paintOrder(), letterbox ? fixedWidth_ : frameWidth_,
                       letterbox ? fixedHeight_ : frameHeight_);
      layers_->clearDirty();
    }
  }
  const bool swapped = SDL_GL_SwapWindow(window);
  if (presentContext_ != nullptr) SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(context_));
  if (swapped) {
    presentedWidth_ = width();
    presentedHeight_ = height();
    return true;
  }
  log(LogLevel::Error, kTag, std::string("SDL_GL_SwapWindow failed: ") + SDL_GetError());
  return false;
}

bool GlSurface::resizedSincePresent() const {
  return width() != presentedWidth_ || height() != presentedHeight_;
}

int GlSurface::width() const {
  if (fixedWidth_ > 0) return fixedWidth_;
  if (layerSurface_) return frameWidth_;
  int w = 0, h = 0;
  if (layer_ != nullptr) SDL_GetWindowSizeInPixels(windowOf(layer_), &w, &h);
  return w;
}

int GlSurface::height() const {
  if (fixedHeight_ > 0) return fixedHeight_;
  if (layerSurface_) return frameHeight_;
  int w = 0, h = 0;
  if (layer_ != nullptr) SDL_GetWindowSizeInPixels(windowOf(layer_), &w, &h);
  return h;
}

}  // namespace gfx
}  // namespace screenkit
