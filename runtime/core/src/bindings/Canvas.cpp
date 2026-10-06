// Copyright (c) ScreenKit contributors. MIT.
//
// `__screenkit.canvas`, the handle API behind a `<canvas>` that is not the
// page's frame. `HTMLCanvasElement`'s semantics -- `getContext`, the size
// properties, `getBoundingClientRect` and the CSS subset that places it -- are
// JavaScript (runtime/js/dom-shim.js); this file only makes the GL context and
// the layer, and takes them away again.
//
// Shaped like bindings/Instance.cpp, deliberately: a handle per element so the
// element's collection releases the context, the same `LayerList` the instance
// binding writes into, and the same rect wire format. What differs is that a
// canvas produces on the very thread that composites it, so there is no fence
// and nothing to join.
#include "Canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <screenkit/Log.h>

#include "../compositor/Compositor.h"
#include "../gfx/GlSurface.h"
#include "../gfx/VendoredWebGL.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

constexpr const char* kTag = "screenkit.canvas";

void setFunction(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned params,
                 jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name),
                                                           params, std::move(body)));
}

double numberArg(jsi::Runtime&, const jsi::Value* args, size_t count, size_t index) {
  return count > index && args[index].isNumber() && std::isfinite(args[index].getNumber())
             ? args[index].getNumber()
             : 0.0;
}

/// A drawing-buffer side, in pixels. A canvas whose rect computes to zero -- a
/// `width: 0`, a transform that collapsed it -- still needs a framebuffer for
/// its context to be complete, so it gets the smallest there is and no visible
/// layer. The ceiling is the instance binding's, and for the same reason: a
/// layer larger than any display is a page's arithmetic gone wrong, not a
/// request the driver should be asked to honour.
int sizeArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, size_t index) {
  const double value = std::floor(numberArg(rt, args, count, index));
  if (!(value >= 1)) return 1;
  if (value <= 8192) return static_cast<int>(value);
  // A gap says so, once (runtime/js/README.md): a page that asks for a drawing
  // buffer wider than any display we target reads `canvas.width` back smaller
  // than it wrote, and silence would make that look like a bug in the page.
  static bool said = false;
  if (!said) {
    said = true;
    log(LogLevel::Warn, kTag,
        "a canvas asked for a drawing buffer of " + std::to_string(static_cast<long long>(value)) +
            " px on one side; it was capped at 8192, which is what `canvas.width` and "
            "`gl.drawingBufferWidth` read back from here on. This is said once.");
  }
  return 8192;
}

}  // namespace

/// JS-thread state: every canvas layer this page made.
class CanvasBinding : public std::enable_shared_from_this<CanvasBinding> {
 public:
  struct Entry {
    std::shared_ptr<gfx::LayerSource> source;
  };

  std::shared_ptr<JsExecutor> executor;
  /// The page's one layer list, shared with the instance binding: a canvas
  /// layer and an `<iframe>` layer sort together (Compositor.h).
  std::shared_ptr<gfx::LayerList> layers;
  std::unordered_map<gfx::GlContextId, Entry> entries;
  bool stopped = false;
  /// A refusal is said once per runtime, not once per `getContext`: an engine
  /// that probes in a loop would otherwise fill the log.
  bool announcedRefusal = false;

  static std::uint64_t layerId(gfx::GlContextId context) {
    return gfx::kCanvasLayerBase + context;
  }

  void refuse(const std::string& why) {
    if (announcedRefusal) return;
    announcedRefusal = true;
    log(LogLevel::Warn, kTag,
        "a second <canvas> could not be given a GL context of its own, so getContext answered "
        "null: " + why);
  }

  void release(jsi::Runtime& rt, gfx::GlContextId id) {
    auto it = entries.find(id);
    if (it == entries.end()) return;
    Entry entry = std::move(it->second);
    entries.erase(it);
    // The layer first: nothing must still be sampling the texture when the
    // surface's destructor deletes it.
    if (layers) layers->remove(layerId(id));
    if (entry.source) gfx::discardFence(entry.source->takeFence());
    gfx::releaseContext(rt, id);
    // The released surface unbound whatever was current on its way out.
    gfx::makeContextCurrent(gfx::primaryContext(rt));
  }

  void shutdown() {
    if (stopped) return;
    stopped = true;
    for (auto& entry : entries) {
      if (layers) layers->remove(layerId(entry.first));
      if (entry.second.source) gfx::discardFence(entry.second.source->takeFence());
    }
    entries.clear();
    layers.reset();
    // The contexts themselves go with every other context of this runtime, on
    // this thread, in `gfx::releaseVendoredWebGL` -- which teardown calls right
    // after this.
  }
};

namespace {

/// Bound to the canvas element. When the collector takes it -- the page dropped
/// the canvas -- the context and the layer go with it. The destructor may run on
/// the collector's thread, so the release itself is posted to the JS thread.
class CanvasHandle final : public jsi::NativeState {
 public:
  CanvasHandle(std::weak_ptr<CanvasBinding> binding, std::shared_ptr<JsExecutor> executor,
               gfx::GlContextId id)
      : binding_(std::move(binding)), executor_(std::move(executor)), id_(id) {}

  ~CanvasHandle() override {
    if (!executor_) return;
    std::weak_ptr<CanvasBinding> binding = binding_;
    const gfx::GlContextId id = id_;
    executor_->invokeAsync([binding, id](jsi::Runtime& rt) {
      if (auto live = binding.lock()) live->release(rt, id);
    });
  }

 private:
  std::weak_ptr<CanvasBinding> binding_;
  std::shared_ptr<JsExecutor> executor_;
  gfx::GlContextId id_;
};

gfx::GlContextId idArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, const char* fn) {
  // The upper bound is not pedantry: converting a double that does not fit the
  // destination is undefined behaviour, so `setPlane(1e300, ...)` from page
  // script must be refused here rather than wrapped into some other canvas's id.
  if (count < 1 || !args[0].isNumber() || !std::isfinite(args[0].getNumber()) ||
      args[0].getNumber() < 0 ||
      args[0].getNumber() > static_cast<double>(std::numeric_limits<gfx::GlContextId>::max())) {
    throw jsi::JSError(rt, std::string("__screenkit.canvas.") + fn + " requires a context id");
  }
  return static_cast<gfx::GlContextId>(args[0].getNumber());
}

}  // namespace

std::shared_ptr<CanvasBinding> installCanvas(jsi::Runtime& runtime,
                                             std::shared_ptr<JsExecutor> executor) {
  auto binding = std::make_shared<CanvasBinding>();
  binding->executor = std::move(executor);

  std::weak_ptr<CanvasBinding> weak = binding;
  jsi::Object api(runtime);

  setFunction(
      runtime, api, "create", 3,
      [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        auto b = weak.lock();
        if (!b || b->stopped) return jsi::Value::null();
        if (count < 1 || !args[0].isObject() || args[0].getObject(rt).isFunction(rt)) {
          throw jsi::JSError(rt, "__screenkit.canvas.create requires the canvas element");
        }
        jsi::Object element = args[0].getObject(rt);
        if (element.hasNativeState(rt)) {
          throw jsi::JSError(rt, "__screenkit.canvas.create: that canvas already has a context");
        }
        const int width = sizeArg(rt, args, count, 1);
        const int height = sizeArg(rt, args, count, 2);

        std::shared_ptr<gfx::GlSurface> page = gfx::surfaceFor(rt);
        if (!page) {
          b->refuse("this runtime has no drawable to composite it over (a windowed host has one)");
          return jsi::Value::null();
        }
        // Sharing is spelled "share with the context current on this thread" on
        // SDL, and the compositor is built in the page's own context, so the
        // page's context has to be the one bound here.
        gfx::makeContextCurrent(gfx::primaryContext(rt));

        std::string error;
        if (!gfx::enableCompositing(rt, *page, error)) {
          // The nesting refusal reads as being about <iframe>s, because that is
          // what the one-level rule was written for. Say what it means here: a
          // page running *inside* an <iframe> draws into a layer already, and a
          // layer cannot hold layers of its own.
          b->refuse(page->isLayer()
                        ? std::string("this page is itself an <iframe> instance, and an instance's "
                                      "layer cannot composite canvases of its own: one level of "
                                      "nesting (Architecture.md 3.1)")
                        : "this platform has no compositor to put a second canvas on: " + error);
          return jsi::Value::null();
        }

        std::shared_ptr<gfx::GlSurface> layer =
            gfx::GlSurface::createShared(*page, width, height, error);
        // Everything below may have bound a context of its own without telling
        // `gfx`: `createShared` leaves the new one current on SDL, and
        // `adopt` binds it directly on both backends. Destroying that surface
        // then leaves *nothing* current, so the restore has to be a real bind
        // and not the "already current" short-circuit -- hence the forget.
        if (!layer) {
          // A driver that will not give another context lands here, which is
          // the web's "getContext may return null" and not an exception.
          b->refuse(error);
          gfx::forgetCurrentContext();
          gfx::makeContextCurrent(gfx::primaryContext(rt));
          return jsi::Value::null();
        }
        if (!layer->adopt(error)) {
          b->refuse(error);
          layer.reset();
          gfx::forgetCurrentContext();
          gfx::makeContextCurrent(gfx::primaryContext(rt));
          return jsi::Value::null();
        }
        auto source = std::make_shared<gfx::LayerSource>();
        // Same thread: this canvas is drawn and composited by one thread in one
        // order, so the publish is a flush and a null fence.
        layer->setLayerSource(source, true);

        const gfx::GlContextId id = gfx::installVendoredWebGL(rt, layer, false);
        if (id == 0) {
          b->refuse("the WebGL context object could not be installed");
          layer.reset();
          gfx::forgetCurrentContext();
          gfx::makeContextCurrent(gfx::primaryContext(rt));
          return jsi::Value::null();
        }
        CanvasBinding::Entry entry;
        entry.source = std::move(source);
        b->entries.emplace(id, std::move(entry));
        if (!b->layers) b->layers = gfx::layersFor(rt);
        element.setNativeState(rt, std::make_shared<CanvasHandle>(weak, b->executor, id));

        jsi::Object out(rt);
        out.setProperty(rt, "id", static_cast<double>(id));
        // Taken out of `__SKGLContexts` rather than read from it: parked there
        // it would be a root, and a canvas the page drops has to be collectable
        // with its context and its layer (VendoredWebGL.h).
        out.setProperty(rt, "gl", gfx::takeContextObject(rt, id));
        // Leave the page's own context bound: between two JS calls, "current"
        // means the frame.
        gfx::makeContextCurrent(gfx::primaryContext(rt));
        return out;
      });

  setFunction(runtime, api, "setSize", 3,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                     size_t count) -> jsi::Value {
                const gfx::GlContextId id = idArg(rt, args, count, "setSize");
                auto b = weak.lock();
                if (!b || b->stopped || b->entries.find(id) == b->entries.end()) {
                  return jsi::Value::undefined();
                }
                std::shared_ptr<gfx::GlSurface> layer = gfx::surfaceFor(rt, id);
                if (!layer || !gfx::makeContextCurrent(id)) return jsi::Value::undefined();
                std::string error;
                if (!layer->resizeLayer(sizeArg(rt, args, count, 1), sizeArg(rt, args, count, 2),
                                        error)) {
                  log(LogLevel::Error, kTag, "resizing a canvas's drawing buffer: " + error);
                }
                gfx::makeContextCurrent(gfx::primaryContext(rt));
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setPlane", 8,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                     size_t count) -> jsi::Value {
                const gfx::GlContextId id = idArg(rt, args, count, "setPlane");
                auto b = weak.lock();
                if (!b || b->stopped || !b->layers) return jsi::Value::undefined();
                auto it = b->entries.find(id);
                if (it == b->entries.end()) return jsi::Value::undefined();
                gfx::Layer layer;
                layer.source = it->second.source;
                layer.rect.x = numberArg(rt, args, count, 1);
                layer.rect.y = numberArg(rt, args, count, 2);
                layer.rect.width = numberArg(rt, args, count, 3);
                layer.rect.height = numberArg(rt, args, count, 4);
                // A rect with no area composites nothing, exactly as an
                // `<iframe>`'s does.
                layer.visible = count > 5 && args[5].isBool() && args[5].getBool() &&
                                layer.rect.width > 0 && layer.rect.height > 0;
                // Clamped, not cast: a `z-index` outside int range would be
                // undefined behaviour on the way in, and the compositor only
                // ever compares orders.
                layer.order =
                    count > 6 && args[6].isNumber() && std::isfinite(args[6].getNumber())
                        ? static_cast<int>(std::max(-2147483648.0,
                                                    std::min(2147483647.0, args[6].getNumber())))
                        : 0;
                layer.opacity = count > 7 && args[7].isNumber() && std::isfinite(args[7].getNumber())
                                    ? std::max(0.0, std::min(1.0, args[7].getNumber()))
                                    : 1.0;
                b->layers->set(CanvasBinding::layerId(id), layer);
                return jsi::Value::undefined();
              });

  jsi::Value io = runtime.global().getProperty(runtime, "__screenkit");
  if (!io.isObject()) {
    throw jsi::JSError(runtime, "installCanvas: __screenkit is missing -- installHostIO runs first");
  }
  io.getObject(runtime).setProperty(runtime, "canvas", std::move(api));
  return binding;
}

void shutdownCanvas(CanvasBinding& binding) { binding.shutdown(); }

}  // namespace screenkit
