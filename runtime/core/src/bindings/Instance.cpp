// Copyright (c) ScreenKit contributors. MIT.
//
// `__screenkit.instances`, the handle API over screenkit::Instance.
// `HTMLIFrameElement`'s semantics -- the element, `contentWindow`, `onload`,
// `sandbox` as a token list, `postMessage` and `MessageEvent` -- are JavaScript
// (runtime/js/dom-shim.js); this file only moves calls one way and events the
// other, across the thread boundary.
//
// Shaped like bindings/Media.cpp, deliberately: one `ChannelTarget`, a
// `WeakObject` per handle so the element's collection releases the instance, a
// `holdWork` that keeps the loop non-idle, native state as the GC hook, and
// serial-stamped events so a reload's answers do not arrive for the load before
// it.
#include "Instance.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "../compositor/Compositor.h"
#include "../gfx/GlSurface.h"
#include "../gfx/VendoredWebGL.h"
#include "../instance/InstanceImpl.h"
#include "../instance/StructuredClone.h"
#include "../loop/EventLoop.h"
#include "EventChannel.h"
#include "HostIO.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

bool isCount(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

void setFunction(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned params,
                 jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name),
                                                           params, std::move(body)));
}

jsi::Value str(jsi::Runtime& rt, const std::string& text) {
  return jsi::String::createFromUtf8(rt, text);
}

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

/// `sandbox="allow-media allow-network"`, whitespace-separated as HTML splits it.
///
/// An element *with* the attribute starts from nothing: an empty attribute is
/// the most restrictive sandbox there is, which is what the web means by it too.
/// An unknown token is ignored with a one-time warning, as a browser ignores
/// one; `allow-storage` is accepted and reserved, because this runtime has no
/// storage API for it to gate.
/// A sandbox diagnostic is said once per process, as the header promises -- not
/// once per `src` write, which is what a launcher does on every navigation.
/// (`announce` in the DOM shim is the same idea on the JS side.)
void announceSandbox(const std::string& tag, const std::string& message) {
  static std::mutex mutex;
  static std::set<std::string> said;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!said.insert(message).second) return;
  }
  log(LogLevel::Warn, tag, message);
}

SandboxPolicy parseSandbox(const std::string& tokens, const std::string& tag) {
  SandboxPolicy policy;
  policy.sandboxed = true;
  std::istringstream stream(tokens);
  std::string token;
  while (stream >> token) {
    const std::string name = lower(token);
    if (name == "allow-network") {
      policy.allowNetwork = true;
    } else if (name == "allow-media") {
      policy.allowMedia = true;
    } else if (name == "allow-storage") {
      policy.allowStorage = true;
      announceSandbox(tag,
                      "sandbox token \"allow-storage\" is accepted and reserved: this runtime has no storage "
                      "API for it to gate (Architecture.md 3.2)");
    } else if (name == "allow-background-audio") {
      policy.allowBackgroundAudio = true;
    } else if (name == "allow-background-timers") {
      policy.allowBackgroundTimers = true;
    } else {
      announceSandbox(tag,
                      "unknown sandbox token \"" + token +
                          "\", ignored: allow-network, allow-media, allow-storage, allow-background-audio and "
                          "allow-background-timers are the tokens this runtime knows");
    }
  }
  return policy;
}

jsi::Object sandboxObject(jsi::Runtime& rt, const SandboxPolicy& policy) {
  jsi::Object out(rt);
  out.setProperty(rt, "sandboxed", policy.sandboxed);
  out.setProperty(rt, "allowNetwork", policy.network());
  out.setProperty(rt, "allowMedia", policy.media());
  out.setProperty(rt, "allowStorage", !policy.sandboxed || policy.allowStorage);
  out.setProperty(rt, "allowBackgroundAudio", policy.backgroundAudio());
  out.setProperty(rt, "allowBackgroundTimers", policy.backgroundTimers());
  return out;
}

}  // namespace

/// JS-thread state: every instance, the element its events go to, and the layer
/// the host's present composites it at.
class InstanceBinding : public ChannelTarget, public std::enable_shared_from_this<InstanceBinding> {
 public:
  struct Entry {
    std::shared_ptr<Instance> instance;
    std::shared_ptr<gfx::LayerSource> layer;
    std::unique_ptr<jsi::WeakObject> target;
    /// One channel per instance, so everything about it -- `load`, `error`,
    /// `message`, `focus`, `blur` -- arrives in the order it was posted, with a
    /// microtask checkpoint between (EventChannel).
    std::shared_ptr<Poster> poster;
    std::uint32_t serial = 0;
  };

  std::shared_ptr<JsExecutor> executor;
  std::weak_ptr<EventLoop> loop;
  std::shared_ptr<InstanceRegistry> registry;
  std::shared_ptr<RuntimeControl> control;
  /// The page's one layer list (`gfx::layersFor`), shared with the canvas
  /// binding: an instance's layer and a `<canvas>` layer sort together by
  /// `z-index` then insertion, so a page may put a HUD canvas over the game it
  /// embeds (Compositor.h).
  std::shared_ptr<gfx::LayerList> layers;
  SandboxPolicy sandbox;
  /// Inherited by every instance this page makes, so a test-suite launcher's
  /// child trusts the same generated CA. Not reachable from JS on either side.
  std::vector<std::vector<std::uint8_t>> testTlsAnchors;
  /// The Hermes heap ceiling this page runs under; every instance it makes gets
  /// the same one.
  std::size_t maxHeapBytes = 0;
  std::string tag;
  /// `__screenkit.instances` itself, where the shim sets `onevent`.
  std::unique_ptr<jsi::Object> api;
  std::unordered_map<std::uint64_t, Entry> entries;
  std::uint64_t nextId = 1;
  std::uint32_t nextSerial = 1;
  /// The focused instance, or 0 for this page itself.
  std::uint64_t focused = 0;
  /// Which context focus is *meant* for, written from either thread: this page
  /// (0) or an instance.
  ///
  /// It exists for one race, and that race is the whole point of `focus()`
  /// being atomic. Freezing this page happens in a task on its own thread --
  /// the page has to hear `pause` while it can still run -- so a
  /// `window.parent.focus()` from the instance can land *before* that task. It
  /// thaws a page that is not frozen yet, and the freeze would then close the
  /// gate on a page with nothing left to open it. The freeze task reads this
  /// and skips itself when focus has already come back.
  std::atomic<std::uint64_t> focusRequest{0};
  bool compositing = false;
  bool stopped = false;

  bool channelStopped() const override { return stopped; }
  const std::string& channelTag() const override { return tag; }

  Entry* find(std::uint64_t id) {
    auto it = entries.find(id);
    return it == entries.end() ? nullptr : &it->second;
  }

  void deliver(jsi::Runtime& rt, std::uint64_t id, const char* type, const jsi::Value& payload,
               bool) override {
    if (stopped || !api) return;
    Entry* entry = find(id);
    if (entry == nullptr) return;
    if (payload.isObject()) {
      jsi::Value serial = payload.getObject(rt).getProperty(rt, "serial");
      // An earlier load's answer, still in flight when `src` was set again: it
      // belongs to nothing the page can see any more.
      if (serial.isNumber() && serial.getNumber() != static_cast<double>(entry->serial)) return;
    }
    jsi::Value target = entry->target->lock(rt);
    if (!target.isObject()) return;  // collected; its handle releases the instance
    jsi::Value handler = api->getProperty(rt, "onevent");
    if (!handler.isObject() || !handler.getObject(rt).isFunction(rt)) return;
    try {
      handler.getObject(rt).getFunction(rt).call(rt, target,
                                                 jsi::String::createFromAscii(rt, type), payload);
    } catch (const jsi::JSError& e) {
      log(LogLevel::Error, tag,
          std::string("uncaught error in an instance ") + type + " event: " + e.getMessage() + "\n" +
              e.getStack());
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, tag, std::string("JSI error in an instance ") + type + " event: " + e.what());
    } catch (const std::exception& e) {
      log(LogLevel::Error, tag, std::string("exception in an instance ") + type + " event: " + e.what());
    }
  }

  /// Freeze this page, unless focus has already come back to it (`focusRequest`).
  void freezeSelf() {
    const std::uint64_t wanted = focusRequest.load();
    if (wanted == 0 || !executor) return;
    std::weak_ptr<InstanceBinding> weak = weak_from_this();
    executor->invokeAsync([weak, wanted](jsi::Runtime& rt) {
      auto b = weak.lock();
      if (!b || b->stopped) return;
      if (b->focusRequest.load() != wanted) return;  // `parent.focus()` got here first
      dispatchLifecycleEvent(rt, "pause");
      if (b->control) b->control->pause();
    });
  }

  /// `focus` or `blur` at the element, so the page's `document.activeElement`
  /// follows a switch it did not make itself -- `window.parent.focus()` in the
  /// instance is the case that matters, because it comes from the other thread.
  void focusEvent(std::uint64_t id, const char* type) {
    Entry* entry = find(id);
    if (entry == nullptr || !entry->poster) return;
    const std::uint32_t serial = entry->serial;
    entry->poster->post(type, false, [serial](jsi::Runtime& js) -> jsi::Value {
      jsi::Object payload(js);
      payload.setProperty(js, "serial", static_cast<double>(serial));
      return payload;
    });
  }

  /// `focus(id)`: pause the outgoing context before resuming the incoming one.
  /// `id` 0 is this page itself -- a launcher taking the remote back.
  void focus(std::uint64_t id) {
    if (stopped) return;
    if (id != 0 && find(id) == nullptr) return;
    if (focused == id) return;
    const std::uint64_t outgoing = focused;

    focusRequest.store(id);
    // Outgoing first. Ordering, not simultaneity, is what "atomic" means here:
    // a context that is told to freeze before another is told to thaw never
    // gets a frame the other one has already had.
    if (outgoing == 0) {
      freezeSelf();
    } else if (Entry* leaving = find(outgoing)) {
      // An entry outlives its instance -- a refused load and `unload()` both
      // leave one behind for the element to load into again -- so every
      // dereference here is guarded, as `setPaused` and `post` already are.
      if (leaving->instance) leaving->instance->setPaused(true);
    }

    focused = id;
    if (outgoing != 0) focusEvent(outgoing, "blur");
    std::shared_ptr<FocusTarget> target = registry ? focusTarget(*registry) : nullptr;
    if (id == 0) {
      thawRuntime(control);
      // Null means "the host's own runtime": InputRouter falls back to the one
      // it was built with, so there is no second handle on it to keep alive.
      if (target) target->set(nullptr);
    } else {
      Entry* incoming = find(id);
      if (incoming == nullptr || !incoming->instance) {
        // Nothing to give the remote to: an element whose load was refused, or
        // one that has been unloaded. The launcher keeps it.
        focused = outgoing;
        focusRequest.store(outgoing);
        return;
      }
      incoming->instance->setPaused(false);
      if (target) target->set(incoming->instance->runtime());
      focusEvent(id, "focus");
    }
  }

  /// Release one instance: its layer goes, its thread is joined, its heap and GL
  /// resources are freed, and the loop may go idle again.
  void destroy(std::uint64_t id) {
    auto it = entries.find(id);
    if (it == entries.end()) return;
    Entry entry = std::move(it->second);
    entries.erase(it);
    // The layer first: the compositor must stop sampling the texture before the
    // instance's teardown deletes it.
    layers->remove(id);
    // A frame the host never got to: its fence is nobody's to wait on now, and
    // this thread's context is in the same share group, so it is ours to free.
    if (entry.layer) gfx::discardFence(entry.layer->takeFence());
    if (focused == id) {
      focused = 0;
      focusRequest.store(0);
      thawRuntime(control);
      if (registry) focusTarget(*registry)->set(nullptr);
    }
    if (entry.instance) entry.instance->terminate();
    if (auto live = loop.lock()) live->releaseWork();
    // The frame served while frozen is for the layers this page still has: with
    // the last one gone, a paused launcher goes back to burning no wakeup.
    // (The offscreen-texture present stays -- `swap()` keys off `presentContext_`
    // and there is no way back from it -- but that costs nothing while paused.)
    if (entries.empty()) {
      if (auto live = loop.lock()) live->setPresentWhilePaused(false);
    }
  }

  void shutdown() {
    if (stopped) return;
    stopped = true;
    for (auto& entry : entries) {
      layers->remove(entry.first);
      if (entry.second.layer) gfx::discardFence(entry.second.layer->takeFence());
      if (entry.second.instance) entry.second.instance->terminate();
      if (auto live = loop.lock()) live->releaseWork();
    }
    entries.clear();
    api.reset();
    if (auto live = loop.lock()) live->setPresentWhilePaused(false);
  }
};

namespace {

/// Bound to the element events go to. When the collector takes it -- the iframe
/// is gone -- the instance goes with it. The destructor may run on the
/// collector's thread, so the release itself is posted to the JS thread.
class InstanceHandle final : public jsi::NativeState {
 public:
  InstanceHandle(std::weak_ptr<InstanceBinding> binding, std::shared_ptr<JsExecutor> executor,
                 std::uint64_t id)
      : binding_(std::move(binding)), executor_(std::move(executor)), id_(id) {}

  ~InstanceHandle() override {
    if (!executor_) return;
    std::weak_ptr<InstanceBinding> binding = binding_;
    const std::uint64_t id = id_;
    executor_->invokeAsync([binding, id](jsi::Runtime&) {
      if (auto live = binding.lock()) live->destroy(id);
    });
  }

 private:
  std::weak_ptr<InstanceBinding> binding_;
  std::shared_ptr<JsExecutor> executor_;
  std::uint64_t id_;
};

std::uint64_t idArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, const char* fn) {
  if (count < 1 || !args[0].isNumber() || !isCount(args[0].getNumber())) {
    throw jsi::JSError(rt, std::string("__screenkit.instances.") + fn + " requires an instance id");
  }
  return static_cast<std::uint64_t>(args[0].getNumber());
}

double numberProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, double fallback) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isNumber() && std::isfinite(value.getNumber()) ? value.getNumber() : fallback;
}

int sizeProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, int fallback) {
  jsi::Value given = object.getProperty(rt, name);
  const bool present = given.isNumber() && std::isfinite(given.getNumber());
  const double value = std::floor(present ? given.getNumber() : static_cast<double>(fallback));
  // A size that is given and is zero means an element with nothing to show --
  // `width: 0`, a transform that collapsed it -- so it gets the smallest layer
  // there is. The fallback is for a size that was not given at all; handing a
  // whole drawable to an invisible element was quietly expensive.
  if (!(value >= 1)) return present ? 1 : fallback;
  // A layer larger than any display is a page's arithmetic gone wrong, not a
  // request: clamp rather than ask the driver for a texture it will refuse.
  return value > 8192 ? 8192 : static_cast<int>(value);
}

}  // namespace

std::shared_ptr<InstanceBinding> installInstances(jsi::Runtime& runtime,
                                                  std::shared_ptr<JsExecutor> executor,
                                                  std::shared_ptr<EventLoop> loop,
                                                  std::shared_ptr<RuntimeControl> control,
                                                  const RuntimeConfig& config) {
  auto binding = std::make_shared<InstanceBinding>();
  binding->executor = std::move(executor);
  binding->loop = loop;
  binding->registry = config.instances;
  binding->control = std::move(control);
  binding->sandbox = config.sandbox;
  binding->testTlsAnchors = config.testTlsAnchors;
  binding->maxHeapBytes = config.maxHeapBytes;
  binding->tag = config.name;
  binding->layers = gfx::layersFor(runtime);

  std::weak_ptr<InstanceBinding> weak = binding;
  jsi::Object api(runtime);

  const bool canEmbed = config.instances != nullptr;
  const bool hasParent = config.parent != nullptr;

  setFunction(runtime, api, "capabilities", 0,
              [weak, canEmbed, hasParent](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*,
                                          size_t) -> jsi::Value {
                auto b = weak.lock();
                jsi::Object out = b ? sandboxObject(rt, b->sandbox) : jsi::Object(rt);
                out.setProperty(rt, "canEmbed", canEmbed);
                out.setProperty(rt, "hasParent", hasParent);
                return out;
              });

  if (hasParent) {
    std::shared_ptr<InstanceChannel> parent = config.parent;
    setFunction(runtime, api, "parentPost", 1,
                [parent](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                         size_t count) -> jsi::Value {
                  const jsi::Value undefined;
                  parent->deliver(cloneOut(rt, count > 0 ? args[0] : undefined));
                  return jsi::Value::undefined();
                });
    setFunction(runtime, api, "parentFocus", 0,
                [parent](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
                  parent->focusParent();
                  return jsi::Value::undefined();
                });
  }

  if (canEmbed) {
    setFunction(
        runtime, api, "create", 1,
        [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
          auto b = weak.lock();
          if (!b || b->stopped) throw jsi::JSError(rt, "the instance layer has shut down");
          if (count < 1 || !args[0].isObject() || args[0].getObject(rt).isFunction(rt)) {
            throw jsi::JSError(rt, "__screenkit.instances.create requires the object its events go to");
          }
          jsi::Object target = args[0].getObject(rt);
          if (target.hasNativeState(rt)) {
            throw jsi::JSError(rt, "__screenkit.instances.create: that object already has an instance");
          }
          const std::uint64_t id = b->nextId++;
          InstanceBinding::Entry entry;
          entry.layer = std::make_shared<gfx::LayerSource>();
          entry.target = std::make_unique<jsi::WeakObject>(rt, target);
          entry.poster = std::make_shared<Poster>(weak, b->executor, id);
          b->entries.emplace(id, std::move(entry));
          if (auto live = b->loop.lock()) live->holdWork();
          target.setNativeState(rt, std::make_shared<InstanceHandle>(weak, b->executor, id));
          return jsi::Value(static_cast<double>(id));
        });

    setFunction(
        runtime, api, "load", 2,
        [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
          auto b = weak.lock();
          const std::uint64_t id = idArg(rt, args, count, "load");
          if (count < 2 || !args[1].isObject()) {
            throw jsi::JSError(rt, "__screenkit.instances.load requires options");
          }
          if (!b || b->stopped) throw jsi::JSError(rt, "the instance layer has shut down");
          InstanceBinding::Entry* entry = b->find(id);
          if (entry == nullptr) throw jsi::JSError(rt, "__screenkit.instances.load: no such instance");

          jsi::Object options = args[1].getObject(rt);
          jsi::Value srcValue = options.getProperty(rt, "src");
          const std::string src = srcValue.isString() ? srcValue.getString(rt).utf8(rt) : std::string();
          const std::uint32_t serial = b->nextSerial++;
          entry->serial = serial;

          // A load replaces whatever was there: the old instance is terminated
          // before the new one is made, so two never draw into one layer.
          if (entry->instance) {
            b->layers->remove(id);
            if (entry->layer) gfx::discardFence(entry->layer->takeFence());
            if (b->focused == id) b->focus(0);
            entry->instance->terminate();
            entry->instance.reset();
            entry->layer = std::make_shared<gfx::LayerSource>();
          }

          const std::shared_ptr<Poster> poster = entry->poster;
          const auto refuse = [&](const std::string& message) {
            poster->post("error", false, [serial, message](jsi::Runtime& js) -> jsi::Value {
              jsi::Object payload(js);
              payload.setProperty(js, "serial", static_cast<double>(serial));
              payload.setProperty(js, "message", str(js, message));
              return payload;
            });
            return jsi::Value(static_cast<double>(serial));
          };

          // `src` names a local package only: a path confined to this page's own
          // package, which the asset root already enforces exactly as it does
          // for a <video>. Nothing is downloaded, so there is no cache and no
          // integrity check to write -- fetching an http(s) src stays M12's job.
          const std::string scheme = lower(src.substr(0, src.find(':')));
          if (src.find(':') != std::string::npos && (scheme == "http" || scheme == "https")) {
            return refuse("an <iframe src> names a package inside this app (" + src +
                          " is a network URL): a launcher ships the games it embeds, and fetching "
                          "one is a later milestone");
          }
          std::string resolved;
          std::string why;
          if (src.empty() || !resolveAssetPath(rt, src, resolved, why)) {
            return refuse(src.empty() ? std::string("an <iframe src> names a package inside this app")
                                      : why);
          }

          // The compositor, on first use. A page with no iframe never gets here,
          // so its present path and its cost are what they were.
          std::shared_ptr<gfx::GlSurface> surface = gfx::surfaceFor(rt);
          if (!surface) {
            return refuse(
                "this page has no drawable, so there is nowhere to composite an <iframe>: run it in "
                "a windowed host");
          }
          // The page's own context is where the compositor is built and what a
          // child's layer is shared with -- and over SDL "shared with" means
          // "shared with whatever is current on this thread"
          // (SDL_GL_SHARE_WITH_CURRENT_CONTEXT), which `createShared` refuses
          // outright when it is not the parent's. A canvas layer may have been
          // the last thing drawn on, so this is restored for every embed, not
          // only the first: the second <iframe> on a page with a HUD canvas is
          // exactly the case that would otherwise fail, and only on the SDL
          // backends.
          gfx::makeContextCurrent(gfx::primaryContext(rt));
          if (!b->compositing) {
            std::string glError;
            if (!gfx::enableCompositing(rt, *surface, glError)) {
              return refuse("this platform has no compositor for <iframe> instances: " + glError);
            }
            b->compositing = true;
            // This page owns a compositor now, so its own freeze must not stop
            // the present: a launcher that pauses itself to give a game the
            // remote still has to put the game on screen.
            if (auto live = b->loop.lock()) live->setPresentWhilePaused(true);
          }

          InstanceOptions instance;
          instance.name = b->tag + ".iframe-" + std::to_string(id);
          instance.src = resolved;
          jsi::Value sandboxValue = options.getProperty(rt, "sandbox");
          if (sandboxValue.isString()) {
            instance.sandbox = parseSandbox(sandboxValue.getString(rt).utf8(rt), instance.name);
          }
          // An instance of a sandboxed instance could not widen it anyway -- one
          // level of nesting -- but a launcher that is itself sandboxed must not
          // hand out what it does not have.
          if (b->sandbox.sandboxed) {
            instance.sandbox.sandboxed = true;
            instance.sandbox.allowNetwork = instance.sandbox.allowNetwork && b->sandbox.network();
            instance.sandbox.allowMedia = instance.sandbox.allowMedia && b->sandbox.media();
          }
          // The heap ceiling this page runs under, inherited: it is not a
          // containment boundary (Architecture.md 5.2), but a launcher that was
          // given one should not be able to hand out a bigger one.
          instance.maxHeapBytes = b->maxHeapBytes;
          instance.width = sizeProperty(rt, options, "width", std::max(1, surface->width()));
          instance.height = sizeProperty(rt, options, "height", std::max(1, surface->height()));
          instance.testTlsAnchors = b->testTlsAnchors;

          InstanceEvents events;
          events.loaded = [poster, serial](bool ok, std::string error) {
            if (ok) {
              poster->post("load", false, [serial](jsi::Runtime& js) -> jsi::Value {
                jsi::Object payload(js);
                payload.setProperty(js, "serial", static_cast<double>(serial));
                return payload;
              });
            } else {
              poster->post("error", false, [serial, error](jsi::Runtime& js) -> jsi::Value {
                jsi::Object payload(js);
                payload.setProperty(js, "serial", static_cast<double>(serial));
                payload.setProperty(js, "message", str(js, error));
                return payload;
              });
            }
          };
          events.failed = [poster, serial](std::string error) {
            // After `load`: the element hears `error` the same way, and the shim
            // clears `loaded` when it does.
            poster->post("error", false, [serial, error](jsi::Runtime& js) -> jsi::Value {
              jsi::Object payload(js);
              payload.setProperty(js, "serial", static_cast<double>(serial));
              payload.setProperty(js, "message", str(js, error));
              return payload;
            });
          };
          events.message = [poster, serial](std::shared_ptr<const CloneValue> message) {
            poster->post("message", false, [serial, message](jsi::Runtime& js) -> jsi::Value {
              jsi::Object payload(js);
              payload.setProperty(js, "serial", static_cast<double>(serial));
              payload.setProperty(js, "data", cloneIn(js, *message));
              return payload;
            });
          };
          auto executor = b->executor;
          events.focusParent = [weak, executor] {
            // Called from the instance's JS thread while this page is frozen, so
            // the thaw happens here and the bookkeeping follows as a task -- one
            // that would otherwise wait behind the gate it just opened. The
            // request is recorded *before* the thaw, so a freeze task that has
            // not run yet sees that focus came back and skips itself.
            auto live = weak.lock();
            if (!live) return;
            live->focusRequest.store(0);
            thawRuntime(live->control);
            executor->invokeAsync([weak](jsi::Runtime&) {
              if (auto b2 = weak.lock()) b2->focus(0);
            });
          };

          std::string error;
          std::shared_ptr<Instance> made = createInstance(*b->registry, *surface, std::move(instance),
                                                          std::move(events), entry->layer, error);
          if (!made) return refuse(error);
          entry->instance = std::move(made);
          return jsi::Value(static_cast<double>(serial));
        });

    setFunction(runtime, api, "focus", 1,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  const std::uint64_t id = idArg(rt, args, count, "focus");
                  if (auto b = weak.lock()) b->focus(id);
                  return jsi::Value::undefined();
                });

    setFunction(runtime, api, "setPaused", 2,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  const std::uint64_t id = idArg(rt, args, count, "setPaused");
                  const bool paused = count > 1 && args[1].isBool() && args[1].getBool();
                  auto b = weak.lock();
                  if (!b || b->stopped) return jsi::Value::undefined();
                  InstanceBinding::Entry* entry = b->find(id);
                  if (entry != nullptr && entry->instance) entry->instance->setPaused(paused);
                  return jsi::Value::undefined();
                });

    setFunction(runtime, api, "post", 2,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  const std::uint64_t id = idArg(rt, args, count, "post");
                  auto b = weak.lock();
                  if (!b || b->stopped) return jsi::Value::undefined();
                  InstanceBinding::Entry* entry = b->find(id);
                  if (entry == nullptr || !entry->instance) return jsi::Value::undefined();
                  // Encoded here, on the sender's thread, and decoded on the
                  // receiver's: the two runtimes share no value, so every
                  // message is a copy whether or not anyone asked for one.
                  const jsi::Value undefined;
                  postToInstance(*entry->instance, cloneOut(rt, count > 1 ? args[1] : undefined));
                  return jsi::Value::undefined();
                });

    setFunction(runtime, api, "setPlane", 8,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  const std::uint64_t id = idArg(rt, args, count, "setPlane");
                  auto b = weak.lock();
                  if (!b || b->stopped) return jsi::Value::undefined();
                  InstanceBinding::Entry* entry = b->find(id);
                  if (entry == nullptr) return jsi::Value::undefined();
                  gfx::Layer layer;
                  layer.source = entry->layer;
                  const auto number = [&](size_t index) {
                    return count > index && args[index].isNumber() &&
                                   std::isfinite(args[index].getNumber())
                               ? args[index].getNumber()
                               : 0.0;
                  };
                  layer.rect.x = number(1);
                  layer.rect.y = number(2);
                  layer.rect.width = number(3);
                  layer.rect.height = number(4);
                  layer.visible = count > 5 && args[5].isBool() && args[5].getBool() &&
                                  layer.rect.width > 0 && layer.rect.height > 0;
                  layer.order = count > 6 && args[6].isNumber() && std::isfinite(args[6].getNumber())
                                    ? static_cast<int>(args[6].getNumber())
                                    : 0;
                  layer.opacity = count > 7 && args[7].isNumber() && std::isfinite(args[7].getNumber())
                                      ? std::max(0.0, std::min(1.0, args[7].getNumber()))
                                      : 1.0;
                  b->layers->set(id, layer);
                  return jsi::Value::undefined();
                });

    setFunction(runtime, api, "unload", 1,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  // The element left the document: HTML terminates an iframe's
                  // browsing context when it is removed, and creates a fresh one
                  // when it goes back in. The handle stays -- it belongs to the
                  // element, which is still there -- so a re-insert is a `load`
                  // rather than a second `create` on an object that already has
                  // native state.
                  const std::uint64_t id = idArg(rt, args, count, "unload");
                  auto b = weak.lock();
                  if (!b || b->stopped) return jsi::Value::undefined();
                  InstanceBinding::Entry* entry = b->find(id);
                  if (entry == nullptr) return jsi::Value::undefined();
                  // Nothing the terminated instance still had in flight reaches
                  // the page.
                  entry->serial = b->nextSerial++;
                  b->layers->remove(id);
                  if (entry->layer) gfx::discardFence(entry->layer->takeFence());
                  if (b->focused == id) b->focus(0);
                  if (entry->instance) {
                    entry->instance->terminate();
                    entry->instance.reset();
                  }
                  entry->layer = std::make_shared<gfx::LayerSource>();
                  return jsi::Value::undefined();
                });

    setFunction(runtime, api, "destroy", 1,
                [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                       size_t count) -> jsi::Value {
                  const std::uint64_t id = idArg(rt, args, count, "destroy");
                  auto b = weak.lock();
                  if (b && !b->stopped) b->destroy(id);
                  return jsi::Value::undefined();
                });
  }

  api.setProperty(runtime, "onevent", jsi::Value::null());
  api.setProperty(runtime, "onparentmessage", jsi::Value::null());

  jsi::Value io = runtime.global().getProperty(runtime, "__screenkit");
  if (!io.isObject()) {
    throw jsi::JSError(runtime, "installInstances: __screenkit is missing -- installHostIO runs first");
  }
  io.getObject(runtime).setProperty(runtime, "instances", std::move(api));
  binding->api = std::make_unique<jsi::Object>(
      io.getObject(runtime).getPropertyAsObject(runtime, "instances"));
  return binding;
}

void shutdownInstances(InstanceBinding& binding) { binding.shutdown(); }

}  // namespace screenkit
