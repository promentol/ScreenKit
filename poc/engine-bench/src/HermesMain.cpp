// bench-hermes: the workload on Hermes, through JSI -- the way the ScreenKit runtime embeds it.
//
//   bench-hermes --jit off|on|force [options] workload.hbc
//
// Bytecode is mapped from its file rather than read into memory, as the runtime's
// BytecodeLoader does, so its pages are file-backed and count under RssFile. A .js file is
// evaluated as source instead, which Hermes compiles lazily -- and a lazily compiled function is
// never JIT-compiled (EMBEDDED_LINUX_EXPERIMENTS.md, "Hermes' JIT"), so measure bytecode.
//
// The JIT exists only in a library built with HERMESVM_ALLOW_JIT; the report names the library
// that was loaded and its BUILDINFO.json, so an interpreter-only build asked for the JIT shows up.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <hermes/hermes.h>
#include <jsi/instrumentation.h>
#include <jsi/jsi.h>

#include "Bench.h"

namespace jsi = facebook::jsi;

namespace {

class MappedFile final : public jsi::Buffer {
 public:
  explicit MappedFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st{};
    ::fstat(fd, &st);
    size_ = static_cast<std::size_t>(st.st_size);
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (data_ == MAP_FAILED) throw std::runtime_error("cannot map " + path);
  }
  ~MappedFile() override { ::munmap(data_, size_); }
  std::size_t size() const override { return size_; }
  const std::uint8_t* data() const override { return static_cast<const std::uint8_t*>(data_); }

 private:
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

using Clock = std::chrono::steady_clock;

facebook::hermes::IHermesRootAPI& hermesApi() {
  static auto* api = jsi::castInterface<facebook::hermes::IHermesRootAPI>(facebook::hermes::makeHermesRootAPI());
  return *api;
}

class HermesEngine final : public bench::Engine {
 public:
  explicit HermesEngine(const bench::Options& options) : mode_(options.jit.empty() ? "off" : options.jit) {
    if (mode_ != "off" && mode_ != "on" && mode_ != "force") throw std::invalid_argument("--jit takes off, on or force");
    jsThread_ = std::this_thread::get_id();
    // Collections are timed from Hermes' own start/end events. A young collection stops the JS
    // thread; Hades does old-generation work on a background thread, which is counted apart.
    auto gc = ::hermes::vm::GCConfig::Builder().withCallback(
        [this](::hermes::vm::GCEventKind kind, const char* what) { onGc(kind, what); });
    // The runtime's own configuration (runtime/core/src/hermes/HermesHost.cpp).
    auto builder = ::hermes::vm::RuntimeConfig::Builder().withMicrotaskQueue(true).withES6BlockScoping(true);
    if (mode_ != "off") builder.withEnableJIT(true);
    if (mode_ == "force") builder.withForceJIT(true);
    for (const auto& param : options.params) {
      const auto value = bench::paramValue(param);
      if (param.first == "initHeapBytes") gc.withInitHeapSize(static_cast<::hermes::vm::gcheapsize_t>(value));
      else if (param.first == "maxHeapBytes") gc.withMaxHeapSize(static_cast<::hermes::vm::gcheapsize_t>(value));
      else if (param.first == "jitThreshold") builder.withJITThreshold(static_cast<uint32_t>(value));
      else if (param.first == "jitMemoryLimit") builder.withJITMemoryLimit(static_cast<uint32_t>(value));
      else throw std::invalid_argument("bench-hermes has no --param " + param.first);
    }
    runtime_ = facebook::hermes::makeHermesRuntime(builder.withGCConfig(gc.build()).build());
    auto& rt = *runtime_;
    rt.global().setProperty(
        rt, "print",
        jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, "print"), 1,
                                              [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t n) {
                                                std::string line;
                                                for (std::size_t i = 0; i < n; i++) {
                                                  if (i) line += ' ';
                                                  line += args[i].toString(rt).utf8(rt);
                                                }
                                                std::fprintf(stderr, "%s\n", line.c_str());
                                                return jsi::Value::undefined();
                                              }));
  }

  std::string name() const override { return "hermes"; }
  std::string version() const override {
    return "Hermes, bytecode " + std::to_string(hermesApi().getBytecodeVersion());
  }
  std::string mode() const override { return mode_; }

  void describe(bench::Json& json) const override {
    const auto library = bench::libraryOf(reinterpret_cast<const void*>(&facebook::hermes::makeHermesRootAPI));
    json.field("library", library).field("jit", mode_);
    std::string buildInfo;
    try {
      buildInfo = bench::readFile(library.substr(0, library.find_last_of('/') + 1) + "BUILDINFO.json");
    } catch (const std::exception&) {
    }
    json.key("buildInfo").raw(buildInfo);
    json.key("gcEvents").open('{');
    std::lock_guard<std::mutex> lock(gcMutex_);
    for (const auto& e : gcKinds_) json.field(e.first, e.second);
    json.close();
  }

  void evaluate(const std::string& path) override {
    std::shared_ptr<const jsi::Buffer> buffer = std::make_shared<MappedFile>(path);
    if (!hermesApi().isHermesBytecode(buffer->data(), buffer->size())) {
      buffer = std::make_shared<jsi::StringBuffer>(bench::readFile(path));
    }
    runtime_->evaluateJavaScript(buffer, path);
    runtime_->drainMicrotasks();
  }

  std::string info() override { return call("info").asString(*runtime_).utf8(*runtime_); }
  std::string plan(const std::string& optionsJson) override {
    auto& rt = *runtime_;
    return call("plan", jsi::String::createFromUtf8(rt, optionsJson)).asString(rt).utf8(rt);
  }
  void setView(int width, int height) override { call("setView", width, height); }
  void enter(const std::string& scene, int load) override {
    call("enter", jsi::String::createFromUtf8(*runtime_, scene), load);
    runtime_->drainMicrotasks();
  }
  void frame() override {
    auto& rt = *runtime_;
    if (!frame_) frame_ = std::make_unique<jsi::Function>(bench().getPropertyAsFunction(rt, "frame"));
    frame_->callWithThis(rt, bench());
    rt.drainMicrotasks();
  }
  std::string exit() override { return call("exit").asString(*runtime_).utf8(*runtime_); }

  void collectGarbage() override { runtime_->instrumentation().collectGarbage("bench"); }

  bench::Heap heap() override {
    bench::Heap h;
    const auto info = runtime_->instrumentation().getHeapInfo(false);
    const std::map<std::string, int64_t> sorted(info.begin(), info.end());
    for (const auto& entry : sorted) h.raw.emplace_back(entry.first, static_cast<double>(entry.second));
    const auto get = [&](const char* key) {
      const auto it = info.find(key);
      return it == info.end() ? -1.0 : static_cast<double>(it->second);
    };
    h.usedBytes = get("hermes_allocatedBytes");
    h.reservedBytes = get("hermes_heapSize");
    h.collections = static_cast<double>(collections_.load());
    h.pauseMs = pauseNs_.load() / 1e6;
    h.maxPauseMs = maxPauseNs_.load() / 1e6;
    h.backgroundMs = backgroundNs_.load() / 1e6;
    return h;
  }
  void resetPauseWindow() override { maxPauseNs_ = 0; }

 private:
  template <typename... Args>
  jsi::Value call(const char* method, Args&&... args) {
    auto& rt = *runtime_;
    return bench().getPropertyAsFunction(rt, method).callWithThis(rt, bench(), std::forward<Args>(args)...);
  }

  jsi::Object& bench() {
    if (!bench_) bench_ = std::make_unique<jsi::Object>(runtime_->global().getPropertyAsObject(*runtime_, "bench"));
    return *bench_;
  }

  // Must not touch the runtime (GCConfig.h). Every event is described just "GC", so a start is
  // paired with the next end on the same thread; one that ends on another thread, or anywhere
  // but the JS thread, is background work rather than a pause.
  void onGc(::hermes::vm::GCEventKind kind, const char* what) {
    const auto thread = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (kind == ::hermes::vm::GCEventKind::CollectionStart) {
      open_[thread].push_back(Clock::now());
      gcKinds_[what ? what : ""]++;
      return;
    }
    auto it = open_.find(thread);
    bool sameThread = true;
    if (it == open_.end() || it->second.empty()) {
      sameThread = false;
      it = std::find_if(open_.begin(), open_.end(), [](const auto& entry) { return !entry.second.empty(); });
      if (it == open_.end()) return;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - it->second.back()).count();
    it->second.pop_back();
    const bool pause = sameThread && thread == jsThread_;
    collections_++;
    if (pause) {
      pauseNs_ += ns;
      if (ns > maxPauseNs_) maxPauseNs_ = ns;
    } else {
      backgroundNs_ += ns;
    }
  }

  std::string mode_;
  std::shared_ptr<facebook::hermes::HermesRuntime> runtime_;
  std::unique_ptr<jsi::Object> bench_;
  std::unique_ptr<jsi::Function> frame_;
  std::thread::id jsThread_;
  mutable std::mutex gcMutex_;
  std::map<std::thread::id, std::vector<Clock::time_point>> open_;
  std::map<std::string, long> gcKinds_;
  std::atomic<long> collections_{0};
  std::atomic<long long> pauseNs_{0}, maxPauseNs_{0}, backgroundNs_{0};

 public:
  ~HermesEngine() override {
    // JSI values must go before the runtime that owns them.
    frame_.reset();
    bench_.reset();
    runtime_.reset();
  }
};

}  // namespace

int main(int argc, char** argv) {
  return bench::run(argc, argv, "bench-hermes", "off (the interpreter) | on | force (JIT every function at once)",
                    "initHeapBytes, maxHeapBytes, jitThreshold (calls), jitMemoryLimit (bytes)",
                    [](const bench::Options& options) { return std::make_unique<HermesEngine>(options); });
}
