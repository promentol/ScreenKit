// Copyright (c) ScreenKit contributors. MIT.
//
// `gl.def` -> host functions. Everything JS-facing about the GLES surface is
// here; everything GL-facing is in the table.
#include "WebGL.h"

#include <GLES3/gl3.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../gfx/GlSurface.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// A real `TypeError`, not a bare `Error`.
///
/// `jsi::JSError(rt, message)` constructs an `Error`, so `catch (e) { e
/// instanceof TypeError }` would be false and a bundle written against the web
/// would take the wrong branch. Constructing the TypeError in JS and wrapping
/// the value is the only way to get the real thing out of JSI.
[[noreturn]] void throwTypeError(jsi::Runtime& runtime, const std::string& message) {
  try {
    jsi::Function constructor = runtime.global().getPropertyAsFunction(runtime, "TypeError");
    throw jsi::JSError(runtime, constructor.call(runtime, message));
  } catch (const jsi::JSError&) {
    throw;
  } catch (const jsi::JSIException&) {
    // No global TypeError, or it refused to construct. An Error still stops the
    // call, which is the part that matters.
    throw jsi::JSError(runtime, message);
  }
}

std::string where(const char* fn, std::size_t index) {
  return std::string("gl.") + fn + ": argument " + std::to_string(index + 1);
}

// ---------------------------------------------------------------------------
// Number coercion
//
// ECMAScript ToInt32/ToUint32 semantics, spelled out rather than left to a C++
// cast. `static_cast<GLuint>(-1.0)` and `static_cast<GLuint>(1e30)` are both
// undefined behaviour, and this suite runs under UBSan -- so out-of-range and
// non-finite inputs are wrapped here, deterministically, before any cast.
// ---------------------------------------------------------------------------

constexpr double kTwo32 = 4294967296.0;

std::uint32_t toUint32(double value) {
  if (!std::isfinite(value)) return 0;
  double integral = std::trunc(value);
  integral = std::fmod(integral, kTwo32);
  if (integral < 0.0) integral += kTwo32;
  return static_cast<std::uint32_t>(integral);
}

std::int32_t toInt32(double value) {
  const std::uint32_t unsignedValue = toUint32(value);
  if (unsignedValue >= 2147483648u) {
    return static_cast<std::int32_t>(static_cast<double>(unsignedValue) - kTwo32);
  }
  return static_cast<std::int32_t>(unsignedValue);
}

float toFloat(double value) {
  if (std::isnan(value)) return std::numeric_limits<float>::quiet_NaN();
  constexpr double kFloatMax = static_cast<double>(std::numeric_limits<float>::max());
  if (value > kFloatMax) return std::numeric_limits<float>::infinity();
  if (value < -kFloatMax) return -std::numeric_limits<float>::infinity();
  return static_cast<float>(value);
}

template <typename T>
T narrow(double value) {
  static_assert(std::is_arithmetic<T>::value, "GL arguments are numbers");
  if constexpr (std::is_floating_point<T>::value) {
    return static_cast<T>(toFloat(value));
  } else if constexpr (std::is_signed<T>::value) {
    return static_cast<T>(toInt32(value));
  } else {
    return static_cast<T>(toUint32(value));
  }
}

double numberArg(jsi::Runtime& runtime, const jsi::Value* args, std::size_t count,
                 std::size_t index, const char* fn) {
  if (index >= count) throwTypeError(runtime, where(fn, index) + " is missing");
  const jsi::Value& value = args[index];
  if (value.isNumber()) return value.getNumber();
  // `true`/`false` for GLboolean-shaped flags is what every WebGL caller
  // writes, so accept it rather than making them spell gl.TRUE.
  if (value.isBool()) return value.getBool() ? 1.0 : 0.0;
  throwTypeError(runtime, where(fn, index) + " must be a number");
}

template <typename T>
T arg(jsi::Runtime& runtime, const jsi::Value* args, std::size_t count, std::size_t index,
      const char* fn) {
  return narrow<T>(numberArg(runtime, args, count, index, fn));
}

bool boolArg(jsi::Runtime& runtime, const jsi::Value* args, std::size_t count, std::size_t index,
             const char* fn) {
  if (index < count && args[index].isBool()) return args[index].getBool();
  return numberArg(runtime, args, count, index, fn) != 0.0;
}

std::string stringArg(jsi::Runtime& runtime, const jsi::Value* args, std::size_t count,
                      std::size_t index, const char* fn) {
  if (index >= count) throwTypeError(runtime, where(fn, index) + " is missing");
  if (!args[index].isString()) throwTypeError(runtime, where(fn, index) + " must be a string");
  return args[index].getString(runtime).utf8(runtime);
}

// ---------------------------------------------------------------------------
// Typed arrays, zero-copy
//
// A `Float32Array` is not an `ArrayBuffer`, so JSI cannot hand back its bytes
// directly. Reading `.buffer` gets the ArrayBuffer behind it and `.byteOffset`
// / `.byteLength` bound the view inside it -- which means the GL upload reads
// the JS heap in place. Nothing is copied into a std::vector on the way.
//
// The pointer is only valid for the duration of the call: Hermes may move or
// free the backing store afterwards, so it must never be stored.
// ---------------------------------------------------------------------------

struct BufferView {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

bool typedArrayView(jsi::Runtime& runtime, const jsi::Value& value, BufferView& out) {
  if (!value.isObject()) return false;
  jsi::Object object = value.getObject(runtime);

  if (object.isArrayBuffer(runtime)) {
    jsi::ArrayBuffer buffer = object.getArrayBuffer(runtime);
    out.data = buffer.data(runtime);
    out.size = buffer.size(runtime);
    return true;
  }

  jsi::Value bufferValue = object.getProperty(runtime, "buffer");
  if (!bufferValue.isObject()) return false;
  jsi::Object bufferObject = bufferValue.getObject(runtime);
  if (!bufferObject.isArrayBuffer(runtime)) return false;

  const jsi::Value offsetValue = object.getProperty(runtime, "byteOffset");
  const jsi::Value lengthValue = object.getProperty(runtime, "byteLength");
  if (!offsetValue.isNumber() || !lengthValue.isNumber()) return false;

  jsi::ArrayBuffer buffer = bufferObject.getArrayBuffer(runtime);
  const double offset = offsetValue.getNumber();
  const double length = lengthValue.getNumber();
  if (offset < 0.0 || length < 0.0) return false;
  const std::size_t total = buffer.size(runtime);
  if (offset + length > static_cast<double>(total)) return false;

  out.data = buffer.data(runtime) + static_cast<std::size_t>(offset);
  out.size = static_cast<std::size_t>(length);
  return true;
}

BufferView requireTypedArray(jsi::Runtime& runtime, const jsi::Value* args, std::size_t count,
                             std::size_t index, const char* fn) {
  BufferView view;
  if (index >= count) throwTypeError(runtime, where(fn, index) + " is missing");
  if (!typedArrayView(runtime, args[index], view)) {
    throwTypeError(runtime, where(fn, index) + " must be a typed array or an ArrayBuffer");
  }
  return view;
}

// ---------------------------------------------------------------------------
// GL_FN: the wrapper deduced from the C function's own type
// ---------------------------------------------------------------------------

template <typename R, typename... A>
constexpr unsigned arityOf(R (*)(A...)) {
  return sizeof...(A);
}

template <typename R, typename... A, std::size_t... I>
jsi::Value invokeGl(R (*function)(A...), jsi::Runtime& runtime, const jsi::Value* args,
                    std::size_t count, const char* fn, std::index_sequence<I...>) {
  // A braced initialiser, not a bare call: C++ leaves the evaluation order of
  // function arguments unspecified, and these can throw. Inside braces the
  // order is left-to-right, so the error a caller sees names the first bad
  // argument rather than an arbitrary one.
  std::tuple<A...> converted{arg<A>(runtime, args, count, I, fn)...};
  if constexpr (std::is_void<R>::value) {
    function(std::get<I>(converted)...);
    return jsi::Value::undefined();
  } else {
    return jsi::Value(static_cast<double>(function(std::get<I>(converted)...)));
  }
}

template <typename R, typename... A>
jsi::Value callGl(R (*function)(A...), jsi::Runtime& runtime, const jsi::Value* args,
                  std::size_t count, const char* fn) {
  return invokeGl(function, runtime, args, count, fn, std::index_sequence_for<A...>{});
}

// ---------------------------------------------------------------------------
// GL_ADAPTED: the entry points whose C signature cannot be driven from JS
// values directly -- strings, out-parameters, buffer pointers.
// ---------------------------------------------------------------------------

/// What an adapter is allowed to reach beyond its arguments.
struct GlContext {
  std::shared_ptr<gfx::GlSurface> surface;
};

using Adapter = jsi::Value (*)(GlContext&, jsi::Runtime&, const jsi::Value*, std::size_t,
                               const char*);

jsi::Value adapt_shaderSource(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                              std::size_t count, const char* fn) {
  const GLuint shader = arg<GLuint>(runtime, args, count, 0, fn);
  const std::string source = stringArg(runtime, args, count, 1, fn);
  const char* text = source.c_str();
  const GLint length = static_cast<GLint>(source.size());
  glShaderSource(shader, 1, &text, &length);
  return jsi::Value::undefined();
}

jsi::Value adapt_getShaderiv(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                             std::size_t count, const char* fn) {
  GLint value = 0;
  glGetShaderiv(arg<GLuint>(runtime, args, count, 0, fn),
                arg<GLenum>(runtime, args, count, 1, fn), &value);
  return jsi::Value(static_cast<double>(value));
}

jsi::Value adapt_getProgramiv(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                              std::size_t count, const char* fn) {
  GLint value = 0;
  glGetProgramiv(arg<GLuint>(runtime, args, count, 0, fn),
                 arg<GLenum>(runtime, args, count, 1, fn), &value);
  return jsi::Value(static_cast<double>(value));
}

/// Both info logs are the same shape: ask for the length, then the text. The
/// length includes the NUL, and `glGet*InfoLog` reports how much it actually
/// wrote, which is what the string is built from -- trusting the queried length
/// would leave a trailing NUL inside a JS string.
template <typename Query, typename Fetch>
jsi::Value infoLog(jsi::Runtime& runtime, GLuint name, Query query, Fetch fetch) {
  GLint length = 0;
  query(name, GL_INFO_LOG_LENGTH, &length);
  if (length <= 1) return jsi::String::createFromUtf8(runtime, "");
  std::vector<char> text(static_cast<std::size_t>(length));
  GLsizei written = 0;
  fetch(name, length, &written, text.data());
  if (written < 0) written = 0;
  return jsi::String::createFromUtf8(runtime,
                                     std::string(text.data(), static_cast<std::size_t>(written)));
}

jsi::Value adapt_getShaderInfoLog(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                  std::size_t count, const char* fn) {
  return infoLog(runtime, arg<GLuint>(runtime, args, count, 0, fn), glGetShaderiv,
                 glGetShaderInfoLog);
}

jsi::Value adapt_getProgramInfoLog(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                   std::size_t count, const char* fn) {
  return infoLog(runtime, arg<GLuint>(runtime, args, count, 0, fn), glGetProgramiv,
                 glGetProgramInfoLog);
}

jsi::Value adapt_genBuffers(GlContext&, jsi::Runtime&, const jsi::Value*, std::size_t,
                            const char*) {
  GLuint buffer = 0;
  glGenBuffers(1, &buffer);
  return jsi::Value(static_cast<double>(buffer));
}

jsi::Value adapt_deleteBuffers(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                               std::size_t count, const char* fn) {
  const GLuint buffer = arg<GLuint>(runtime, args, count, 0, fn);
  glDeleteBuffers(1, &buffer);
  return jsi::Value::undefined();
}

jsi::Value adapt_genVertexArrays(GlContext&, jsi::Runtime&, const jsi::Value*, std::size_t,
                                 const char*) {
  GLuint array = 0;
  glGenVertexArrays(1, &array);
  return jsi::Value(static_cast<double>(array));
}

jsi::Value adapt_deleteVertexArrays(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                    std::size_t count, const char* fn) {
  const GLuint array = arg<GLuint>(runtime, args, count, 0, fn);
  glDeleteVertexArrays(1, &array);
  return jsi::Value::undefined();
}

jsi::Value adapt_bufferData(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                            std::size_t count, const char* fn) {
  const GLenum target = arg<GLenum>(runtime, args, count, 0, fn);
  if (count < 2) throwTypeError(runtime, where(fn, 1) + " is missing");

  // A bare number is GL's "allocate this many bytes and leave them undefined",
  // which is a legitimate second argument and not a mistyped array.
  if (args[1].isNumber()) {
    const GLsizeiptr size = static_cast<GLsizeiptr>(arg<GLint>(runtime, args, count, 1, fn));
    glBufferData(target, size, nullptr, arg<GLenum>(runtime, args, count, 2, fn));
    return jsi::Value::undefined();
  }

  const BufferView view = requireTypedArray(runtime, args, count, 1, fn);
  const GLenum usage = arg<GLenum>(runtime, args, count, 2, fn);
  glBufferData(target, static_cast<GLsizeiptr>(view.size), view.data, usage);
  return jsi::Value::undefined();
}

jsi::Value adapt_vertexAttribPointer(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                     std::size_t count, const char* fn) {
  const GLuint index = arg<GLuint>(runtime, args, count, 0, fn);
  const GLint size = arg<GLint>(runtime, args, count, 1, fn);
  const GLenum type = arg<GLenum>(runtime, args, count, 2, fn);
  const GLboolean normalized = boolArg(runtime, args, count, 3, fn) ? GL_TRUE : GL_FALSE;
  const GLsizei stride = arg<GLsizei>(runtime, args, count, 4, fn);
  // The last parameter is a pointer only by history: with a buffer bound it is
  // a byte offset into that buffer, which is the only form ES3 allows.
  const double offset = numberArg(runtime, args, count, 5, fn);
  const auto byteOffset = static_cast<std::uintptr_t>(toUint32(offset));
  glVertexAttribPointer(index, size, type, normalized, stride,
                        reinterpret_cast<const void*>(byteOffset));
  return jsi::Value::undefined();
}

jsi::Value adapt_getAttribLocation(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                   std::size_t count, const char* fn) {
  const GLuint program = arg<GLuint>(runtime, args, count, 0, fn);
  const std::string name = stringArg(runtime, args, count, 1, fn);
  return jsi::Value(static_cast<double>(glGetAttribLocation(program, name.c_str())));
}

jsi::Value adapt_bindAttribLocation(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                    std::size_t count, const char* fn) {
  const GLuint program = arg<GLuint>(runtime, args, count, 0, fn);
  const GLuint index = arg<GLuint>(runtime, args, count, 1, fn);
  const std::string name = stringArg(runtime, args, count, 2, fn);
  glBindAttribLocation(program, index, name.c_str());
  return jsi::Value::undefined();
}

jsi::Value adapt_getUniformLocation(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                    std::size_t count, const char* fn) {
  const GLuint program = arg<GLuint>(runtime, args, count, 0, fn);
  const std::string name = stringArg(runtime, args, count, 1, fn);
  return jsi::Value(static_cast<double>(glGetUniformLocation(program, name.c_str())));
}

jsi::Value adapt_uniformMatrix4fv(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                                  std::size_t count, const char* fn) {
  const GLint location = arg<GLint>(runtime, args, count, 0, fn);
  const GLboolean transpose = boolArg(runtime, args, count, 1, fn) ? GL_TRUE : GL_FALSE;
  const BufferView view = requireTypedArray(runtime, args, count, 2, fn);
  constexpr std::size_t kMatrixBytes = 16 * sizeof(GLfloat);
  if (view.size < kMatrixBytes || view.size % kMatrixBytes != 0) {
    throwTypeError(runtime,
                   where(fn, 2) + " must hold a whole number of 4x4 float matrices (got " +
                       std::to_string(view.size) + " bytes)");
  }
  // reinterpret_cast, not a copy: `requireTypedArray` already bounds-checked the
  // view against its ArrayBuffer, and a Float32Array's backing store is
  // float-aligned by construction.
  glUniformMatrix4fv(location, static_cast<GLsizei>(view.size / kMatrixBytes), transpose,
                     reinterpret_cast<const GLfloat*>(view.data));
  return jsi::Value::undefined();
}

/// The one entry point that writes *into* the JS heap rather than reading from
/// it. Same view machinery, same in-place access: the pixels land directly in
/// the caller's typed array.
///
/// It is here because it is what turns "a triangle is visible" from a
/// screenshot somebody has to look at into an assertion the headless suite can
/// make. It touches no texture and no framebuffer object, so it stays inside
/// this milestone's scope.
jsi::Value adapt_readPixels(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                            std::size_t count, const char* fn) {
  const GLint x = arg<GLint>(runtime, args, count, 0, fn);
  const GLint y = arg<GLint>(runtime, args, count, 1, fn);
  const GLsizei width = arg<GLsizei>(runtime, args, count, 2, fn);
  const GLsizei height = arg<GLsizei>(runtime, args, count, 3, fn);
  const GLenum format = arg<GLenum>(runtime, args, count, 4, fn);
  const GLenum type = arg<GLenum>(runtime, args, count, 5, fn);
  const BufferView view = requireTypedArray(runtime, args, count, 6, fn);

  // GL would happily write past the end of a destination that is too small, and
  // that end is inside the JS heap. Only RGBA/UNSIGNED_BYTE is reachable from
  // this table, so the required size is exact rather than a guess.
  if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
    throwTypeError(runtime, std::string("gl.") + fn +
                                ": only RGBA / UNSIGNED_BYTE is supported by this binding");
  }
  if (width < 0 || height < 0) {
    throwTypeError(runtime, std::string("gl.") + fn + ": width and height must not be negative");
  }
  const std::size_t needed =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
  if (view.size < needed) {
    throwTypeError(runtime, std::string("gl.") + fn + ": destination holds " +
                                std::to_string(view.size) + " bytes, needs " +
                                std::to_string(needed));
  }

  glReadPixels(x, y, width, height, format, type, view.data);
  return jsi::Value::undefined();
}

jsi::Value adapt_getString(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                           std::size_t count, const char* fn) {
  const GLubyte* value = glGetString(arg<GLenum>(runtime, args, count, 0, fn));
  if (value == nullptr) return jsi::Value::null();
  return jsi::String::createFromUtf8(runtime, reinterpret_cast<const char*>(value));
}

jsi::Value adapt_getIntegerv(GlContext&, jsi::Runtime& runtime, const jsi::Value* args,
                             std::size_t count, const char* fn) {
  GLint value = 0;
  glGetIntegerv(arg<GLenum>(runtime, args, count, 0, fn), &value);
  return jsi::Value(static_cast<double>(value));
}

jsi::Value adapt_surfaceWidth(GlContext& context, jsi::Runtime&, const jsi::Value*, std::size_t,
                              const char*) {
  return jsi::Value(context.surface ? static_cast<double>(context.surface->width()) : 0.0);
}

jsi::Value adapt_surfaceHeight(GlContext& context, jsi::Runtime&, const jsi::Value*, std::size_t,
                               const char*) {
  return jsi::Value(context.surface ? static_cast<double>(context.surface->height()) : 0.0);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

void installAdapter(jsi::Runtime& runtime, jsi::Object& gl, const char* name, unsigned argCount,
                    const std::shared_ptr<GlContext>& context, Adapter adapter) {
  gl.setProperty(
      runtime, name,
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, name), argCount,
          [context, adapter, name](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                                   std::size_t count) -> jsi::Value {
            return adapter(*context, rt, args, count, name);
          }));
}

}  // namespace

void installWebGLBindings(jsi::Runtime& runtime, std::shared_ptr<gfx::GlSurface> surface) {
  // The context -- and through it the surface -- is captured by every host
  // function below. That is deliberate: it is what makes the EGL teardown
  // happen on the JS thread, because the last reference dies when the runtime
  // destroys its host functions, which it does on the thread that owns it.
  auto context = std::make_shared<GlContext>();
  context->surface = std::move(surface);

  jsi::Object gl(runtime);

#define GL_FN(jsName, glFunction)                                                             \
  gl.setProperty(runtime, #jsName,                                                            \
                 jsi::Function::createFromHostFunction(                                       \
                     runtime, jsi::PropNameID::forAscii(runtime, #jsName), arityOf(glFunction), \
                     [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,          \
                        std::size_t count) -> jsi::Value {                                    \
                       return callGl(glFunction, rt, args, count, #jsName);                   \
                     }));

#define GL_ADAPTED(jsName, argCount) \
  installAdapter(runtime, gl, #jsName, argCount, context, &adapt_##jsName);

#define GL_CONST(name) \
  gl.setProperty(runtime, #name, jsi::Value(static_cast<double>(GL_##name)));

#include "gl.def"

  runtime.global().setProperty(runtime, "gl", gl);
}

}  // namespace screenkit
