// Copyright (c) ScreenKit contributors. MIT.
//
// `__screenkit.media`, the handle API over media::MediaPlayer. HTMLMediaElement's
// semantics -- readyState, the event order, play() promises -- and Shaka's are
// JavaScript (runtime/js/dom-shim.js, packages/@screenkit/shaka); this file only
// moves calls one way and events the other, across the thread boundary.
#include "Media.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "../loop/EventLoop.h"
#include "../media/MediaPlayer.h"
#include "EventChannel.h"
#include "HostIO.h"

namespace jsi = facebook::jsi;

namespace screenkit {

// ---- the registry: players reachable from any thread --------------------------

class MediaRegistry {
 public:
  void add(std::uint64_t id, std::shared_ptr<media::MediaPlayer> player) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    entries_[id] = Entry{std::move(player), false};
  }

  void remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(id);
  }

  /// JS asked for play (true) or pause (false). The native call is made here,
  /// under the lock, so it cannot interleave with a runtime pause: a runtime
  /// that is paused records the wish and plays on resume.
  void setWantsPlay(std::uint64_t id, bool wants) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return;
    it->second.wantsPlay = wants;
    if (!wants) {
      it->second.player->pause();
    } else if (!paused_) {
      it->second.player->play();
    }
  }

  /// A new load starts paused, whatever was playing before.
  void resetWantsPlay(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it != entries_.end()) it->second.wantsPlay = false;
  }

  void setRuntimePaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ == paused || closed_) {
      paused_ = paused;
      return;
    }
    paused_ = paused;
    for (auto& entry : entries_) {
      if (!entry.second.wantsPlay) continue;
      if (paused) {
        entry.second.player->pause();
      } else {
        entry.second.player->play();
      }
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    entries_.clear();
  }

 private:
  struct Entry {
    std::shared_ptr<media::MediaPlayer> player;
    bool wantsPlay = false;
  };
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, Entry> entries_;
  bool paused_ = false;
  bool closed_ = false;
};

std::shared_ptr<MediaRegistry> makeMediaRegistry() { return std::make_shared<MediaRegistry>(); }

void setMediaRuntimePaused(MediaRegistry& registry, bool paused) { registry.setRuntimePaused(paused); }

namespace {

class OwnedArrayBuffer : public jsi::MutableBuffer {
 public:
  explicit OwnedArrayBuffer(media::Bytes bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t* data() override { return bytes_.data(); }

 private:
  media::Bytes bytes_;
};

bool isCount(double value) {
  return std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value;
}

/// An ArrayBuffer, a typed array or a DataView, copied.
bool bytesFrom(jsi::Runtime& rt, const jsi::Value& value, media::Bytes& out) {
  if (!value.isObject()) return false;
  jsi::Object object = value.getObject(rt);
  if (object.isArrayBuffer(rt)) {
    jsi::ArrayBuffer buffer = object.getArrayBuffer(rt);
    const uint8_t* data = buffer.data(rt);
    out.assign(data, data + buffer.size(rt));
    return true;
  }
  jsi::Value inner = object.getProperty(rt, "buffer");
  if (!inner.isObject() || !inner.getObject(rt).isArrayBuffer(rt)) return false;
  jsi::ArrayBuffer buffer = inner.getObject(rt).getArrayBuffer(rt);
  const double offset = object.getProperty(rt, "byteOffset").asNumber();
  const double length = object.getProperty(rt, "byteLength").asNumber();
  const size_t total = buffer.size(rt);
  if (!isCount(offset) || !isCount(length) || offset + length > static_cast<double>(total)) return false;
  const uint8_t* data = buffer.data(rt) + static_cast<size_t>(offset);
  out.assign(data, data + static_cast<size_t>(length));
  return true;
}

std::string stringProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name,
                           const std::string& fallback = std::string()) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isString() ? value.getString(rt).utf8(rt) : fallback;
}

double numberProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, double fallback) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isNumber() ? value.getNumber() : fallback;
}

int intProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, int fallback) {
  const double value = numberProperty(rt, object, name, fallback);
  if (std::isnan(value)) return fallback;
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
  if (value <= 0) return 0;
  return static_cast<int>(value);
}

bool boolProperty(jsi::Runtime& rt, const jsi::Object& object, const char* name, bool fallback) {
  jsi::Value value = object.getProperty(rt, name);
  return value.isBool() ? value.getBool() : fallback;
}

bool isHex(const std::string& text, size_t length) {
  if (text.size() != length) return false;
  for (unsigned char c : text) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

/// A file path as a `file:` URL, percent-encoding what a URL cannot hold.
std::string fileUrl(const std::string& path) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

media::AbrConfig abrFrom(jsi::Runtime& rt, const jsi::Value& value) {
  media::AbrConfig abr;
  if (!value.isObject()) return abr;
  jsi::Object o = value.getObject(rt);
  abr.enabled = boolProperty(rt, o, "enabled", true);
  abr.minBandwidth = numberProperty(rt, o, "minBandwidth", 0);
  abr.maxBandwidth = numberProperty(rt, o, "maxBandwidth", std::numeric_limits<double>::infinity());
  abr.minWidth = intProperty(rt, o, "minWidth", 0);
  abr.maxWidth = intProperty(rt, o, "maxWidth", std::numeric_limits<int>::max());
  abr.minHeight = intProperty(rt, o, "minHeight", 0);
  abr.maxHeight = intProperty(rt, o, "maxHeight", std::numeric_limits<int>::max());
  if (std::isnan(abr.minBandwidth) || abr.minBandwidth < 0) abr.minBandwidth = 0;
  if (std::isnan(abr.maxBandwidth)) abr.maxBandwidth = std::numeric_limits<double>::infinity();
  return abr;
}

void setFunction(jsi::Runtime& rt, jsi::Object& target, const char* name, unsigned params,
                 jsi::HostFunctionType body) {
  target.setProperty(rt, name,
                     jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, name), params,
                                                           std::move(body)));
}

jsi::Value str(jsi::Runtime& rt, const std::string& text) { return jsi::String::createFromUtf8(rt, text); }

jsi::Object withSerial(jsi::Runtime& rt, std::uint32_t serial) {
  jsi::Object payload(rt);
  payload.setProperty(rt, "serial", static_cast<double>(serial));
  return payload;
}

jsi::Array ranges(jsi::Runtime& rt, const std::vector<media::TimeRange>& list) {
  jsi::Array out(rt, list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    jsi::Array pair(rt, 2);
    pair.setValueAtIndex(rt, 0, list[i].start);
    pair.setValueAtIndex(rt, 1, list[i].end);
    out.setValueAtIndex(rt, i, std::move(pair));
  }
  return out;
}

jsi::Object tracksObject(jsi::Runtime& rt, const media::TrackList& tracks) {
  jsi::Object out(rt);
  jsi::Array variants(rt, tracks.variants.size());
  for (size_t i = 0; i < tracks.variants.size(); ++i) {
    const auto& v = tracks.variants[i];
    jsi::Object o(rt);
    o.setProperty(rt, "id", v.id);
    o.setProperty(rt, "bandwidth", v.bandwidth);
    o.setProperty(rt, "width", v.width);
    o.setProperty(rt, "height", v.height);
    o.setProperty(rt, "frameRate", v.frameRate);
    o.setProperty(rt, "videoCodec", str(rt, v.videoCodec));
    o.setProperty(rt, "audioCodec", str(rt, v.audioCodec));
    o.setProperty(rt, "language", str(rt, v.language));
    o.setProperty(rt, "label", str(rt, v.label));
    o.setProperty(rt, "audioId", v.audioId);
    o.setProperty(rt, "channels", v.channels);
    o.setProperty(rt, "active", v.active);
    variants.setValueAtIndex(rt, i, std::move(o));
  }
  out.setProperty(rt, "variants", std::move(variants));
  jsi::Array audio(rt, tracks.audio.size());
  for (size_t i = 0; i < tracks.audio.size(); ++i) {
    const auto& a = tracks.audio[i];
    jsi::Object o(rt);
    o.setProperty(rt, "id", a.id);
    o.setProperty(rt, "language", str(rt, a.language));
    o.setProperty(rt, "label", str(rt, a.label));
    o.setProperty(rt, "role", str(rt, a.role));
    o.setProperty(rt, "codec", str(rt, a.codec));
    o.setProperty(rt, "channels", a.channels);
    o.setProperty(rt, "active", a.active);
    audio.setValueAtIndex(rt, i, std::move(o));
  }
  out.setProperty(rt, "audio", std::move(audio));
  jsi::Array text(rt, tracks.text.size());
  for (size_t i = 0; i < tracks.text.size(); ++i) {
    const auto& t = tracks.text[i];
    jsi::Object o(rt);
    o.setProperty(rt, "id", t.id);
    o.setProperty(rt, "language", str(rt, t.language));
    o.setProperty(rt, "label", str(rt, t.label));
    o.setProperty(rt, "kind", str(rt, t.kind));
    o.setProperty(rt, "mimeType", str(rt, t.mimeType));
    o.setProperty(rt, "forced", t.forced);
    o.setProperty(rt, "active", t.active);
    text.setValueAtIndex(rt, i, std::move(o));
  }
  out.setProperty(rt, "text", std::move(text));
  return out;
}

jsi::Array stringArray(jsi::Runtime& rt, const std::vector<std::string>& list) {
  jsi::Array out(rt, list.size());
  for (size_t i = 0; i < list.size(); ++i) out.setValueAtIndex(rt, i, str(rt, list[i]));
  return out;
}

/// What a player emits, turned into posts on its channel. Closed at shutdown,
/// after which a late event from a platform thread is dropped here.
class JsMediaSink final : public media::MediaSink {
 public:
  explicit JsMediaSink(Poster poster) : poster_(std::move(poster)) {}

  void close() { closed_.store(true); }

  void onMetadata(std::uint32_t serial, media::MediaInfo info) override {
    post("metadata", [serial, info](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "duration", info.duration);
      p.setProperty(rt, "live", info.live);
      p.setProperty(rt, "seekStart", info.seekStart);
      p.setProperty(rt, "seekEnd", info.seekEnd);
      p.setProperty(rt, "width", info.width);
      p.setProperty(rt, "height", info.height);
      p.setProperty(rt, "hasVideo", info.hasVideo);
      p.setProperty(rt, "hasAudio", info.hasAudio);
      p.setProperty(rt, "manifest", str(rt, info.manifest));
      return p;
    });
  }

  void onState(std::uint32_t serial, media::PlaybackState state) override {
    post("state", [serial, state](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "state", jsi::String::createFromAscii(rt, media::playbackStateName(state)));
      return p;
    });
  }

  void onTime(std::uint32_t serial, double position, double seekStart, double seekEnd) override {
    post("time", [serial, position, seekStart, seekEnd](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "position", position);
      p.setProperty(rt, "seekStart", seekStart);
      p.setProperty(rt, "seekEnd", seekEnd);
      return p;
    });
  }

  void onSeeked(std::uint32_t serial, double position) override {
    post("seeked", [serial, position](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "position", position);
      return p;
    });
  }

  void onBuffered(std::uint32_t serial, std::vector<media::TimeRange> list) override {
    auto shared = std::make_shared<std::vector<media::TimeRange>>(std::move(list));
    post("buffered", [serial, shared](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "ranges", ranges(rt, *shared));
      return p;
    });
  }

  void onTracks(std::uint32_t serial, media::TrackList tracks) override {
    auto shared = std::make_shared<media::TrackList>(std::move(tracks));
    post("tracks", [serial, shared](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = tracksObject(rt, *shared);
      p.setProperty(rt, "serial", static_cast<double>(serial));
      return p;
    });
  }

  void onVariantChanged(std::uint32_t serial, int variantId) override {
    post("variant", [serial, variantId](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "id", variantId);
      return p;
    });
  }

  void onSize(std::uint32_t serial, int width, int height) override {
    post("size", [serial, width, height](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "width", width);
      p.setProperty(rt, "height", height);
      return p;
    });
  }

  void onCues(std::uint32_t serial, int textTrackId, std::vector<media::Cue> cues) override {
    auto shared = std::make_shared<std::vector<media::Cue>>(std::move(cues));
    post("cues", [serial, textTrackId, shared](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "track", textTrackId);
      jsi::Array list(rt, shared->size());
      for (size_t i = 0; i < shared->size(); ++i) {
        jsi::Object cue(rt);
        cue.setProperty(rt, "start", (*shared)[i].start);
        cue.setProperty(rt, "end", (*shared)[i].end);
        cue.setProperty(rt, "text", str(rt, (*shared)[i].text));
        list.setValueAtIndex(rt, i, std::move(cue));
      }
      p.setProperty(rt, "cues", std::move(list));
      return p;
    });
  }

  void onLicenceRequest(std::uint32_t serial, std::uint64_t requestId, std::string keySystem,
                        media::Bytes challenge, std::string contentId) override {
    auto shared = std::make_shared<media::Bytes>(std::move(challenge));
    post("licence", [serial, requestId, keySystem, shared, contentId](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "requestId", static_cast<double>(requestId));
      p.setProperty(rt, "keySystem", str(rt, keySystem));
      p.setProperty(rt, "challenge", jsi::ArrayBuffer(rt, std::make_shared<OwnedArrayBuffer>(std::move(*shared))));
      p.setProperty(rt, "contentId", str(rt, contentId));
      return p;
    });
  }

  void onStats(std::uint32_t serial, media::MediaStats stats) override {
    post("stats", [serial, stats](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "width", stats.width);
      p.setProperty(rt, "height", stats.height);
      p.setProperty(rt, "frameRate", stats.frameRate);
      p.setProperty(rt, "streamBandwidth", stats.streamBandwidth);
      p.setProperty(rt, "estimatedBandwidth", stats.estimatedBandwidth);
      p.setProperty(rt, "decodedFrames", static_cast<double>(stats.decodedFrames));
      p.setProperty(rt, "droppedFrames", static_cast<double>(stats.droppedFrames));
      p.setProperty(rt, "corruptedFrames", static_cast<double>(stats.corruptedFrames));
      return p;
    });
  }

  void onError(std::uint32_t serial, media::MediaErrorKind kind, int httpStatus, std::string message) override {
    post("error", [serial, kind, httpStatus, message](jsi::Runtime& rt) -> jsi::Value {
      jsi::Object p = withSerial(rt, serial);
      p.setProperty(rt, "kind", jsi::String::createFromAscii(rt, media::mediaErrorKindName(kind)));
      p.setProperty(rt, "httpStatus", httpStatus);
      p.setProperty(rt, "message", str(rt, message));
      return p;
    });
  }

 private:
  void post(const char* type, EventChannel::MakePayload make) {
    if (closed_.load()) return;
    poster_.post(type, false, std::move(make));
  }

  Poster poster_;
  std::atomic<bool> closed_{false};
};

}  // namespace

/// JS-thread state: every player, the object its events go to, and the serial
/// of its latest load.
class MediaBinding : public ChannelTarget, public std::enable_shared_from_this<MediaBinding> {
 public:
  struct Player {
    std::shared_ptr<media::MediaPlayer> player;
    std::shared_ptr<JsMediaSink> sink;
    std::unique_ptr<jsi::WeakObject> target;
    std::uint32_t serial = 0;
  };

  std::shared_ptr<MediaRegistry> registry;
  std::shared_ptr<JsExecutor> executor;
  std::weak_ptr<EventLoop> loop;
  std::string tag;
  /// `__screenkit.media` itself, where the shim sets `onevent`.
  std::unique_ptr<jsi::Object> api;
  std::unordered_map<std::uint64_t, Player> players;
  std::uint64_t nextId = 1;
  std::uint32_t nextSerial = 1;
  bool stopped = false;

  bool channelStopped() const override { return stopped; }
  const std::string& channelTag() const override { return tag; }

  Player* find(std::uint64_t id) {
    auto it = players.find(id);
    return it == players.end() ? nullptr : &it->second;
  }

  void deliver(jsi::Runtime& rt, std::uint64_t id, const char* type, const jsi::Value& payload,
               bool) override {
    if (stopped || !api) return;
    Player* p = find(id);
    if (p == nullptr) return;
    if (payload.isObject()) {
      jsi::Value serial = payload.getObject(rt).getProperty(rt, "serial");
      // An earlier load's event, still in flight when the next load (or an
      // unload) started: it belongs to nothing the page can see any more.
      if (serial.isNumber() && serial.getNumber() != static_cast<double>(p->serial)) return;
    }
    jsi::Value target = p->target->lock(rt);
    if (!target.isObject()) return;  // collected; its handle releases the player
    jsi::Value handler = api->getProperty(rt, "onevent");
    if (!handler.isObject() || !handler.getObject(rt).isFunction(rt)) return;
    try {
      handler.getObject(rt).getFunction(rt).call(rt, target, jsi::String::createFromAscii(rt, type), payload);
    } catch (const jsi::JSError& e) {
      log(LogLevel::Error, tag,
          std::string("uncaught error in a media ") + type + " event: " + e.getMessage() + "\n" + e.getStack());
    } catch (const jsi::JSIException& e) {
      log(LogLevel::Error, tag, std::string("JSI error in a media ") + type + " event: " + e.what());
    } catch (const std::exception& e) {
      log(LogLevel::Error, tag, std::string("exception in a media ") + type + " event: " + e.what());
    }
  }

  /// Release one player: its channel goes quiet, the platform player is shut
  /// down (synchronously -- nothing of it is delivered afterwards), and the loop
  /// may go idle again.
  void destroy(std::uint64_t id) {
    auto it = players.find(id);
    if (it == players.end()) return;
    Player player = std::move(it->second);
    players.erase(it);
    registry->remove(id);
    player.sink->close();
    player.player->shutdown();
    if (auto live = loop.lock()) live->releaseWork();
  }

  void shutdown() {
    if (stopped) return;
    stopped = true;
    registry->close();
    for (auto& entry : players) {
      entry.second.sink->close();
      entry.second.player->shutdown();
      if (auto live = loop.lock()) live->releaseWork();
    }
    players.clear();
    api.reset();
  }
};

namespace {

/// Bound to the object events go to. When the collector takes that object --
/// the element is gone -- the player goes with it. The destructor may run on
/// the collector's thread, so the release itself is posted to the JS thread.
class PlayerHandle final : public jsi::NativeState {
 public:
  PlayerHandle(std::weak_ptr<MediaBinding> binding, std::shared_ptr<JsExecutor> executor, std::uint64_t id)
      : binding_(std::move(binding)), executor_(std::move(executor)), id_(id) {}

  ~PlayerHandle() override {
    if (!executor_) return;
    std::weak_ptr<MediaBinding> binding = binding_;
    const std::uint64_t id = id_;
    executor_->invokeAsync([binding, id](jsi::Runtime&) {
      if (auto live = binding.lock()) live->destroy(id);
    });
  }

 private:
  std::weak_ptr<MediaBinding> binding_;
  std::shared_ptr<JsExecutor> executor_;
  std::uint64_t id_;
};

std::uint64_t idArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, const char* fn) {
  if (count < 1 || !args[0].isNumber() || !isCount(args[0].getNumber())) {
    throw jsi::JSError(rt, std::string("__screenkit.media.") + fn + " requires a player id");
  }
  return static_cast<std::uint64_t>(args[0].getNumber());
}

double numberArg(jsi::Runtime& rt, const jsi::Value* args, size_t count, size_t index, const char* fn) {
  if (count <= index || !args[index].isNumber()) {
    throw jsi::JSError(rt, std::string("__screenkit.media.") + fn + " requires a number");
  }
  return args[index].getNumber();
}

/// The player an id names, or null when it was destroyed or the binding stopped:
/// a late call on a released element is a no-op, not an error.
std::shared_ptr<media::MediaPlayer> playerFor(const std::shared_ptr<MediaBinding>& b, std::uint64_t id) {
  if (!b || b->stopped) return nullptr;
  MediaBinding::Player* p = b->find(id);
  return p == nullptr ? nullptr : p->player;
}

media::DrmConfig drmFrom(jsi::Runtime& rt, const jsi::Value& value) {
  media::DrmConfig drm;
  if (!value.isObject()) return drm;
  jsi::Object o = value.getObject(rt);
  drm.keySystem = stringProperty(rt, o, "keySystem");
  drm.licenceServer = stringProperty(rt, o, "licenceServer");
  drm.videoRobustness = stringProperty(rt, o, "videoRobustness");
  drm.audioRobustness = stringProperty(rt, o, "audioRobustness");
  jsi::Value keys = o.getProperty(rt, "clearKeys");
  if (keys.isObject() && keys.getObject(rt).isArray(rt)) {
    jsi::Array list = keys.getObject(rt).getArray(rt);
    for (size_t i = 0; i < list.size(rt); ++i) {
      jsi::Value entry = list.getValueAtIndex(rt, i);
      if (!entry.isObject() || !entry.getObject(rt).isArray(rt)) continue;
      jsi::Array pair = entry.getObject(rt).getArray(rt);
      if (pair.size(rt) < 2) continue;
      const std::string kid = lower(pair.getValueAtIndex(rt, 0).toString(rt).utf8(rt));
      const std::string key = lower(pair.getValueAtIndex(rt, 1).toString(rt).utf8(rt));
      if (!isHex(kid, 32) || !isHex(key, 32)) {
        throw jsi::JSError(rt, "__screenkit.media.load: a ClearKey key id and key are 32 hex digits each");
      }
      drm.clearKeys.emplace_back(kid, key);
    }
  }
  jsi::Value certificate = o.getProperty(rt, "serverCertificate");
  if (!certificate.isUndefined() && !certificate.isNull()) {
    media::Bytes bytes;
    if (!bytesFrom(rt, certificate, bytes)) {
      throw jsi::JSError(rt, "__screenkit.media.load: drm.serverCertificate must be bytes");
    }
    if (!bytes.empty()) drm.serverCertificate = std::make_shared<const media::Bytes>(std::move(bytes));
  }
  return drm;
}

}  // namespace

std::shared_ptr<MediaBinding> installMedia(jsi::Runtime& runtime, std::shared_ptr<JsExecutor> executor,
                                           std::shared_ptr<EventLoop> loop,
                                           std::shared_ptr<MediaRegistry> registry,
                                           const RuntimeConfig& config) {
  auto binding = std::make_shared<MediaBinding>();
  binding->registry = registry ? std::move(registry) : makeMediaRegistry();
  binding->executor = std::move(executor);
  binding->loop = loop;
  binding->tag = config.name;

  std::weak_ptr<MediaBinding> weak = binding;
  jsi::Object api(runtime);

  setFunction(runtime, api, "capabilities", 0,
              [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
                const media::MediaCapabilities caps = media::mediaCapabilities();
                jsi::Object out(rt);
                out.setProperty(rt, "available", caps.available);
                out.setProperty(rt, "platform", str(rt, caps.platform));
                out.setProperty(rt, "hls", caps.hls);
                out.setProperty(rt, "dash", caps.dash);
                out.setProperty(rt, "progressive", caps.progressive);
                out.setProperty(rt, "keySystems", stringArray(rt, caps.keySystems));
                out.setProperty(rt, "containers", stringArray(rt, caps.containers));
                out.setProperty(rt, "codecs", stringArray(rt, caps.codecs));
                out.setProperty(rt, "videoOutput", caps.videoOutput);
                out.setProperty(rt, "videoOutputProblem", str(rt, caps.videoOutputProblem));
                return out;
              });

  setFunction(runtime, api, "create", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                if (!b || b->stopped) throw jsi::JSError(rt, "the media layer has shut down");
                if (count < 1 || !args[0].isObject() || args[0].getObject(rt).isFunction(rt)) {
                  throw jsi::JSError(rt, "__screenkit.media.create requires the object its events go to");
                }
                jsi::Object target = args[0].getObject(rt);
                if (target.hasNativeState(rt)) {
                  throw jsi::JSError(rt, "__screenkit.media.create: that object already has a player");
                }
                const std::uint64_t id = b->nextId++;
                auto sink = std::make_shared<JsMediaSink>(Poster(weak, b->executor, id));
                media::MediaConfig config;
                config.name = b->tag;
                config.sink = sink;
                MediaBinding::Player player;
                player.player = media::MediaPlayer::create(std::move(config));
                player.sink = sink;
                player.target = std::make_unique<jsi::WeakObject>(rt, target);
                b->registry->add(id, player.player);
                b->players.emplace(id, std::move(player));
                if (auto live = b->loop.lock()) live->holdWork();
                target.setNativeState(rt, std::make_shared<PlayerHandle>(weak, b->executor, id));
                return jsi::Value(static_cast<double>(id));
              });

  setFunction(runtime, api, "load", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "load");
                if (count < 2 || !args[1].isObject()) {
                  throw jsi::JSError(rt, "__screenkit.media.load requires options");
                }
                jsi::Object options = args[1].getObject(rt);
                if (!b || b->stopped) throw jsi::JSError(rt, "the media layer has shut down");
                MediaBinding::Player* p = b->find(id);
                if (p == nullptr) throw jsi::JSError(rt, "__screenkit.media.load: no such player");

                media::LoadRequest request;
                request.url = stringProperty(rt, options, "url");
                request.startTime = numberProperty(rt, options, "startTime", std::numeric_limits<double>::quiet_NaN());
                // NaN is "from wherever the stream starts"; an infinity is not a
                // position at all, and the three players saturate it differently.
                if (std::isinf(request.startTime)) {
                  throw jsi::JSError(rt, "__screenkit.media.load: startTime is not a finite time");
                }
                request.mimeType = stringProperty(rt, options, "mimeType");
                request.drm = drmFrom(rt, options.getProperty(rt, "drm"));
                request.abr = abrFrom(rt, options.getProperty(rt, "abr"));
                request.audioLanguage = stringProperty(rt, options, "audioLanguage");
                request.textLanguage = stringProperty(rt, options, "textLanguage");
                request.serial = b->nextSerial++;
                p->serial = request.serial;
                b->registry->resetWantsPlay(id);

                // A package asset is the confined file it resolves to. Anything
                // but http(s) and that is refused here, as the platform players
                // would each refuse it differently.
                std::string why;
                const std::string asset = stringProperty(rt, options, "asset");
                if (!asset.empty()) {
                  std::string resolved;
                  if (resolveAssetPath(rt, asset, resolved, why)) {
                    request.url = fileUrl(resolved);
                    why.clear();
                  }
                } else {
                  const std::string scheme = lower(request.url.substr(0, request.url.find(':')));
                  if (request.url.find(':') == std::string::npos || (scheme != "http" && scheme != "https")) {
                    why = "unsupported URL: " + request.url + " (http:, https: and package assets play)";
                  }
                }
                if (!why.empty()) {
                  p->sink->onError(request.serial, media::MediaErrorKind::Media, 0, why);
                  p->player->unload();
                  return jsi::Value(static_cast<double>(request.serial));
                }
                p->player->load(std::move(request));
                return jsi::Value(static_cast<double>(p->serial));
              });

  setFunction(runtime, api, "play", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "play");
                if (playerFor(b, id)) b->registry->setWantsPlay(id, true);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "pause", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "pause");
                if (playerFor(b, id)) b->registry->setWantsPlay(id, false);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "seek", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "seek");
                const double position = numberArg(rt, args, count, 1, "seek");
                if (!std::isfinite(position)) throw jsi::JSError(rt, "__screenkit.media.seek: not a finite time");
                if (auto player = playerFor(weak.lock(), id)) player->seek(position);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setRate", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "setRate");
                const double rate = numberArg(rt, args, count, 1, "setRate");
                if (!std::isfinite(rate)) throw jsi::JSError(rt, "__screenkit.media.setRate: not a finite rate");
                if (auto player = playerFor(weak.lock(), id)) player->setRate(rate);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setVolume", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "setVolume");
                const double volume = numberArg(rt, args, count, 1, "setVolume");
                if (!(volume >= 0 && volume <= 1)) throw jsi::JSError(rt, "__screenkit.media.setVolume: 0 to 1");
                if (auto player = playerFor(weak.lock(), id)) player->setVolume(volume);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setMuted", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "setMuted");
                const bool muted = count > 1 && args[1].isBool() && args[1].getBool();
                if (auto player = playerFor(weak.lock(), id)) player->setMuted(muted);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setPlane", 7,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "setPlane");
                media::PlaneRect rect;
                rect.x = numberArg(rt, args, count, 1, "setPlane");
                rect.y = numberArg(rt, args, count, 2, "setPlane");
                rect.width = numberArg(rt, args, count, 3, "setPlane");
                rect.height = numberArg(rt, args, count, 4, "setPlane");
                bool visible = count > 5 && args[5].isBool() && args[5].getBool();
                rect.order = count > 6 && args[6].isNumber() && std::isfinite(args[6].getNumber())
                                 ? static_cast<int>(args[6].getNumber())
                                 : 0;
                if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
                    !std::isfinite(rect.height) || rect.width <= 0 || rect.height <= 0) {
                  visible = false;
                  rect = media::PlaneRect();
                }
                if (auto player = playerFor(weak.lock(), id)) player->setPlane(rect, visible);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "selectVariant", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "selectVariant");
                const double variant = numberArg(rt, args, count, 1, "selectVariant");
                // As seek and setRate: a non-finite number has no int to become.
                if (!std::isfinite(variant)) throw jsi::JSError(rt, "__screenkit.media.selectVariant: not a finite id");
                if (auto player = playerFor(weak.lock(), id)) player->selectVariant(static_cast<int>(variant));
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "setAbr", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "setAbr");
                media::AbrConfig abr;
                if (count > 1) abr = abrFrom(rt, args[1]);
                if (auto player = playerFor(weak.lock(), id)) player->setAbr(abr);
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "selectAudioLanguage", 3,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "selectAudioLanguage");
                std::string language = count > 1 && args[1].isString() ? args[1].getString(rt).utf8(rt) : "";
                std::string role = count > 2 && args[2].isString() ? args[2].getString(rt).utf8(rt) : "";
                if (auto player = playerFor(weak.lock(), id)) {
                  player->selectAudioLanguage(std::move(language), std::move(role));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "selectText", 2,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "selectText");
                const double track = numberArg(rt, args, count, 1, "selectText");
                if (auto player = playerFor(weak.lock(), id)) {
                  player->selectText(track < 0 || !std::isfinite(track) ? -1 : static_cast<int>(track));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "provideLicence", 3,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                const std::uint64_t id = idArg(rt, args, count, "provideLicence");
                const double request = numberArg(rt, args, count, 1, "provideLicence");
                if (!isCount(request)) throw jsi::JSError(rt, "__screenkit.media.provideLicence: bad request id");
                std::shared_ptr<const media::Bytes> licence;
                if (count > 2 && !args[2].isNull() && !args[2].isUndefined()) {
                  media::Bytes bytes;
                  if (!bytesFrom(rt, args[2], bytes)) {
                    throw jsi::JSError(rt, "__screenkit.media.provideLicence: the licence must be bytes or null");
                  }
                  licence = std::make_shared<const media::Bytes>(std::move(bytes));
                }
                if (auto player = playerFor(weak.lock(), id)) {
                  player->provideLicence(static_cast<std::uint64_t>(request), std::move(licence));
                }
                return jsi::Value::undefined();
              });

  setFunction(runtime, api, "unload", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "unload");
                if (!b || b->stopped) return jsi::Value(0);
                MediaBinding::Player* p = b->find(id);
                if (p == nullptr) return jsi::Value(0);
                // Nothing the unloaded item still has in flight reaches the page.
                p->serial = b->nextSerial++;
                b->registry->resetWantsPlay(id);
                p->player->pause();
                p->player->unload();
                return jsi::Value(static_cast<double>(p->serial));
              });

  setFunction(runtime, api, "destroy", 1,
              [weak](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                auto b = weak.lock();
                const std::uint64_t id = idArg(rt, args, count, "destroy");
                if (b && !b->stopped) b->destroy(id);
                return jsi::Value::undefined();
              });

  api.setProperty(runtime, "onevent", jsi::Value::null());

  jsi::Value io = runtime.global().getProperty(runtime, "__screenkit");
  if (!io.isObject()) {
    throw jsi::JSError(runtime, "installMedia: __screenkit is missing -- installHostIO runs first");
  }
  io.getObject(runtime).setProperty(runtime, "media", std::move(api));
  binding->api = std::make_unique<jsi::Object>(io.getObject(runtime).getPropertyAsObject(runtime, "media"));
  return binding;
}

void shutdownMedia(MediaBinding& binding) { binding.shutdown(); }

}  // namespace screenkit
