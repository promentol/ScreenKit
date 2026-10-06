// Copyright (c) ScreenKit contributors. MIT.
#include "Compositor.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#if defined(__APPLE__)
#include <EGL/egl.h>
#else
#include <SDL3/SDL_video.h>
#endif

#include <screenkit/Log.h>

namespace screenkit {
namespace gfx {
namespace {

constexpr const char* kTag = "screenkit.compositor";

/// Resolve a GL entry point at run time rather than at link time.
///
/// `glFenceSync` and friends are ES 3.0. Apple's ANGLE always has them, but a
/// Raspberry Pi 3's VideoCore IV is ES 2.0 and the Android NDK's `libGLESv2.so`
/// exports the ES 2 set alone -- so naming them at link time would either fail
/// to link or fail to load on exactly the devices that matter. Asking the
/// driver, and flushing when the answer is null, is the "a fence where the
/// platform has one, a flush where it does not" rule made concrete.
void* glProc(const char* name) {
#if defined(__APPLE__)
  return reinterpret_cast<void*>(eglGetProcAddress(name));
#else
  return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
#endif
}

using FenceSyncFn = GLsync (*)(GLenum, GLbitfield);
using WaitSyncFn = void (*)(GLsync, GLbitfield, GLuint64);
using DeleteSyncFn = void (*)(GLsync);

struct SyncApi {
  FenceSyncFn fenceSync = nullptr;
  WaitSyncFn waitSync = nullptr;
  DeleteSyncFn deleteSync = nullptr;
  bool available = false;
};

const SyncApi& syncApi() {
  static const SyncApi api = [] {
    SyncApi resolved;
    resolved.fenceSync = reinterpret_cast<FenceSyncFn>(glProc("glFenceSync"));
    resolved.waitSync = reinterpret_cast<WaitSyncFn>(glProc("glWaitSync"));
    resolved.deleteSync = reinterpret_cast<DeleteSyncFn>(glProc("glDeleteSync"));
    resolved.available =
        resolved.fenceSync != nullptr && resolved.waitSync != nullptr && resolved.deleteSync != nullptr;
    if (!resolved.available) {
      log(LogLevel::Log, kTag,
          "no GL fences on this driver; instance frames are ordered by a flush instead");
    }
    return resolved;
  }();
  return api;
}

// GLSL ES 1.00, so one program serves a GLES 2 context as well as a GLES 3 one
// -- the same reason the SDL present quad is written this way.
constexpr const char* kLayerVertex = R"(
attribute vec2 position;
uniform vec4 rect;
varying vec2 uv;
void main() {
  uv = position;
  gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
})";

constexpr const char* kLayerFragment = R"(
precision mediump float;
varying vec2 uv;
uniform sampler2D layer;
uniform float alpha;
void main() {
  vec4 c = texture2D(layer, uv);
  gl_FragColor = vec4(c.rgb, c.a * alpha);
})";

GLuint compileShader(GLenum type, const char* source, std::string& error) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[512] = {};
    glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
    if (error.empty()) error = std::string("the layer shader did not compile: ") + info;
  }
  return shader;
}

}  // namespace

// ---- LayerSource -------------------------------------------------------------

void LayerSource::produced(unsigned texture, void* fence) {
  void* stale = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    texture_ = texture;
    // A fence the host never consumed -- it did not present that frame -- is
    // replaced rather than kept: waiting on the newer one implies the older.
    stale = fence_;
    fence_ = fence;
    dirty_ = true;
  }
  // Destroying it here, on the producer's thread, is safe: a sync object belongs
  // to the share group, not to one context.
  if (stale != nullptr) {
    const SyncApi& api = syncApi();
    if (api.available) api.deleteSync(static_cast<GLsync>(stale));
  }
}

void* LayerSource::takeFence() {
  std::lock_guard<std::mutex> lock(mutex_);
  void* fence = fence_;
  fence_ = nullptr;
  return fence;
}

unsigned LayerSource::texture() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return texture_;
}

bool LayerSource::dirty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_;
}

void LayerSource::clearDirty() {
  std::lock_guard<std::mutex> lock(mutex_);
  dirty_ = false;
}

// ---- LayerList ---------------------------------------------------------------

void LayerList::set(std::uint64_t id, Layer layer) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : layers_) {
    if (entry.first != id) continue;
    // Keep the sequence it was first given: moving a layer must not reorder it
    // against a sibling with the same z-index.
    layer.sequence = entry.second.sequence;
    entry.second = std::move(layer);
    return;
  }
  layer.sequence = nextSequence_++;
  layers_.emplace_back(id, std::move(layer));
}

void LayerList::remove(std::uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  layers_.erase(std::remove_if(layers_.begin(), layers_.end(),
                               [id](const std::pair<std::uint64_t, Layer>& entry) {
                                 return entry.first == id;
                               }),
                layers_.end());
}

bool LayerList::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return layers_.empty();
}

std::vector<Layer> LayerList::paintOrder() const {
  std::vector<Layer> out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(layers_.size());
    for (const auto& entry : layers_) {
      if (!entry.second.visible || !entry.second.source) continue;
      if (entry.second.source->texture() == 0) continue;  // no frame yet
      out.push_back(entry.second);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Layer& a, const Layer& b) {
    if (a.order != b.order) return a.order < b.order;
    return a.sequence < b.sequence;
  });
  return out;
}

bool LayerList::dirty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : layers_) {
    if (entry.second.source && entry.second.source->dirty()) return true;
  }
  return false;
}

void LayerList::clearDirty() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : layers_) {
    if (entry.second.source) entry.second.source->clearDirty();
  }
}

// ---- CompositePass -----------------------------------------------------------

std::unique_ptr<CompositePass> CompositePass::create(std::string& error) {
  std::unique_ptr<CompositePass> pass(new CompositePass());

  const GLuint vertex = compileShader(GL_VERTEX_SHADER, kLayerVertex, error);
  const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kLayerFragment, error);
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "position");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    if (error.empty()) {
      char info[512] = {};
      glGetProgramInfoLog(program, sizeof(info), nullptr, info);
      error = std::string("the layer program did not link: ") + info;
    }
    glDeleteProgram(program);
    return nullptr;
  }
  pass->program_ = program;
  pass->rectUniform_ = glGetUniformLocation(program, "rect");
  pass->opacityUniform_ = glGetUniformLocation(program, "alpha");
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "layer"), 0);

  // A unit quad: the vertex shader places it, so one buffer serves every rect.
  static constexpr GLfloat kQuad[] = {0, 0, 1, 0, 0, 1, 1, 1};
  GLuint buffer = 0;
  glGenBuffers(1, &buffer);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  pass->buffer_ = buffer;
  return pass;
}

CompositePass::~CompositePass() {
  if (buffer_ != 0) glDeleteBuffers(1, &buffer_);
  if (program_ != 0) glDeleteProgram(program_);
}

void CompositePass::draw(const std::vector<Layer>& layers, int drawableWidth, int drawableHeight) {
  if (layers.empty() || drawableWidth <= 0 || drawableHeight <= 0) return;

  glUseProgram(program_);
  glBindBuffer(GL_ARRAY_BUFFER, buffer_);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  const double w = static_cast<double>(drawableWidth);
  const double h = static_cast<double>(drawableHeight);
  for (const Layer& layer : layers) {
    // Wait for the instance's frame before sampling it: a fence where the
    // platform has one, its flush where it does not. Never glFinish.
    waitAndDestroyFence(layer.source->takeFence());

    // Drawable pixels, origin top left (the CSS rect planeFor computes), into
    // NDC, origin centre and y up.
    const double x0 = 2.0 * (layer.rect.x / w) - 1.0;
    const double x1 = 2.0 * ((layer.rect.x + layer.rect.width) / w) - 1.0;
    const double yTop = 1.0 - 2.0 * (layer.rect.y / h);
    const double yBottom = 1.0 - 2.0 * ((layer.rect.y + layer.rect.height) / h);
    glUniform4f(rectUniform_, static_cast<GLfloat>(x0), static_cast<GLfloat>(yBottom),
                static_cast<GLfloat>(x1 - x0), static_cast<GLfloat>(yTop - yBottom));
    glUniform1f(opacityUniform_, static_cast<GLfloat>(layer.opacity));
    glBindTexture(GL_TEXTURE_2D, layer.source->texture());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
}

// ---- fences ------------------------------------------------------------------

void* insertFenceOrFlush() {
  const SyncApi& api = syncApi();
  if (!api.available) {
    glFlush();
    return nullptr;
  }
  GLsync fence = api.fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (fence == nullptr) {
    glFlush();
    return nullptr;
  }
  // A fence is only visible to another context once the commands before it have
  // been flushed; the spec makes this the producer's job.
  glFlush();
  return fence;
}

void discardFence(void* fence) {
  if (fence == nullptr) return;
  const SyncApi& api = syncApi();
  if (api.available) api.deleteSync(static_cast<GLsync>(fence));
}

void waitAndDestroyFence(void* fence) {
  if (fence == nullptr) return;
  const SyncApi& api = syncApi();
  if (!api.available) return;
  // Server-side: the GPU waits, the JS thread does not.
  api.waitSync(static_cast<GLsync>(fence), 0, GL_TIMEOUT_IGNORED);
  api.deleteSync(static_cast<GLsync>(fence));
}

}  // namespace gfx
}  // namespace screenkit
