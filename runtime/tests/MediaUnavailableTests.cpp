// Copyright (c) ScreenKit contributors. MIT.
//
// The Unavailable player (core/src/media/MediaPlayerUnavailable.cpp), which the
// runtime builds on a platform with none of the three real players. No target
// this repo ships is such a platform, so without this nothing would ever
// compile it, let alone run it -- and it is what keeps a new port building and
// failing loads cleanly before it has a player of its own (`media-unavailable`).
//
// Its own executable because it *replaces* the platform's player:
// `MediaPlayer::create`, `mediaCapabilities` and `mediaAvailable` are defined in
// this binary, so the linker takes them from here and never pulls the real
// player out of libscreenkit-core. No fixture and no JS: the seam directly.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/src/media/MediaPlayer.h"

namespace media = screenkit::media;

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "  FAIL %s\n", what);
  ++gFailures;
}

struct Sink final : media::MediaSink {
  std::atomic<int> errors{0};
  std::atomic<int> other{0};
  std::atomic<int> kind{-1};
  std::atomic<std::uint32_t> serial{0};
  std::atomic<bool> fromCaller{false};
  std::thread::id caller;
  void onMetadata(std::uint32_t, media::MediaInfo) override { other.fetch_add(1); }
  void onState(std::uint32_t, media::PlaybackState) override { other.fetch_add(1); }
  void onTime(std::uint32_t, double, double, double) override { other.fetch_add(1); }
  void onSeeked(std::uint32_t, double) override { other.fetch_add(1); }
  void onBuffered(std::uint32_t, std::vector<media::TimeRange>) override { other.fetch_add(1); }
  void onTracks(std::uint32_t, media::TrackList) override { other.fetch_add(1); }
  void onVariantChanged(std::uint32_t, int) override { other.fetch_add(1); }
  void onSize(std::uint32_t, int, int) override { other.fetch_add(1); }
  void onCues(std::uint32_t, int, std::vector<media::Cue>) override { other.fetch_add(1); }
  void onLicenceRequest(std::uint32_t, std::uint64_t, std::string, media::Bytes, std::string) override {
    other.fetch_add(1);
  }
  void onStats(std::uint32_t, media::MediaStats) override { other.fetch_add(1); }
  void onError(std::uint32_t s, media::MediaErrorKind error, int, std::string) override {
    kind.store(static_cast<int>(error));
    serial.store(s);
    if (std::this_thread::get_id() == caller) fromCaller.store(true);
    errors.fetch_add(1);
  }
};

template <typename Done>
bool waitUntil(Done done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

}  // namespace

int main() {
  std::fprintf(stderr, "== media-unavailable ==\n");

  // Nothing to play with, and it says so: canPlayType answers "" from this.
  check(!media::mediaAvailable(), "mediaAvailable() is false");
  const media::MediaCapabilities caps = media::mediaCapabilities();
  check(!caps.available && caps.platform == "none", "capabilities: unavailable, platform none");
  check(!caps.hls && !caps.dash && !caps.progressive && caps.keySystems.empty() && caps.codecs.empty(),
        "capabilities: plays nothing");
  check(!caps.videoOutput && !caps.videoOutputProblem.empty(), "capabilities: no video output, with a reason");

  auto sink = std::make_shared<Sink>();
  sink->caller = std::this_thread::get_id();
  media::MediaConfig config;
  config.name = "unavailable-test";
  config.sink = sink;
  auto player = media::MediaPlayer::create(config);
  check(player != nullptr, "create() returns a player");

  // Every load fails with Unavailable, on a later turn and never inside the call.
  media::LoadRequest request;
  request.serial = 7;
  request.url = "https://example.invalid/stream.m3u8";
  player->load(request);
  check(waitUntil([&] { return sink->errors.load() == 1; }), "the load failed");
  check(sink->kind.load() == static_cast<int>(media::MediaErrorKind::Unavailable), "... with Unavailable");
  check(sink->serial.load() == 7, "... for that load's serial");
  check(!sink->fromCaller.load(), "... not from inside load()");

  // Everything else is accepted and does nothing.
  player->play();
  player->pause();
  player->seek(3);
  player->setRate(2);
  player->setVolume(0.5);
  player->setMuted(true);
  player->setPlane(media::PlaneRect{0, 0, 100, 50, 0}, true);
  player->selectVariant(0);
  player->setAbr(media::AbrConfig());
  player->selectAudioLanguage("en", "");
  player->selectText(-1);
  player->provideLicence(1, nullptr);
  player->unload();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(sink->other.load() == 0 && sink->errors.load() == 1, "no other event, ever");

  // Shutdown is synchronous and final: a load afterwards delivers nothing.
  player->shutdown();
  player->shutdown();
  request.serial = 8;
  player->load(request);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  check(sink->errors.load() == 1, "nothing after shutdown");

  // A player made with no sink loads without anywhere to report to.
  media::MediaConfig bare;
  auto quiet = media::MediaPlayer::create(bare);
  quiet->load(request);
  quiet->shutdown();
  quiet.reset();
  player.reset();
  config.sink.reset();
  check(sink.use_count() == 1, "the player let go of its sink");

  // The plane's arithmetic, which only a fixed-size drawable reaches: the app's
  // frame is scaled by the smaller ratio and centred (GlSurfaceSdl's present),
  // and the plane has to land in the same place on the window.
  const media::PlaneRect whole{0, 0, 1280, 720};
  const media::PlaneRect same = media::planeInWindow(whole, 1280, 720, 1280, 720);
  check(same.x == 0 && same.width == 1280, "planeInWindow: a drawable the window's size is unchanged");
  const media::PlaneRect none = media::planeInWindow(whole, 1600, 900, 0, 0);
  check(none.width == 1280 && none.height == 720, "planeInWindow: no fixed drawable, unchanged");
  // 1280x720 into 1600x900: 1.25 both ways, nothing left over.
  const media::PlaneRect up = media::planeInWindow({10, 20, 100, 50}, 1600, 900, 1280, 720);
  check(up.x == 12.5 && up.y == 25 && up.width == 125 && up.height == 62.5, "planeInWindow: scaled, no letterbox");
  // 1280x720 into 1280x800: the narrower ratio wins, so 40px above and below.
  const media::PlaneRect tall = media::planeInWindow({0, 0, 1280, 720}, 1280, 800, 1280, 720);
  check(tall.x == 0 && tall.y == 40 && tall.width == 1280 && tall.height == 720,
        "planeInWindow: centred in the taller window");
  const media::PlaneRect wide = media::fitContain({0, 0, 1000, 1000}, 16, 9);
  check(wide.width == 1000 && wide.height == 562.5 && wide.x == 0 && wide.y == 218.75,
        "fitContain: 16:9 letterboxed in a square");
  const media::PlaneRect narrow = media::fitContain({0, 0, 1000, 1000}, 9, 16);
  check(narrow.width == 562.5 && narrow.height == 1000 && narrow.x == 218.75 && narrow.y == 0,
        "fitContain: 9:16 pillarboxed in a square");
  const media::PlaneRect unknown = media::fitContain({0, 0, 640, 360}, 0, 0);
  check(unknown.width == 640 && unknown.height == 360, "fitContain: no picture size, unchanged");

  if (gFailures > 0) {
    std::fprintf(stderr, "== media-unavailable: %d failure(s) ==\n", gFailures);
    return 1;
  }
  std::fprintf(stderr, "== media-unavailable: ok ==\n");
  return 0;
}
