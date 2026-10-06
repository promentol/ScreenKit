// Copyright (c) ScreenKit contributors. MIT.
#include "VendoredWebGL.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include <GLES3/gl3.h>

#include "GlSurface.h"
#include "SKGLContextSeam.h"
#include "SKGLImageUtils.h"
#include "SKGLNativeContext.h"
#include "SKWebGLRenderer.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// The registry behind `screenkit::gl::ContextGet`.
///
/// Deliberately minimal. Upstream's EXGLContextManager owns EGL surfaces and
/// tears them down; this owns one bookkeeping object and a lock, because
/// `gfx::GlSurface` already owns everything real. Keeping that split is the
/// whole reason the vendoring spec adapted at this seam instead of importing
/// upstream's manager.
struct Entry {
  /// The runtime the context was installed on: a frame flushes only its own
  /// runtime's contexts, and teardown releases only its own.
  const jsi::Runtime* runtime = nullptr;
  std::unique_ptr<gl::SKGLContext> context;
  // The drawable is held here for as long as the context is. Without this the
  // caller's shared_ptr is the only owner, so the surface dies the moment
  // install returns, eglMakeCurrent unbinds, and every later glGetString comes
  // back null -- which surfaces as empty strings from gl.getParameter rather
  // than as anything that points at lifetime.
  std::shared_ptr<gfx::GlSurface> surface;
};

/// What belongs to one runtime rather than to one context, now that a runtime
/// has several: which of its contexts is the page's own drawable, and the one
/// layer list its canvases and its `<iframe>` instances both sort into.
struct RuntimeState {
  gl::SKGLContextId primary = 0;
  std::shared_ptr<gfx::LayerList> layers;
};

struct Registry {
  std::shared_mutex mutex;
  std::unordered_map<gl::SKGLContextId, Entry> contexts;
  std::unordered_map<const jsi::Runtime*, RuntimeState> runtimes;

  static Registry& get() {
    static Registry instance;
    return instance;
  }
};

gl::SKGLContextId g_nextId = 0;

/// Which context is current **on this thread**.
///
/// Every canvas has a real GL context of its own (Architecture.md 3.1), so the
/// page moving between canvases is a `makeCurrent`. Ids are unique across the
/// process and a context is only ever current on its owning thread, so one
/// thread-local answers for the page and for every instance at once. Zero means
/// "unknown": the next lookup binds whatever it is asked for rather than
/// assuming.
thread_local gl::SKGLContextId t_currentContext = 0;
thread_local bool t_warnedSwitch = false;

/// Bind `id`'s context here, unless it is already bound. The registry's lock is
/// held by the caller -- shared is enough, nothing in the map is written.
bool makeCurrentLocked(Registry& registry, gl::SKGLContextId id) {
  if (id == 0) return false;
  if (id == t_currentContext) return true;
  auto it = registry.contexts.find(id);
  if (it == registry.contexts.end() || !it->second.surface) return false;
  if (!it->second.surface->makeCurrent()) {
    if (!t_warnedSwitch) {
      t_warnedSwitch = true;
      log(LogLevel::Error, "screenkit.gl",
          "a canvas's GL context could not be made current on this thread; its drawing is lost");
    }
    return false;
  }
  t_currentContext = id;
  return true;
}

void removeContext(gl::SKGLContextId id) {
  Registry& registry = Registry::get();
  std::unique_lock<std::shared_mutex> lock(registry.mutex);
  auto it = registry.contexts.find(id);
  if (it == registry.contexts.end()) return;
  auto state = registry.runtimes.find(it->second.runtime);
  if (state != registry.runtimes.end() && state->second.primary == id) state->second.primary = 0;
  // Erasing it destroys the surface, which unbinds if it was current. Saying so
  // keeps `makeContextCurrent` honest: it answers "already current" from this
  // variable, and a destroyed context is current to nobody.
  if (id == t_currentContext) t_currentContext = 0;
  registry.contexts.erase(it);
}

/// The backstop for a runtime torn down without `releaseVendoredWebGL` -- one
/// that never went through HermesHost's teardown.
///
/// Upstream drops contexts through EXGLContextManager, which we deliberately do
/// not vendor -- so without a release the registry keeps every context ever
/// created, each holding a `jsi::Runtime&` that is long dead, and a process that
/// builds more than one runtime crashes. A host object's destructor runs when
/// the runtime's heap goes away, but on whichever thread Hermes finalizes on --
/// its background GC thread, measured with `leaks` -- which is why it is only
/// the backstop.
class ContextReaper : public jsi::HostObject {
 public:
  explicit ContextReaper(gl::SKGLContextId id) : id_(id) {}
  ~ContextReaper() override { removeContext(id_); }

 private:
  gl::SKGLContextId id_;
};

}  // namespace

namespace gl {

// The seam SKGLContextSeam.h declares and deliberately leaves undefined. Every
// vendored call site checks for null, so a lookup racing teardown yields
// `undefined` in JS rather than a crash.

namespace {

/// Which WebGL class a binding query answers with.
SKWebGLClass classForBinding(GLenum pname) {
  switch (pname) {
    case GL_TEXTURE_BINDING_2D:
    case GL_TEXTURE_BINDING_2D_ARRAY:
    case GL_TEXTURE_BINDING_3D:
    case GL_TEXTURE_BINDING_CUBE_MAP:
      return SKWebGLClass::WebGLTexture;
    case GL_DRAW_FRAMEBUFFER_BINDING:
    case GL_READ_FRAMEBUFFER_BINDING:
      return SKWebGLClass::WebGLFramebuffer;
    case GL_RENDERBUFFER_BINDING:
      return SKWebGLClass::WebGLRenderbuffer;
    case GL_SAMPLER_BINDING:
      return SKWebGLClass::WebGLSampler;
    case GL_TRANSFORM_FEEDBACK_BINDING:
      return SKWebGLClass::WebGLTransformFeedback;
    case GL_VERTEX_ARRAY_BINDING:
      return SKWebGLClass::WebGLVertexArrayObject;
    default:  // COPY_READ/COPY_WRITE, TRANSFORM_FEEDBACK_BUFFER, UNIFORM_BUFFER
      return SKWebGLClass::WebGLBuffer;
  }
}

}  // namespace

void readSupportedExtensions(SKGLContext* ctx) {
  std::vector<std::string> names;
  ctx->addBlockingToNextBatch([&] {
    if (ctx->supportsWebGL2) {
      GLint count = 0;
      glGetIntegerv(GL_NUM_EXTENSIONS, &count);
      for (GLint i = 0; i < count; ++i) {
        names.emplace_back(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)));
      }
    } else if (const auto* list = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS))) {
      std::istringstream words(list);
      for (std::string name; words >> name;) names.push_back(std::move(name));
    }
  });

  auto& supported = ctx->supportedExtensions;
  for (std::string& name : names) {
    // GLES prefixes extension names with `GL_`; WebGL does not.
    if (name.compare(0, 3, "GL_") == 0) name.erase(0, 3);
    supported.insert(name);
  }
#ifdef __APPLE__
  // As upstream: every Apple GPU decodes PVRTC.
  supported.insert("WEBGL_compressed_texture_pvrtc");
#endif

  if (ctx->supportsWebGL2) {
    // Upstream's list, unchanged: WebGL2 has vertex array objects built in, and
    // ES 3.0 always has these.
    supported.erase("OES_vertex_array_object");
    supported.insert({"OES_texture_float_linear", "OES_texture_half_float_linear",
                      "WEBGL_compressed_texture_astc", "WEBGL_compressed_texture_etc"});
    return;
  }

  // WebGL1 names for the GLES 2 extensions that back them.
  static const std::pair<const char*, const char*> kWebGLNames[] = {
      {"ANGLE_instanced_arrays", "ANGLE_instanced_arrays"},
      {"EXT_instanced_arrays", "ANGLE_instanced_arrays"},
      {"NV_instanced_arrays", "ANGLE_instanced_arrays"},
      {"OES_depth_texture", "WEBGL_depth_texture"},
      {"EXT_draw_buffers", "WEBGL_draw_buffers"},
      {"EXT_texture_compression_s3tc", "WEBGL_compressed_texture_s3tc"},
      {"OES_compressed_ETC1_RGB8_texture", "WEBGL_compressed_texture_etc1"},
      {"KHR_texture_compression_astc_ldr", "WEBGL_compressed_texture_astc"},
  };
  for (const auto& [gles, webgl] : kWebGLNames) {
    if (supported.count(gles) != 0) supported.insert(webgl);
  }
}

jsi::Value getObjectBindingParameter(jsi::Runtime& runtime, SKGLContext* ctx, GLenum pname) {
  GLint name = 0;
  ctx->addBlockingToNextBatch([&] { glGetIntegerv(pname, &name); });

  // Nothing bound. `null` is what WebGL specifies, and it is exactly the value
  // Lightning's context wrapper seeds each texture unit's cache entry with.
  if (name == 0) return jsi::Value::null();

  // The reverse of upstream's id -> GL name map. Safe to read here: this runs on
  // the JS thread, which is also the GL thread, and the blocking batch above has
  // already flushed anything that could still be writing to `objects`.
  for (const auto& entry : ctx->objects) {
    if (entry.second == static_cast<GLuint>(name)) {
      return createWebGLObject(runtime, classForBinding(pname),
                               {static_cast<double>(entry.first)});
    }
  }
  // Bound to something JS never created -- the host's default framebuffer, say.
  // There is no JS object that can stand for it; null is the least wrong answer
  // and matches what WebGL reports for the default framebuffer.
  return jsi::Value::null();
}

// --- bufferData / bufferSubData ----------------------------------------------
//
// See SKGLContextSeam.h for the behaviour and why it follows Chromium. The shape
// here is WebIDL's: resolve the overload from the argument count and the type of
// the data argument, convert every argument in order, then run the operation.

namespace {

const char* contextClassName(const SKGLContext* ctx) {
  return ctx->supportsWebGL2 ? "WebGL2RenderingContext" : "WebGLRenderingContext";
}

std::string failedToExecute(const SKGLContext* ctx, const char* method) {
  return std::string("Failed to execute '") + method + "' on '" + contextClassName(ctx) + "': ";
}

[[noreturn]] void throwTypeError(jsi::Runtime& runtime, const std::string& message) {
  jsi::Value error = runtime.global()
                         .getPropertyAsFunction(runtime, "TypeError")
                         .callAsConstructor(runtime, jsi::String::createFromUtf8(runtime, message));
  throw jsi::JSError(runtime, std::move(error));
}

/// ECMAScript ToNumber. `Number(v)` is exactly that for every type except BigInt,
/// which ToNumber rejects and `Number` accepts, so BigInt is refused first. A
/// Symbol, or an object whose valueOf throws, throws out of the call as in JS.
double toNumber(jsi::Runtime& runtime, const jsi::Value& value) {
  if (value.isNumber()) return value.getNumber();
  if (value.isBigInt()) throwTypeError(runtime, "Cannot convert a BigInt value to a number");
  return runtime.global().getPropertyAsFunction(runtime, "Number").call(runtime, value).getNumber();
}

/// WebIDL's integer conversion without [EnforceRange]: NaN and the infinities
/// become 0, anything else truncates toward zero and wraps modulo 2^64. The
/// narrower types are this, wrapped again by the cast.
uint64_t toWrappedInteger(jsi::Runtime& runtime, const jsi::Value& value) {
  const double x = toNumber(runtime, value);
  if (!std::isfinite(x)) return 0;
  // fmod is exact, and its result is strictly inside (-2^64, 2^64), so both casts
  // below are in range. Adding 2^64 to a negative instead would round -1 up to
  // 2^64 itself.
  const double wrapped = std::fmod(std::trunc(x), 18446744073709551616.0);
  if (wrapped >= 0) return static_cast<uint64_t>(wrapped);
  return uint64_t{0} - static_cast<uint64_t>(-wrapped);
}

GLenum toGLenum(jsi::Runtime& runtime, const jsi::Value& value) {  // unsigned long
  return static_cast<GLenum>(static_cast<uint32_t>(toWrappedInteger(runtime, value)));
}

int64_t toLongLong(jsi::Runtime& runtime, const jsi::Value& value) {  // GLsizeiptr, GLintptr
  return static_cast<int64_t>(toWrappedInteger(runtime, value));
}

/// True for a DataView, and for anything else `ArrayBuffer.isView` accepts that
/// `jsi::Object::isTypedArray` does not. This is the slow way -- a call back into
/// JS -- and it is only on the path a typed array has already left.
bool isViewThroughJs(jsi::Runtime& runtime, const jsi::Value& value) {
  jsi::Value result = runtime.global()
                          .getPropertyAsObject(runtime, "ArrayBuffer")
                          .getPropertyAsFunction(runtime, "isView")
                          .call(runtime, value);
  return result.isBool() && result.getBool();
}

bool isArrayBufferView(jsi::Runtime& runtime, const jsi::Value& value) {
  if (!value.isObject()) return false;
  // The engine's own answer first: a real type check against the object, not a
  // function call into JS. Every typed array WebGL is handed in anger -- the
  // Float32Array of vertices, the Uint8Array of pixels -- is answered here.
  if (value.getObject(runtime).isTypedArray(runtime)) return true;
  return isViewThroughJs(runtime, value);
}

bool isArrayBuffer(jsi::Runtime& runtime, const jsi::Value& value) {
  return value.isObject() && value.getObject(runtime).isArrayBuffer(runtime);
}

/// The bytes of an ArrayBuffer or an ArrayBufferView, and the view's element
/// size (1 for an ArrayBuffer and for a DataView).
///
/// A browser reads a view's internal slots; JSI can only read its properties,
/// and an instance can shadow `byteOffset` or `byteLength` with anything. So the
/// range is checked against the real buffer before a pointer is formed.
///
/// `data` points into the JS heap and is good for the length of the call that
/// produced it, nothing longer: JS gets control back the moment the host
/// function returns and may write to that array again. Passing it straight to GL
/// is only correct because `addToNextBatch` runs the op there and then
/// (SKGLNativeContext.h) -- anything that queued it for later would have to own
/// a copy, which is what upstream did and what this used to.
///
/// JSI's TypedArray accessors do not help here, however much they look like they
/// should: `jsi.h` says "the 'byteLength' property of", and means it. Measured
/// against a Float32Array with `byteLength` shadowed to 4 and `byteOffset` to
/// 999999, `view.byteLength()` answers 4 -- so they are the same property reads
/// spelled differently, and the range check below is what makes either safe
/// (gl-typed-array-slots). Only a JSI that exposed the slots, as a browser's
/// bindings do, could drop it.
struct Bytes {
  const uint8_t* data = nullptr;
  size_t byteLength = 0;
  size_t elementSize = 1;
};

Bytes bytesOf(jsi::Runtime& runtime, const SKGLContext* ctx, const char* method,
              const jsi::Value& value) {
  jsi::Object object = value.getObject(runtime);
  if (object.isArrayBuffer(runtime)) {
    jsi::ArrayBuffer buffer = object.getArrayBuffer(runtime);
    return {buffer.data(runtime), buffer.size(runtime), 1};
  }
  jsi::Value backing = object.getProperty(runtime, "buffer");
  jsi::Value offset = object.getProperty(runtime, "byteOffset");
  jsi::Value length = object.getProperty(runtime, "byteLength");
  jsi::Value perElement = object.getProperty(runtime, "BYTES_PER_ELEMENT");
  if (!isArrayBuffer(runtime, backing) || !offset.isNumber() || !length.isNumber()) {
    throwTypeError(runtime, failedToExecute(ctx, method) + "the ArrayBufferView has no usable buffer.");
  }
  jsi::ArrayBuffer buffer = backing.getObject(runtime).getArrayBuffer(runtime);
  const double byteOffset = offset.getNumber();
  const double byteLength = length.getNumber();
  const double capacity = static_cast<double>(buffer.size(runtime));
  if (!(byteOffset >= 0 && byteLength >= 0 && byteOffset + byteLength <= capacity)) {
    throwTypeError(runtime, failedToExecute(ctx, method) +
                                "the ArrayBufferView's byte range is outside its buffer.");
  }
  size_t elementSize = 1;
  if (perElement.isNumber() && perElement.getNumber() >= 1) {
    elementSize = static_cast<size_t>(perElement.getNumber());
  }
  return {buffer.data(runtime) + static_cast<size_t>(byteOffset), static_cast<size_t>(byteLength),
          elementSize};
}

/// Raise GL_INVALID_VALUE in ANGLE's error flag, which is what gl.getError()
/// reads. Chromium raises it for null data before it looks at the target or
/// the bound buffer; a negative size is likewise the first thing ANGLE rejects,
/// so this raises INVALID_VALUE and nothing else, and changes no state.
void raiseInvalidValue(SKGLContext* ctx) {
  ctx->addToNextBatch([] { glBufferData(GL_ARRAY_BUFFER, -1, nullptr, GL_STATIC_DRAW); });
}

/// Raise GL_INVALID_ENUM the same way: a pname no GL knows.
void raiseInvalidEnum(SKGLContext* ctx) {
  ctx->addToNextBatch([] { glPixelStorei(GL_NONE, 0); });
}

/// WebGL2's `srcOffset` / `length`, in elements. False, with INVALID_VALUE
/// raised, when the range does not fit the view.
bool sliceElements(SKGLContext* ctx, Bytes& bytes, uint64_t srcOffset, uint32_t length) {
  const uint64_t elements = bytes.byteLength / bytes.elementSize;
  if (srcOffset > elements || (length != 0 && length > elements - srcOffset)) {
    raiseInvalidValue(ctx);
    return false;
  }
  const uint64_t count = length != 0 ? length : elements - srcOffset;
  bytes.data += srcOffset * bytes.elementSize;
  bytes.byteLength = static_cast<size_t>(count * bytes.elementSize);
  return true;
}

size_t webglArgumentCount(const SKGLContext* ctx, size_t count) {
  // WebGL1 has only the three-argument forms, and WebIDL ignores extras.
  return std::min(count, ctx->supportsWebGL2 ? size_t{5} : size_t{3});
}

void requireArguments(jsi::Runtime& runtime, const SKGLContext* ctx, const char* method,
                      size_t count) {
  if (count < 3) {
    throwTypeError(runtime, failedToExecute(ctx, method) + "3 arguments required, but only " +
                                std::to_string(count) + " present.");
  }
}

}  // namespace

jsi::Value webIdlBufferData(jsi::Runtime& runtime, SKGLContext* ctx, const jsi::Value* args,
                            size_t count) {
  requireArguments(runtime, ctx, "bufferData", count);
  const size_t argc = webglArgumentCount(ctx, count);

  if (argc >= 4) {
    // bufferData(target, ArrayBufferView srcData, usage, srcOffset, length = 0)
    if (!isArrayBufferView(runtime, args[1])) {
      throwTypeError(runtime, failedToExecute(ctx, "bufferData") +
                                  "parameter 2 is not of type 'ArrayBufferView'.");
    }
    const GLenum target = toGLenum(runtime, args[0]);
    const GLenum usage = toGLenum(runtime, args[2]);
    const uint64_t srcOffset = toWrappedInteger(runtime, args[3]);
    const uint32_t length = argc == 5 ? static_cast<uint32_t>(toWrappedInteger(runtime, args[4])) : 0;
    Bytes bytes = bytesOf(runtime, ctx, "bufferData", args[1]);
    if (!sliceElements(ctx, bytes, srcOffset, length)) return jsi::Value::undefined();
    ctx->addToNextBatch([&] {
      glBufferData(target, static_cast<GLsizeiptr>(bytes.byteLength), bytes.data, usage);
    });
    return jsi::Value::undefined();
  }

  const jsi::Value& sizeOrData = args[1];
  const GLenum target = toGLenum(runtime, args[0]);

  // bufferData(target, BufferSource? srcData, usage) -- the nullable overload.
  if (sizeOrData.isNull() || sizeOrData.isUndefined()) {
    toGLenum(runtime, args[2]);  // converted even though unused, as WebIDL does
    raiseInvalidValue(ctx);
    return jsi::Value::undefined();
  }
  if (isArrayBuffer(runtime, sizeOrData) || isArrayBufferView(runtime, sizeOrData)) {
    const GLenum usage = toGLenum(runtime, args[2]);
    const Bytes bytes = bytesOf(runtime, ctx, "bufferData", sizeOrData);
    ctx->addToNextBatch([&] {
      glBufferData(target, static_cast<GLsizeiptr>(bytes.byteLength), bytes.data, usage);
    });
    return jsi::Value::undefined();
  }

  // bufferData(target, GLsizeiptr size, usage) -- everything else lands here,
  // which is why an Array allocates zero bytes rather than throwing.
  const int64_t size = toLongLong(runtime, sizeOrData);
  const GLenum usage = toGLenum(runtime, args[2]);
  ctx->addToNextBatch([=] { glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, usage); });
  return jsi::Value::undefined();
}

jsi::Value webIdlBufferSubData(jsi::Runtime& runtime, SKGLContext* ctx, const jsi::Value* args,
                               size_t count) {
  requireArguments(runtime, ctx, "bufferSubData", count);
  const size_t argc = webglArgumentCount(ctx, count);
  const jsi::Value& srcData = args[2];

  if (argc >= 4) {
    // bufferSubData(target, dstByteOffset, ArrayBufferView srcData, srcOffset, length = 0)
    if (!isArrayBufferView(runtime, srcData)) {
      throwTypeError(runtime, failedToExecute(ctx, "bufferSubData") +
                                  "parameter 3 is not of type 'ArrayBufferView'.");
    }
  } else if (!isArrayBuffer(runtime, srcData) && !isArrayBufferView(runtime, srcData)) {
    // bufferSubData(target, dstByteOffset, BufferSource srcData) -- not nullable,
    // and there is no size overload to fall back to.
    throwTypeError(runtime, failedToExecute(ctx, "bufferSubData") +
                                "parameter 3 is not of type '(ArrayBuffer or ArrayBufferView)'.");
  }

  const GLenum target = toGLenum(runtime, args[0]);
  const int64_t dstByteOffset = toLongLong(runtime, args[1]);
  uint64_t srcOffset = 0;
  uint32_t length = 0;
  if (argc >= 4) {
    srcOffset = toWrappedInteger(runtime, args[3]);
    if (argc == 5) length = static_cast<uint32_t>(toWrappedInteger(runtime, args[4]));
  }
  Bytes bytes = bytesOf(runtime, ctx, "bufferSubData", srcData);
  if (argc >= 4 && !sliceElements(ctx, bytes, srcOffset, length)) return jsi::Value::undefined();
  ctx->addToNextBatch([&] {
    glBufferSubData(target, static_cast<GLintptr>(dstByteOffset),
                    static_cast<GLsizeiptr>(bytes.byteLength), bytes.data);
  });
  return jsi::Value::undefined();
}

// --- texImage2D / texSubImage2D -----------------------------------------------
//
// See SKGLContextSeam.h. The upload keeps upstream's shape -- decode or copy on
// the JS thread, flip if asked, queue the GL call -- and adds the premultiply step
// between the flip and the queue, where a browser does it.

namespace {

/// Pixels ready to upload, and the size they really have.
struct Pixels {
  std::shared_ptr<uint8_t> decoded;  // from the vendored file loader
  std::vector<uint8_t> copied;       // from a typed array or ImageData
  GLsizei width = 0;
  GLsizei height = 0;

  uint8_t* data() {
    if (decoded) return decoded.get();
    return copied.empty() ? nullptr : copied.data();
  }
};

/// An image source: a file behind `localUri`, or pixels in `data`. False, with
/// nothing to upload, for anything else -- upstream's behaviour, a 0x0 texture.
bool readImageSource(jsi::Runtime& runtime, const jsi::Object& source, Pixels& out) {
  if (source.getProperty(runtime, "localUri").isString()) {
    int width = 0;
    int height = 0;
    out.decoded = loadImage(runtime, source, &width, &height, nullptr);
    out.width = width;
    out.height = height;
    return out.decoded != nullptr;
  }
  jsi::Value data = source.getProperty(runtime, "data");
  // An <img> of a downloaded image carries its decoded pixels in `data`; its
  // `width`/`height` are what the page set, and the pixels are the natural size.
  jsi::Value width = source.getProperty(runtime, "naturalWidth");
  jsi::Value height = source.getProperty(runtime, "naturalHeight");
  if (!width.isNumber() || !height.isNumber()) {
    width = source.getProperty(runtime, "width");
    height = source.getProperty(runtime, "height");
  }
  if (!data.isObject() || !width.isNumber() || !height.isNumber()) return false;
  jsi::Object array = data.getObject(runtime);
  if (!array.isArrayBuffer(runtime) && !isTypedArray(runtime, array)) return false;
  out.copied = rawTypedArray(runtime, array);
  out.width = static_cast<GLsizei>(width.getNumber());
  out.height = static_cast<GLsizei>(height.getNumber());
  const size_t needed = static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4;
  if (out.width <= 0 || out.height <= 0 || out.copied.size() < needed) {
    out.copied.clear();
    return false;
  }
  return true;
}

/// Chromium's rule: an ImageBitmap is uploaded in the alpha state it was created
/// with, whatever the flag says; every other source follows the flag.
bool premultiplyFor(jsi::Runtime& runtime, const SKGLContext* ctx, const jsi::Object* source) {
  if (source != nullptr) {
    jsi::Value state = source->getProperty(runtime, "_premultiplied");
    if (state.isBool()) return state.getBool();
  }
  return ctx->unpackPremultiplyAlpha;
}

/// Multiply colour by alpha in place, rounding as Chromium does: 200 at alpha 128
/// becomes 100, not 101.
void premultiplyRGBA8(uint8_t* pixels, size_t count) {
  for (size_t i = 0; i < count; ++i, pixels += 4) {
    const unsigned alpha = pixels[3];
    if (alpha == 255) continue;
    for (int c = 0; c < 3; ++c) pixels[c] = static_cast<uint8_t>((pixels[c] * alpha + 127) / 255);
  }
}

/// Flip, then premultiply, in the order a browser applies the two unpack steps.
void prepare(const SKGLContext* ctx, Pixels& pixels, GLenum format, GLenum type, bool premultiply) {
  uint8_t* data = pixels.data();
  if (data == nullptr) return;
  if (ctx->unpackFLipY) flipPixels(data, pixels.width * bytesPerPixel(type, format), pixels.height);
  if (premultiply && format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
    premultiplyRGBA8(data, static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height));
  }
}

GLint toGLint(jsi::Runtime& runtime, const jsi::Value& value) {
  return static_cast<GLint>(toLongLong(runtime, value));
}

bool isBufferData(jsi::Runtime& runtime, const jsi::Value& value) {
  return value.isObject() &&
         (value.getObject(runtime).isArrayBuffer(runtime) || isTypedArray(runtime, value.getObject(runtime)));
}

}  // namespace

double webIdlToNumber(jsi::Runtime& runtime, const jsi::Value& value) { return toNumber(runtime, value); }

uint64_t webIdlToInteger(jsi::Runtime& runtime, const jsi::Value& value) {
  return toWrappedInteger(runtime, value);
}

jsi::Value webglPixelStorei(jsi::Runtime& runtime, SKGLContext* ctx, const jsi::Value* args,
                            size_t count) {
  if (count < 2) {
    throwTypeError(runtime, failedToExecute(ctx, "pixelStorei") + "2 arguments required, but only " +
                                std::to_string(count) + " present.");
  }
  const GLenum pname = toGLenum(runtime, args[0]);
  const GLint param = static_cast<GLint>(static_cast<int32_t>(toWrappedInteger(runtime, args[1])));
  switch (pname) {
    case GL_UNPACK_FLIP_Y_WEBGL:
      ctx->unpackFLipY = param != 0;
      break;
    case GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL:
      ctx->unpackPremultiplyAlpha = param != 0;
      break;
    case GL_UNPACK_COLORSPACE_CONVERSION_WEBGL:
      if (param == GL_BROWSER_DEFAULT_WEBGL || param == GL_NONE) {
        ctx->unpackColorspaceConversion = static_cast<GLenum>(param);
      } else {
        raiseInvalidEnum(ctx);
      }
      break;
    default:
      // PACK_ALIGNMENT, UNPACK_ALIGNMENT and WebGL2's row length, image height and
      // skips are GL's; GL rejects anything else with INVALID_ENUM, and range
      // errors (an alignment of 3) with INVALID_VALUE, as WebGL specifies.
      ctx->addToNextBatch([pname, param] { glPixelStorei(pname, param); });
      break;
  }
  return jsi::Value::undefined();
}

jsi::Value webglTexImage2D(jsi::Runtime& runtime, SKGLContext* ctx, const jsi::Value* args,
                           size_t count) {
  if (count != 9 && count != 6) {
    throw std::runtime_error("SKGL: Invalid number of arguments to gl.texImage2D()!");
  }
  const GLenum target = toGLenum(runtime, args[0]);
  const GLint level = toGLint(runtime, args[1]);
  const GLint internalformat = toGLint(runtime, args[2]);

  if (count == 9) {
    // texImage2D(target, level, internalformat, width, height, border, format, type, pixels)
    const GLsizei width = toGLint(runtime, args[3]);
    const GLsizei height = toGLint(runtime, args[4]);
    const GLint border = toGLint(runtime, args[5]);
    const GLenum format = toGLenum(runtime, args[6]);
    const GLenum type = toGLenum(runtime, args[7]);
    const jsi::Value& source = args[8];
    if (source.isNull() || source.isUndefined()) {
      ctx->addToNextBatch([=] {
        glTexImage2D(target, level, internalformat, width, height, border, format, type, nullptr);
      });
      return jsi::Value::null();
    }
    auto pixels = std::make_shared<Pixels>();
    if (isBufferData(runtime, source)) {
      pixels->copied = rawTypedArray(runtime, source.getObject(runtime));
      pixels->width = width;
      pixels->height = height;
      prepare(ctx, *pixels, format, type, ctx->unpackPremultiplyAlpha);
    } else {
      // An image source's own size wins over the arguments, as upstream had it.
      jsi::Object object = source.getObject(runtime);
      readImageSource(runtime, object, *pixels);
      prepare(ctx, *pixels, format, type, premultiplyFor(runtime, ctx, &object));
    }
    ctx->addToNextBatch([=] {
      glTexImage2D(target, level, internalformat, pixels->width, pixels->height, border, format,
                   type, pixels->data());
    });
    return jsi::Value::null();
  }

  // texImage2D(target, level, internalformat, format, type, source)
  const GLenum format = toGLenum(runtime, args[3]);
  const GLenum type = toGLenum(runtime, args[4]);
  auto pixels = std::make_shared<Pixels>();
  if (args[5].isObject()) {
    jsi::Object object = args[5].getObject(runtime);
    readImageSource(runtime, object, *pixels);
    prepare(ctx, *pixels, format, type, premultiplyFor(runtime, ctx, &object));
  }
  ctx->addToNextBatch([=] {
    glTexImage2D(target, level, internalformat, pixels->width, pixels->height, 0, format, type,
                 pixels->data());
  });
  return jsi::Value::null();
}

jsi::Value webglTexSubImage2D(jsi::Runtime& runtime, SKGLContext* ctx, const jsi::Value* args,
                              size_t count) {
  if (count != 9 && count != 7) {
    throw std::runtime_error("SKGL: Invalid number of arguments to gl.texSubImage2D()!");
  }
  const GLenum target = toGLenum(runtime, args[0]);
  const GLint level = toGLint(runtime, args[1]);
  const GLint xoffset = toGLint(runtime, args[2]);
  const GLint yoffset = toGLint(runtime, args[3]);

  if (count == 9) {
    // texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels)
    const GLsizei width = toGLint(runtime, args[4]);
    const GLsizei height = toGLint(runtime, args[5]);
    const GLenum format = toGLenum(runtime, args[6]);
    const GLenum type = toGLenum(runtime, args[7]);
    const jsi::Value& source = args[8];
    if (source.isNull() || source.isUndefined()) {
      // There is nothing to copy. Upstream called glTexImage2D with sub-image
      // arguments here; a negative size raises INVALID_VALUE and changes nothing.
      ctx->addToNextBatch([=] {
        glTexSubImage2D(target, level, xoffset, yoffset, -1, -1, format, type, nullptr);
      });
      return jsi::Value::null();
    }
    auto pixels = std::make_shared<Pixels>();
    if (isBufferData(runtime, source)) {
      pixels->copied = rawTypedArray(runtime, source.getObject(runtime));
      pixels->width = width;
      pixels->height = height;
      prepare(ctx, *pixels, format, type, ctx->unpackPremultiplyAlpha);
    } else {
      jsi::Object object = source.getObject(runtime);
      readImageSource(runtime, object, *pixels);
      prepare(ctx, *pixels, format, type, premultiplyFor(runtime, ctx, &object));
    }
    ctx->addToNextBatch([=] {
      glTexSubImage2D(target, level, xoffset, yoffset, pixels->width, pixels->height, format, type,
                      pixels->data());
    });
    return jsi::Value::null();
  }

  // texSubImage2D(target, level, xoffset, yoffset, format, type, source)
  const GLenum format = toGLenum(runtime, args[4]);
  const GLenum type = toGLenum(runtime, args[5]);
  auto pixels = std::make_shared<Pixels>();
  if (args[6].isObject()) {
    jsi::Object object = args[6].getObject(runtime);
    readImageSource(runtime, object, *pixels);
    prepare(ctx, *pixels, format, type, premultiplyFor(runtime, ctx, &object));
  }
  ctx->addToNextBatch([=] {
    glTexSubImage2D(target, level, xoffset, yoffset, pixels->width, pixels->height, format, type,
                    pixels->data());
  });
  return jsi::Value::null();
}

ContextWithLock ContextGet(SKGLContextId id) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  auto it = registry.contexts.find(id);
  if (it == registry.contexts.end()) {
    return ContextWithLock{nullptr, std::shared_lock<std::shared_mutex>{}};
  }
  // Every canvas has a GL context of its own, so this is where the page moving
  // between canvases costs anything: an id comparison on every vendored call,
  // and a `makeCurrent` only where the page actually alternates. A page with
  // one canvas never switches (spec-m6-composited-canvases, "Design Notes").
  if (id != t_currentContext) makeCurrentLocked(registry, id);
  return {it->second.context.get(), std::move(lock)};
}

namespace {

// --- drawingBufferWidth / drawingBufferHeight -----------------------------------
//
// Upstream set both once, as plain data properties, from the viewport at
// install time. GlSurface::width()/height() are live (eglQuerySurface), so after a
// resize the surface was right and `gl` -- and `canvas.width`, which the DOM shim
// reads from it -- kept reporting the old size. A browser defines them as
// getter-only accessors on the context prototypes, so that is what replaces them,
// and the snapshot is gone from the vendored tree itself (tools/vendor/expo-gl.rules)
// -- with one context per canvas, the second install would otherwise write a data
// property through an accessor that has no setter, and throw.

enum class Dimension { Width, Height };

jsi::Function drawingBufferGetter(jsi::Runtime& runtime, const char* name, Dimension dimension) {
  return jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, name), 0,
      [dimension](jsi::Runtime& rt, const jsi::Value& self, const jsi::Value*,
                  size_t) -> jsi::Value {
        jsi::Value id = self.isObject() ? self.getObject(rt).getProperty(rt, "contextId")
                                        : jsi::Value::undefined();
        if (!id.isNumber()) throwTypeError(rt, "Illegal invocation");

        Registry& registry = Registry::get();
        std::shared_lock<std::shared_mutex> lock(registry.mutex);
        auto it = registry.contexts.find(static_cast<SKGLContextId>(id.getNumber()));
        // A context that is gone reports 0, as a lost WebGL context does.
        if (it == registry.contexts.end() || !it->second.surface) return 0;
        const gfx::GlSurface& surface = *it->second.surface;
        return dimension == Dimension::Width ? surface.width() : surface.height();
      });
}

void installDrawingBufferSize(jsi::Runtime& runtime, jsi::Object& gl) {
  (void)gl;  // nothing of the context object's own shadows the accessors now
  jsi::Object global = runtime.global();
  jsi::Object object = global.getPropertyAsObject(runtime, "Object");
  jsi::Function defineProperty = object.getPropertyAsFunction(runtime, "defineProperty");

  const struct {
    const char* name;
    Dimension dimension;
  } kProperties[] = {{"drawingBufferWidth", Dimension::Width},
                     {"drawingBufferHeight", Dimension::Height}};

  // Both prototypes carry their own accessor in a browser; WebGL2's is not merely
  // inherited.
  for (const char* className : {"WebGLRenderingContext", "WebGL2RenderingContext"}) {
    jsi::Value constructor = global.getProperty(runtime, className);
    if (!constructor.isObject()) continue;
    jsi::Value prototype = constructor.getObject(runtime).getProperty(runtime, "prototype");
    if (!prototype.isObject()) continue;
    for (const auto& property : kProperties) {
      jsi::Object descriptor(runtime);
      descriptor.setProperty(runtime, "get",
                             drawingBufferGetter(runtime, property.name, property.dimension));
      descriptor.setProperty(runtime, "enumerable", true);
      descriptor.setProperty(runtime, "configurable", true);
      defineProperty.call(runtime, prototype,
                          jsi::String::createFromAscii(runtime, property.name), descriptor);
    }
  }
}

}  // namespace

}  // namespace gl

namespace gfx {

GlContextId installVendoredWebGL(jsi::Runtime& runtime, const std::shared_ptr<GlSurface>& surface,
                                 bool asGlobal) {
  if (!surface) {
    log(LogLevel::Error, "screenkit", "installVendoredWebGL: no GL surface");
    return 0;
  }

  // The vendored prepareOpenGLESContext does `std::string(glGetString(GL_VERSION))`
  // with no null check, so a context that is not current here is not an error
  // message -- it is strlen(NULL) deep inside the GL bindings. Refuse early and
  // say so.
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (version == nullptr) {
    log(LogLevel::Error, "screenkit",
        "installVendoredWebGL: no current GL context on this thread (glGetString(GL_VERSION) "
        "returned null) -- refusing to install WebGL");
    return 0;
  }

  gl::SKGLContext* context = nullptr;
  gl::SKGLContextId id = 0;
  {
    Registry& registry = Registry::get();
    std::unique_lock<std::shared_mutex> lock(registry.mutex);
    id = ++g_nextId;
    Entry entry;
    entry.runtime = &runtime;
    entry.context = std::make_unique<gl::SKGLContext>(id);
    entry.surface = surface;  // keep the drawable alive for the context's life
    context = entry.context.get();
    registry.contexts.emplace(id, std::move(entry));
    RuntimeState& state = registry.runtimes[&runtime];
    if (asGlobal && state.primary == 0) state.primary = id;
  }
  // The caller made this surface current before calling -- both the null check
  // above and `prepareOpenGLESContext` below depend on it -- so record it
  // rather than let the first vendored call rebind what is already bound.
  t_currentContext = id;

  // Upstream queues GL work and asks the GL thread to drain it. Here the JS
  // thread *is* the GL thread and the context is already current, so draining
  // inline is the whole of it -- there is no thread to hop to.
  // A fixed-size surface draws into its own framebuffer; WebGL's null binding is that one.
  context->defaultFramebuffer = static_cast<GLint>(surface->defaultFramebuffer());
  context->prepareContext(runtime, [context] { context->flush(); });

  // Upstream parks each context in `__SKGLContexts[id]`, because React Native
  // can have several. So do we, now that every canvas has one; the page's own
  // drawable is bound as `gl` besides, because a bundle should not have to know
  // its context id to draw and because that is the object the DOM shim hands
  // the first canvas.
  try {
    jsi::Value map = runtime.global().getProperty(runtime, "__SKGLContexts");
    if (map.isObject()) {
      jsi::Value ctx = map.getObject(runtime).getProperty(runtime, std::to_string(id).c_str());
      if (ctx.isObject()) {
        jsi::Object glObject = ctx.getObject(runtime);
        gl::installDrawingBufferSize(runtime, glObject);
        if (asGlobal) runtime.global().setProperty(runtime, "gl", std::move(glObject));
      } else {
        log(LogLevel::Error, "screenkit", "vendored WebGL: context " + std::to_string(id) +
                                              " missing from __SKGLContexts");
      }
    }
  } catch (const jsi::JSIException& error) {
    log(LogLevel::Error, "screenkit",
        std::string("vendored WebGL: could not bind `gl`: ") + error.what());
  }

  // One reaper per context, not one global: a runtime has several now, and a
  // single `__screenkitGLReaper` property meant the second install dropped the
  // first canvas's backstop on the floor.
  try {
    jsi::Object global = runtime.global();
    jsi::Value reapers = global.getProperty(runtime, "__screenkitGLReapers");
    if (!reapers.isObject()) {
      global.setProperty(runtime, "__screenkitGLReapers", jsi::Object(runtime));
      reapers = global.getProperty(runtime, "__screenkitGLReapers");
    }
    reapers.getObject(runtime).setProperty(
        runtime, std::to_string(id).c_str(),
        jsi::Object::createFromHostObject(runtime, std::make_shared<ContextReaper>(id)));
  } catch (const jsi::JSIException& error) {
    log(LogLevel::Error, "screenkit",
        std::string("vendored WebGL: could not park the context reaper: ") + error.what());
  }
  return static_cast<GlContextId>(id);
}

jsi::Value takeContextObject(jsi::Runtime& runtime, GlContextId id) {
  if (id == 0) return jsi::Value::undefined();
  jsi::Value map = runtime.global().getProperty(runtime, "__SKGLContexts");
  if (!map.isObject()) return jsi::Value::undefined();
  jsi::Object contexts = map.getObject(runtime);
  const std::string key = std::to_string(id);
  jsi::Value context = contexts.getProperty(runtime, key.c_str());
  try {
    runtime.global()
        .getPropertyAsObject(runtime, "Reflect")
        .getPropertyAsFunction(runtime, "deleteProperty")
        .call(runtime, contexts, jsi::String::createFromAscii(runtime, key));
  } catch (const jsi::JSIException& error) {
    log(LogLevel::Error, "screenkit",
        std::string("vendored WebGL: could not unpark a context: ") + error.what());
  }
  return context;
}

GlContextId primaryContext(jsi::Runtime& runtime) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  auto state = registry.runtimes.find(&runtime);
  return state == registry.runtimes.end() ? 0 : static_cast<GlContextId>(state->second.primary);
}

bool makeContextCurrent(GlContextId id) {
  if (id != 0 && id == t_currentContext) return true;
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  return makeCurrentLocked(registry, id);
}

void forgetCurrentContext() { t_currentContext = 0; }

std::shared_ptr<GlSurface> surfaceFor(jsi::Runtime& runtime) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  auto state = registry.runtimes.find(&runtime);
  if (state != registry.runtimes.end() && state->second.primary != 0) {
    auto it = registry.contexts.find(state->second.primary);
    if (it != registry.contexts.end() && it->second.surface) return it->second.surface;
  }
  // No primary: a runtime whose only contexts are canvas layers, which a page
  // cannot reach today but a partly torn-down one can. Answering with any of
  // them would hand out a layer's 1x1 pbuffer as if it were the window.
  return nullptr;
}

std::shared_ptr<GlSurface> surfaceFor(jsi::Runtime& runtime, GlContextId id) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  auto it = registry.contexts.find(id);
  if (it == registry.contexts.end() || it->second.runtime != &runtime) return nullptr;
  return it->second.surface;
}

std::shared_ptr<LayerList> layersFor(jsi::Runtime& runtime) {
  Registry& registry = Registry::get();
  std::unique_lock<std::shared_mutex> lock(registry.mutex);
  RuntimeState& state = registry.runtimes[&runtime];
  if (!state.layers) state.layers = std::make_shared<LayerList>();
  return state.layers;
}

bool enableCompositing(jsi::Runtime& runtime, GlSurface& surface, std::string& error) {
  if (!surface.compositing()) {
    if (!surface.enableCompositing(error)) return false;
    // WebGL's `null` framebuffer binding is the page's frame texture now.
    refreshDefaultFramebuffer(runtime, surface);
  }
  surface.setLayers(layersFor(runtime));
  return true;
}

std::size_t contextCount() {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  return registry.contexts.size();
}

void releaseContext(jsi::Runtime& runtime, GlContextId id) {
  Entry released;
  {
    Registry& registry = Registry::get();
    std::unique_lock<std::shared_mutex> lock(registry.mutex);
    auto it = registry.contexts.find(id);
    if (it == registry.contexts.end() || it->second.runtime != &runtime) return;
    released = std::move(it->second);
    registry.contexts.erase(it);
    auto state = registry.runtimes.find(&runtime);
    if (state != registry.runtimes.end() && state->second.primary == id) state->second.primary = 0;
  }
  // The JS side goes too. `__SKGLContexts` holds the context object for the
  // life of the runtime, so without this a canvas that was collected would keep
  // its whole WebGL object graph alive -- the leak the probe-canvas row looks
  // for.
  try {
    jsi::Object global = runtime.global();
    jsi::Function deleteProperty =
        global.getPropertyAsObject(runtime, "Reflect").getPropertyAsFunction(runtime, "deleteProperty");
    const jsi::String key = jsi::String::createFromAscii(runtime, std::to_string(id));
    for (const char* name : {"__SKGLContexts", "__screenkitGLReapers", "__screenkitLayerPainted"}) {
      jsi::Value map = global.getProperty(runtime, name);
      if (map.isObject()) deleteProperty.call(runtime, map.getObject(runtime), key);
    }
  } catch (const jsi::JSIException& error) {
    log(LogLevel::Error, "screenkit",
        std::string("vendored WebGL: could not drop a released context: ") + error.what());
  }
  // Destroyed here, outside the lock and on the calling thread -- the JS
  // thread, where the context is current. `~GlSurface` binds its own context to
  // free its objects and unbinds afterwards, so nothing is current when it
  // returns and the next lookup has to bind for real.
  released = Entry();
  t_currentContext = 0;
}

void refreshDefaultFramebuffer(jsi::Runtime& runtime, const GlSurface& surface) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  for (auto& entry : registry.contexts) {
    if (entry.second.runtime != &runtime || entry.second.surface.get() != &surface) continue;
    entry.second.context->defaultFramebuffer = static_cast<GLint>(surface.defaultFramebuffer());
  }
}

namespace {

/// One canvas layer's frame, if that canvas painted this frame.
///
/// The flag is the shim's, one per context (`__screenkitLayerPainted[id]`, see
/// "Present only frames that painted" in dom-shim.js), so an idle canvas keeps
/// its last image and costs nothing -- the same rule the page's own frame has
/// had since M4, applied per layer.
void publishCanvasLayers(jsi::Runtime& runtime, const std::vector<gl::SKGLContextId>& ids) {
  jsi::Value flags = runtime.global().getProperty(runtime, "__screenkitLayerPainted");
  if (!flags.isObject()) return;
  jsi::Object painted = flags.getObject(runtime);
  for (gl::SKGLContextId id : ids) {
    const std::string key = std::to_string(id);
    jsi::Value flag = painted.getProperty(runtime, key.c_str());
    if (!flag.isBool() || !flag.getBool()) continue;
    std::shared_ptr<GlSurface> layer = surfaceFor(runtime, static_cast<GlContextId>(id));
    if (!layer) continue;
    if (!makeContextCurrent(static_cast<GlContextId>(id))) continue;
    // Cleared only once the frame is really going out: a canvas whose context
    // could not be made current keeps its flag and publishes on the next frame,
    // rather than having its drawing dropped on the floor.
    painted.setProperty(runtime, key.c_str(), false);
    // Not a present: a layer surface finishes into its own texture and
    // publishes it for the compositor.
    layer->swap();
  }
}

}  // namespace

bool presentFrame(jsi::Runtime& runtime, GlSurface& surface, const std::function<void()>& beforePresent) {
  gl::SKGLContextId primary = 0;
  std::vector<gl::SKGLContextId> canvasLayers;
  {
    Registry& registry = Registry::get();
    std::shared_lock<std::shared_mutex> lock(registry.mutex);
    auto state = registry.runtimes.find(&runtime);
    if (state != registry.runtimes.end()) primary = state->second.primary;
    for (auto& entry : registry.contexts) {
      if (entry.second.runtime != &runtime) continue;
      // Both are no-ops now that a GL call runs where it is made
      // (SKGLNativeContext.h). Kept as the frame boundary: if a queue ever comes
      // back -- a real GL thread, a command buffer -- this is where it drains,
      // and every caller already ends its frame through here.
      entry.second.context->endNextBatch();
      entry.second.context->flush();
      // Every other surface of this runtime is one canvas's own layer. (An
      // instance's own layer surface is the one passed in here, so it is never
      // one of these.)
      if (entry.second.surface && entry.second.surface.get() != &surface) {
        canvasLayers.push_back(entry.first);
      }
    }
  }
  // Outside the lock: reading the shim's flags runs JS, and a vendored call
  // from a getter would take this thread's shared lock a second time.
  if (!canvasLayers.empty()) publishCanvasLayers(runtime, canvasLayers);
  // The page's own frame is drawn, and presented, in the page's own context.
  // Whatever a canvas was last drawing on is not it.
  makeContextCurrent(static_cast<GlContextId>(primary));

  bool painted = true;
  try {
    jsi::Object global = runtime.global();
    jsi::Value flag = global.getProperty(runtime, "__screenkitPainted");
    if (flag.isBool()) {
      painted = flag.getBool();
      global.setProperty(runtime, "__screenkitPainted", false);
    }
  } catch (const jsi::JSIException&) {
    painted = true;  // never lose a frame over the bookkeeping
  }

  // An `<iframe>` instance that painted while the page did not is still a new
  // frame on screen, and the page's own frame is in a texture by then, so the
  // present can recompose both. `layersDirty()` is false for a surface with no
  // layers, which is what keeps a page without an iframe on exactly the present
  // path -- and the cost -- it had before the compositor existed.
  if (!painted && !surface.layersDirty()) return false;
  if (beforePresent) beforePresent();
  surface.swap();
  return true;
}

void releaseStaleDrawables(jsi::Runtime& runtime) {
  Registry& registry = Registry::get();
  std::shared_lock<std::shared_mutex> lock(registry.mutex);
  auto state = registry.runtimes.find(&runtime);
  const gl::SKGLContextId primary = state == registry.runtimes.end() ? 0 : state->second.primary;
  for (auto& entry : registry.contexts) {
    // Only the window's own surface has a drawable to let go of; a canvas's
    // layer is a texture that never sees one.
    if (entry.first != primary || !entry.second.surface) continue;
    if (!entry.second.surface->resizedSincePresent()) continue;
    makeCurrentLocked(registry, entry.first);
    // Whatever the page drew goes out with the old drawable, not the new one.
    // The calls have already run; the swap below is what still has to happen
    // against the drawable they were made for.
    entry.second.context->endNextBatch();
    entry.second.context->flush();
    entry.second.surface->swap();
  }
}

void releaseVendoredWebGL(jsi::Runtime& runtime) {
  std::vector<Entry> released;
  std::shared_ptr<LayerList> layers;
  {
    Registry& registry = Registry::get();
    std::unique_lock<std::shared_mutex> lock(registry.mutex);
    for (auto it = registry.contexts.begin(); it != registry.contexts.end();) {
      if (it->second.runtime == &runtime) {
        released.push_back(std::move(it->second));
        it = registry.contexts.erase(it);
      } else {
        ++it;
      }
    }
    auto state = registry.runtimes.find(&runtime);
    if (state != registry.runtimes.end()) {
      layers = std::move(state->second.layers);
      registry.runtimes.erase(state);
    }
  }
  // Destroyed here, outside the lock and on the calling thread -- the JS thread,
  // where the context is current. Every canvas of the page goes with it, which
  // is what makes "released in the owning thread before shutdown returns" true
  // for a page with three canvases as well as for one with one.
  released.clear();
  layers.reset();
  t_currentContext = 0;
}

}  // namespace gfx
}  // namespace screenkit
