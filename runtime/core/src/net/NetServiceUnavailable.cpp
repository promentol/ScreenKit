// Copyright (c) ScreenKit contributors. MIT.
//
// The client for a platform that has none of its own: the runtime still builds
// there, and every request, socket and cookie call fails with `Unsupported`
// rather than the build failing or a request hanging. No target this repo ships
// is such a platform -- Apple, Android and Linux each have a client -- so this is
// what a new port starts from, and runtime/tests/NetUnavailableTests.cpp is
// what keeps it compiling and honest in the meantime (`net-unavailable`).
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "IoQueue.h"
#include "NetService.h"

namespace screenkit::net {
namespace {

constexpr const char* kWhy = "this platform has no HTTP client yet";

class UnavailableNetService final : public NetService {
 public:
  explicit UnavailableNetService(NetConfig config) : queue_(makeIoQueue(config.name + ".net")) {}
  ~UnavailableNetService() override { shutdown(); }

  void startRequest(std::uint64_t, HttpRequestSpec, std::shared_ptr<HttpSink> sink) override {
    if (stopped_ || sink == nullptr) return;
    // Through the queue, so the failure reaches the sink on a later turn, as a
    // real client's would: a caller that starts a request and then registers
    // its own bookkeeping must not be re-entered mid-call.
    queue_->post([sink] { sink->onError(NetError::Unsupported, kWhy); });
  }
  void appendRequestBody(std::uint64_t, std::shared_ptr<const Bytes>) override {}
  void finishRequestBody(std::uint64_t) override {}
  void abortRequest(std::uint64_t) override {}
  void acknowledgeResponseData(std::uint64_t, std::uint64_t) override {}

  void openSocket(std::uint64_t, Url, std::vector<std::string>, std::shared_ptr<SocketSink> sink) override {
    if (stopped_ || sink == nullptr) return;
    queue_->post([sink] {
      sink->onError(NetError::Unsupported, kWhy);
      sink->onClose(1006, "", false);
    });
  }
  void sendSocket(std::uint64_t, bool, std::shared_ptr<const Bytes>) override {}
  void closeSocket(std::uint64_t, int, std::string) override {}

  std::string cookiesFor(const Url&) override { return std::string(); }
  bool setCookie(const Url&, const std::string&) override { return false; }

  void shutdown() override {
    if (stopped_.exchange(true)) return;
    // Whatever is still queued runs and is dropped: the sinks it holds go with
    // it, and nothing is delivered afterwards.
    queue_->sync([] {});
  }

 private:
  std::shared_ptr<IoQueue> queue_;
  /// Read from whatever thread calls in and written by `shutdown`, as in both
  /// other implementations.
  std::atomic<bool> stopped_{false};
};

}  // namespace

std::shared_ptr<NetService> NetService::create(NetConfig config) {
  return std::make_shared<UnavailableNetService>(std::move(config));
}

bool networkAvailable() { return false; }

}  // namespace screenkit::net
