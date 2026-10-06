// Copyright (c) ScreenKit contributors. MIT.
//
// The player for a platform that has none of its own: the runtime still builds
// there, `canPlayType` answers "" and every load fails with `Unavailable`
// rather than the build failing or a load hanging. No target this repo ships is
// such a platform -- Apple, Android and Linux each have a player -- so this is
// what a new port starts from, and runtime/tests/MediaUnavailableTests.cpp is
// what keeps it compiling and honest in the meantime (`media-unavailable`).
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "../net/IoQueue.h"
#include "MediaPlayer.h"

namespace screenkit::media {
namespace {

constexpr const char* kWhy = "this platform has no media player";

class UnavailableMediaPlayer final : public MediaPlayer {
 public:
  explicit UnavailableMediaPlayer(MediaConfig config)
      : queue_(net::makeIoQueue(config.name + ".media")), sink_(std::move(config.sink)) {}
  ~UnavailableMediaPlayer() override { shutdown(); }

  void load(LoadRequest request) override {
    if (stopped_) return;
    // Through the queue, so the failure reaches the sink on a later turn, as a
    // real player's would: a caller that loads and then does its own
    // bookkeeping must not be re-entered mid-call.
    auto sink = sink_;
    const std::uint32_t serial = request.serial;
    queue_->post([sink, serial] {
      if (sink) sink->onError(serial, MediaErrorKind::Unavailable, 0, kWhy);
    });
  }
  void play() override {}
  void pause() override {}
  void seek(double) override {}
  void setRate(double) override {}
  void setVolume(double) override {}
  void setMuted(bool) override {}
  void setPlane(PlaneRect, bool) override {}
  void selectVariant(int) override {}
  void setAbr(AbrConfig) override {}
  void selectAudioLanguage(std::string, std::string) override {}
  void selectText(int) override {}
  void provideLicence(std::uint64_t, std::shared_ptr<const Bytes>) override {}
  void unload() override {}

  void shutdown() override {
    if (stopped_.exchange(true)) return;
    // Whatever is still queued runs and is dropped with the sink it holds.
    queue_->sync([] {});
    sink_.reset();
  }

 private:
  std::shared_ptr<net::IoQueue> queue_;
  std::shared_ptr<MediaSink> sink_;
  std::atomic<bool> stopped_{false};
};

}  // namespace

std::shared_ptr<MediaPlayer> MediaPlayer::create(MediaConfig config) {
  return std::make_shared<UnavailableMediaPlayer>(std::move(config));
}

MediaCapabilities mediaCapabilities() {
  MediaCapabilities caps;
  caps.available = false;
  caps.platform = "none";
  caps.videoOutput = false;
  caps.videoOutputProblem = kWhy;
  return caps;
}

bool mediaAvailable() { return false; }

}  // namespace screenkit::media
