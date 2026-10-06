// Copyright (c) ScreenKit contributors. MIT.
#include "Text.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "text/FontLibrary.h"
#include "text/SystemFonts.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

constexpr int kFlagBold = 1;
constexpr int kFlagItalic = 2;
constexpr int kFlagNoKerning = 4;
constexpr int kFlagJoinRound = 8;
constexpr int kFlagJoinBevel = 16;

class MaskBuffer final : public jsi::MutableBuffer {
 public:
  explicit MaskBuffer(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  std::size_t size() const override { return bytes_.size(); }
  std::uint8_t* data() override { return bytes_.data(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

[[noreturn]] void fail(jsi::Runtime& rt, const char* fn, const std::string& why) {
  throw jsi::JSError(rt, std::string("__screenkit.text.") + fn + ": " + why);
}

double number(jsi::Runtime& rt, const jsi::Value* args, std::size_t count, std::size_t i, const char* fn) {
  if (i >= count || !args[i].isNumber()) fail(rt, fn, "argument " + std::to_string(i + 1) + " must be a number");
  return args[i].asNumber();
}

std::string string(jsi::Runtime& rt, const jsi::Value* args, std::size_t count, std::size_t i, const char* fn) {
  if (i >= count || !args[i].isString()) fail(rt, fn, "argument " + std::to_string(i + 1) + " must be a string");
  return args[i].getString(rt).utf8(rt);
}

/// The bytes of an ArrayBuffer or a view on one, copied: the library keeps them
/// for as long as the face exists, well past this call.
std::vector<std::uint8_t> bytesOf(jsi::Runtime& rt, const jsi::Value& value, const char* fn) {
  if (!value.isObject()) fail(rt, fn, "requires an ArrayBuffer or a view");
  jsi::Object source = value.getObject(rt);
  if (source.isArrayBuffer(rt)) {
    jsi::ArrayBuffer buffer = source.getArrayBuffer(rt);
    return std::vector<std::uint8_t>(buffer.data(rt), buffer.data(rt) + buffer.size(rt));
  }
  jsi::Value inner = source.getProperty(rt, "buffer");
  if (!inner.isObject() || !inner.getObject(rt).isArrayBuffer(rt)) fail(rt, fn, "requires an ArrayBuffer or a view");
  jsi::ArrayBuffer buffer = inner.getObject(rt).getArrayBuffer(rt);
  const jsi::Value offsetValue = source.getProperty(rt, "byteOffset");
  const jsi::Value lengthValue = source.getProperty(rt, "byteLength");
  const double offset = offsetValue.isNumber() ? offsetValue.asNumber() : -1;
  const double length = lengthValue.isNumber() ? lengthValue.asNumber() : -1;
  if (!(offset >= 0) || !(length >= 0) || offset + length > static_cast<double>(buffer.size(rt))) {
    fail(rt, fn, "view out of range");
  }
  const std::uint8_t* start = buffer.data(rt) + static_cast<std::size_t>(offset);
  return std::vector<std::uint8_t>(start, start + static_cast<std::size_t>(length));
}

text::FontStyle styleOf(jsi::Runtime& rt, const jsi::Value* args, std::size_t count, const char* fn) {
  text::FontStyle style;
  style.size = static_cast<float>(number(rt, args, count, 1, fn));
  const int flags = static_cast<int>(number(rt, args, count, 2, fn));
  style.bold = (flags & kFlagBold) != 0;
  style.italic = (flags & kFlagItalic) != 0;
  style.kerning = (flags & kFlagNoKerning) == 0;
  style.join = (flags & kFlagJoinRound) != 0   ? text::FontStyle::Join::Round
               : (flags & kFlagJoinBevel) != 0 ? text::FontStyle::Join::Bevel
                                               : text::FontStyle::Join::Miter;
  return style;
}

jsi::Object faceObject(jsi::Runtime& rt, int id, const text::FaceInfo& info) {
  jsi::Object out(rt);
  out.setProperty(rt, "face", id);
  out.setProperty(rt, "family", jsi::String::createFromUtf8(rt, info.family));
  out.setProperty(rt, "style", jsi::String::createFromUtf8(rt, info.style));
  out.setProperty(rt, "weight", info.weight);
  out.setProperty(rt, "italic", info.italic);
  return out;
}

void method(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned arity,
            jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name), arity,
                                                           std::move(body)));
}

}  // namespace

std::shared_ptr<text::FontLibrary> installText(jsi::Runtime& runtime) {
  jsi::Value host = runtime.global().getProperty(runtime, "__screenkit");
  if (!host.isObject()) throw jsi::JSError(runtime, "installText: __screenkit is missing -- installHostIO runs first");

  auto library = std::make_shared<text::FontLibrary>();
  jsi::Object api(runtime);

  method(runtime, api, "addFont", 1,
         [library](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           if (count < 1) fail(rt, "addFont", "requires font data");
           std::string error;
           const int id = library->addFace(bytesOf(rt, args[0], "addFont"), error);
           if (id < 0) fail(rt, "addFont", error);
           return faceObject(rt, id, *library->face(id));
         });

  method(runtime, api, "systemFaces", 1,
         [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const auto faces = text::systemFaces(string(rt, args, count, 0, "systemFaces"));
           jsi::Array out(rt, faces.size());
           for (std::size_t i = 0; i < faces.size(); ++i) {
             jsi::Object face(rt);
             face.setProperty(rt, "name", jsi::String::createFromUtf8(rt, faces[i].name));
             face.setProperty(rt, "weight", faces[i].weight);
             face.setProperty(rt, "italic", faces[i].italic);
             out.setValueAtIndex(rt, i, std::move(face));
           }
           return out;
         });

  method(runtime, api, "addSystemFont", 1,
         [library](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           auto bytes = text::systemFontData(string(rt, args, count, 0, "addSystemFont"));
           if (bytes.empty()) return jsi::Value::null();
           std::string error;
           const int id = library->addFace(std::move(bytes), error);
           if (id < 0) return jsi::Value::null();
           return faceObject(rt, id, *library->face(id));
         });

  method(runtime, api, "genericFamily", 1,
         [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           return jsi::String::createFromUtf8(
               rt, text::systemGenericFamily(string(rt, args, count, 0, "genericFamily")));
         });

  method(runtime, api, "metrics", 3,
         [library](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const int face = static_cast<int>(number(rt, args, count, 0, "metrics"));
           text::FontMetrics metrics;
           std::string error;
           if (!library->metrics(face, styleOf(rt, args, count, "metrics"), metrics, error)) {
             fail(rt, "metrics", error);
           }
           jsi::Object out(rt);
           out.setProperty(rt, "ascent", metrics.ascent);
           out.setProperty(rt, "descent", metrics.descent);
           out.setProperty(rt, "lineSkip", metrics.lineSkip);
           return out;
         });

  method(runtime, api, "measure", 4,
         [library](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const int face = static_cast<int>(number(rt, args, count, 0, "measure"));
           const text::FontStyle style = styleOf(rt, args, count, "measure");
           text::TextExtent extent;
           std::string error;
           if (!library->measure(face, style, string(rt, args, count, 3, "measure"), extent, error)) {
             fail(rt, "measure", error);
           }
           jsi::Object out(rt);
           out.setProperty(rt, "width", extent.width);
           out.setProperty(rt, "left", extent.left);
           out.setProperty(rt, "right", extent.right);
           out.setProperty(rt, "ascent", extent.ascent);
           out.setProperty(rt, "descent", extent.descent);
           return out;
         });

  method(runtime, api, "render", 5,
         [library](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) -> jsi::Value {
           const int face = static_cast<int>(number(rt, args, count, 0, "render"));
           text::FontStyle style = styleOf(rt, args, count, "render");
           const double outline = number(rt, args, count, 3, "render");
           style.outline = std::isfinite(outline) && outline > 0 ? static_cast<int>(std::lround(outline)) : 0;
           if (count > 5 && args[5].isNumber()) style.miterLimit = static_cast<float>(args[5].asNumber());
           text::TextMask mask;
           std::string error;
           if (!library->render(face, style, string(rt, args, count, 4, "render"), mask, error)) {
             fail(rt, "render", error);
           }
           if (mask.width == 0 || mask.height == 0) return jsi::Value::null();
           jsi::Object out(rt);
           out.setProperty(rt, "width", mask.width);
           out.setProperty(rt, "height", mask.height);
           out.setProperty(rt, "originX", mask.originX);
           out.setProperty(rt, "baseline", mask.baseline);
           out.setProperty(rt, "data",
                           jsi::ArrayBuffer(rt, std::make_shared<MaskBuffer>(std::move(mask.alpha))));
           return out;
         });

  host.getObject(runtime).setProperty(runtime, "text", std::move(api));
  return library;
}

void shutdownText(text::FontLibrary& library) { library.clear(); }

}  // namespace screenkit
