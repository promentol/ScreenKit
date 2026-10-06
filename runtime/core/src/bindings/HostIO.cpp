// Copyright (c) ScreenKit contributors. MIT.
#include "HostIO.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The implementation is already compiled into the vendored GL archive
// (SKGLImageUtils.cpp defines STB_IMAGE_IMPLEMENTATION). Including the header
// alone declares the functions; defining it again here would duplicate them.
#include "stb_image.h"

#include "ImageDecode.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

struct AssetRoot {
  std::string path;  // canonical, with a trailing '/'
  bool set = false;
};

/// realpath() the root and the candidate, then require the candidate to sit
/// under the root. Canonicalising both sides is what defeats `..`, symlinks and
/// double slashes; a string-prefix check on the raw input would not.
bool confined(const AssetRoot& root, const std::string& candidate, std::string& resolved,
              std::string& why) {
  if (!root.set) {
    why = "no asset root set -- the host must call __screenkit.setAssetRoot first";
    return false;
  }
  std::string full = candidate;
  if (full.empty()) {
    why = "empty path";
    return false;
  }
  if (full[0] != '/') full = root.path + full;

  char buf[PATH_MAX];
  if (::realpath(full.c_str(), buf) == nullptr) {
    why = "no such asset: " + candidate;
    return false;
  }
  resolved = buf;
  const std::string& r = root.path;
  // `r` ends in '/', so a sibling directory whose name merely starts with the
  // root's name ("assets-evil" vs "assets") does not pass.
  if (resolved.compare(0, r.size(), r) != 0 && resolved + "/" != r) {
    why = "path escapes the asset root: " + candidate;
    return false;
  }
  return true;
}

/// One root **per runtime**, not per process: an `<iframe>` instance is a second
/// app with a package of its own (Architecture.md 5), and a shared root would
/// let either of them read the other's assets -- or, worse, let whichever
/// started first decide where both of them read from.
///
/// Keyed by the runtime pointer, as the vendored GL registry keys its contexts.
/// Reached only from the owning JS thread once the entry exists; the lock is for
/// the install and the teardown, which run on different threads' behalf.
std::mutex& rootsMutex() {
  static std::mutex m;
  return m;
}

std::unordered_map<jsi::Runtime*, std::shared_ptr<AssetRoot>>& roots() {
  static auto* map = new std::unordered_map<jsi::Runtime*, std::shared_ptr<AssetRoot>>();
  return *map;
}

/// The root for `runtime`, creating it when `create` is set. Held by shared_ptr
/// so the host functions can capture it without a dangling reference at
/// teardown.
std::shared_ptr<AssetRoot> rootFor(jsi::Runtime& runtime, bool create) {
  std::lock_guard<std::mutex> lock(rootsMutex());
  if (create) {
    // A fresh root, even for a pointer an earlier runtime once had: an instance
    // that reuses the address must not inherit the dead one's confinement.
    auto root = std::make_shared<AssetRoot>();
    roots()[&runtime] = root;
    return root;
  }
  const auto it = roots().find(&runtime);
  return it == roots().end() ? nullptr : it->second;
}

std::string argString(jsi::Runtime& rt, const jsi::Value* args, size_t count, const char* fn) {
  if (count < 1 || !args[0].isString()) {
    throw jsi::JSError(rt, std::string("__screenkit.") + fn + " requires a string path");
  }
  return args[0].getString(rt).utf8(rt);
}

/// An ArrayBuffer JS owns, holding a copy of the file. Copying is deliberate:
/// the alternative is mmap-backed storage whose lifetime JS cannot see.
class OwnedBuffer : public jsi::MutableBuffer {
 public:
  explicit OwnedBuffer(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t* data() override { return bytes_.data(); }

 private:
  std::vector<uint8_t> bytes_;
};

bool isCount(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

}  // namespace

bool resolveAssetPath(jsi::Runtime& runtime, const std::string& candidate, std::string& resolved,
                      std::string& why) {
  const std::shared_ptr<AssetRoot> root = rootFor(runtime, /*create=*/false);
  if (!root) {
    why = "no asset root set -- the host must call __screenkit.setAssetRoot first";
    return false;
  }
  return confined(*root, candidate, resolved, why);
}

void shutdownHostIO(jsi::Runtime& runtime) {
  std::lock_guard<std::mutex> lock(rootsMutex());
  roots().erase(&runtime);
}

void installHostIO(jsi::Runtime& runtime, std::shared_ptr<FailureReport> failures) {
  auto root = rootFor(runtime, /*create=*/true);
  jsi::Object io(runtime);

  // A JS-side fatal. A rejected entry module has no exception to unwind into
  // the host -- it settles in a microtask long after evaluateBundle returned --
  // so without this the host sees an app that simply went idle, and exits 0.
  // Records the message (String()-ed) and returns; the host exits at its next
  // loop iteration. Deliberately not write-once like the asset root: a report
  // can only make the process exit sooner, never widen what the app can do.
  io.setProperty(runtime, "reportFailure",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "reportFailure"), 1,
          [failures](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                     size_t count) -> jsi::Value {
            if (count < 1) {
              throw jsi::JSError(rt, "__screenkit.reportFailure requires a message");
            }
            failures->report(args[0].toString(rt).utf8(rt));
            return jsi::Value::undefined();
          }));

  io.setProperty(runtime, "setAssetRoot",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "setAssetRoot"), 1,
          [root](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                 size_t count) -> jsi::Value {
            const std::string requested = argString(rt, args, count, "setAssetRoot");
            // Write-once. The host sets it before any app code runs; if app code
            // could call this again it could point readFile anywhere on disk.
            if (root->set) {
              throw jsi::JSError(rt, "__screenkit.setAssetRoot may only be called once");
            }
            char buf[PATH_MAX];
            if (::realpath(requested.c_str(), buf) == nullptr) {
              throw jsi::JSError(rt, "__screenkit.setAssetRoot: no such directory: " + requested);
            }
            root->path = std::string(buf);
            if (root->path.empty() || root->path.back() != '/') root->path += '/';
            root->set = true;
            return jsi::String::createFromUtf8(rt, root->path);
          }));

  io.setProperty(runtime, "readFile",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "readFile"), 1,
          [root](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                 size_t count) -> jsi::Value {
            const std::string requested = argString(rt, args, count, "readFile");
            std::string resolved;
            std::string why;
            if (!confined(*root, requested, resolved, why)) throw jsi::JSError(rt, why);

            std::ifstream in(resolved, std::ios::binary | std::ios::ate);
            if (!in) throw jsi::JSError(rt, "could not open asset: " + requested);
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size < 0 ? 0 : size));
            if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
              throw jsi::JSError(rt, "could not read asset: " + requested);
            }
            return jsi::ArrayBuffer(rt, std::make_shared<OwnedBuffer>(std::move(bytes)));
          }));

  io.setProperty(runtime, "imageInfo",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "imageInfo"), 1,
          [root](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                 size_t count) -> jsi::Value {
            const std::string requested = argString(rt, args, count, "imageInfo");
            std::string resolved;
            std::string why;
            if (!confined(*root, requested, resolved, why)) throw jsi::JSError(rt, why);

            // Header only. The pixels are decoded later, by the vendored
            // texImage2D, from the same path -- so they are decoded exactly once.
            int w = 0;
            int h = 0;
            int comp = 0;
            if (!stbi_info(resolved.c_str(), &w, &h, &comp)) {
              const char* reason = stbi_failure_reason();
              throw jsi::JSError(rt, "not a decodable image: " + requested +
                                         (reason ? std::string(" (") + reason + ")" : ""));
            }
            jsi::Object info(rt);
            info.setProperty(rt, "width", w);
            info.setProperty(rt, "height", h);
            info.setProperty(rt, "channels", comp);
            info.setProperty(rt, "path", jsi::String::createFromUtf8(rt, resolved));
            return info;
          }));

  // Bytes that are already in memory -- a downloaded image -- decoded to RGBA
  // with the same stb_image the file path uses. Not confined, because it reads
  // nothing: the bytes are the caller's. The result is exactly the
  // `{width, height, data}` shape texImage2D's image-source path uploads.
  io.setProperty(runtime, "decodeImage",
      jsi::Function::createFromHostFunction(
          runtime, jsi::PropNameID::forAscii(runtime, "decodeImage"), 1,
          [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
             size_t count) -> jsi::Value {
            if (count < 1 || !args[0].isObject()) {
              throw jsi::JSError(rt, "__screenkit.decodeImage requires an ArrayBuffer or a view");
            }
            jsi::Object source = args[0].getObject(rt);
            const uint8_t* bytes = nullptr;
            size_t length = 0;
            if (source.isArrayBuffer(rt)) {
              jsi::ArrayBuffer buffer = source.getArrayBuffer(rt);
              bytes = buffer.data(rt);
              length = buffer.size(rt);
            } else {
              jsi::Value inner = source.getProperty(rt, "buffer");
              if (!inner.isObject() || !inner.getObject(rt).isArrayBuffer(rt)) {
                throw jsi::JSError(rt, "__screenkit.decodeImage requires an ArrayBuffer or a view");
              }
              jsi::ArrayBuffer buffer = inner.getObject(rt).getArrayBuffer(rt);
              const double offset = source.getProperty(rt, "byteOffset").asNumber();
              const double size = source.getProperty(rt, "byteLength").asNumber();
              if (!isCount(offset) || !isCount(size) || offset + size > static_cast<double>(buffer.size(rt))) {
                throw jsi::JSError(rt, "__screenkit.decodeImage: view out of range");
              }
              bytes = buffer.data(rt) + static_cast<size_t>(offset);
              length = static_cast<size_t>(size);
            }
            DecodedImage image;
            std::string error;
            if (!decodeImage(bytes, length, image, error)) throw jsi::JSError(rt, error);
            return imageObject(rt, image);
          }));

  runtime.global().setProperty(runtime, "__screenkit", io);
}

}  // namespace screenkit
