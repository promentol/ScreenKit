// Copyright (c) ScreenKit contributors. MIT.
//
// screenkit::Runtime over HermesHost. Everything here is about getting a bundle
// onto the JS thread and an answer back off it; the thread discipline itself
// lives in HermesHost.
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

#include "BytecodeLoader.h"
#include "HermesHost.h"

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

EvalResult evalFailure(std::string error) {
  EvalResult r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

/// Turn whatever came out of the VM into an EvalResult. A JS throw at the top
/// level is caught here rather than escaping into the host: the message and the
/// JS stack are reported and the host survives, which is the whole point of
/// having a boundary.
EvalResult guarded(const std::function<jsi::Value(jsi::Runtime&)>& body, jsi::Runtime& runtime) {
  try {
    jsi::Value value = body(runtime);
    EvalResult r;
    r.ok = true;
    try {
      if (!value.isUndefined() && !value.isNull()) {
        r.value = value.toString(runtime).utf8(runtime);
      }
    } catch (const jsi::JSIException&) {
      // A completion value that refuses to stringify is not a failure.
    }
    return r;
  } catch (const jsi::JSError& e) {
    std::string message = e.getMessage();
    const std::string& stack = e.getStack();
    if (!stack.empty()) message += "\n" + stack;
    return evalFailure(std::move(message));
  } catch (const jsi::JSIException& e) {
    return evalFailure(std::string("JSI error: ") + e.what());
  } catch (const std::exception& e) {
    return evalFailure(std::string("native exception: ") + e.what());
  } catch (...) {
    return evalFailure("unknown native exception");
  }
}

/// One-shot rendezvous between the embedder thread and the JS thread. It is
/// satisfied either by the work running or by the host cancelling it on
/// teardown, so the waiter cannot be left holding a promise nobody will keep.
struct ResultCell {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  EvalResult result;

  void settle(EvalResult r) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) return;
      done = true;
      result = std::move(r);
    }
    cv.notify_all();
  }

  EvalResult wait() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return done; });
    return std::move(result);
  }
};

class RuntimeImpl final : public Runtime {
 public:
  RuntimeImpl(std::shared_ptr<HermesHost> host, std::shared_ptr<JsExecutor> executor)
      : host_(std::move(host)), executor_(std::move(executor)) {}

  ~RuntimeImpl() override { shutdown(); }

  EvalResult evaluateBundle(const std::string& path) override {
    LoadResult loaded = mapBundleFile(path);
    if (!loaded.ok) return evalFailure(std::move(loaded.error));

    // Validate before the VM ever sees the buffer. evaluateJavaScript would take
    // raw HBC happily and then fault inside the engine on a bad one.
    loaded = validateBundle(std::move(loaded.bundle));
    if (!loaded.ok) return evalFailure(std::move(loaded.error));

    auto buffer = loaded.bundle.buffer;
    auto url = loaded.bundle.url;
    return run([buffer, url](jsi::Runtime& runtime) {
      auto prepared = runtime.prepareJavaScript(buffer, url);
      return runtime.evaluatePreparedJavaScript(prepared);
    });
  }

  EvalResult evaluateSource(std::string source, std::string url) override {
    auto buffer = std::make_shared<jsi::StringBuffer>(std::move(source));
    auto owned = std::move(url);
    return run([buffer, owned](jsi::Runtime& runtime) {
      auto prepared = runtime.prepareJavaScript(buffer, owned);
      return runtime.evaluatePreparedJavaScript(prepared);
    });
  }

  const std::shared_ptr<JsExecutor>& executor() const override { return executor_; }

  // Lifecycle and frame plumbing live on the host, which owns the loop; these
  // forward rather than duplicating the gating logic.
  void pause() override {
    if (host_) host_->pause();
  }
  void resume() override {
    if (host_) host_->resume();
  }
  bool paused() const override { return host_ && host_->paused(); }
  void tickFrame(double timestampMs) override {
    if (host_) host_->tickFrame(timestampMs);
  }
  bool setFrameFinishedCallback(std::function<void(jsi::Runtime&)> callback) override {
    return host_ && host_->setFrameFinishedCallback(std::move(callback));
  }
  bool idle() const override { return !host_ || host_->idle(); }
  std::optional<std::string> failure() const override {
    return host_ ? host_->failure() : std::nullopt;
  }
  bool registerModule(std::string name, ModuleFactory factory) override {
    return host_ && host_->registerModule(std::move(name), std::move(factory));
  }

  void shutdown() override {
    if (!host_) return;
    if (host_->onJsThread()) {
      // Same rejection as run(): stopping the JS thread from the JS thread
      // cannot join, so the host would have to detach and the runtime would
      // outlive the call in a way nobody can observe.
      log(LogLevel::Error, "screenkit",
          "shutdown() called from the JS thread -- ignored; shut the runtime down "
          "from the thread that created it");
      return;
    }
    host_->stop();
  }

 private:
  EvalResult run(std::function<jsi::Value(jsi::Runtime&)> body) {
    if (!host_) return evalFailure("runtime not started");
    if (host_->onJsThread()) {
      return evalFailure(
          "evaluate() called from the JS thread -- that is a deadlock, not a "
          "synchronous call; use executor()->invokeAsync instead");
    }

    auto cell = std::make_shared<ResultCell>();
    HermesHost::Task task;
    task.run = [cell, body](jsi::Runtime& runtime) { cell->settle(guarded(body, runtime)); };
    task.cancel = [cell] {
      cell->settle(evalFailure("runtime shut down before the bundle ran"));
    };

    if (!host_->post(std::move(task))) {
      return evalFailure("runtime has shut down");
    }
    return cell->wait();
  }

  std::shared_ptr<HermesHost> host_;
  std::shared_ptr<JsExecutor> executor_;
};

}  // namespace

std::shared_ptr<Runtime> Runtime::create(RuntimeConfig config) {
  auto host = HermesHost::start(std::move(config));
  if (!host) return nullptr;
  auto executor = std::make_shared<HermesExecutor>(std::weak_ptr<HermesHost>(host));
  return std::make_shared<RuntimeImpl>(std::move(host), std::move(executor));
}

std::uint32_t Runtime::hermesBytecodeVersion() { return supportedBytecodeVersion(); }

}  // namespace screenkit
