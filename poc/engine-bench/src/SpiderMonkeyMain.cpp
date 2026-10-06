// bench-spidermonkey: the workload on SpiderMonkey (mozjs-128), embedded the plain way -- one
// context, one global, the internal job queue, and `print`.
//
//   bench-spidermonkey --jit off|baseline|on [options] workload.js
//
// SpiderMonkey runs source: it parses lazily and compiles bytecode for a function when the
// function first runs, then tiers up on its own:
//   off       the C++ interpreter only                 (the nearest thing to Hermes' interpreter)
//   baseline  + the Baseline Interpreter and JIT        (a template JIT, like Hermes')
//   on        + Ion/Warp, the optimising JIT (default)  (what Firefox runs)
//
// A script is source (.js), parsed here, or a stencil (.stencil) -- SpiderMonkey's
// compiled form, written ahead of time by screenkit-smc in the ScreenKit runtime's
// format (runtime/core/src/spidermonkey/SpiderMonkeyRuntime.cpp: magic, build id,
// then the engine's XDR). A stencil is mapped from its file and run in place, as
// the runtime runs one: its bytecode stays in file-backed pages (RssFile).
//
// Collections are timed from SpiderMonkey's own slice and nursery callbacks, which run on the JS
// thread, so each one is a pause. Work its helper threads do (parallel marking, sweeping,
// off-thread Ion compiles) shows in the report's CPU time, not here.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include <cstdio>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <js/BuildId.h>
#include <js/CallAndConstruct.h>
#include <js/CompilationAndEvaluation.h>
#include <js/Conversions.h>
#include <js/ErrorReport.h>
#include <js/Exception.h>
#include <js/GCAPI.h>
#include <js/Initialization.h>
#include <js/PropertyAndElement.h>
#include <js/Realm.h>
#include <js/SourceText.h>
#include <js/Stack.h>
#include <js/String.h>
#include <js/Transcoding.h>
#include <js/experimental/JSStencil.h>
#include <jsapi.h>
#include <jsfriendapi.h>

#include "Bench.h"

namespace {

using Clock = std::chrono::steady_clock;

const JSClass kGlobalClass = {"BenchGlobal", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

bool print(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  std::string line;
  for (unsigned i = 0; i < args.length(); i++) {
    JS::RootedString str(cx, JS::ToString(cx, args[i]));
    if (!str) return false;
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) return false;
    if (i) line += ' ';
    line += utf8.get();
  }
  std::fprintf(stderr, "%s\n", line.c_str());
  args.rval().setUndefined();
  return true;
}

// The pending exception as "message at file:line" and its stack.
std::string takeError(JSContext* cx) {
  JS::ExceptionStack stack(cx);
  if (!JS::StealPendingExceptionStack(cx, &stack)) return "uncatchable exception (out of memory, or interrupted)";
  JS::ErrorReportBuilder report(cx);
  if (!report.init(cx, stack, JS::ErrorReportBuilder::WithSideEffects)) return "exception (no report could be built)";
  std::string text = report.toStringResult().c_str();
  if (const JSErrorReport* r = report.report(); r && r->filename) {
    text += std::string(" at ") + r->filename.c_str() + ":" + std::to_string(r->lineno);
  }
  JS::RootedObject frames(cx, stack.stack());
  JS::RootedString trace(cx);
  if (frames && JS::BuildStackString(cx, nullptr, frames, &trace)) {
    if (JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, trace)) text += std::string("\n") + utf8.get();
  }
  return text;
}

// ---- stencils, in the ScreenKit runtime's format ------------------------------------

constexpr char kStencilMagic[8] = {'S', 'K', 'S', 'T', 'N', 'C', 'L', '1'};

/// The runtime's build id: the engine refuses to decode XDR written under another,
/// and screenkit-smc wrote these under this one.
std::string stencilBuildId() {
  return std::string("screenkit-mozjs/") + JS_GetImplementationVersion() + "/" + std::to_string(sizeof(void*) * 8);
}

bool buildIdOp(JS::BuildIdCharVector* out) {
  const std::string id = stencilBuildId();
  return out->append(id.data(), id.size());
}

/// Header -> (build id, offset of the XDR payload), or false when this is not a stencil.
bool parseStencilHeader(const std::uint8_t* data, std::size_t size, std::string* id, std::size_t* payload) {
  if (size < sizeof(kStencilMagic) + 4 || std::memcmp(data, kStencilMagic, sizeof(kStencilMagic)) != 0) return false;
  const std::uint8_t* p = data + sizeof(kStencilMagic);
  const std::uint32_t length = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  const std::size_t header = (sizeof(kStencilMagic) + 4 + length + 7) / 8 * 8;
  if (header > size) return false;
  id->assign(reinterpret_cast<const char*>(p + 4), length);
  *payload = header;
  return true;
}

class SpiderMonkeyEngine;
SpiderMonkeyEngine* gEngine = nullptr;  // one engine per process; the GC callbacks find it here

class SpiderMonkeyEngine final : public bench::Engine {
 public:
  explicit SpiderMonkeyEngine(const bench::Options& options) : mode_(options.jit.empty() ? "on" : options.jit) {
    if (mode_ != "off" && mode_ != "baseline" && mode_ != "on") throw std::invalid_argument("--jit takes off, baseline or on");
    JS::SetProcessBuildIdOp(buildIdOp);
    if (!JS_Init()) throw std::runtime_error("JS_Init failed");
    cx_ = JS_NewContext(JS::DefaultHeapMaxBytes);
    if (!cx_) throw std::runtime_error("JS_NewContext failed");
    // DefaultHeapMaxBytes (32 MB) is a hard ceiling, not a tuning hint: past it allocation fails.
    // Firefox lifts it the same way.
    JS_SetGCParameter(cx_, JSGC_MAX_BYTES, 0xffffffff);
    if (!js::UseInternalJobQueues(cx_) || !JS::InitSelfHostedCode(cx_)) throw std::runtime_error(takeError(cx_));
    if (mode_ == "off") {
      JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE, 0);
      JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_BASELINE_ENABLE, 0);
    }
    if (mode_ != "on") JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_ION_ENABLE, 0);
    const std::pair<const char*, JSGCParamKey> params[] = {
        {"maxNurseryBytes", JSGC_MAX_NURSERY_BYTES},
        {"minNurseryBytes", JSGC_MIN_NURSERY_BYTES},
        {"incremental", JSGC_INCREMENTAL_GC_ENABLED},
        {"sliceMs", JSGC_SLICE_TIME_BUDGET_MS},
        {"parallelMarking", JSGC_PARALLEL_MARKING_ENABLED},
        {"maxHelperThreads", JSGC_MAX_HELPER_THREADS},
    };
    for (const auto& param : options.params) {
      const auto match = std::find_if(std::begin(params), std::end(params),
                                      [&](const auto& p) { return param.first == p.first; });
      if (match == std::end(params)) throw std::invalid_argument("bench-spidermonkey has no --param " + param.first);
      const auto value = static_cast<uint32_t>(bench::paramValue(param));
      JS_SetGCParameter(cx_, match->second, value);
      // It returns nothing and ignores a value it will not take; read it back.
      if (JS_GetGCParameter(cx_, match->second) != value) {
        throw std::invalid_argument("SpiderMonkey did not take --param " + param.first + "=" + param.second);
      }
    }

    gEngine = this;
    JS::SetGCSliceCallback(cx_, &onSlice);
    JS::AddGCNurseryCollectionCallback(cx_, &onNursery, nullptr);

    JS::RealmOptions realmOptions;
    global_ = std::make_unique<JS::PersistentRootedObject>(
        cx_, JS_NewGlobalObject(cx_, &kGlobalClass, nullptr, JS::FireOnNewGlobalHook, realmOptions));
    if (!*global_) throw std::runtime_error("JS_NewGlobalObject failed");
    realm_ = std::make_unique<JSAutoRealm>(cx_, *global_);
    if (!JS::InitRealmStandardClasses(cx_) || !JS_DefineFunction(cx_, *global_, "print", &print, 0, 0)) {
      throw std::runtime_error(takeError(cx_));
    }
  }

  ~SpiderMonkeyEngine() override {
    // Roots go before the context, the context before the engine shuts down.
    frame_.reset();
    bench_.reset();
    realm_.reset();
    global_.reset();
    JS_DestroyContext(cx_);
    JS_ShutDown();
    // Pinned bytecode may be referenced until JS_ShutDown, and not after.
    for (const auto& m : mapped_) ::munmap(m.first, m.second);
    gEngine = nullptr;
  }

  std::string name() const override { return "spidermonkey"; }
  std::string version() const override { return JS_GetImplementationVersion(); }
  std::string mode() const override { return mode_; }

  void describe(bench::Json& json) const override {
    json.field("library", bench::libraryOf(reinterpret_cast<const void*>(&JS_GetImplementationVersion)))
        .field("jit", mode_)
        .key("jitOptions")
        .open('{');
    // What the engine says it is running with, not what was asked for.
    const std::pair<const char*, JSJitCompilerOption> options[] = {
        {"baselineInterpreter", JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE},
        {"baseline", JSJITCOMPILER_BASELINE_ENABLE},
        {"ion", JSJITCOMPILER_ION_ENABLE},
        {"baselineWarmupThreshold", JSJITCOMPILER_BASELINE_WARMUP_TRIGGER},
        {"ionNormalWarmupThreshold", JSJITCOMPILER_ION_NORMAL_WARMUP_TRIGGER},
        {"offThreadCompilation", JSJITCOMPILER_OFFTHREAD_COMPILATION_ENABLE},
    };
    for (const auto& option : options) {
      uint32_t value = 0;
      if (JS_GetGlobalJitCompilerOption(cx_, option.second, &value)) json.field(option.first, static_cast<long>(value));
    }
    json.close()
        .field("maxNurseryBytes", static_cast<long>(JS_GetGCParameter(cx_, JSGC_MAX_NURSERY_BYTES)))
        .field("minNurseryBytes", static_cast<long>(JS_GetGCParameter(cx_, JSGC_MIN_NURSERY_BYTES)))
        .field("incrementalGc", JS_GetGCParameter(cx_, JSGC_INCREMENTAL_GC_ENABLED) != 0)
        .field("sliceMs", static_cast<long>(JS_GetGCParameter(cx_, JSGC_SLICE_TIME_BUDGET_MS)))
        .field("helperThreads", static_cast<long>(JS_GetGCParameter(cx_, JSGC_HELPER_THREAD_COUNT)))
        .field("stencilBuildId", stencilBuildId())
        .key("scripts")
        .open('{');
    // How each script was loaded: "source", or "stencil" (run in place).
    for (const auto& script : scripts_) json.field(script.first, script.second);
    json.close();
  }

  void evaluate(const std::string& path) override {
    if (evaluateStencil(path)) return;
    scripts_.emplace_back(path, "source");
    const auto text = bench::readFile(path);
    JS::CompileOptions options(cx_);
    options.setFileAndLine(path.c_str(), 1);
    JS::SourceText<mozilla::Utf8Unit> source;
    if (!source.init(cx_, text.data(), text.size(), JS::SourceOwnership::Borrowed)) throw std::runtime_error(takeError(cx_));
    JS::RootedValue result(cx_);
    if (!JS::Evaluate(cx_, options, source, &result)) throw std::runtime_error(takeError(cx_));
    js::RunJobs(cx_);
  }

  /// A stencil: mapped, checked for the build that wrote it, decoded to run in
  /// place, instantiated and run. False when the file is not a stencil.
  bool evaluateStencil(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    struct stat st{};
    ::fstat(fd, &st);
    const auto size = static_cast<std::size_t>(st.st_size);
    void* data = size ? ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (data == MAP_FAILED) throw std::runtime_error("cannot map " + path);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::string id;
    std::size_t payload = 0;
    if (!parseStencilHeader(bytes, size, &id, &payload)) {
      ::munmap(data, size);
      return false;
    }
    if (id != stencilBuildId()) {
      ::munmap(data, size);
      throw std::runtime_error(path + " is a stencil for " + id + ", this engine is " + stencilBuildId());
    }
    mapped_.emplace_back(data, size);  // kept until JS_ShutDown: its bytecode runs in place
    JS::CompileOptions options(cx_);
    options.setFileAndLine(path.c_str(), 1);
    JS::DecodeOptions decode(options);
    decode.borrowBuffer = true;
    decode.usePinnedBytecode = true;
    JS::TranscodeRange range(bytes + payload, size - payload);
    JS::Stencil* decoded = nullptr;
    if (JS::DecodeStencil(cx_, decode, range, &decoded) != JS::TranscodeResult::Ok || !decoded) {
      throw std::runtime_error(path + ": the stencil could not be decoded");
    }
    RefPtr<JS::Stencil> stencil = already_AddRefed<JS::Stencil>(decoded);
    JS::InstantiateOptions instantiate(options);
    JS::RootedScript script(cx_, JS::InstantiateGlobalStencil(cx_, instantiate, stencil));
    JS::RootedValue result(cx_);
    if (!script || !JS_ExecuteScript(cx_, script, &result)) throw std::runtime_error(takeError(cx_));
    js::RunJobs(cx_);
    scripts_.emplace_back(path, "stencil");
    return true;
  }

  std::string info() override { return callForString("info", JS::HandleValueArray::empty()); }
  std::string plan(const std::string& optionsJson) override {
    JS::RootedValueArray<1> args(cx_);
    args[0].setString(JS_NewStringCopyN(cx_, optionsJson.data(), optionsJson.size()));
    return callForString("plan", args);
  }
  void setView(int width, int height) override {
    JS::RootedValueArray<2> args(cx_);
    args[0].setInt32(width);
    args[1].setInt32(height);
    call("setView", args);
  }
  void enter(const std::string& scene, int load) override {
    JS::RootedValueArray<2> args(cx_);
    args[0].setString(JS_NewStringCopyN(cx_, scene.data(), scene.size()));
    args[1].setInt32(load);
    call("enter", args);
    js::RunJobs(cx_);
  }
  void frame() override {
    if (!frame_) {
      frame_ = std::make_unique<JS::PersistentRootedValue>(cx_);
      if (!JS_GetProperty(cx_, bench(), "frame", &*frame_)) throw std::runtime_error(takeError(cx_));
    }
    JS::RootedValue result(cx_);
    if (!JS_CallFunctionValue(cx_, bench(), *frame_, JS::HandleValueArray::empty(), &result)) {
      throw std::runtime_error(takeError(cx_));
    }
    js::RunJobs(cx_);
  }
  std::string exit() override { return callForString("exit", JS::HandleValueArray::empty()); }

  void collectGarbage() override { JS_GC(cx_); }

  bench::Heap heap() override {
    const auto param = [&](JSGCParamKey key) { return static_cast<double>(JS_GetGCParameter(cx_, key)); };
    bench::Heap h;
    const std::pair<const char*, JSGCParamKey> keys[] = {
        {"bytes", JSGC_BYTES},
        {"nurseryBytes", JSGC_NURSERY_BYTES},
        {"totalChunks", JSGC_TOTAL_CHUNKS},
        {"unusedChunks", JSGC_UNUSED_CHUNKS},
        {"gcNumber", JSGC_NUMBER},
        {"majorGcNumber", JSGC_MAJOR_GC_NUMBER},
        {"minorGcNumber", JSGC_MINOR_GC_NUMBER},
    };
    for (const auto& key : keys) h.raw.emplace_back(key.first, param(key.second));
    h.raw.emplace_back("majorSlices", static_cast<double>(slices_));
    h.raw.emplace_back("majorPauseMs", majorNs_ / 1e6);
    h.raw.emplace_back("minorPauseMs", minorNs_ / 1e6);
    // The tenured heap; the nursery is a fixed-size bump region on top of it.
    h.usedBytes = param(JSGC_BYTES);
    h.reservedBytes = param(JSGC_TOTAL_CHUNKS) * 1024 * 1024 + param(JSGC_NURSERY_BYTES);
    h.collections = param(JSGC_MAJOR_GC_NUMBER) + param(JSGC_MINOR_GC_NUMBER);
    h.pauseMs = (majorNs_ + minorNs_) / 1e6;
    h.maxPauseMs = maxPauseNs_ / 1e6;
    return h;
  }
  void resetPauseWindow() override { maxPauseNs_ = 0; }

 private:
  JS::HandleObject bench() {
    if (!bench_) {
      JS::RootedValue value(cx_);
      if (!JS_GetProperty(cx_, *global_, "bench", &value) || !value.isObject()) {
        throw std::runtime_error("the workload defined no `bench` object");
      }
      bench_ = std::make_unique<JS::PersistentRootedObject>(cx_, &value.toObject());
    }
    return *bench_;
  }

  void call(const char* method, const JS::HandleValueArray& args, JS::MutableHandleValue result) {
    if (!JS_CallFunctionName(cx_, bench(), method, args, result)) throw std::runtime_error(takeError(cx_));
  }
  void call(const char* method, const JS::HandleValueArray& args) {
    JS::RootedValue result(cx_);
    call(method, args, &result);
  }
  std::string callForString(const char* method, const JS::HandleValueArray& args) {
    JS::RootedValue result(cx_);
    call(method, args, &result);
    JS::RootedString str(cx_, JS::ToString(cx_, result));
    JS::UniqueChars utf8 = str ? JS_EncodeStringToUTF8(cx_, str) : nullptr;
    if (!utf8) throw std::runtime_error(takeError(cx_));
    return utf8.get();
  }

  void pause(long long ns) {
    if (ns > maxPauseNs_) maxPauseNs_ = ns;
  }
  static void onSlice(JSContext*, JS::GCProgress progress, const JS::GCDescription&) {
    if (progress == JS::GC_SLICE_BEGIN) {
      gEngine->sliceStart_ = Clock::now();
    } else if (progress == JS::GC_SLICE_END) {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - gEngine->sliceStart_).count();
      gEngine->majorNs_ += ns;
      gEngine->slices_++;
      gEngine->pause(ns);
    }
  }
  static void onNursery(JSContext*, JS::GCNurseryProgress progress, JS::GCReason, void*) {
    if (progress == JS::GCNurseryProgress::GC_NURSERY_COLLECTION_START) {
      gEngine->nurseryStart_ = Clock::now();
    } else {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - gEngine->nurseryStart_).count();
      gEngine->minorNs_ += ns;
      gEngine->pause(ns);
    }
  }

  std::string mode_;
  JSContext* cx_ = nullptr;
  std::unique_ptr<JS::PersistentRootedObject> global_, bench_;
  std::unique_ptr<JS::PersistentRootedValue> frame_;
  std::unique_ptr<JSAutoRealm> realm_;
  Clock::time_point sliceStart_, nurseryStart_;
  long long majorNs_ = 0, minorNs_ = 0, maxPauseNs_ = 0;
  std::vector<std::pair<void*, std::size_t>> mapped_;
  std::vector<std::pair<std::string, std::string>> scripts_;
  long slices_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  return bench::run(argc, argv, "bench-spidermonkey",
                    "off (C++ interpreter) | baseline (+ Baseline Interpreter and JIT) | on (+ Ion; default)",
                    "maxNurseryBytes, minNurseryBytes, incremental (0/1), sliceMs, parallelMarking (0/1), maxHelperThreads",
                    [](const bench::Options& options) { return std::make_unique<SpiderMonkeyEngine>(options); });
}
