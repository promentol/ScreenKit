// Copyright (c) ScreenKit contributors. MIT.
//
// The Unavailable client (core/src/net/NetServiceUnavailable.cpp), which the
// runtime builds on a platform with none of the three real clients. No target
// this repo ships is such a platform any more, so without this nothing would
// ever compile it, let alone run it -- and it is what keeps a new port building
// and failing requests cleanly before it has a client of its own.
//
// Its own executable because it *replaces* the platform's client:
// `NetService::create` and `networkAvailable` are defined in this binary, so the
// linker takes them from here and never pulls the real client out of
// libscreenkit-core. No fixture and no JS: the seam directly.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/src/net/NetService.h"

namespace net = screenkit::net;

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "  FAIL %s\n", what);
  ++gFailures;
}

struct Request final : net::HttpSink {
  std::atomic<int> errors{0};
  std::atomic<int> other{0};
  std::atomic<int> kind{-1};
  std::thread::id thread;
  void onHead(net::ResponseHead) override { other.fetch_add(1); }
  void onData(net::Bytes) override { other.fetch_add(1); }
  void onEnd() override { other.fetch_add(1); }
  void onError(net::NetError error, std::string) override {
    kind.store(static_cast<int>(error));
    thread = std::this_thread::get_id();
    errors.fetch_add(1);
  }
  void onUploadProgress(std::uint64_t, std::int64_t, bool) override { other.fetch_add(1); }
};

struct Socket final : net::SocketSink {
  std::atomic<int> errors{0};
  std::atomic<int> closes{0};
  std::atomic<int> code{0};
  std::atomic<bool> errorFirst{false};
  std::atomic<int> other{0};
  void onOpen(std::string, std::string) override { other.fetch_add(1); }
  void onMessage(bool, net::Bytes) override { other.fetch_add(1); }
  void onSent(std::uint64_t) override { other.fetch_add(1); }
  void onError(net::NetError, std::string) override { errors.fetch_add(1); }
  void onClose(int status, std::string, bool wasClean) override {
    errorFirst.store(errors.load() == 1 && !wasClean);
    code.store(status);
    closes.fetch_add(1);
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
  check(!net::networkAvailable(), "networkAvailable() is false");

  net::NetConfig config;
  config.name = "net.unavailable";
  auto service = net::NetService::create(config);
  check(service != nullptr, "create() returns a service");
  if (!service) return 1;

  net::Url url;
  std::string error;
  check(net::parseUrl("http://127.0.0.1:9/x", "http", url, error), "the URL parses");

  // A request fails with `unsupported`, once, and on the client's own thread --
  // never re-entering the caller from inside startRequest.
  auto request = std::make_shared<Request>();
  net::HttpRequestSpec spec;
  spec.url = url;
  service->startRequest(1, spec, request);
  check(request->errors.load() == 0, "the failure is not delivered inside startRequest");
  check(waitUntil([&] { return request->errors.load() == 1; }), "a request fails");
  check(request->kind.load() == static_cast<int>(net::NetError::Unsupported), "...as unsupported");
  check(request->thread != std::this_thread::get_id(), "...on the client's own thread");

  // A socket gets its error and then a 1006 close, and nothing else.
  auto socket = std::make_shared<Socket>();
  service->openSocket(2, url, {}, socket);
  check(waitUntil([&] { return socket->closes.load() == 1; }), "a socket closes");
  check(socket->errorFirst.load(), "...after exactly one error, uncleanly");
  check(socket->code.load() == 1006, "...with 1006");

  // Every other entry point is a harmless no-op on ids it has never seen.
  service->appendRequestBody(1, std::make_shared<const net::Bytes>(net::Bytes{1, 2, 3}));
  service->finishRequestBody(1);
  service->acknowledgeResponseData(1, 10);
  service->abortRequest(1);
  service->sendSocket(2, true, std::make_shared<const net::Bytes>(net::Bytes{'h', 'i'}));
  service->closeSocket(2, 1000, "bye");
  check(service->cookiesFor(url).empty(), "no cookies to read");
  check(!service->setCookie(url, "a=1"), "and none to set");

  // Shutdown is idempotent, and nothing is delivered after it.
  service->shutdown();
  service->shutdown();
  auto late = std::make_shared<Request>();
  service->startRequest(3, spec, late);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  check(late->errors.load() == 0 && late->other.load() == 0, "nothing after shutdown");
  service.reset();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(request->errors.load() == 1 && request->other.load() == 0, "the request heard one thing only");
  check(socket->errors.load() == 1 && socket->closes.load() == 1 && socket->other.load() == 0,
        "the socket heard two things only");

  if (gFailures > 0) {
    std::fprintf(stderr, "net-unavailable: %d failure(s)\n", gFailures);
    return 1;
  }
  std::fprintf(stderr, "net-unavailable: ok\n");
  return 0;
}
