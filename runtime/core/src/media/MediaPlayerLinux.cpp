// Copyright (c) ScreenKit contributors. MIT.
//
// The Linux player: libvlc (the image's VLC 3.0.21) owns the manifest, adaptive
// streaming, demuxing, decoding, A/V sync and audio (ALSA); what is here is
// translation -- libvlc's events and counters into MediaSink events, the seam's
// calls into libvlc's -- plus presenting VLC's pictures on a Wayland subsurface
// beneath the app's window (linux/WaylandVideo.h), so video never passes
// through the runtime's GL.
//
//   one libvlc instance       shared by every player alive in the process -- a
//                             runtime's players, and so one per runtime -- made
//                             with the first and released with the last, after
//                             VLC_PLUGIN_PATH names the ScreenKit plugin
//                             (linux/vlc-plugin/: the V4L2 decoder, the "ts"
//                             demuxer adaptive streaming asks for, the manifest
//                             probe)
//   a media player per element  its video through libvlc's memory output (vmem)
//                             into dma-bufs on the subsurface; its audio to ALSA
//
// Threads. Every libvlc call and every piece of player state lives on the
// player's worker thread, which also ticks every 100 ms to read what libvlc
// only answers when asked (time, length, tracks, stats); a seam call posts
// there and returns. libvlc's events arrive on VLC's threads and are posted to
// the worker, stamped with the load they belong to. VLC's vout thread calls the
// vmem callbacks, which touch only the surface; the surface thread serves the
// surface's Wayland queue. Nothing here waits for the main thread.
//
// What libvlc 3 cannot say, and so what Linux cannot do (spec-video-player.md,
// the recorded divergences): it lists no variants and switches none on request
// -- the one variant listed is the one playing, and ABR's restrictions apply at
// load as the adaptive module's options; it hands out no cue text -- VLC draws
// the selected text track into the picture, and no cues are reported; it has no
// content protection -- every key system is 6001; and it says nothing of what it
// has buffered, so the buffered range is an estimate.
//
// What it says only in its log is read there (VlcLog): the HTTP status behind a
// failed load, and what the plugin's manifest probe found -- the ladder, live or
// not, protection.
#include "MediaPlayer.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL_video.h>

#include <vlc/vlc.h>

#include <screenkit/Log.h>

#include "linux/V4l2Probe.h"
#include "linux/WaylandVideo.h"

namespace screenkit::media {
namespace {

constexpr const char* kTag = "screenkit.media";

/// SCREENKIT_MEDIA_TRACE=1: one line per player transition -- VLC's events, and
/// every pause, play, seek and reopen this player asks VLC for -- with the time
/// since the load. Cheap enough to leave on while chasing a timing problem,
/// unlike SCREENKIT_VLC_LOG, whose volume changes the timing it would show.
bool traceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("SCREENKIT_MEDIA_TRACE");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point then) {
  return std::chrono::duration<double>(Clock::now() - then).count();
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The path of `url` without query or fragment, lower-cased: what its extension is read from.
std::string urlPath(const std::string& url) {
  std::string path = url.substr(0, url.find_first_of("?#"));
  return lower(path);
}

// ---- what VLC says only in its log ------------------------------------------------------

/// What the plugin's manifest probe logged for one URL.
struct ManifestInfo {
  std::string type;  // "hls" or "dash"
  bool live = false;
  double seconds = 0;  // VOD: the length; live: the window
  bool protectedContent = false;
  bool complete = false;  // the summary line arrived (after every variant line)
  std::vector<VariantTrack> ladder;  // the video rungs, in manifest order
  double audioBandwidth = 0;         // DASH: the first audio representation's
};

/// The value of `key=` in a "k=v k=v ... url=..." line; `last` takes the rest of the line.
std::string field(const std::string& line, const std::string& key, bool last = false) {
  const std::string needle = key + "=";
  size_t at = 0;
  while ((at = line.find(needle, at)) != std::string::npos) {
    if (at == 0 || line[at - 1] == ' ') break;
    at += needle.size();
  }
  if (at == std::string::npos) return std::string();
  const size_t start = at + needle.size();
  const size_t end = last ? line.size() : line.find(' ', start);
  return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// libvlc's log, for every instance: parsed for what the players need, and the
/// rest dropped -- except the decoder choices and errors, which go to the
/// runtime's log, and everything with SCREENKIT_VLC_LOG=1.
class VlcLog {
 public:
  static VlcLog& get() {
    static VlcLog* log = new VlcLog();  // never destroyed: VLC's threads may log at exit
    return *log;
  }

  static void callback(void* data, int level, const libvlc_log_t* ctx, const char* fmt, va_list args) {
    static_cast<VlcLog*>(data)->handle(level, ctx, fmt, args);
  }

  /// The status of the last failed fetch of `url`: >0 an HTTP status, 0 a
  /// connection that failed. False when nothing failed.
  bool httpFailure(const std::string& url, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = failures_.find(url);
    if (it == failures_.end()) return false;
    status = it->second;
    return true;
  }

  bool manifest(const std::string& url, ManifestInfo& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = manifests_.find(url);
    if (it == manifests_.end()) return false;
    out = it->second;
    return true;
  }

  /// A new load of `url`: what an earlier one left is forgotten.
  void forget(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    failures_.erase(url);
    manifests_.erase(url);
  }

 private:
  VlcLog() {
    const char* all = std::getenv("SCREENKIT_VLC_LOG");
    verbose_ = all != nullptr && *all != '\0' && std::strcmp(all, "0") != 0;
  }

  static std::string format(const char* fmt, va_list args) {
    char buffer[2048];
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);
    if (n < 0) return std::string();
    return std::string(buffer, std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1));
  }

  void handle(int level, const libvlc_log_t* ctx, const char* fmt, va_list args) {
    const char* module = nullptr;
    const char* file = nullptr;
    unsigned line = 0;
    libvlc_log_get_context(ctx, &module, &file, &line);
    const char* name = nullptr;
    const char* header = nullptr;
    uintptr_t id = 0;
    libvlc_log_get_object(ctx, &name, &header, &id);

    if (std::strncmp(fmt, "creating access: ", 17) == 0) {
      const std::string text = format(fmt, args);
      std::lock_guard<std::mutex> lock(mutex_);
      if (accessUrls_.size() > 256) accessUrls_.clear();
      accessUrls_[id] = text.substr(17);
    } else if (std::strcmp(fmt, "HTTP %d error") == 0 || std::strcmp(fmt, "HTTP connection failure") == 0 ||
               std::strncmp(fmt, "cannot connect to ", 18) == 0 || std::strncmp(fmt, "connection failed: ", 19) == 0 ||
               std::strncmp(fmt, "cannot resolve ", 15) == 0) {
      int status = 0;
      if (std::strcmp(fmt, "HTTP %d error") == 0) {
        va_list copy;
        va_copy(copy, args);
        status = va_arg(copy, int);
        va_end(copy);
      }
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = accessUrls_.find(id);
      if (it != accessUrls_.end()) {
        // An HTTP status is the better answer: the old http module's own
        // complaint about the same response must not replace it.
        auto failure = failures_.find(it->second);
        if (failure == failures_.end() || failure->second == 0) failures_[it->second] = status;
        if (failures_.size() > 256) failures_.clear();
      }
    } else if (std::strncmp(fmt, "ScreenKit manifest: ", 20) == 0 || std::strncmp(fmt, "ScreenKit variant: ", 19) == 0) {
      noteManifest(format(fmt, args));
    } else if (std::strncmp(fmt, "ScreenKit decoder: ", 19) == 0 ||
               (std::strncmp(fmt, "using %s module", 15) == 0 && level >= LIBVLC_DEBUG)) {
      const std::string text = format(fmt, args);
      if (text.rfind("ScreenKit", 0) == 0 || text.find("video decoder") != std::string::npos) {
        log(LogLevel::Log, kTag, "vlc: " + text);
      }
      return;
    }
    if (verbose_ || level == LIBVLC_ERROR) {
      const std::string text = format(fmt, args);
      if (level == LIBVLC_ERROR && !verbose_) {
        // At most a few a second: a broken stream can say the same thing per frame.
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (now - errorWindow_ > std::chrono::seconds(1)) {
          errorWindow_ = now;
          errorsInWindow_ = 0;
        }
        if (++errorsInWindow_ > 5) return;
      }
      log(level == LIBVLC_ERROR ? LogLevel::Warn : LogLevel::Log, kTag,
          std::string("vlc ") + (module != nullptr ? module : "?") + ": " + text);
    }
  }

  void noteManifest(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (text.rfind("ScreenKit variant: ", 0) == 0) {
      const std::string url = field(text, "manifest");
      ManifestInfo& info = manifests_[url];
      if (info.complete) info = ManifestInfo();  // probed again: start over
      if (field(text, "kind") == "audio") {
        if (info.audioBandwidth <= 0) info.audioBandwidth = std::atof(field(text, "bandwidth").c_str());
        return;
      }
      VariantTrack v;
      v.id = static_cast<int>(info.ladder.size());
      v.bandwidth = std::atof(field(text, "bandwidth").c_str());
      v.width = std::atoi(field(text, "width").c_str());
      v.height = std::atoi(field(text, "height").c_str());
      v.frameRate = std::atof(field(text, "fps").c_str());
      const std::string codecs = field(text, "codecs");
      if (codecs != "-") {
        size_t start = 0;
        while (start <= codecs.size()) {
          const size_t comma = codecs.find(',', start);
          const std::string codec = codecs.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          const std::string prefix = lower(codec.substr(0, 4));
          if (prefix == "mp4a" || prefix == "ac-3" || prefix == "ec-3" || prefix == "opus" || prefix == "flac") {
            v.audioCodec = codec;
          } else if (!codec.empty()) {
            v.videoCodec = codec;
          }
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
      }
      info.ladder.push_back(v);
      return;
    }
    const std::string url = field(text, "url", true);
    ManifestInfo& info = manifests_[url];
    if (info.complete) {
      // A second probe of the same stream (another filter took it and the
      // probe ran again on top): its variant lines came in again after this.
      info.ladder.clear();
      info.audioBandwidth = 0;
    }
    info.type = field(text, "type");
    info.live = field(text, "live") == "1";
    info.seconds = std::atof(field(text, "seconds").c_str());
    info.protectedContent = field(text, "protected") == "1";
    info.complete = true;
    if (manifests_.size() > 64) {
      ManifestInfo keep = info;
      manifests_.clear();
      manifests_[url] = keep;
    }
  }

  std::mutex mutex_;
  bool verbose_ = false;
  std::unordered_map<uintptr_t, std::string> accessUrls_;
  std::unordered_map<std::string, int> failures_;
  std::map<std::string, ManifestInfo> manifests_;
  Clock::time_point errorWindow_{};
  int errorsInWindow_ = 0;
};

// ---- the instance ---------------------------------------------------------------------

/// Where the ScreenKit plugin is: $SCREENKIT_VLC_PLUGINS, else vlc-plugins/
/// beside the executable or in the lib/ next to its bin/.
std::string pluginDirectory() {
  auto has = [](const std::string& dir) {
    struct stat st;
    return stat((dir + "/libscreenkit_plugin.so").c_str(), &st) == 0;
  };
  const char* configured = std::getenv("SCREENKIT_VLC_PLUGINS");
  if (configured != nullptr && *configured != '\0') return has(configured) ? configured : std::string();
  char exe[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return std::string();
  exe[n] = '\0';
  std::string dir(exe);
  dir = dir.substr(0, dir.rfind('/'));
  for (const std::string& candidate : {dir + "/vlc-plugins", dir + "/../lib/vlc-plugins"}) {
    if (has(candidate)) return candidate;
  }
  return std::string();
}

struct Instance {
  libvlc_instance_t* vlc = nullptr;
  ~Instance() {
    if (vlc != nullptr) {
      libvlc_log_unset(vlc);
      libvlc_release(vlc);
    }
  }
};

std::mutex& instanceMutex() {
  static std::mutex m;
  return m;
}

std::shared_ptr<Instance> acquireInstance(std::string& error) {
  std::lock_guard<std::mutex> lock(instanceMutex());
  static std::weak_ptr<Instance> current;
  static bool pathSet = false;
  if (auto live = current.lock()) return live;
  if (!pathSet) {
    pathSet = true;
    const std::string dir = pluginDirectory();
    if (dir.empty()) {
      log(LogLevel::Warn, kTag,
          "no ScreenKit VLC plugin (libscreenkit_plugin.so in vlc-plugins/ beside the executable, or "
          "$SCREENKIT_VLC_PLUGINS): video decodes in software, and HLS with TS segments does not play");
    } else {
      // Read by libvlccore when it loads its plugins, which it does in libvlc_new.
      const char* existing = std::getenv("VLC_PLUGIN_PATH");
      std::string path = dir;
      if (existing != nullptr && *existing != '\0') path = std::string(existing) + ":" + dir;
      setenv("VLC_PLUGIN_PATH", path.c_str(), 1);
    }
  }
  const char* args[] = {
      "--no-video-title-show",   // no file name drawn over the first frames
      "--no-snapshot-preview",
      "--no-osd",
      "--no-sub-autodetect-file",  // nothing beside the stream is looked for
      "--no-lua",                  // no playlist scripts probing every URL
      "--no-xlib",
      "--aout=alsa,any",
  };
  auto instance = std::make_shared<Instance>();
  instance->vlc = libvlc_new(static_cast<int>(sizeof(args) / sizeof(args[0])), args);
  if (instance->vlc == nullptr) {
    const char* why = libvlc_errmsg();
    error = std::string("libvlc_new failed") + (why != nullptr ? std::string(": ") + why : std::string());
    return nullptr;
  }
  libvlc_log_set(instance->vlc, &VlcLog::callback, &VlcLog::get());
  log(LogLevel::Log, kTag, std::string("libvlc ") + libvlc_get_version());
  current = instance;
  return instance;
}

// ---- the ABR ceiling --------------------------------------------------------------------

/// The largest picture this board should be asked to show: the smaller of the
/// display mode and what the decoder VLC will use for H.264 -- the ScreenKit
/// plugin's V4L2 decoder where there is one -- can decode. 0 for no limit.
struct Ceiling {
  int width = 0;
  int height = 0;
  std::string why;
};

Ceiling boardCeiling() {
  Ceiling ceiling;
  // The ladder is capped before the stream's codec is known, so the cap is the
  // largest picture *any* decoder on this board takes: capping an HEVC ladder by
  // what the H.264 decoder manages would refuse rungs the board can play, and a
  // rung the chosen decoder cannot in fact sustain is what the drop-rate
  // step-down is for (checkDrops).
  struct Widest {
    unsigned width = 0;
    unsigned height = 0;
    std::string driver;
  };
  static const Widest widest = [] {
    Widest best;
    for (int codec = 0; codec < SK_V4L2_CODEC_COUNT; ++codec) {
      sk_v4l2_decoder found;
      if (sk_v4l2_find(static_cast<sk_v4l2_codec>(codec), 1, &found) != 0) continue;
      if (found.max_width == 0 || found.max_height == 0) continue;
      if (static_cast<std::uint64_t>(found.max_width) * found.max_height <=
          static_cast<std::uint64_t>(best.width) * best.height) {
        continue;
      }
      best.width = found.max_width;
      best.height = found.max_height;
      best.driver = found.driver;
    }
    return best;
  }();
  std::string parts;
  if (widest.width > 0 && widest.height > 0) {
    ceiling.width = static_cast<int>(widest.width);
    ceiling.height = static_cast<int>(widest.height);
    parts = widest.driver + " decodes up to " + std::to_string(widest.width) + "x" + std::to_string(widest.height);
  }
  // The display's mode, read on the main thread when the window was made
  // (host/Host.cpp): SDL's display calls belong there, and this runs on the
  // player's own thread.
  const VideoHost host = videoHost();
  if (host.displayWidth > 0 && host.displayHeight > 0) {
    // A display in portrait still shows a landscape ladder at its longer side.
    const int w = std::max(host.displayWidth, host.displayHeight);
    const int h = std::min(host.displayWidth, host.displayHeight);
    ceiling.width = ceiling.width > 0 ? std::min(ceiling.width, w) : w;
    ceiling.height = ceiling.height > 0 ? std::min(ceiling.height, h) : h;
    parts += (parts.empty() ? "" : ", ") + std::string("the display is ") + std::to_string(host.displayWidth) + "x" +
             std::to_string(host.displayHeight);
  }
  ceiling.why = parts;
  return ceiling;
}

// ---- small translations -------------------------------------------------------------------

std::string fourcc(uint32_t codec) {
  char text[5];
  std::memcpy(text, &codec, 4);
  text[4] = '\0';
  return text;
}

/// An RFC 6381 prefix for VLC's codec fourcc: what Shaka's track fields hold.
std::string codecName(uint32_t codec) {
  const std::string f = fourcc(codec);
  if (f == "h264") return "avc1";
  if (f == "hevc") return "hvc1";
  if (f == "VP80") return "vp8";
  if (f == "VP90") return "vp09";
  if (f == "av01") return "av01";
  if (f == "mp4a") return "mp4a";
  if (f == "mpga" || f == "mp3 ") return "mp3";
  if (f == "a52 ") return "ac-3";
  if (f == "eac3") return "ec-3";
  if (f == "opus") return "opus";
  if (f == "flac") return "flac";
  if (f == "wvtt" || f == "webv") return "wvtt";
  if (f == "ttml") return "stpp";
  size_t end = f.find_last_not_of(' ');
  return end == std::string::npos ? f : f.substr(0, end + 1);
}

std::string textMime(uint32_t codec) {
  const std::string f = fourcc(codec);
  if (f == "wvtt" || f == "webv") return "text/vtt";
  if (f == "ttml") return "application/ttml+xml";
  if (f == "subt") return "text/plain";
  if (f == "tx3g") return "application/mp4";
  return "text/vtt";
}

// ---- the player -------------------------------------------------------------------------

/// One element's player. libvlc's callbacks hold it as a raw pointer: it
/// outlives its media player, which the worker releases before it exits.
class LinuxPlayer final : public MediaPlayer {
 public:
  explicit LinuxPlayer(MediaConfig config) : sink_(std::move(config.sink)) {
    worker_ = std::thread([this] { run(); });
  }
  ~LinuxPlayer() override { shutdown(); }

  void load(LoadRequest request) override {
    post([this, request = std::move(request)]() mutable { doLoad(std::move(request)); });
  }
  void play() override {
    post([this] {
      wantsPlay_ = true;
      if (ended_ && media_ != nullptr) {
        reopen(0, true);  // play at the end starts again from the start
        return;
      }
      applyPlay();
    });
  }
  void pause() override {
    post([this] {
      wantsPlay_ = false;
      applyPlay();
    });
  }
  void seek(double position) override { post([this, position] { doSeek(position); }); }
  void setRate(double rate) override {
    post([this, rate] {
      // A rate of 0 freezes the picture and leaves the element unpaused, as
      // HTMLMediaElement says and AVPlayer does; VLC has no rate 0, so the
      // freeze is a pause it does not know the page asked for -- `wantsPlay_`
      // stays as it was, and the ordinary rate resumes.
      rateIsZero_ = rate == 0;
      rate_ = rate > 0 ? rate : rate_;
      if (mp_ == nullptr) return;
      if (!rateIsZero_) libvlc_media_player_set_rate(mp_, static_cast<float>(rate_));
      applyPlay();
    });
  }
  void setVolume(double volume) override {
    post([this, volume] {
      volume_ = std::clamp(volume, 0.0, 1.0);
      applyVolume();
    });
  }
  void setMuted(bool muted) override {
    post([this, muted] {
      muted_ = muted;
      applyVolume();
    });
  }
  void setPlane(PlaneRect rect, bool visible) override {
    std::lock_guard<std::mutex> lock(planeMutex_);
    plane_ = rect;
    planeVisible_ = visible;
    if (surface_) surface_->setPlane(rect, visible);
  }
  void selectVariant(int) override {
    // libvlc cannot switch variants on request: the one playing stays, and is
    // reported again so a page waiting for `variantchanged` hears the answer.
    post([this] {
      if (!metadataSent_) return;
      const std::uint32_t serial = serial_;
      const int id = activeVariant_;
      emit([serial, id](MediaSink& sink) { sink.onVariantChanged(serial, id); });
    });
  }
  void setAbr(AbrConfig abr) override {
    // Applied at the next load: VLC's adaptive options are the media's.
    post([this, abr] { abr_ = abr; });
  }
  void selectAudioLanguage(std::string language, std::string role) override {
    post([this, language = std::move(language), role = std::move(role)] { doSelectAudio(language, role); });
  }
  void selectText(int textTrackId) override { post([this, textTrackId] { doSelectText(textTrackId); }); }
  void provideLicence(std::uint64_t, std::shared_ptr<const Bytes>) override {}  // never asks for one
  void unload() override { post([this] { stopMedia(); }); }

  void shutdown() override {
    if (stopped_.exchange(true)) return;
    {
      std::lock_guard<std::mutex> lock(sinkMutex_);
      sink_.reset();
    }
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      jobs_.push_back([this] { teardown(); });
      exiting_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) {
      if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();  // not reachable: the sink never calls back synchronously
      } else {
        worker_.join();
      }
    }
  }

  // ---- libvlc's callbacks, on VLC's threads ----------------------------------------------

  static void onEvent(const libvlc_event_t* event, void* data) {
    auto* self = static_cast<LinuxPlayer*>(data);
    const std::uint64_t epoch = self->epoch_.load();
    const std::uint64_t seekGeneration = self->seekGeneration_.load();
    const int type = event->type;
    double value = 0;
    int esType = -1;
    if (type == libvlc_MediaPlayerBuffering) value = event->u.media_player_buffering.new_cache;
    if (type == libvlc_MediaPlayerESAdded || type == libvlc_MediaPlayerESDeleted ||
        type == libvlc_MediaPlayerESSelected) {
      esType = event->u.media_player_es_changed.i_type;
    }
    self->enqueue([self, epoch, seekGeneration, type, value, esType] {
      self->onVlcEvent(epoch, seekGeneration, type, value, esType);
    });
  }

  static unsigned vmemSetup(void** opaque, char* chroma, unsigned* width, unsigned* height, unsigned* pitches,
                            unsigned* lines) {
    auto* self = static_cast<LinuxPlayer*>(*opaque);
    wl::VideoSurface* surface = self->surfaceRaw_.load();
    if (surface == nullptr) return 0;
    const bool nv12 = std::strncmp(chroma, "NV12", 4) == 0;
    unsigned p[3] = {0, 0, 0};
    unsigned l[3] = {0, 0, 0};
    if (!surface->configure(nv12, *width, *height, p, l)) return 0;
    for (int i = 0; i < 3; ++i) {
      pitches[i] = p[i];
      lines[i] = l[i];
    }
    // What the buffers hold: I420 unless the compositor has only NV12.
    std::memcpy(chroma, p[2] == 0 ? "NV12" : "I420", 4);
    // The same layout for the scratch buffer vmemLock falls back to.
    std::size_t total = 0;
    for (int i = 0; i < 3; ++i) {
      self->sinkOffsets_[i].store(static_cast<std::uint32_t>(total), std::memory_order_relaxed);
      total += static_cast<std::size_t>(p[i]) * l[i];
    }
    self->sinkBytes_.store(total, std::memory_order_relaxed);
    const int w = static_cast<int>(*width);
    const int h = static_cast<int>(*height);
    const std::uint64_t epoch = self->epoch_.load();
    self->enqueue([self, epoch, w, h] {
      if (epoch == self->epoch_.load()) self->onDecodedSize(w, h);
    });
    return 3;
  }
  static void vmemCleanup(void* opaque) {
    auto* self = static_cast<LinuxPlayer*>(opaque);
    if (wl::VideoSurface* surface = self->surfaceRaw_.load()) surface->unconfigure();
  }
  static void* vmemLock(void* opaque, void** planes) {
    auto* self = static_cast<LinuxPlayer*>(opaque);
    wl::VideoSurface* surface = self->surfaceRaw_.load();
    void* picture = surface != nullptr ? surface->lock(planes) : nullptr;
    if (picture == nullptr) {
      // Nowhere to write: VLC still copies, into a buffer no one shows. Sized
      // from the layout VLC was handed in vmemSetup, with a plane each -- a
      // fixed size would be a write past the end for a large enough picture, and
      // one base for all three planes lets them write over each other.
      static thread_local std::vector<std::uint8_t> sink;
      // A page's worth even before a layout is known: VLC asks for one first,
      // but a null plane pointer would be worse than a wasted page.
      const std::size_t need = std::max<std::size_t>(self->sinkBytes_.load(std::memory_order_relaxed), 4096);
      if (sink.size() < need) sink.resize(need);
      for (int i = 0; i < 3; ++i) {
        planes[i] = sink.data() + self->sinkOffsets_[i].load(std::memory_order_relaxed);
      }
    }
    return picture;
  }
  static void vmemUnlock(void* opaque, void* picture, void* const*) {
    auto* self = static_cast<LinuxPlayer*>(opaque);
    if (wl::VideoSurface* surface = self->surfaceRaw_.load()) surface->unlock(picture);
  }
  static void vmemDisplay(void* opaque, void* picture) {
    auto* self = static_cast<LinuxPlayer*>(opaque);
    if (wl::VideoSurface* surface = self->surfaceRaw_.load()) surface->display(picture);
    if (self->awaitingFirstPicture_.exchange(false)) {
      const std::uint64_t epoch = self->epoch_.load();
      self->enqueue([self, epoch] {
        if (epoch == self->epoch_.load()) self->holdPreroll();
      });
    }
  }

 private:
  // ---- the worker ---------------------------------------------------------------------------

  /// A seam call's work: dropped once shutdown has begun.
  void post(std::function<void()> job) {
    if (stopped_.load()) return;
    enqueue(std::move(job));
  }

  /// libvlc's events, which still arrive while the teardown stops it: run after
  /// it, and find nothing loaded.
  void enqueue(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (closed_) return;
      jobs_.push_back(std::move(job));
    }
    queueCv_.notify_one();
  }

  void run() {
    auto nextTick = Clock::now();
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_until(lock, nextTick, [&] { return !jobs_.empty() || exiting_; });
        if (!jobs_.empty()) {
          job = std::move(jobs_.front());
          jobs_.pop_front();
        } else if (exiting_) {
          closed_ = true;
          break;
        }
      }
      // A job, then the tick if it is due: a steady stream of libvlc events must
      // not starve the poll that reports time and stats.
      if (job) job();
      if (Clock::now() >= nextTick) {
        tick();
        nextTick = Clock::now() + std::chrono::milliseconds(100);
      }
    }
  }

  template <typename Deliver>
  void emit(Deliver&& deliver) {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_) deliver(*sink_);
  }

  void teardown() {
    stopMedia();
    if (mp_ != nullptr) {
      libvlc_media_player_release(mp_);
      mp_ = nullptr;
    }
    stopSurface();
    instance_.reset();
  }

  // ---- the surface ---------------------------------------------------------------------------

  bool ensureSurface(std::string& error) {
    if (surface_) return true;
    auto surface = wl::VideoSurface::create(error);
    if (!surface) {
      if (error.find("Wayland") == std::string::npos) error = "the Wayland video plane: " + error;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(planeMutex_);
      surface_ = std::move(surface);
      surface_->setPlane(plane_, planeVisible_);
    }
    surfaceRaw_.store(surface_.get());
    surfaceStop_.store(false);
    surfaceThread_ = std::thread([this] {
      while (!surfaceStop_.load()) surfaceRaw_.load()->dispatch(50);
    });
    return true;
  }

  void stopSurface() {
    if (!surface_) return;
    surfaceStop_.store(true);
    if (surfaceThread_.joinable()) surfaceThread_.join();
    surfaceRaw_.store(nullptr);
    std::lock_guard<std::mutex> lock(planeMutex_);
    surface_.reset();
  }

  bool ensurePlayer(std::string& error) {
    if (mp_ != nullptr) return true;
    if (!instance_) instance_ = acquireInstance(error);
    if (!instance_) return false;
    mp_ = libvlc_media_player_new(instance_->vlc);
    if (mp_ == nullptr) {
      const char* why = libvlc_errmsg();
      error = std::string("libvlc_media_player_new failed") + (why != nullptr ? std::string(": ") + why : "");
      return false;
    }
    libvlc_video_set_callbacks(mp_, &vmemLock, &vmemUnlock, &vmemDisplay, this);
    libvlc_video_set_format_callbacks(mp_, &vmemSetup, &vmemCleanup);
    libvlc_event_manager_t* events = libvlc_media_player_event_manager(mp_);
    for (libvlc_event_type_t type :
         {libvlc_MediaPlayerBuffering, libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused,
          libvlc_MediaPlayerEndReached, libvlc_MediaPlayerEncounteredError, libvlc_MediaPlayerLengthChanged,
          libvlc_MediaPlayerESAdded, libvlc_MediaPlayerESDeleted, libvlc_MediaPlayerESSelected}) {
      libvlc_event_attach(events, type, &LinuxPlayer::onEvent, this);
    }
    return true;
  }

  // ---- loading ----------------------------------------------------------------------------------

  void resetLoad() {
    loaded_ = false;
    prerolled_ = false;
    metadataSent_ = false;
    ended_ = false;
    stalled_ = false;
    playing_ = false;
    seeking_ = false;
    reopening_ = false;
    reportSeekOnReopen_ = false;
    pendingSeek_ = kNaN;
    decodedWidth_ = decodedHeight_ = 0;
    reportedWidth_ = reportedHeight_ = 0;
    activeVariant_ = 0;
    tracksDirty_ = true;
    tracks_ = TrackList();
    tracksKey_.clear();
    textSelected_ = -1;
    info_ = MediaInfo();
    manifest_ = ManifestInfo();
    stateSent_ = false;
    lastTimeSent_ = kNaN;
    lastRaw_ = -1;
    base_ = 0;
    baseAt_ = Clock::now();
    continuous_ = false;
    samples_.clear();
    lastStats_ = Clock::time_point();
    buffered_.clear();
    bufferedFrom_ = 0;
    stepHeight_ = 0;
    decodedBefore_ = droppedBefore_ = lastDecoded_ = lastLost_ = 0;
    surfaceDroppedAtLoad_ = surfaceRaw_.load() != nullptr ? surfaceRaw_.load()->dropped() : 0;
  }

  /// Stop whatever is loaded; its last picture leaves the plane.
  void stopMedia() {
    epoch_.fetch_add(1);
    if (mp_ != nullptr && media_ != nullptr) libvlc_media_player_stop(mp_);
    // Again once the stop has returned: VLC's video thread can still hand over a
    // picture while it stops, and the job that picture posts carries whatever
    // epoch it read -- the one above. Without this second step that job looks
    // current, and on a teardown nothing bumps the epoch afterwards, so it ran
    // against a released player (`media-interrupted`, a segfault in
    // libvlc_media_player_can_pause).
    epoch_.fetch_add(1);
    if (media_ != nullptr) {
      libvlc_media_release(media_);
      media_ = nullptr;
    }
    if (wl::VideoSurface* surface = surfaceRaw_.load()) surface->clear();
    resetLoad();
  }

  void fail(MediaErrorKind kind, int httpStatus, const std::string& message) {
    log(LogLevel::Warn, kTag, std::string("load failed (") + mediaErrorKindName(kind) + "): " + message);
    const std::uint32_t serial = serial_;
    stopMedia();
    emit([serial, kind, httpStatus, message](MediaSink& sink) { sink.onError(serial, kind, httpStatus, message); });
  }

  std::string manifestGuess() const {
    const std::string path = urlPath(request_.url);
    const std::string mime = lower(request_.mimeType);
    if (endsWith(path, ".m3u8") || mime.find("mpegurl") != std::string::npos) return "hls";
    if (endsWith(path, ".mpd") || mime.find("dash") != std::string::npos) return "dash";
    return "progressive";
  }

  void doLoad(LoadRequest request) {
    stopMedia();
    request_ = std::move(request);
    serial_ = request_.serial;
    wantsPlay_ = false;
    rate_ = 1.0;
    rateIsZero_ = false;
    ceiling_ = Ceiling();
    const std::uint32_t serial = serial_;

    // No key system on Linux (spec-video-player.md): any configured one is
    // Shaka's REQUESTED_KEY_SYSTEM_CONFIG_UNAVAILABLE, before anything is fetched.
    if (!request_.drm.keySystem.empty()) {
      fail(MediaErrorKind::KeySystem, 0, "Linux has no key system: " + request_.drm.keySystem + " is unavailable");
      return;
    }
    std::string error;
    if (!ensureSurface(error)) {
      fail(MediaErrorKind::VideoOutput, 0, error);
      return;
    }
    if (!ensurePlayer(error)) {
      fail(MediaErrorKind::Unavailable, 0, error);
      return;
    }
    emit([serial](MediaSink& sink) { sink.onState(serial, PlaybackState::Loading); });
    loaded_ = true;
    ceiling_ = boardCeiling();
    VlcLog::get().forget(request_.url);
    if (!openMedia(std::isfinite(request_.startTime) ? request_.startTime : kNaN)) return;
    // The start position reads back at once; the first report comes from VLC.
    if (std::isfinite(request_.startTime)) base_ = request_.startTime;
  }

  /// Open `request_` at `start` (NaN: the default), paused until prerolled.
  bool openMedia(double start) {
    media_ = libvlc_media_new_location(instance_->vlc, request_.url.c_str());
    if (media_ == nullptr) {
      fail(MediaErrorKind::Media, 0, "libvlc cannot open " + request_.url);
      return false;
    }
    // Not :start-paused. A VLC input opened paused decodes nothing -- its
    // decoders wait while it is paused, and its preroll then stops waiting for
    // a first picture -- so playback would begin with a cold decoder and a video
    // output still being made, and VLC would drop the first second's frames to
    // catch up (27 of 1841 over a minute of 1080p30 on a Pi 3). Opened playing,
    // VLC prerolls properly: it buffers, the decoder hands over its first
    // picture, the output shows it (the frame a paused video shows), and the
    // player holds it there, muted until then, unless the page is playing
    // (`holdAtPreroll_`).
    std::vector<std::string> options = {":no-sub-autodetect-file"};
    // The ceiling and the restrictions, as the adaptive module's options: it
    // never picks a representation larger than these.
    int maxWidth = ceiling_.width;
    int maxHeight = ceiling_.height;
    if (abr_.maxWidth > 0 && abr_.maxWidth < std::numeric_limits<int>::max()) {
      maxWidth = maxWidth > 0 ? std::min(maxWidth, abr_.maxWidth) : abr_.maxWidth;
    }
    if (abr_.maxHeight > 0 && abr_.maxHeight < std::numeric_limits<int>::max()) {
      maxHeight = maxHeight > 0 ? std::min(maxHeight, abr_.maxHeight) : abr_.maxHeight;
    }
    if (stepHeight_ > 0) maxHeight = maxHeight > 0 ? std::min(maxHeight, stepHeight_) : stepHeight_;
    if (maxWidth > 0) options.push_back(":adaptive-maxwidth=" + std::to_string(maxWidth));
    if (maxHeight > 0) options.push_back(":adaptive-maxheight=" + std::to_string(maxHeight));
    // Never VLC's own switching. A representation switch mid-stream hands the
    // next segment to a fresh demuxer (the image has no "ts" module, and the
    // shim's avformat is what reads it), and the timestamps it starts from put
    // VLC's clock out: measured on the Pi with a two-rung HLS ladder, the default
    // logic dropped 39 audio blocks as too late and ended the 10 s clip early,
    // while `highest` played it through with none -- and inside the runtime the
    // same switch ended the clip within a second of `play()` (media-element,
    // media-hls). So one representation per open: the best under the ceiling
    // and restrictions, and the ceiling's step-down (a reopen) is the
    // adaptation. Linux lists only the playing variant for the same reason.
    if (std::isfinite(abr_.maxBandwidth) && abr_.maxBandwidth > 0) {
      // A fixed rate in KiB/s: the best representation under it, and no switching.
      options.push_back(":adaptive-logic=fixedrate");
      options.push_back(":adaptive-bw=" + std::to_string(static_cast<long long>(abr_.maxBandwidth / 8192)));
    } else {
      options.push_back(":adaptive-logic=highest");
    }
    if (std::isfinite(start) && start > 0) {
      char text[64];
      std::snprintf(text, sizeof(text), ":start-time=%.3f", start);
      options.push_back(text);
    }
    if (!request_.audioLanguage.empty()) options.push_back(":audio-language=" + request_.audioLanguage);
    for (const std::string& option : options) libvlc_media_add_option(media_, option.c_str());
    std::string summary;
    for (const std::string& option : options) summary += " " + option;
    log(LogLevel::Log, kTag,
        "load " + request_.url + summary + (ceiling_.why.empty() ? "" : " (ceiling: " + ceiling_.why + ")"));
    epoch_.fetch_add(1);
    holdAtPreroll_ = !wantsPlay_;
    awaitingFirstPicture_.store(true);
    openedAt_ = Clock::now();
    openReadBytes_ = -1;
    libvlc_media_player_set_media(mp_, media_);
    applyVolume();
    libvlc_media_player_set_rate(mp_, static_cast<float>(rate_));
    if (libvlc_media_player_play(mp_) != 0) {
      fail(MediaErrorKind::Media, 0, "libvlc could not start " + request_.url);
      return false;
    }
    return true;
  }

  /// Open the same stream again at `position` -- after its end, or with a lower
  /// ceiling -- without a new load as far as the page is concerned.
  void reopen(double position, bool reportSeek) {
    if (media_ == nullptr || mp_ == nullptr) return;
    trace("reopen at " + std::to_string(position));
    // VLC's counters start again with the new input: keep what they had.
    libvlc_media_stats_t st;
    std::memset(&st, 0, sizeof(st));
    if (libvlc_media_get_stats(media_, &st)) {
      decodedBefore_ += decodedPictures(st);
      droppedBefore_ += static_cast<std::uint64_t>(std::max(0, st.i_lost_pictures));
    }
    lastDecoded_ = lastLost_ = 0;
    epoch_.fetch_add(1);
    libvlc_media_player_stop(mp_);
    epoch_.fetch_add(1);  // as in stopMedia: pictures handed over during the stop
    libvlc_media_release(media_);
    media_ = nullptr;
    prerolled_ = false;
    ended_ = false;
    stalled_ = false;
    playing_ = false;
    seeking_ = false;
    reopening_ = true;
    reportSeekOnReopen_ = reportSeek;
    reopenTarget_ = position;
    base_ = position;
    lastRaw_ = -1;
    continuous_ = false;
    bufferedFrom_ = position;
    samples_.clear();
    const std::uint32_t serial = serial_;
    if (reportSeek) {
      const PlaybackState state = PlaybackState::Buffering;
      state_ = state;
      stateSent_ = true;
      emit([serial](MediaSink& sink) { sink.onState(serial, PlaybackState::Buffering); });
    }
    openMedia(position);
  }

  // ---- libvlc's events, on the worker ------------------------------------------------------------

  void onVlcEvent(std::uint64_t epoch, std::uint64_t seekGeneration, int type, double value, int esType) {
    if (epoch != epoch_.load() || !loaded_ || media_ == nullptr) return;
    switch (type) {
      case libvlc_MediaPlayerBuffering:
        if (value < 100.0) {
          if (seeking_ && seekGeneration == seekGeneration_.load()) seekSawBuffering_ = true;
          if (prerolled_ && !seeking_ && !reopening_) {
            if (!stalled_) trace("stalled: vlc Buffering " + std::to_string(value));
            stalled_ = true;
          }
        } else {
          if (stalled_) trace("unstalled: vlc Buffering 100");
          stalled_ = false;
          if (!prerolled_) {
            prerolled_ = true;
            prerolledAt_ = Clock::now();
            // Extrapolation starts here, not at whatever `baseAt_` last was: a
            // stale one had the first position read 1.2 s ahead of VLC's 0.
            baseAt_ = prerolledAt_;
            // A stream with video holds once its first picture is shown
            // (vmemDisplay): VLC's preroll is still waiting for that picture
            // when it reports this, and a pause now would cut the wait short.
            if (!hasVideoTrack()) holdPreroll();
            if (reopening_) finishReopen();
            if (metadataSent_) applyPlay();
          }
          if (seeking_ && seekSawBuffering_ && seekGeneration == seekGeneration_.load()) finishSeek();
        }
        break;
      case libvlc_MediaPlayerPlaying:
        trace("vlc Playing");
        playing_ = true;
        applyVolume();
        break;
      case libvlc_MediaPlayerPaused:
        trace("vlc Paused");
        playing_ = false;
        break;
      case libvlc_MediaPlayerEndReached:
        trace("vlc EndReached");
        onEnded();
        return;
      case libvlc_MediaPlayerEncounteredError:
        onVlcError();
        return;
      case libvlc_MediaPlayerLengthChanged:
        break;
      case libvlc_MediaPlayerESAdded:
      case libvlc_MediaPlayerESDeleted:
      case libvlc_MediaPlayerESSelected:
        tracksDirty_ = true;
        (void)esType;
        break;
      default:
        break;
    }
    if (metadataSent_) {
      updateState();
      sendTime(false);
    }
  }

  void onVlcError() {
    // A protected stream VLC could not play is the key system's failure, as it
    // is on a platform with none: Shaka's 6001, not a media error.
    ManifestInfo manifest;
    if (VlcLog::get().manifest(request_.url, manifest) && manifest.complete && manifest.protectedContent) {
      fail(MediaErrorKind::KeySystem, 0, "the stream is protected, and Linux has no key system");
      return;
    }
    int status = 0;
    if (VlcLog::get().httpFailure(request_.url, status)) {
      fail(MediaErrorKind::Network, status,
           status > 0 ? "HTTP " + std::to_string(status) + " fetching " + request_.url
                      : "could not connect to fetch " + request_.url);
      return;
    }
    fail(MediaErrorKind::Media, 0, "VLC could not play " + request_.url + " (unreadable or undecodable)");
  }

  /// A load that ended, or stalled, before anything played: nothing in it VLC
  /// could use -- unless the log says why (an HTTP failure, a protected stream).
  void failUnplayable() {
    ManifestInfo manifest;
    int status = 0;
    if (VlcLog::get().httpFailure(request_.url, status) ||
        (VlcLog::get().manifest(request_.url, manifest) && manifest.complete && manifest.protectedContent)) {
      onVlcError();
      return;
    }
    fail(MediaErrorKind::Media, 0, "VLC found nothing it could play in " + request_.url);
  }

  /// A stream VLC opened but found no audio or video in -- a corrupt file taken
  /// by one of its lenient demuxers -- neither fails nor ends while paused at
  /// the start: VLC waits there with nothing to decode. Once the input has
  /// stopped reading and still no track has appeared, the load fails.
  void checkStalledOpen() {
    if (prerolled_ || reopening_ || secondsSince(openedAt_) < 3.0) return;
    libvlc_media_track_t** list = nullptr;
    const unsigned count = libvlc_media_tracks_get(media_, &list);
    if (count > 0) {
      libvlc_media_tracks_release(list, count);
      return;
    }
    libvlc_media_stats_t st;
    std::memset(&st, 0, sizeof(st));
    const long long read = libvlc_media_get_stats(media_, &st) ? st.i_read_bytes : 0;
    if (read != openReadBytes_) {
      openReadBytes_ = read;
      openReadAt_ = Clock::now();
      return;
    }
    if (secondsSince(openReadAt_) >= 2.0) failUnplayable();
  }

  void onEnded() {
    if (!metadataSent_ && !prerolled_) {
      failUnplayable();
      return;
    }
    if (info_.live) return;  // a live stream ends when it does; the page keeps its window
    ended_ = true;
    stalled_ = false;
    playing_ = false;
    if (seeking_) finishSeek();
    if (reopening_) finishReopen();
    if (std::isfinite(info_.duration)) base_ = info_.duration;
    updateState();
    sendTime(true);
    sendStats();
  }

  void onDecodedSize(int width, int height) {
    decodedWidth_ = width;
    decodedHeight_ = height;
    tracksDirty_ = true;
  }

  // ---- the poll ----------------------------------------------------------------------------------

  void tick() {
    if (!loaded_ || media_ == nullptr || mp_ == nullptr) return;
    // No first picture two seconds after the preroll: hold without it.
    if (holdAtPreroll_ && prerolled_ && secondsSince(prerolledAt_) > 2.0) holdPreroll();
    if (!metadataSent_) {
      checkStalledOpen();
      if (media_ != nullptr) trySendMetadata();
      return;
    }
    if (tracksDirty_) updateTracks(false);
    updateMetadata();
    checkSeek();
    updateState();
    sendTime(false);
    updateBuffered();
    if (secondsSince(lastStats_) >= 1.0) {
      lastStats_ = Clock::now();
      sendStats();
    }
  }

  void trace(const std::string& what) {
    if (!traceEnabled()) return;
    char prefix[160];
    std::snprintf(prefix, sizeof(prefix), "trace +%.3fs pos=%.2f raw=%.2f want=%d playing=%d prerolled=%d hold=%d: ",
                  secondsSince(openedAt_), metadataSent_ ? position() : -1.0, mp_ != nullptr ? rawTime() : -1.0,
                  wantsPlay_ ? 1 : 0, playing_ ? 1 : 0, prerolled_ ? 1 : 0, holdAtPreroll_ ? 1 : 0);
    log(LogLevel::Log, kTag, std::string(prefix) + what);
  }

  double rawTime() const {
    const libvlc_time_t ms = libvlc_media_player_get_time(mp_);
    return ms >= 0 ? ms / 1000.0 : 0;
  }

  /// Metadata once the first picture is in -- HTMLMediaElement promises
  /// videoWidth at loadedmetadata -- with the tracks and the one variant first,
  /// as Shaka's load waits for them.
  void trySendMetadata() {
    if (!prerolled_) return;
    ManifestInfo manifest;
    if (VlcLog::get().manifest(request_.url, manifest) && manifest.complete) {
      manifest_ = manifest;
      if (manifest.protectedContent) {
        fail(MediaErrorKind::KeySystem, 0, "the stream is protected, and Linux has no key system");
        return;
      }
    }
    updateTracks(true);
    const bool video = hasVideo_;
    if (video && decodedWidth_ <= 0 && secondsSince(prerolledAt_) < 2.0) return;

    info_.manifest = manifest_.type.empty() ? manifestGuess() : manifest_.type;
    const libvlc_time_t length = libvlc_media_player_get_length(mp_);
    if (!manifest_.type.empty()) {
      info_.live = manifest_.live;
    } else {
      info_.live = length <= 0 && info_.manifest != "progressive";
    }
    info_.duration = info_.live ? kInf : length > 0 ? length / 1000.0 : manifest_.seconds > 0 ? manifest_.seconds : kNaN;
    window(info_.seekStart, info_.seekEnd);
    info_.width = decodedWidth_ > 0 ? decodedWidth_ : trackWidth_;
    info_.height = decodedHeight_ > 0 ? decodedHeight_ : trackHeight_;
    info_.hasVideo = video;
    info_.hasAudio = hasAudio_;
    reportedWidth_ = info_.width;
    reportedHeight_ = info_.height;
    metadataSent_ = true;

    const std::uint32_t serial = serial_;
    const MediaInfo info = info_;
    const TrackList tracks = tracks_;
    const int variant = activeVariant_;
    emit([serial, info, tracks, variant](MediaSink& sink) {
      sink.onTracks(serial, tracks);
      sink.onVariantChanged(serial, variant);
      sink.onMetadata(serial, info);
      if (info.width > 0) sink.onSize(serial, info.width, info.height);
    });
    log(LogLevel::Log, kTag,
        request_.url + ": " + info_.manifest + (info_.live ? " live" : "") + ", " + std::to_string(info_.width) +
            "x" + std::to_string(info_.height) + (tracks_.variants.empty() ? std::string()
                                                                            : ", variant " +
                                                                                  std::to_string(tracks_.variants[0].height) + "p of " +
                                                                                  std::to_string(std::max<size_t>(1, manifest_.ladder.size()))));
    lastStats_ = Clock::now();
    if (std::isfinite(pendingSeek_)) {
      const double target = pendingSeek_;
      pendingSeek_ = kNaN;
      doSeek(target);
    }
    applyPlay();
    updateState();
    sendTime(true);
    sendStats();
  }

  void window(double& start, double& end) {
    if (!info_.live) {
      start = 0;
      end = std::isfinite(info_.duration) ? info_.duration : 0;
      return;
    }
    // VLC's adaptive module reports a live stream's time on the playlist's
    // timeline, its length as the window's, and its position in the window.
    const double time = rawTime();
    const libvlc_time_t length = libvlc_media_player_get_length(mp_);
    const double span = length > 0 ? length / 1000.0 : manifest_.seconds;
    const float position = libvlc_media_player_get_position(mp_);
    if (span > 0 && position > 0 && position <= 1) {
      start = time - position * span;
    } else {
      start = std::max(0.0, time - span);
    }
    end = start + span;
  }

  void updateMetadata() {
    if (info_.live) return;
    const libvlc_time_t length = libvlc_media_player_get_length(mp_);
    if (length <= 0) return;
    const double duration = length / 1000.0;
    if (std::isfinite(info_.duration) && std::fabs(duration - info_.duration) < 0.05) return;
    info_.duration = duration;
    info_.seekStart = 0;
    info_.seekEnd = duration;
    const std::uint32_t serial = serial_;
    const MediaInfo info = info_;
    emit([serial, info](MediaSink& sink) { sink.onMetadata(serial, info); });
  }

  // ---- tracks -------------------------------------------------------------------------------------

  /// The ladder's rung for the picture VLC is decoding: the one of its size
  /// (the highest-bandwidth one of that size), else the nearest in height.
  int matchRung(int width, int height) const {
    int best = -1;
    for (const VariantTrack& rung : manifest_.ladder) {
      if (rung.height == height && (rung.width == width || rung.width == 0)) {
        if (best < 0 || rung.bandwidth > manifest_.ladder[best].bandwidth) best = rung.id;
      }
    }
    if (best >= 0 || height <= 0) return best;
    for (const VariantTrack& rung : manifest_.ladder) {
      if (best < 0 || std::abs(rung.height - height) < std::abs(manifest_.ladder[best].height - height)) best = rung.id;
    }
    return best;
  }

  void updateTracks(bool quiet) {
    tracksDirty_ = false;
    libvlc_media_track_t** list = nullptr;
    const unsigned count = libvlc_media_tracks_get(media_, &list);
    const int audioEs = libvlc_audio_get_track(mp_);
    const int spuEs = libvlc_video_get_spu(mp_);
    TrackList tracks;
    hasVideo_ = false;
    hasAudio_ = false;
    std::string audioCodec, audioLanguage;
    int audioChannels = 0;
    int audioId = -1;
    double fps = 0;
    std::string videoCodec;
    unsigned bitrate = 0;
    for (unsigned i = 0; i < count; ++i) {
      const libvlc_media_track_t* t = list[i];
      const std::string language = t->psz_language != nullptr ? t->psz_language : "";
      const std::string label = t->psz_description != nullptr ? t->psz_description : "";
      if (t->i_type == libvlc_track_video) {
        hasVideo_ = true;
        if (t->video != nullptr) {
          if (t->video->i_width > 0) {
            trackWidth_ = static_cast<int>(t->video->i_width);
            trackHeight_ = static_cast<int>(t->video->i_height);
          }
          if (t->video->i_frame_rate_den > 0) {
            fps = static_cast<double>(t->video->i_frame_rate_num) / t->video->i_frame_rate_den;
          }
          sampleAspect_ = t->video->i_sar_num > 0 && t->video->i_sar_den > 0
                              ? static_cast<double>(t->video->i_sar_num) / t->video->i_sar_den
                              : 1.0;
        }
        videoCodec = codecName(t->i_codec);
        bitrate += t->i_bitrate;
      } else if (t->i_type == libvlc_track_audio) {
        hasAudio_ = true;
        AudioTrack a;
        a.id = t->i_id;
        a.language = language;
        a.label = label;
        a.codec = codecName(t->i_codec);
        a.channels = t->audio != nullptr ? static_cast<int>(t->audio->i_channels) : 0;
        a.active = t->i_id == audioEs;
        if (a.active || audioId < 0) {
          audioId = a.id;
          audioCodec = a.codec;
          audioLanguage = a.language;
          audioChannels = a.channels;
        }
        bitrate += t->i_bitrate;
        tracks.audio.push_back(a);
      } else if (t->i_type == libvlc_track_text) {
        TextTrack x;
        x.id = t->i_id;
        x.language = language;
        x.label = label;
        const std::string l = lower(label);
        x.kind = l.find("caption") != std::string::npos || l.find("cc") == 0 ? "captions" : "subtitles";
        x.mimeType = textMime(t->i_codec);
        x.active = t->i_id == spuEs && spuEs >= 0;
        tracks.text.push_back(x);
      }
    }
    if (count > 0) libvlc_media_tracks_release(list, count);

    // Text stays off until the page selects it: VLC may pick one on its own.
    if (spuEs >= 0 && spuEs != textSelected_) {
      libvlc_video_set_spu(mp_, textSelected_);
      for (TextTrack& x : tracks.text) x.active = x.id == textSelected_;
    }

    // The one variant: the stream VLC is playing.
    const int width = decodedWidth_ > 0 ? decodedWidth_ : trackWidth_;
    const int height = decodedHeight_ > 0 ? decodedHeight_ : trackHeight_;
    if (surfaceRaw_.load() != nullptr && width > 0) surfaceRaw_.load()->setPicture(width, height, sampleAspect_);
    VariantTrack v;
    const int rung = matchRung(width, height);
    if (rung >= 0) {
      v = manifest_.ladder[rung];
      if (v.bandwidth > 0 && manifest_.type == "dash") v.bandwidth += manifest_.audioBandwidth;
    } else {
      v.id = 0;
      v.bandwidth = bitrate > 0 ? bitrate : estimatedBandwidth_;
      v.frameRate = fps;
      v.videoCodec = videoCodec;
      v.audioCodec = audioCodec;
    }
    if (v.width <= 0 || rung < 0) v.width = width;
    if (v.height <= 0 || rung < 0) v.height = height;
    if (v.frameRate <= 0) v.frameRate = fps;
    if (v.videoCodec.empty()) v.videoCodec = videoCodec;
    if (v.audioCodec.empty()) v.audioCodec = audioCodec;
    v.language = audioLanguage;
    v.audioId = audioId;
    v.channels = audioChannels;
    v.active = true;
    if (hasVideo_ || hasAudio_) tracks.variants.push_back(v);

    const int previousVariant = activeVariant_;
    activeVariant_ = v.id;
    // Report only what changed.
    std::string key = std::to_string(v.id) + "/" + std::to_string(v.width) + "x" + std::to_string(v.height) + "/" +
                      std::to_string(static_cast<long long>(v.bandwidth)) + "/" + std::to_string(audioId) + "/" +
                      std::to_string(spuEs);
    for (const AudioTrack& a : tracks.audio) key += "|a" + std::to_string(a.id);
    for (const TextTrack& x : tracks.text) key += "|t" + std::to_string(x.id) + (x.active ? "*" : "");
    tracks_ = tracks;
    if (quiet) {
      tracksKey_ = key;
      return;
    }
    const std::uint32_t serial = serial_;
    if (key != tracksKey_) {
      tracksKey_ = key;
      emit([serial, tracks](MediaSink& sink) { sink.onTracks(serial, tracks); });
    }
    if (previousVariant != activeVariant_) {
      const int id = activeVariant_;
      emit([serial, id](MediaSink& sink) { sink.onVariantChanged(serial, id); });
    }
    if (width > 0 && (width != reportedWidth_ || height != reportedHeight_)) {
      reportedWidth_ = width;
      reportedHeight_ = height;
      emit([serial, width, height](MediaSink& sink) { sink.onSize(serial, width, height); });
    }
  }

  void doSelectAudio(const std::string& language, const std::string& role) {
    if (!metadataSent_ || mp_ == nullptr) return;
    (void)role;  // VLC knows no roles
    const std::string want = lower(language);
    for (const AudioTrack& a : tracks_.audio) {
      const std::string have = lower(a.language);
      if (want.empty() || have == want || have.rfind(want + "-", 0) == 0 || want.rfind(have + "-", 0) == 0) {
        if (!a.active) libvlc_audio_set_track(mp_, a.id);
        break;
      }
    }
    updateTracks(false);
    // The variant is the same stream with that audio: reported as a change, as
    // Shaka reports a language switch.
    const std::uint32_t serial = serial_;
    const int id = activeVariant_;
    emit([serial, id](MediaSink& sink) { sink.onVariantChanged(serial, id); });
  }

  void doSelectText(int id) {
    if (mp_ == nullptr) return;
    textSelected_ = id >= 0 ? id : -1;
    if (!metadataSent_) return;
    libvlc_video_set_spu(mp_, textSelected_);
    updateTracks(false);
  }

  // ---- state, time, buffered, stats ------------------------------------------------------------

  void updateState() {
    if (!metadataSent_) return;
    PlaybackState state;
    if (ended_) {
      state = PlaybackState::Ended;
    } else if (seeking_ || reopening_ || stalled_ || !prerolled_) {
      state = PlaybackState::Buffering;
    } else {
      state = PlaybackState::Ready;
    }
    if (stateSent_ && state == state_) return;
    state_ = state;
    stateSent_ = true;
    const std::uint32_t serial = serial_;
    emit([serial, state](MediaSink& sink) { sink.onState(serial, state); });
    sendTime(true);
  }

  /// Where playback is. VLC updates its time four times a second at most (the
  /// adaptive module once a second), and not at all while paused, so between
  /// its updates the position runs on from the last one while playing.
  double position() {
    const double raw = rawTime();
    const bool advancing = playing_ && !stalled_ && !seeking_ && !reopening_ && !ended_ && prerolled_;
    const double expected = base_ + (advancing ? std::min(secondsSince(baseAt_), 1.5) * rate_ : 0);
    if (raw != lastRaw_) {
      lastRaw_ = raw;
      // After a load, a seek or a reopen VLC's time is stale until it has played
      // there: only a value near where playback now is replaces the target. And
      // held on the first picture, VLC's time counts what its demuxer read ahead
      // (a second and more): not until playback moves.
      const bool near = advancing ? raw >= expected - 1.0 && raw <= expected + 2.5 : std::fabs(raw - expected) < 0.25;
      if (continuous_ || info_.live || near) {
        base_ = raw;
        baseAt_ = Clock::now();
        continuous_ = true;
        return base_;
      }
    }
    if (!advancing) {
      baseAt_ = Clock::now();
      return base_;
    }
    const double elapsed = std::min(secondsSince(baseAt_), info_.live ? 1.5 : 1.2);
    double at = base_ + elapsed * rate_;
    if (std::isfinite(info_.duration) && !info_.live) at = std::min(at, info_.duration);
    return at;
  }

  void sendTime(bool force) {
    if (!metadataSent_) return;
    double at = position();
    if (ended_ && std::isfinite(info_.duration)) at = info_.duration;
    // Continuous playback never steps back: VLC's own time can trail what was
    // extrapolated from it by a few hundred milliseconds.
    if (continuous_ && !info_.live && std::isfinite(lastTimeSent_) && at < lastTimeSent_ &&
        lastTimeSent_ - at < 0.6 && !seeking_ && !reopening_) {
      at = lastTimeSent_;
    }
    double start = 0, end = 0;
    window(start, end);
    if (info_.live) {
      // The page reads the window while paused too.
      info_.seekStart = start;
      info_.seekEnd = end;
    }
    const bool moved = !std::isfinite(lastTimeSent_) || std::fabs(at - lastTimeSent_) > 1e-3 ||
                       (info_.live && (std::fabs(start - lastWindowStart_) > 1e-3 || std::fabs(end - lastWindowEnd_) > 1e-3));
    if (!force && (!moved || secondsSince(lastTimeAt_) < 0.2)) return;
    lastTimeSent_ = at;
    lastTimeAt_ = Clock::now();
    lastWindowStart_ = start;
    lastWindowEnd_ = end;
    const std::uint32_t serial = serial_;
    emit([serial, at, start, end](MediaSink& sink) { sink.onTime(serial, at, start, end); });
  }

  /// libvlc does not say what it has buffered: from where playback last started
  /// to a network cache's worth ahead of it -- all of it once it has ended.
  void updateBuffered() {
    if (secondsSince(lastBufferedAt_) < 0.25) return;
    lastBufferedAt_ = Clock::now();
    std::vector<TimeRange> ranges;
    const double at = std::isfinite(lastTimeSent_) ? lastTimeSent_ : base_;
    if (!info_.live && prerolled_) {
      TimeRange r;
      r.start = std::min(bufferedFrom_, at);
      r.end = ended_ ? info_.duration : at + (stalled_ ? 0.0 : 1.0);
      if (std::isfinite(info_.duration)) r.end = std::min(r.end, info_.duration);
      if (r.end > r.start) ranges.push_back(r);
    }
    bool same = ranges.size() == buffered_.size();
    for (size_t i = 0; same && i < ranges.size(); ++i) {
      same = std::fabs(ranges[i].start - buffered_[i].start) < 0.05 && std::fabs(ranges[i].end - buffered_[i].end) < 0.05;
    }
    if (same) return;
    buffered_ = ranges;
    const std::uint32_t serial = serial_;
    emit([serial, ranges](MediaSink& sink) { sink.onBuffered(serial, ranges); });
  }

  /// Pictures decoded: every one VLC's video output showed or dropped. Not VLC's
  /// own `i_decoded_video`, which VLC 3 counts twice for each picture -- once
  /// when the decoder takes the packet, once when it queues the picture
  /// (src/input/decoder.c) -- and so would halve the dropped share.
  static std::uint64_t decodedPictures(const libvlc_media_stats_t& st) {
    return static_cast<std::uint64_t>(std::max(0, st.i_displayed_pictures)) +
           static_cast<std::uint64_t>(std::max(0, st.i_lost_pictures));
  }

  void sendStats() {
    if (!metadataSent_ || media_ == nullptr) return;
    libvlc_media_stats_t st;
    std::memset(&st, 0, sizeof(st));
    if (!libvlc_media_get_stats(media_, &st)) return;
    MediaStats stats;
    stats.width = reportedWidth_;
    stats.height = reportedHeight_;
    stats.frameRate = tracks_.variants.empty() ? 0 : tracks_.variants[0].frameRate;
    // Frames since the load: a reopen starts VLC's counters again.
    const std::uint64_t surfaceDropped =
        surfaceRaw_.load() != nullptr ? surfaceRaw_.load()->dropped() - surfaceDroppedAtLoad_ : 0;
    stats.decodedFrames = decodedBefore_ + decodedPictures(st);
    stats.droppedFrames =
        droppedBefore_ + static_cast<std::uint64_t>(std::max(0, st.i_lost_pictures)) + surfaceDropped;
    lastDecoded_ = decodedPictures(st);
    lastLost_ = static_cast<std::uint64_t>(std::max(0, st.i_lost_pictures));
    if (st.f_input_bitrate > 0) {
      estimatedBandwidth_ = st.f_input_bitrate * 8e6;  // bytes per microsecond
      stats.estimatedBandwidth = estimatedBandwidth_;
    }
    if (!tracks_.variants.empty() && tracks_.variants[0].bandwidth > 0) {
      stats.streamBandwidth = tracks_.variants[0].bandwidth;
    } else if (st.f_demux_bitrate > 0) {
      stats.streamBandwidth = st.f_demux_bitrate * 8e6;
    }
    const std::uint32_t serial = serial_;
    emit([serial, stats](MediaSink& sink) { sink.onStats(serial, stats); });
    checkDrops(stats.decodedFrames, stats.droppedFrames);
  }

  /// Sustained drops -- over 1% of frames across 10 s of playback -- lower the
  /// ceiling one rung and reopen where playback is: VLC 3 cannot switch a
  /// stream's variant while it plays.
  void checkDrops(std::uint64_t decoded, std::uint64_t dropped) {
    const bool steady = playing_ && !stalled_ && !seeking_ && !reopening_ && !ended_ && prerolled_ &&
                        secondsSince(prerolledAt_) > 2.0;
    if (!steady) {
      samples_.clear();
      return;
    }
    samples_.push_back({Clock::now(), decoded, dropped});
    while (samples_.size() > 1 && secondsSince(samples_.front().at) > 10.5) samples_.pop_front();
    if (samples_.size() < 2 || std::chrono::duration<double>(samples_.back().at - samples_.front().at).count() < 9.5) {
      return;
    }
    const double frames = static_cast<double>(samples_.back().decoded - samples_.front().decoded);
    const double lost = static_cast<double>(samples_.back().dropped - samples_.front().dropped);
    if (frames <= 0 || lost / frames <= 0.01) return;
    if (info_.manifest != "hls" && info_.manifest != "dash") return;
    const int height = reportedHeight_;
    int next = 0;
    for (const VariantTrack& rung : manifest_.ladder) {
      if (rung.height > 0 && rung.height < height) next = std::max(next, rung.height);
    }
    if (manifest_.ladder.empty() && height > 1) next = height - 1;
    samples_.clear();
    if (next <= 0) return;  // already the lowest rung
    log(LogLevel::Warn, kTag,
        "dropped " + std::to_string(static_cast<long long>(lost)) + " of " +
            std::to_string(static_cast<long long>(frames)) + " frames over 10 s at " + std::to_string(height) +
            "p: the ceiling steps down to " + std::to_string(next) + "p");
    stepHeight_ = next;
    reopen(position(), false);
  }

  // ---- seeking and playing ---------------------------------------------------------------------------

  void doSeek(double target) {
    if (!loaded_ || mp_ == nullptr || media_ == nullptr) return;
    if (!std::isfinite(target)) return;
    if (!metadataSent_) {
      pendingSeek_ = target;
      return;
    }
    if (!info_.live && std::isfinite(info_.duration)) target = std::clamp(target, 0.0, info_.duration);
    if (ended_ || reopening_) {
      // VLC's input is gone at the end: open the stream again there.
      reopen(target, true);
      return;
    }
    seekGeneration_.fetch_add(1);
    seeking_ = true;
    seekSawBuffering_ = false;
    seekTarget_ = target;
    seekIssuedAt_ = Clock::now();
    base_ = target;
    baseAt_ = Clock::now();
    lastRaw_ = rawTime();
    continuous_ = false;
    bufferedFrom_ = target;
    libvlc_media_player_set_time(mp_, static_cast<libvlc_time_t>(std::llround(target * 1000.0)));
    updateState();
  }

  void checkSeek() {
    if (!seeking_) return;
    const double elapsed = secondsSince(seekIssuedAt_);
    // No buffering reported for this seek: VLC had what it needed.
    if ((!seekSawBuffering_ && elapsed > 2.0) || elapsed > 10.0) finishSeek();
  }

  void finishSeek() {
    if (!seeking_) return;
    seeking_ = false;
    const double where = seekTarget_;
    base_ = where;
    baseAt_ = Clock::now();
    lastRaw_ = rawTime();
    const std::uint32_t serial = serial_;
    emit([serial, where](MediaSink& sink) { sink.onSeeked(serial, where); });
    updateState();
    sendTime(true);
  }

  void finishReopen() {
    reopening_ = false;
    lastRaw_ = rawTime();
    base_ = reopenTarget_;
    baseAt_ = Clock::now();
    tracksDirty_ = true;
    if (reportSeekOnReopen_) {
      reportSeekOnReopen_ = false;
      const std::uint32_t serial = serial_;
      const double where = reopenTarget_;
      emit([serial, where](MediaSink& sink) { sink.onSeeked(serial, where); });
    }
    if (textSelected_ >= 0) libvlc_video_set_spu(mp_, textSelected_);
    applyPlay();
    updateState();
    sendTime(true);
  }

  void applyPlay() {
    if (mp_ == nullptr || media_ == nullptr || !prerolled_ || !metadataSent_) return;
    // Unconditionally: VLC applies a pause asynchronously, so its state may not
    // show one just asked for (holdPreroll's), and asking for the state it is
    // already in does nothing.
    if (wantsPlay_ && !ended_ && !rateIsZero_) {
      trace("applyPlay: resume");
      libvlc_media_player_set_pause(mp_, 0);
      libvlc_media_player_set_rate(mp_, static_cast<float>(rate_));
      // Playing now, not on VLC's Playing event. VLC merges a pause and a resume
      // that reach its input back to back -- holdPreroll's hold, then the page's
      // play() a few milliseconds later -- into no change and no event at all,
      // and this clock then stood still while VLC played the stream to its end
      // (media-hls, media-element on the Pi). If VLC really was paused, its
      // Playing event follows and changes nothing.
      if (!playing_) {
        base_ = position();
        baseAt_ = Clock::now();
        playing_ = true;
      }
    } else if (!wantsPlay_ || rateIsZero_) {
      trace("applyPlay: pause");
      if (libvlc_media_player_can_pause(mp_)) libvlc_media_player_set_pause(mp_, 1);
      // The position holds where VLC stopped.
      base_ = position();
      playing_ = false;
    }
  }

  bool hasVideoTrack() {
    libvlc_media_track_t** list = nullptr;
    const unsigned count = media_ != nullptr ? libvlc_media_tracks_get(media_, &list) : 0;
    bool video = false;
    for (unsigned i = 0; i < count; ++i) video = video || list[i]->i_type == libvlc_track_video;
    if (count > 0) libvlc_media_tracks_release(list, count);
    return video;
  }

  /// Prerolled with the page not playing: stop where VLC's clock just started,
  /// on the first picture.
  void holdPreroll() {
    if (!holdAtPreroll_ || mp_ == nullptr || media_ == nullptr) return;
    holdAtPreroll_ = false;
    const bool canPause = libvlc_media_player_can_pause(mp_) != 0;
    const bool hold = !wantsPlay_ && canPause;
    if (hold) libvlc_media_player_set_pause(mp_, 1);
    log(LogLevel::Log, kTag,
        std::string("preroll: first picture in, ") +
            (wantsPlay_ ? "the page is playing" : canPause ? "held paused" : "VLC says it cannot pause"));
    // Stopped only when it did stop. With the page already playing this used to
    // clear `playing_` anyway, and the position then moved only when VLC's own
    // coarse time did -- up to a second behind the page's currentTime, which put
    // the buffered estimate (a second past that position) behind it too
    // (media-element's `buffered=false` on the Pi).
    if (hold) {
      base_ = position();
      baseAt_ = Clock::now();
      playing_ = false;
    }
    applyVolume();
  }

  void applyVolume() {
    if (mp_ == nullptr) return;
    libvlc_audio_set_volume(mp_, static_cast<int>(std::lround(volume_ * 100)));
    // Silent while held at the preroll: the page has not asked for sound yet.
    libvlc_audio_set_mute(mp_, muted_ || holdAtPreroll_ ? 1 : 0);
  }

  // ---- state ------------------------------------------------------------------------------------------

  std::mutex sinkMutex_;
  std::shared_ptr<MediaSink> sink_;
  std::atomic<bool> stopped_{false};

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<std::function<void()>> jobs_;
  bool exiting_ = false;
  bool closed_ = false;
  std::thread worker_;

  std::mutex planeMutex_;
  PlaneRect plane_;
  bool planeVisible_ = false;
  std::unique_ptr<wl::VideoSurface> surface_;
  std::atomic<wl::VideoSurface*> surfaceRaw_{nullptr};
  std::atomic<bool> surfaceStop_{false};
  std::thread surfaceThread_;

  // Everything below is the worker's.
  std::shared_ptr<Instance> instance_;
  libvlc_media_player_t* mp_ = nullptr;
  libvlc_media_t* media_ = nullptr;
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<std::uint64_t> seekGeneration_{0};

  LoadRequest request_;
  std::uint32_t serial_ = 0;
  AbrConfig abr_;
  Ceiling ceiling_;
  int stepHeight_ = 0;
  bool loaded_ = false;
  bool prerolled_ = false;
  bool holdAtPreroll_ = false;
  std::atomic<bool> awaitingFirstPicture_{false};
  /// The picture layout VLC was given, for the scratch buffer in vmemLock.
  std::atomic<std::size_t> sinkBytes_{0};
  std::atomic<std::uint32_t> sinkOffsets_[3]{};
  Clock::time_point prerolledAt_{};
  Clock::time_point openedAt_{};
  long long openReadBytes_ = -1;
  Clock::time_point openReadAt_{};
  bool metadataSent_ = false;
  bool ended_ = false;
  bool stalled_ = false;
  bool playing_ = false;
  bool wantsPlay_ = false;
  double rate_ = 1.0;
  /// playbackRate 0: the picture frozen with the element still unpaused.
  bool rateIsZero_ = false;
  double volume_ = 1.0;
  bool muted_ = false;

  bool seeking_ = false;
  bool seekSawBuffering_ = false;
  double seekTarget_ = 0;
  Clock::time_point seekIssuedAt_{};
  double pendingSeek_ = kNaN;
  bool reopening_ = false;
  bool reportSeekOnReopen_ = false;
  double reopenTarget_ = 0;

  MediaInfo info_;
  ManifestInfo manifest_;
  PlaybackState state_ = PlaybackState::Loading;
  bool stateSent_ = false;

  bool tracksDirty_ = true;
  TrackList tracks_;
  std::string tracksKey_;
  int activeVariant_ = 0;
  int textSelected_ = -1;
  bool hasVideo_ = false;
  bool hasAudio_ = false;
  int trackWidth_ = 0;
  int trackHeight_ = 0;
  double sampleAspect_ = 1.0;
  int decodedWidth_ = 0;
  int decodedHeight_ = 0;
  int reportedWidth_ = 0;
  int reportedHeight_ = 0;

  double base_ = 0;
  Clock::time_point baseAt_{};
  double lastRaw_ = -1;
  bool continuous_ = false;
  double lastTimeSent_ = kNaN;
  Clock::time_point lastTimeAt_{};
  double lastWindowStart_ = 0;
  double lastWindowEnd_ = 0;

  std::vector<TimeRange> buffered_;
  double bufferedFrom_ = 0;
  Clock::time_point lastBufferedAt_{};

  Clock::time_point lastStats_{};
  double estimatedBandwidth_ = 0;
  std::uint64_t decodedBefore_ = 0;
  std::uint64_t droppedBefore_ = 0;
  std::uint64_t lastDecoded_ = 0;
  std::uint64_t lastLost_ = 0;
  std::uint64_t surfaceDroppedAtLoad_ = 0;
  struct Sample {
    Clock::time_point at;
    std::uint64_t decoded;
    std::uint64_t dropped;
  };
  std::deque<Sample> samples_;
};

bool pluginFileExists(const char* name) {
  for (const char* dir : {"/usr/lib/vlc/plugins/codec/", "/usr/lib/aarch64-linux-gnu/vlc/plugins/codec/",
                          "/usr/lib/x86_64-linux-gnu/vlc/plugins/codec/", "/usr/lib64/vlc/plugins/codec/"}) {
    struct stat st;
    if (stat((std::string(dir) + name).c_str(), &st) == 0) return true;
  }
  return false;
}

}  // namespace

std::shared_ptr<MediaPlayer> MediaPlayer::create(MediaConfig config) {
  return std::make_shared<LinuxPlayer>(std::move(config));
}

MediaCapabilities mediaCapabilities() {
  MediaCapabilities caps;
  caps.available = true;
  caps.platform = "linux";
  caps.hls = true;
  caps.dash = true;
  caps.progressive = true;
  // No key system: VLC 3 decrypts no CENC and has no Widevine (spec-video-player.md).
  caps.containers = {"video/mp4",  "audio/mp4",   "video/webm", "audio/webm", "video/mp2t",
                     "application/vnd.apple.mpegurl", "application/x-mpegurl", "audio/mpegurl",
                     "application/dash+xml"};
  // VLC's avcodec decodes each of these in software where the ScreenKit plugin
  // has no hardware for it.
  caps.codecs = {"avc1", "avc3", "hvc1", "hev1", "vp08", "vp09", "mp4a", "ac-3", "ec-3", "opus", "flac"};
  if (pluginFileExists("libdav1d_plugin.so") || pluginFileExists("libaom_plugin.so")) caps.codecs.push_back("av01");
  std::string why;
  caps.videoOutput = wl::waylandAvailable(why);
  caps.videoOutputProblem = caps.videoOutput ? std::string() : why;
  return caps;
}

bool mediaAvailable() { return true; }

}  // namespace screenkit::media
