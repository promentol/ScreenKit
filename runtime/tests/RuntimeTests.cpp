// Copyright (c) ScreenKit contributors. MIT.
//
// One case per row of the spec's I/O & edge-case matrix, plus the version pin.
// Each case is registered with CTest separately so a failure names the row.
#include <limits>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <GLES3/gl3.h>
#include <jsi/instrumentation.h>
#include <SDL3/SDL.h>

#include <screenkit/Input.h>
#include <screenkit/Instance.h>
#include <screenkit/Runtime.h>
#include <screenkit/Viewport.h>

#include "gfx/VendoredWebGL.h"
#include "gfx/FrameStats.h"

#include "../host/FramePacing.h"
#include "gfx/GlSurface.h"
#include "input/Gamepads.h"
#include "jsi/EventEmitter.h"
#include "jsi/JSIUtils.h"
#include "jsi/LazyObject.h"
#include "jsi/NativeModule.h"
#include "jsi/SharedObject.h"
#include "jsi/SharedRef.h"
#include "media/MediaPlayer.h"
#include "net/NetService.h"

#if defined(SCREENKIT_NET_TESTS_ONLY) && defined(__ANDROID__)
// Two rows of the Android client's matrix are about the client itself rather
// than about what JS sees: running with no JavaVM wired in, and the JNI global
// references it holds across a teardown. They exist only in that build.
#define SCREENKIT_ANDROID_NET_ROWS 1
#include "media/MediaPlayerAndroid.h"
#include "net/NetServiceAndroid.h"
#endif

#include "TestSupport.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#endif

namespace {

/// Wait up to `ms` on the calling thread, letting its run loop run when it is
/// the main thread on Apple: AVFoundation publishes AVPlayerItem state through
/// the main queue, so a row waiting on a player has to service it the way a
/// host's event pump does (media rows, core/src/media/MediaPlayerApple.mm).
void hostWait(int ms) {
#ifdef __APPLE__
  if (pthread_main_np() != 0 &&
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, ms / 1000.0, true) != kCFRunLoopRunFinished) {
    return;
  }
#endif
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string gFixtures = ".";

std::string fixture(const std::string& name) { return gFixtures + "/" + name; }

screenkit::RuntimeConfig testConfig() {
  screenkit::RuntimeConfig config;
  config.name = "test";
  config.maxHeapBytes = 64ull << 20;
  return config;
}

// --- matrix row: valid bytecode ---------------------------------------------
/// Drive the runtime until it has nothing left to do. `idle()` is the runtime's
/// own answer, so a test that waits on it is waiting on the same condition a
/// real host would, not on a guessed sleep.
bool pumpUntilIdle(const std::shared_ptr<screenkit::Runtime>& runtime, int timeoutMs = 4000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime->idle()) return true;
    hostWait(2);
  }
  return runtime->idle();
}

/// Index of `needle` in the fixture's recorded order, or npos.
std::size_t orderPos(const std::string& order, const char* needle) {
  return order.find(needle);
}

// --- matrix rows: timers -------------------------------------------------------
void timersOrdering() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("timers.hbc"));
  CHECK(result.ok);
  CHECK(capture.has(screenkit::LogLevel::Log, "sync done"));
  CHECK(pumpUntilIdle(runtime));

  // The fixture logs "order: a,b,c" from its last timer.
  std::string order;
  for (const auto& line : capture.lines()) {
    if (line.message.rfind("order: ", 0) == 0) order = line.message;
  }
  CHECK(!order.empty());
  if (order.empty()) return;

  // A cancelled timeout must never have run.
  CHECK(order.find("CANCELLED") == std::string::npos);

  // The load-bearing guarantee: microtasks drain before the next macrotask, so
  // both land ahead of even a 0ms timer.
  const std::size_t micro = orderPos(order, "microtask");
  const std::size_t queued = orderPos(order, "queued-microtask");
  const std::size_t t0 = orderPos(order, "timeout-0");
  const std::size_t t10 = orderPos(order, "timeout-10");
  CHECK(micro != std::string::npos && t0 != std::string::npos);
  CHECK(queued != std::string::npos);
  CHECK(micro < t0);
  CHECK(queued < t0);

  // setInterval repeated and then stopped itself: exactly two ticks.
  CHECK(orderPos(order, "interval-1") != std::string::npos);
  CHECK(orderPos(order, "interval-2") != std::string::npos);
  CHECK(orderPos(order, "interval-3") == std::string::npos);

  // The longer timeout is last.
  CHECK(t10 != std::string::npos && t10 > t0);
  if (!(micro < t0)) std::fprintf(stderr, "  order was: %s\n", order.c_str());
}

void timersCancel() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "var fired = false;"
      "var id = setTimeout(function () { fired = true; console.log('SHOULD NOT RUN'); }, 1);"
      "clearTimeout(id);"
      "setTimeout(function () { console.log('sentinel ' + fired); }, 20);",
      "cancel.js");
  CHECK(result.ok);
  CHECK(pumpUntilIdle(runtime));

  // The sentinel proves the loop really ran; the cancelled timer proves it did
  // not run everything.
  CHECK(capture.has(screenkit::LogLevel::Log, "sentinel false"));
  CHECK(!capture.has(screenkit::LogLevel::Log, "SHOULD NOT RUN"));

  // An unknown handle is ignored, not an error.
  const auto ignored = runtime->evaluateSource("clearTimeout(987654); clearTimeout(-1);", "x.js");
  CHECK(ignored.ok);

  // A 0 ms interval repeats until it is cleared, rather than firing once.
  const auto zero = runtime->evaluateSource(
      "var zeroTicks = 0;"
      "var zeroId = setInterval(function () {"
      "  if (++zeroTicks === 5) { clearInterval(zeroId); console.log('zero interval ran ' + zeroTicks); }"
      "}, 0);",
      "zero.js");
  CHECK(zero.ok);
  CHECK(pumpUntilIdle(runtime));
  CHECK(capture.has(screenkit::LogLevel::Log, "zero interval ran 5"));
}

void timersFreeze() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "setTimeout(function () { console.log('thawed'); }, 30);", "freeze.js");
  CHECK(result.ok);

  runtime->pause();
  CHECK(runtime->paused());

  // Well past the timer's deadline: a paused runtime must fire nothing.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  CHECK(!capture.has(screenkit::LogLevel::Log, "thawed"));

  runtime->resume();
  CHECK(!runtime->paused());
  CHECK(pumpUntilIdle(runtime));

  // And the timer is not lost -- a thaw re-arms it rather than dropping it.
  CHECK(capture.has(screenkit::LogLevel::Log, "thawed"));
}

void validBytecode() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  error: %s\n", result.error.c_str());

  CHECK(capture.has(screenkit::LogLevel::Log, "hello from hermes"));
  CHECK_EQ(result.value, std::string("hello-ok"));
}

// --- matrix row: source fallback ---------------------------------------------
void sourceFallback() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  // From disk: the loader must notice this is not bytecode and evaluate it as
  // source rather than refusing it.
  const auto fromFile = runtime->evaluateBundle(fixture("hello.js"));
  CHECK(fromFile.ok);
  if (!fromFile.ok) std::fprintf(stderr, "  error: %s\n", fromFile.error.c_str());
  CHECK(capture.has(screenkit::LogLevel::Log, "hello from hermes"));

  // And in memory: the dev module-runner path.
  const auto inMemory = runtime->evaluateSource("console.log('from source'); 41 + 1;", "dev.js");
  CHECK(inMemory.ok);
  CHECK_EQ(inMemory.value, std::string("42"));
  CHECK(capture.has(screenkit::LogLevel::Log, "from source"));
}

// --- ES6 block scoping --------------------------------------------------------
// Per-iteration `let` bindings, through both loaders: the bytecode because every
// hermesc call passes -Xes6-block-scoping, the source because the runtime config
// enables it. Either missing gives "3,3,3|20,20,20".
void blockScoping() {
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto bytecode = runtime->evaluateBundle(fixture("block-scoping.hbc"));
  CHECK(bytecode.ok);
  if (!bytecode.ok) std::fprintf(stderr, "  error: %s\n", bytecode.error.c_str());
  CHECK_EQ(bytecode.value, std::string("0,1,2|0,10,20"));

  const auto source = runtime->evaluateBundle(fixture("block-scoping.js"));
  CHECK(source.ok);
  if (!source.ok) std::fprintf(stderr, "  error: %s\n", source.error.c_str());
  CHECK_EQ(source.value, std::string("0,1,2|0,10,20"));
}

// --- float bit patterns ---------------------------------------------------------
// Hermes NaN-boxes its values, so a NaN read out of a Float32Array comes back
// canonical and a uint32 packed into those bits is lost. Lightning's partial quad
// upload copied colours exactly that way and drew them as rgb(0,0,192) at half
// alpha; poc/lightning3-blits/vite.screenkit.config.js rewrites the copy to
// TypedArray.prototype.set, which moves bytes and never surfaces a number.
// Both halves are pinned: `set` must stay bit-exact or that fix is wrong, and if
// the element copy ever stops canonicalising, the build transform can go.
void floatBitPatterns() {
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;
  const auto result = runtime->evaluateSource(
      "var src = new Float32Array(1); new Uint32Array(src.buffer)[0] = 0xFFE25A5A;"
      "var byElement = new Float32Array(1); byElement[0] = src[0];"
      "var bySet = new Float32Array(1); bySet.set(src.subarray(0, 1));"
      "[new Uint32Array(byElement.buffer)[0], new Uint32Array(bySet.buffer)[0]]"
      "  .map(function (v) { return v.toString(16); }).join('|');",
      "float-bits.js");
  CHECK(result.ok);
  CHECK_EQ(result.value, std::string("7fc00000|ffe25a5a"));
}

// --- unparseable source --------------------------------------------------------
void garbageSource() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("garbage.js"));
  CHECK(!result.ok);
  CHECK(!result.error.empty());

  // A syntax error is a catchable JSError, not a crash, and the host still works.
  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- bytecode too short to hold its own header ------------------------------------
void truncatedHeader() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  // 10 bytes: the magic is there but the version field is cut in half. This must
  // be refused as truncated bytecode, not silently parsed as source -- which is
  // what a plain "does it have 8 magic bytes and 12 total" check would do.
  const auto result = runtime->evaluateBundle(fixture("stub.hbc"));
  CHECK(!result.ok);
  CHECK_CONTAINS(result.error, "truncated Hermes bytecode");

  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- matrix row: version mismatch --------------------------------------------
void versionMismatch() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("version-mismatch.hbc"));
  CHECK(!result.ok);
  CHECK_CONTAINS(result.error, "version mismatch");
  // The error must name both sides, or it is useless for diagnosing a stale build.
  CHECK_CONTAINS(result.error, std::to_string(screenkit::Runtime::hermesBytecodeVersion()));
  CHECK_CONTAINS(result.error, std::to_string(screenkit::Runtime::hermesBytecodeVersion() + 1));

  // Refused before evaluation: nothing from the bundle ran.
  CHECK(!capture.has(screenkit::LogLevel::Log, "hello from hermes"));

  // The host survives it and still works.
  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- matrix row: corrupt bytecode ---------------------------------------------
void corruptBytecode() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("truncated.hbc"));
  CHECK(!result.ok);
  // Either the sanity check caught it or the header was too short to read --
  // both are refusals with a reason, which is the contract.
  const bool refusedWithReason = result.error.find("corrupt Hermes bytecode") != std::string::npos ||
                                 result.error.find("truncated Hermes bytecode") != std::string::npos;
  CHECK(refusedWithReason);
  if (!refusedWithReason) std::fprintf(stderr, "  error: %s\n", result.error.c_str());

  CHECK(!capture.has(screenkit::LogLevel::Log, "hello from hermes"));

  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- matrix row: JS throws -----------------------------------------------------
void jsThrows() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("throws.hbc"));
  CHECK(!result.ok);
  CHECK_CONTAINS(result.error, "boom from the bundle");
  // The JS stack, not just the message: two named frames are in the fixture.
  CHECK_CONTAINS(result.error, "inner");
  CHECK_CONTAINS(result.error, "outer");

  // It got far enough to log before throwing, and the host survived.
  CHECK(capture.has(screenkit::LogLevel::Log, "about to throw"));
  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- matrix row: teardown race ---------------------------------------------------
void teardownRace() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  auto executor = runtime->executor();
  std::atomic<int> ran{0};

  // Occupy the JS thread so the follow-up work has to queue behind something.
  std::mutex gate;
  std::condition_variable gateCv;
  bool entered = false;
  bool released = false;

  executor->invokeAsync([&](facebook::jsi::Runtime&) {
    std::unique_lock<std::mutex> lock(gate);
    entered = true;
    gateCv.notify_all();
    gateCv.wait(lock, [&] { return released; });
  });
  {
    std::unique_lock<std::mutex> lock(gate);
    gateCv.wait(lock, [&] { return entered; });
  }

  // Queued against a runtime that is about to go away.
  for (int i = 0; i < 8; ++i) {
    executor->invokeAsync([&ran](facebook::jsi::Runtime&) { ++ran; });
  }

  std::thread opener([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::mutex> lock(gate);
    released = true;
    gateCv.notify_all();
  });

  runtime->shutdown();
  opener.join();
  CHECK_EQ(ran.load(), 0);

  // Posted after shutdown: dropped by the stopped host.
  executor->invokeAsync([&ran](facebook::jsi::Runtime&) { ++ran; });
  CHECK_EQ(ran.load(), 0);

  // Posted after the runtime object itself is gone: dropped by the weak ref
  // failing to lock. Under ASan this is where a use-after-free would show.
  runtime.reset();
  for (int i = 0; i < 8; ++i) {
    executor->invokeAsync([&ran](facebook::jsi::Runtime&) { ++ran; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_EQ(ran.load(), 0);
}

// --- console levels ---------------------------------------------------------------
void consoleLevels() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "console.log('l', 1, true, null, undefined);"
      "console.warn('w');"
      "console.error('e');"
      "console.log({a: 1, b: [2, 3]});"
      "var cyc = {}; cyc.self = cyc; console.log(cyc);"
      // Aliases: console replaces the global wholesale, so a missing name is a
      // TypeError that kills the bundle, not a cosmetic gap.
      "console.info('i'); console.debug('d'); console.trace('t'); console.dir('r');"
      // An Error must not JSON.stringify to "{}" -- message and stack are
      // non-enumerable, and they are the entire reason for the call.
      "console.error(new Error('boom-in-console'));",
      "console.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  error: %s\n", result.error.c_str());

  CHECK(capture.has(screenkit::LogLevel::Log, "l 1 true null undefined"));
  CHECK(capture.has(screenkit::LogLevel::Warn, "w"));
  CHECK(capture.has(screenkit::LogLevel::Error, "e"));
  CHECK(capture.has(screenkit::LogLevel::Log, "{\"a\":1,\"b\":[2,3]}"));
  // A cyclic object must not take the host down with it.
  CHECK(capture.has(screenkit::LogLevel::Log, "[object Object]"));

  CHECK(capture.has(screenkit::LogLevel::Log, "i"));
  CHECK(capture.has(screenkit::LogLevel::Log, "d"));
  CHECK(capture.has(screenkit::LogLevel::Log, "t"));
  CHECK(capture.has(screenkit::LogLevel::Log, "r"));

  CHECK(capture.has(screenkit::LogLevel::Error, "boom-in-console"));
  CHECK(!capture.has(screenkit::LogLevel::Error, "{}"));
}

// --- evaluate cancelled by a racing shutdown ----------------------------------------
void evaluateCancelledByShutdown() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  auto executor = runtime->executor();

  // Occupy the JS thread so the evaluate below has to queue behind something.
  std::mutex gate;
  std::condition_variable gateCv;
  bool entered = false;
  bool released = false;
  bool posting = false;

  executor->invokeAsync([&](facebook::jsi::Runtime&) {
    std::unique_lock<std::mutex> lock(gate);
    entered = true;
    gateCv.notify_all();
    gateCv.wait(lock, [&] { return released; });
  });
  {
    std::unique_lock<std::mutex> lock(gate);
    gateCv.wait(lock, [&] { return entered; });
  }

  screenkit::EvalResult blocked;
  std::thread caller([&] {
    {
      std::lock_guard<std::mutex> lock(gate);
      posting = true;
      gateCv.notify_all();
    }
    blocked = runtime->evaluateBundle(fixture("hello.hbc"));
  });

  // Ordering: `posting` is set immediately before evaluateBundle, whose first
  // act is a mutex-protected push. The wait below plus the sleep put that push
  // far ahead of the shutdown, so the task is queued and then cancelled -- the
  // path under test -- rather than refused at post time.
  {
    std::unique_lock<std::mutex> lock(gate);
    gateCv.wait(lock, [&] { return posting; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::thread opener([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::mutex> lock(gate);
    released = true;
    gateCv.notify_all();
  });

  runtime->shutdown();
  opener.join();
  caller.join();

  CHECK(!blocked.ok);
  CHECK_CONTAINS(blocked.error, "shut down before the bundle ran");
  // It never ran, so nothing from the bundle was logged.
  CHECK(!capture.has(screenkit::LogLevel::Log, "hello from hermes"));
}

// --- evaluate from inside the JS thread ---------------------------------------------
void evaluateFromJsThread() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  screenkit::Runtime* raw = runtime.get();
  screenkit::EvalResult reentrant;
  std::mutex done;
  std::condition_variable doneCv;
  bool finished = false;

  runtime->executor()->invokeAsync([&](facebook::jsi::Runtime&) {
    // A nested evaluate would post to the thread it is already running on and
    // then wait for it: a deadlock. It must be reported, not entered.
    reentrant = raw->evaluateSource("1 + 1;", "nested.js");
    std::lock_guard<std::mutex> lock(done);
    finished = true;
    doneCv.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(done);
    const bool answered =
        doneCv.wait_for(lock, std::chrono::seconds(5), [&] { return finished; });
    CHECK(answered);  // a hang here is the deadlock this guards against
  }

  CHECK(!reentrant.ok);
  CHECK_CONTAINS(reentrant.error, "deadlock");

  // shutdown() from the JS thread cannot join its own thread, so it is rejected
  // the same way rather than detaching behind the caller's back.
  runtime->executor()->invokeAsync([raw](facebook::jsi::Runtime&) { raw->shutdown(); });

  // The runtime is unharmed by either rejected call.
  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
}

// --- a queued callback that throws --------------------------------------------------
void callbackThrows() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  // Nothing catches this on the caller's behalf, so without a boundary in the
  // dispatch loop it escapes threadMain and calls std::terminate.
  runtime->executor()->invokeAsync([](facebook::jsi::Runtime& rt) {
    throw facebook::jsi::JSError(rt, "thrown from a queued callback");
  });

  // Reaching this at all means the process survived; the result proves the JS
  // thread is still dispatching.
  const auto after = runtime->evaluateBundle(fixture("hello.hbc"));
  CHECK(after.ok);
  CHECK(capture.has(screenkit::LogLevel::Error, "thrown from a queued callback"));
  CHECK(capture.has(screenkit::LogLevel::Log, "hello from hermes"));
}

// --- the pin ------------------------------------------------------------------------
void bytecodeVersionPin() {
  // SCREENKIT_MANIFEST_BYTECODE_VERSION comes from manifest.json via CMake. If
  // these disagree, the manifest is lying about what the pinned engine accepts
  // and every .skpkg built from it would be wrong.
  CHECK_EQ(screenkit::Runtime::hermesBytecodeVersion(),
           static_cast<std::uint32_t>(SCREENKIT_MANIFEST_BYTECODE_VERSION));
}

// --- missing bundle -------------------------------------------------------------------
void missingBundle() {
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateBundle(fixture("does-not-exist.hbc"));
  CHECK(!result.ok);
  CHECK_CONTAINS(result.error, "cannot open bundle");
}

// ============================================================================
// M4: the GLES entry points
//
// Every row below runs against a real ANGLE context on an offscreen pbuffer.
// That is the same code path the windowed host takes -- same display, same
// config, same ES 3.0 context -- with a drawable that needs no window server,
// so these are not mocks standing in for GL.
// ============================================================================

/// Create the GL surface on the JS thread and install `gl` there, exactly as
/// the host does. Blocks until the JS thread has answered.
bool installGl(const std::shared_ptr<screenkit::Runtime>& runtime, int width, int height,
               std::string& error) {
  struct Bootstrap {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string error;
  };
  auto bootstrap = std::make_shared<Bootstrap>();

  runtime->executor()->invokeAsync([bootstrap, width, height](facebook::jsi::Runtime& js) {
    screenkit::gfx::GlSurface::Desc desc;
    desc.nativeLayer = nullptr;  // pbuffer
    desc.width = width;
    desc.height = height;

    std::string reason;
    std::shared_ptr<screenkit::gfx::GlSurface> surface =
        screenkit::gfx::GlSurface::create(desc, reason);
    const bool created = surface != nullptr;
    if (created) screenkit::gfx::installVendoredWebGL(js, surface);
    {
      std::lock_guard<std::mutex> lock(bootstrap->mutex);
      bootstrap->ok = created;
      bootstrap->error = std::move(reason);
      bootstrap->done = true;
    }
    bootstrap->cv.notify_all();
  });

  std::unique_lock<std::mutex> lock(bootstrap->mutex);
  if (!bootstrap->cv.wait_for(lock, std::chrono::seconds(30), [&] { return bootstrap->done; })) {
    error = "the JS thread never answered the GL bootstrap";
    return false;
  }
  error = bootstrap->error;
  return bootstrap->ok;
}

/// A runtime with `gl` installed, or nullptr with the failure already reported.
/// `height <= 0` means a square drawable, which is all most rows need; the DOM
/// rows ask for a rectangle so a transposed width/height cannot pass.
std::shared_ptr<screenkit::Runtime> glRuntime(int width = 256, int height = 0,
                                              screenkit::RuntimeConfig config = testConfig()) {
  auto runtime = screenkit::Runtime::create(std::move(config));
  CHECK(runtime != nullptr);
  if (!runtime) return nullptr;

  std::string error;
  const bool ok = installGl(runtime, width, height > 0 ? height : width, error);
  CHECK(ok);
  if (!ok) {
    std::fprintf(stderr, "  GL bootstrap failed: %s\n", error.c_str());
    return nullptr;
  }
  return runtime;
}

/// Drive frames until the runtime has nothing left to draw. `tickFrame`
/// coalesces, so this ticks far more often than the frame count it is waiting
/// for and stops on the runtime's own `idle()`.
bool pumpFrames(const std::shared_ptr<screenkit::Runtime>& runtime, int timeoutMs = 20000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  int tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime->idle()) return true;
    runtime->tickFrame(tick++ * (1000.0 / 60.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return runtime->idle();
}

// --- matrix row: context creation ---------------------------------------------
void glContext() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  // ANGLE's Metal backend was asked for by name, so the renderer string is the
  // evidence that no silent fallback happened.
  const auto renderer = runtime->evaluateSource("gl.getParameter(gl.RENDERER);", "renderer.js");
  CHECK(renderer.ok);
  CHECK_CONTAINS(renderer.value, "ANGLE");
  CHECK_CONTAINS(renderer.value, "Metal");

  // ES 3.0 exactly -- 3.1 and 3.2 are unreachable on Apple (Architecture.md 9).
  const auto version = runtime->evaluateSource("gl.getParameter(gl.VERSION);", "version.js");
  CHECK(version.ok);
  CHECK_CONTAINS(version.value, "3.0");

  // Headers and libraries from one ANGLE commit. The libraries print theirs in
  // GL_VERSION ("git hash: aaebda1c5a40"); the headers are whatever fetch.mjs
  // checked out for manifest angle.headers.ref. Enum values and struct layouts
  // in eglext_angle.h move between commits, and nothing else would notice.
  const std::string headersRef = SCREENKIT_ANGLE_HEADERS_REF;
  CHECK_EQ(headersRef.size(), static_cast<std::size_t>(40));
  CHECK_CONTAINS(version.value, "git hash: " + headersRef.substr(0, 12));

  // A constant from the table, and an entry point that uses it.
  const auto attribs =
      runtime->evaluateSource("gl.getParameter(gl.MAX_VERTEX_ATTRIBS) >= 8;", "limits.js");
  CHECK(attribs.ok);
  CHECK_EQ(attribs.value, std::string("true"));

  // The drawable has the depth and stencil buffers the context reports, and the
  // depth test works on it: a far quad drawn after a near one stays hidden.
  const auto depth = runtime->evaluateSource(
      "var a = gl.getContextAttributes();"
      "function sh(k, src) { var s = gl.createShader(k); gl.shaderSource(s, src); gl.compileShader(s); return s; }"
      "var p = gl.createProgram();"
      "gl.attachShader(p, sh(gl.VERTEX_SHADER, 'attribute vec3 a; void main(){ gl_Position = vec4(a, 1.0); }'));"
      "gl.attachShader(p, sh(gl.FRAGMENT_SHADER, 'precision mediump float; uniform vec4 c; void main(){ gl_FragColor = c; }'));"
      "gl.linkProgram(p); gl.useProgram(p);"
      "var b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b);"
      "function quad(z, r, g) {"
      "  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,z, 1,-1,z, -1,1,z, 1,1,z]), gl.STATIC_DRAW);"
      "  var l = gl.getAttribLocation(p, 'a'); gl.enableVertexAttribArray(l); gl.vertexAttribPointer(l, 3, gl.FLOAT, false, 0, 0);"
      "  gl.uniform4f(gl.getUniformLocation(p, 'c'), r, g, 0, 1); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);"
      "}"
      "gl.enable(gl.DEPTH_TEST); gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);"
      "quad(-0.5, 1, 0); quad(0.5, 0, 1);"
      "var px = new Uint8Array(4); gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "gl.disable(gl.DEPTH_TEST);"
      "[a.depth, a.stencil, gl.getParameter(gl.DEPTH_BITS) >= 24, gl.getParameter(gl.STENCIL_BITS) >= 8, px[0] + ',' + px[1]].join(' ');",
      "depth.js");
  CHECK(depth.ok);
  if (!depth.ok) std::fprintf(stderr, "  %s\n", depth.error.c_str());
  CHECK_EQ(depth.value, std::string("true true true true 255,0"));

  // Numeric arguments convert as WebIDL converts them: an array, a string, a
  // boolean and null are ToNumber'd, and integers wrap -- Phaser hands
  // uniform1i a one-element array.
  const auto webidl = runtime->evaluateSource(
      "gl.clearColor([0.5], '0.25', true, null);"
      "gl.viewport([1], '2', 30.9, 40);"
      "var c = gl.getParameter(gl.COLOR_CLEAR_VALUE), v = gl.getParameter(gl.VIEWPORT);"
      "var threw; try { gl.clearColor(Symbol('x'), 0, 0, 0); threw = 'no throw'; } catch (e) { threw = e.name; }"
      "[Array.prototype.join.call(c, ','), v[0], v[1], v[2], threw].join(' ');",
      "webidl.js");
  CHECK(webidl.ok);
  if (!webidl.ok) std::fprintf(stderr, "  %s\n", webidl.error.c_str());
  CHECK_EQ(webidl.value, std::string("0.5,0.25,1,0 1 2 30 TypeError"));
}

// --- matrix row: shader compile error -------------------------------------------
void glShaderError() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "var s = gl.createShader(gl.FRAGMENT_SHADER);"
      "gl.shaderSource(s, '#version 300 es\\nvoid main() { this is not glsl }');"
      "gl.compileShader(s);"
      "var ok = gl.getShaderParameter(s, gl.COMPILE_STATUS);"
      "var log = gl.getShaderInfoLog(s);"
      "console.log('compiled=' + ok + ' loglen=' + log.length);"
      "console.log('shader log: ' + log);"
      "gl.deleteShader(s);"
      "ok;",
      "badshader.js");
  CHECK(result.ok);
  // Compilation failed, and the reason reached JS as text rather than being
  // swallowed natively.
  CHECK_EQ(result.value, std::string("false"));
  CHECK(capture.has(screenkit::LogLevel::Log, "compiled=false"));
  CHECK(!capture.has(screenkit::LogLevel::Log, "loglen=0"));

  // The host survives it and the context is still usable.
  const auto after = runtime->evaluateSource("gl.getError();", "after.js");
  CHECK(after.ok);
}

// --- matrix row: program link error --------------------------------------------
void glLinkError() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  // The varyings disagree: the vertex stage writes a vec3 the fragment stage
  // declares as a vec2, which compiles on both sides and fails at link.
  const auto result = runtime->evaluateSource(
      "function sh(t, src) {"
      "  var s = gl.createShader(t); gl.shaderSource(s, src); gl.compileShader(s);"
      "  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))"
      "    throw new Error('unexpected compile failure: ' + gl.getShaderInfoLog(s));"
      "  return s;"
      "}"
      "var v = sh(gl.VERTEX_SHADER, '#version 300 es\\nout vec3 v_x;\\n"
      "void main() { v_x = vec3(1.0); gl_Position = vec4(0.0,0.0,0.0,1.0); }');"
      "var f = sh(gl.FRAGMENT_SHADER, '#version 300 es\\nprecision mediump float;\\n"
      "in vec2 v_x;\\nout vec4 c;\\nvoid main() { c = vec4(v_x, 0.0, 1.0); }');"
      "var p = gl.createProgram();"
      "gl.attachShader(p, v); gl.attachShader(p, f); gl.linkProgram(p);"
      "var linked = gl.getProgramParameter(p, gl.LINK_STATUS);"
      "var log = gl.getProgramInfoLog(p);"
      "console.log('linked=' + linked + ' loglen=' + log.length);"
      "console.log('program log: ' + log);"
      "gl.deleteProgram(p); gl.deleteShader(v); gl.deleteShader(f);"
      "linked;",
      "badlink.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  error: %s\n", result.error.c_str());
  // WebGL returns a boolean here, unlike glGetProgramiv's GLint.
  CHECK_EQ(result.value, std::string("false"));
  CHECK(capture.has(screenkit::LogLevel::Log, "linked=false"));
  CHECK(!capture.has(screenkit::LogLevel::Log, "loglen=0"));

  const auto after = runtime->evaluateSource("gl.getError();", "after.js");
  CHECK(after.ok);
}

// --- matrix row: typed-array upload ---------------------------------------------
void glTypedArray() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  // A plain Float32Array, and a subarray view whose byteOffset is not zero --
  // the case a naive "read .buffer and upload all of it" would get wrong.
  const auto upload = runtime->evaluateSource(
      "var b = gl.createBuffer();"
      "gl.bindBuffer(gl.ARRAY_BUFFER, b);"
      "var whole = new Float32Array([0,1,2,3,4,5,6,7]);"
      "gl.bufferData(gl.ARRAY_BUFFER, whole, gl.STATIC_DRAW);"
      "var e1 = gl.getError();"
      "var view = whole.subarray(2, 6);"
      "gl.bufferData(gl.ARRAY_BUFFER, view, gl.STATIC_DRAW);"
      "var e2 = gl.getError();"
      "gl.bufferData(gl.ARRAY_BUFFER, 64, gl.DYNAMIC_DRAW);"  // the size form
      "var e3 = gl.getError();"
      "gl.deleteBuffer(b);"
      "[view.byteOffset, view.byteLength, e1, e2, e3].join(',');",
      "upload.js");
  CHECK(upload.ok);
  if (!upload.ok) std::fprintf(stderr, "  error: %s\n", upload.error.c_str());
  CHECK_EQ(upload.value, std::string("8,16,0,0,0"));

  // Bad input is gl-buffer-data's row; this one is the happy path.
  const auto after = runtime->evaluateSource("gl.getError();", "after.js");
  CHECK(after.ok);
  CHECK_EQ(after.value, std::string("0"));
}

// --- a view that lies about its own range -------------------------------------
//
// JSI can only read an ArrayBufferView's *properties*, and an instance can
// shadow `byteOffset` or `byteLength` with anything -- so bytesOf range-checks
// what it reads against the real buffer before forming a pointer.
//
// JSI's TypedArray accessors look like the fix and are not: jsi.h says "the
// 'byteLength' property of", and means it. With `byteLength` shadowed to 4,
// `view.byteLength(runtime)` answers 4, so they are the same property reads
// spelled differently. Reading them and trusting them uploaded 4 bytes from
// offset 999999 -- a read outside the buffer, which is what this test was
// written to catch and did.
//
// A browser reads the internal slots and ignores the shadowing entirely, so
// Chrome uploads the real 32 bytes where this throws. That divergence is
// deliberate: refusing is the safe answer available to a binding that cannot
// see slots.
void glTypedArrayShadowed() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  const auto shadowed = runtime->evaluateSource(
      "var b = gl.createBuffer();"
      "gl.bindBuffer(gl.ARRAY_BUFFER, b);"
      "var a = new Float32Array([1,2,3,4,5,6,7,8]);"  // 32 real bytes at offset 0
      "Object.defineProperty(a, 'byteLength', { value: 4 });"
      "Object.defineProperty(a, 'byteOffset', { value: 999999 });"
      "var threw = '';"
      "try { gl.bufferData(gl.ARRAY_BUFFER, a, gl.STATIC_DRAW); }"
      "catch (e) { threw = e.constructor.name; }"
      "var size = gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE);"
      "gl.deleteBuffer(b);"
      "[a.byteLength, a.byteOffset, threw, size].join(',');",
      "shadowed.js");
  CHECK(shadowed.ok);
  if (!shadowed.ok) std::fprintf(stderr, "  error: %s\n", shadowed.error.c_str());
  // The object still says 4 and 999999; nothing was uploaded and a TypeError
  // came back instead of a pointer outside the buffer.
  CHECK_EQ(shadowed.value, std::string("4,999999,TypeError,0"));

  // A plain object wearing a view's properties is not a view. `isTypedArray` is
  // a real type check on the object, not a look at what it carries, which is
  // what makes it safe to ask before falling back to `ArrayBuffer.isView`.
  const auto fake = runtime->evaluateSource(
      "var b2 = gl.createBuffer();"
      "gl.bindBuffer(gl.ARRAY_BUFFER, b2);"
      "var f = { buffer: new ArrayBuffer(8), byteOffset: 0, byteLength: 8 };"
      "gl.bufferData(gl.ARRAY_BUFFER, f, gl.STATIC_DRAW);"
      "var size = gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE);"
      "gl.getError(); gl.deleteBuffer(b2);"
      "String(size);",
      "fake.js");
  CHECK(fake.ok);
  // Not read as a view: it lands on the `bufferData(target, size, usage)`
  // overload, where an object converts to 0.
  CHECK_EQ(fake.value, std::string("0"));

  // A DataView is an ArrayBufferView that isTypedArray does not cover, so it
  // goes the long way round. It has to keep working.
  const auto dataView = runtime->evaluateSource(
      "var b3 = gl.createBuffer();"
      "gl.bindBuffer(gl.ARRAY_BUFFER, b3);"
      "var dv = new DataView(new ArrayBuffer(24), 8, 12);"
      "gl.bufferData(gl.ARRAY_BUFFER, dv, gl.STATIC_DRAW);"
      "var e = gl.getError();"
      "var size = gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE);"
      "gl.deleteBuffer(b3);"
      "[e, size].join(',');",
      "dataview.js");
  CHECK(dataView.ok);
  if (!dataView.ok) std::fprintf(stderr, "  error: %s\n", dataView.error.c_str());
  CHECK_EQ(dataView.value, std::string("0,12"));
}

// --- matrix row: GL error surfaced ----------------------------------------------
void glErrorPassthrough() {
  test::LogCapture capture;
  auto runtime = glRuntime();
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "gl.enable(0x9999);"              // not a capability
      "var e1 = gl.getError();"
      "var e2 = gl.getError();"         // GL clears the flag on read
      "gl.drawArrays(0x9999, 0, 3);"    // not a primitive mode
      "var e3 = gl.getError();"
      "[e1 === gl.INVALID_ENUM, e2 === gl.NO_ERROR, e3 === gl.INVALID_ENUM].join(',');",
      "glerror.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  error: %s\n", result.error.c_str());
  CHECK_EQ(result.value, std::string("true,true,true"));

  // Nothing asserted and nothing aborted: the runtime is still answering.
  const auto after = runtime->evaluateSource("1 + 1;", "after.js");
  CHECK(after.ok);
  CHECK_EQ(after.value, std::string("2"));
}

// --- matrix row: the triangle, and the frame loop -------------------------------
void glTriangle() {
  test::LogCapture capture;
  auto runtime = glRuntime(256);
  if (!runtime) return;

  // The fixture animates forever in the app; this bounds it so the row can end.
  const auto limit = runtime->evaluateSource("globalThis.__triangleFrames = 120;", "limit.js");
  CHECK(limit.ok);

  const auto loaded = runtime->evaluateBundle(fixture("triangle.hbc"));
  CHECK(loaded.ok);
  if (!loaded.ok) {
    std::fprintf(stderr, "  error: %s\n", loaded.error.c_str());
    return;
  }
  CHECK(capture.has(screenkit::LogLevel::Log, "triangle: first frame requested"));

  // 120 frames of rAF, driven by the same tickFrame the host calls.
  CHECK(pumpFrames(runtime));
  CHECK(capture.has(screenkit::LogLevel::Log, "triangle: 120 frames, 0 gl errors"));
  // Not one GL error along the way, and none left behind.
  CHECK(!capture.has(screenkit::LogLevel::Error, "gl error"));

  // The pbuffer still holds the last frame, so this is the drawn triangle
  // rather than an inference from "no error was reported". The centre of the
  // drawable is inside the triangle at every rotation, because the triangle
  // is rotated about the origin and contains it.
  const auto pixel = runtime->evaluateSource(
      "var px = new Uint8Array(4);"
      "gl.readPixels(gl.drawingBufferWidth >> 1, gl.drawingBufferHeight >> 1, 1, 1,"
      "              gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "[px[0], px[1], px[2], px[3]].join(',');",
      "readback.js");
  CHECK(pixel.ok);
  if (!pixel.ok) std::fprintf(stderr, "  error: %s\n", pixel.error.c_str());

  // The clear colour is (0.02, 0.01, 0.05) -- about 5,3,13 in bytes. Anything
  // the triangle drew is far brighter than that.
  int r = 0;
  int g = 0;
  int b = 0;
  int a = 0;
  const int parsed = std::sscanf(pixel.value.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a);
  CHECK_EQ(parsed, 4);
  CHECK_EQ(a, 255);
  CHECK(r + g + b > 120);
  if (!(r + g + b > 120)) {
    std::fprintf(stderr, "  centre pixel was %s -- that is the clear colour, not a triangle\n",
                 pixel.value.c_str());
  }

  // A corner is outside the triangle, so it must still be the clear colour --
  // otherwise a full-screen fill would pass the test above.
  const auto corner = runtime->evaluateSource(
      "var px = new Uint8Array(4);"
      "gl.readPixels(1, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "(px[0] + px[1] + px[2]);",
      "corner.js");
  CHECK(corner.ok);
  CHECK(std::stoi(corner.value) < 60);
}

// --- matrix row: teardown with GL alive -----------------------------------------
void glTeardown() {
  test::LogCapture capture;
  {
    auto runtime = glRuntime(64);
    if (!runtime) return;

    // Leave real GL objects behind: buffers, a vertex array, a linked program
    // and a pending rAF callback. Nothing is deleted from JS, so teardown is
    // what has to release the context with all of it still live.
    const auto result = runtime->evaluateSource(
        "var b = gl.createBuffer();"
        "gl.bindBuffer(gl.ARRAY_BUFFER, b);"
        "gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(1024), gl.STATIC_DRAW);"
        "var vao = gl.createVertexArray(); gl.bindVertexArray(vao);"
        "requestAnimationFrame(function () { console.log('SHOULD NOT RUN'); });"
        "gl.getError();",
        "leaky.js");
    CHECK(result.ok);
    CHECK_EQ(result.value, std::string("0"));

    // The EGL display, context and surface are destroyed on the JS thread
    // because the only owners are JS-thread-owned: the `gl` host functions and
    // the loop's frame hook. Under ASan a leak or a cross-thread destroy here
    // is what fails the row.
    runtime->shutdown();
  }
  CHECK(!capture.has(screenkit::LogLevel::Log, "SHOULD NOT RUN"));
  // Released by teardown on the JS thread, not by a finalizer on Hermes' GC
  // thread after the runtime's heap is gone.
  CHECK(!capture.has(screenkit::LogLevel::Warn, "GL surface destroyed off the thread that created it"));

  // A second runtime must be able to take the display again -- eglTerminate
  // released it rather than leaving it initialised for the process.
  auto again = glRuntime(64);
  CHECK(again != nullptr);
  if (!again) return;
  const auto renderer = again->evaluateSource("gl.getParameter(gl.RENDERER);", "again.js");
  CHECK(renderer.ok);
  CHECK_CONTAINS(renderer.value, "ANGLE");
}

// --- how the windowed loop paces requestAnimationFrame ----------------------------
//
// host/FramePacing.h. Both halves are cheap to get wrong and expensive to
// notice: a loop that ticks too fast only shows as CPU burnt on a still screen,
// and one that waits on a frame the display already paced only shows as a
// halved frame rate under load -- which is how the stress benchmark, not a
// test, caught it the first time.
void framePacingInterval() {
  const double sixty = screenkit::host::frameIntervalMsForRefreshRate(60.0);
  CHECK(std::abs(sixty - 1000.0 / 60.0) < 1e-9);
  CHECK(std::abs(screenkit::host::frameIntervalMsForRefreshRate(120.0) - 1000.0 / 120.0) < 1e-9);
  CHECK(std::abs(screenkit::host::frameIntervalMsForRefreshRate(59.94) - 1000.0 / 59.94) < 1e-9);

  // SDL reports 0 when it does not know the rate -- some Wayland compositors,
  // every headless display -- and anything absurd is a display lying about
  // itself. Both take 60 Hz rather than a spin or a one-frame-a-second app.
  for (double bogus : {0.0, -1.0, 0.5, 19.0, 1001.0, 1e9}) {
    CHECK(std::abs(screenkit::host::frameIntervalMsForRefreshRate(bogus) - sixty) < 1e-9);
  }
  CHECK(std::abs(screenkit::host::frameIntervalMsForRefreshRate(
                     std::numeric_limits<double>::quiet_NaN()) -
                 sixty) < 1e-9);
}

void framePacingDeadline() {
  constexpr double kInterval = 1000.0 / 60.0;

  // A frame that painted was paced by the swap, which blocks on the refresh.
  // It is due again as soon as the loop can yield: holding it to a deadline on
  // top of the swap would pace it twice and halve the rate.
  const double afterPaint =
      screenkit::host::nextFrameDeadlineMs(100.0, 100.0, 118.0, kInterval, true);
  CHECK(afterPaint <= 118.0 + 2.0);
  CHECK(afterPaint > 118.0);

  // A frame that painted nothing advances from the deadline, not from now, so
  // the rate does not drift with what the frame happened to cost.
  CHECK(std::abs(screenkit::host::nextFrameDeadlineMs(100.0, 100.0, 104.0, kInterval, false) -
                 (100.0 + kInterval)) < 1e-9);
  CHECK(std::abs(screenkit::host::nextFrameDeadlineMs(100.0, 100.5, 112.0, kInterval, false) -
                 (100.0 + kInterval)) < 1e-9);

  // Behind, either because the frame overran or because the loop sat idle past
  // a stale deadline: restart from this frame rather than firing the run of
  // catch-up frames the old deadline is owed.
  CHECK(std::abs(screenkit::host::nextFrameDeadlineMs(100.0, 500.0, 505.0, kInterval, false) -
                 (500.0 + kInterval)) < 1e-9);
  const double afterIdle =
      screenkit::host::nextFrameDeadlineMs(100.0, 60000.0, 60001.0, kInterval, false);
  CHECK(std::abs(afterIdle - (60000.0 + kInterval)) < 1e-9);

  // Ticking at the rate the deadline sets keeps that rate exactly, with no
  // drift accumulating over a minute of frames.
  double deadline = 0.0;
  double tick = 0.0;
  for (int i = 0; i < 3600; ++i) {
    tick = deadline;
    deadline = screenkit::host::nextFrameDeadlineMs(deadline, tick, tick + 3.0, kInterval, false);
  }
  CHECK(std::abs(deadline - 3600.0 * kInterval) < 1e-6);
}

// --- frame time is reported, not gated -------------------------------------------
void glFrameStats() {
  test::LogCapture capture;
  screenkit::gfx::FrameStats stats("frames");

  // Five seconds at a perfect 60 Hz. A report window closes on the first frame
  // at or past the interval, so the count can drift by one against wall time --
  // "roughly once a second" is the claim, and that is what is asserted.
  constexpr int kFrames = 301;  // t = 0 .. 5000 ms inclusive
  for (int i = 0; i < kFrames; ++i) stats.frame(i * (1000.0 / 60.0));

  int reports = 0;
  for (const auto& line : capture.lines()) {
    if (line.message.rfind("frame time", 0) == 0) ++reports;
  }
  CHECK(reports >= 4 && reports <= 5);
  if (!(reports >= 4 && reports <= 5)) {
    std::fprintf(stderr, "  %d report lines over 5 seconds:\n%s", reports,
                 capture.joined().c_str());
  }
  CHECK_EQ(stats.totalFrames(), static_cast<std::size_t>(kFrames));

  // The number itself has to be right, not merely present -- a rate nobody can
  // trust is worse than none. 60 Hz in, 60 fps and 16.67 ms out.
  CHECK(capture.has(screenkit::LogLevel::Log, "(60.0 fps)"));
  CHECK(capture.has(screenkit::LogLevel::Log, "16.67 ms mean"));

  // The first frame starts the window rather than being counted as one
  // enormous interval, which would show up as a nonsense `worst`.
  CHECK(!capture.has(screenkit::LogLevel::Log, "ms worst over 0 frames"));
}

// ============================================================================
// The DOM shim
//
// One row per line of the spec's I/O matrix. Every row runs the real prelude
// bytecode -- the same dom-shim.hbc the host loads -- against a real ANGLE
// context, because the whole claim of the shim is that it hands out the context
// the host already made. A mocked `gl` would test the mock.
// ============================================================================

/// Evaluate the DOM shim prelude, exactly as the host does: after the graphics
/// bootstrap, before anything that uses `document`.
bool installDomShim(const std::shared_ptr<screenkit::Runtime>& runtime) {
  // The build's own prelude, unless the environment names another -- which is
  // how the cross-compiled net binary finds the copy that was pushed beside it
  // (tools/android/android.sh test).
  const char* override_ = std::getenv("SCREENKIT_DOM_SHIM_HBC");
  const std::string shim = override_ != nullptr && *override_ != '\0' ? override_ : SCREENKIT_DOM_SHIM_HBC;
  const screenkit::EvalResult result = runtime->evaluateBundle(shim);
  CHECK(result.ok);
  if (!result.ok) {
    std::fprintf(stderr, "  prelude failed: %s\n", result.error.c_str());
    return false;
  }
  // The prelude's completion value names itself, so a bundle that happened to
  // load and do nothing cannot pass for it.
  CHECK_EQ(result.value, std::string("screenkit-dom-shim/1"));
  return true;
}

/// A runtime with `gl` and the prelude on it, drawable `width` x `height`.
std::shared_ptr<screenkit::Runtime> domRuntime(int width = 320, int height = 180) {
  auto runtime = glRuntime(width, height);
  if (!runtime) return nullptr;
  if (!installDomShim(runtime)) return nullptr;
  return runtime;
}

/// Evaluate `source` and return its completion value, failing the row with the
/// JS error if it threw. Every DOM row is a one-line question about the shim, so
/// this keeps the rows about the answer rather than about the plumbing.
std::string domEval(const std::shared_ptr<screenkit::Runtime>& runtime, const char* source) {
  const screenkit::EvalResult result = runtime->evaluateSource(source, "dom.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  %s\n  threw: %s\n", source, result.error.c_str());
  return result.value;
}

// --- matrix row: canvas creation ------------------------------------------------
void domCanvasCreate() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  CHECK_EQ(domEval(runtime,
                   "globalThis.c = document.createElement('canvas');"
                   "[typeof c, typeof c.width, typeof c.height, typeof c.getContext].join(',');"),
           std::string("object,number,number,function"));

  // An element, not a bare bag of three members: HTMLElement.style and
  // Node.appendChild are traced on every element, canvas included.
  CHECK_EQ(domEval(runtime, "[c.tagName, typeof c.style, typeof c.appendChild].join(',');"),
           std::string("CANVAS,object,function"));

  // createElementNS, the way three.js makes its canvas: the HTML namespace gives
  // the same backed canvas; another namespace a plain, inert Element.
  CHECK_EQ(domEval(runtime,
                   "var ns = document.createElementNS('http://www.w3.org/1999/xhtml', 'canvas');"
                   "var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg:rect');"
                   "[ns instanceof HTMLCanvasElement, ns.getContext('webgl2') === gl, ns.namespaceURI, c.namespaceURI === ns.namespaceURI,"
                   " svg instanceof Element, svg instanceof HTMLElement, svg.namespaceURI, svg.tagName, svg.localName].join(',');"),
           std::string("true,true,http://www.w3.org/1999/xhtml,true,true,false,http://www.w3.org/2000/svg,svg:rect,rect"));

  // <video> is a media element over the platform's player (AVPlayer here), so
  // canPlayType answers for what it plays; <audio> plays nothing and answers ""
  // for every type, as a browser says for a format it cannot play.
  CHECK_EQ(domEval(runtime,
                   "var v = document.createElement('video'), a = document.createElement('audio');"
                   "[v instanceof HTMLVideoElement, v instanceof HTMLMediaElement, a instanceof HTMLAudioElement,"
                   " JSON.stringify(v.canPlayType('video/mp4')), JSON.stringify(v.canPlayType('application/dash+xml')),"
                   " JSON.stringify(v.canPlayType('video/mp4; codecs=\"avc1.42E01E\"')),"
                   " JSON.stringify(a.canPlayType('audio/ogg')), JSON.stringify(a.canPlayType('audio/mp4')),"
                   " v.HAVE_NOTHING, HTMLMediaElement.HAVE_ENOUGH_DATA, v.tagName].join(',');"),
           std::string("true,true,true,\"maybe\",\"\",\"probably\",\"\",\"\",0,4,VIDEO"));
}

// --- matrix row: GL handoff -----------------------------------------------------
void domGlHandoff() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  // The *same* object as the `gl` global -- not a new context and not a wrapper.
  CHECK_EQ(domEval(runtime,
                   "globalThis.c = document.createElement('canvas');"
                   "globalThis.ctx = c.getContext('webgl');"
                   "ctx === gl;"),
           std::string("true"));

  // Asking twice on the same canvas is the web contract: the same object back,
  // never a second context.
  CHECK_EQ(domEval(runtime, "c.getContext('webgl') === ctx;"), std::string("true"));

  // `gl.canvas` is the canvas that owns the context: Lightning sizes its
  // resolution uniform from `glw.canvas.width` on every render op.
  CHECK_EQ(domEval(runtime, "[ctx.canvas === c, ctx.canvas.width === c.width].join(',');"),
           std::string("true,true"));

  // Reached through document.createElement, ANGLE's Metal backend is still what
  // answers -- so nothing about the handoff swapped the context out.
  const std::string renderer = domEval(runtime, "ctx.getParameter(ctx.RENDERER);");
  CHECK_CONTAINS(renderer, "ANGLE");
  CHECK_CONTAINS(renderer, "Metal");
}

// --- matrix row: WebGL2 request -------------------------------------------------
void domWebgl2() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  // Lightning asks `e ? 'webgl2' : 'webgl'` and falls back to
  // 'experimental-webgl'. ANGLE grants ES 3.0, which is what WebGL2 is defined
  // against, so all three spellings resolve to one honest context.
  CHECK_EQ(domEval(runtime,
                   "var c = document.createElement('canvas');"
                   "[c.getContext('webgl2') === gl, c.getContext('webgl') === gl,"
                   " c.getContext('experimental-webgl') === gl].join(',');"),
           std::string("true,true,true"));

  CHECK_EQ(domEval(runtime, "gl.getParameter(gl.VERSION).indexOf('3.0') >= 0;"),
           std::string("true"));

  // Two separate interfaces, as in a browser: a WebGL2 context is not a
  // WebGLRenderingContext (PixiJS picks its WebGL version from exactly this), and
  // the WebGL2 prototype has the WebGL1 methods and constants of its own.
  CHECK_EQ(domEval(runtime,
                   "[gl instanceof WebGL2RenderingContext, gl instanceof WebGLRenderingContext,"
                   " Object.getPrototypeOf(WebGL2RenderingContext.prototype) === Object.prototype,"
                   " WebGL2RenderingContext.prototype.hasOwnProperty('drawArrays'),"
                   " WebGL2RenderingContext.prototype.hasOwnProperty('createVertexArray'),"
                   " WebGLRenderingContext.prototype.hasOwnProperty('createVertexArray'),"
                   " gl.TRIANGLES === WebGL2RenderingContext.TRIANGLES].join(',');"),
           std::string("true,false,true,true,true,false,true"));
}

// --- matrix row: dimensions -----------------------------------------------------
void domDimensions() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  // The real drawable, and a rectangle so a transposed pair cannot pass.
  CHECK_EQ(domEval(runtime,
                   "globalThis.c = document.createElement('canvas');"
                   "[c.width, c.height].join('x');"),
           std::string("320x180"));

  // Same number, not merely the same value: both come from the GL surface.
  CHECK_EQ(domEval(runtime,
                   "c.width === gl.drawingBufferWidth && c.height === gl.drawingBufferHeight;"),
           std::string("true"));
}

// --- matrix row: dimension write ------------------------------------------------
void domDimensionWrite() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  // On the canvas holding the GL surface a write is legal and inert: it must not
  // throw -- Lightning writes once at startup -- and a read after it is still
  // the true size.
  const screenkit::EvalResult wrote = runtime->evaluateSource(
      "var c = document.createElement('canvas'); c.getContext('webgl'); document.body.appendChild(c);"
      "c.width = 640; c.height = 480;"
      "[c.width, c.height, gl.drawingBufferWidth, gl.drawingBufferHeight].join(',');",
      "write.js");
  CHECK(wrote.ok);
  if (!wrote.ok) std::fprintf(stderr, "  threw: %s\n", wrote.error.c_str());
  CHECK_EQ(wrote.value, std::string("320,180,320,180"));

  // The ignored write is visible rather than silent, and said once -- Lightning
  // reads canvas.width eleven times a frame, so a log per access would be a bug
  // of its own.
  int announcements = 0;
  for (const auto& line : capture.lines()) {
    if (line.message.find("was ignored") != std::string::npos) ++announcements;
  }
  CHECK_EQ(announcements, 2);  // one for width, one for height

  // Any other canvas keeps what is written -- a 2D scratch canvas is the size its
  // script made it. A canvas sized first and given the GL surface afterwards, as
  // Lightning does, reads the surface from then on and says so once.
  CHECK_EQ(domEval(runtime,
                   "var scratch = document.createElement('canvas'); scratch.width = 64; scratch.height = '32';"
                   "var later = document.createElement('canvas'); later.width = 1920; later.height = 1080;"
                   "var before = later.width + 'x' + later.height;"
                   "document.body.removeChild(c);"
                   "var took = later.getContext('webgl') === gl;"
                   "[scratch.width + 'x' + scratch.height, before, took, later.width + 'x' + later.height].join(' ');"),
           std::string("64x32 1920x1080 true 320x180"));
  CHECK(capture.has(screenkit::LogLevel::Warn, "was sized 1920x1080 before it took the GL surface"));

  // Writing the size it already has is not ignoring anything, and says nothing --
  // three.js's setSize(innerWidth, innerHeight) does exactly this.
  auto quiet = domRuntime(320, 180);
  if (!quiet) return;
  test::LogCapture quietCapture;
  CHECK_EQ(domEval(quiet, "var q = document.createElement('canvas'); q.width = 320; q.height = '180'; q.width + ',' + q.height;"),
           std::string("320,180"));
  for (const auto& line : quietCapture.lines()) {
    CHECK(line.message.find("was ignored") == std::string::npos);
  }
}

// --- matrix row: a second GL canvas -----------------------------------------------
// It gets a real GL context of its own, not the page's, and keeps it: this is
// the row that used to assert the refusal (spec-m6-composited-canvases). The
// probe hand-off is unchanged -- a canvas that asked and was never attached
// gives the frame to the next one rather than spending a context on itself.
void domSecondCanvas() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  CHECK_EQ(domEval(runtime,
                   "globalThis.probe = document.createElement('canvas');"
                   "globalThis.probeCtx = probe.getContext('webgl');"
                   "globalThis.first = document.createElement('canvas');"
                   "[probeCtx === gl, first.getContext('webgl') === gl, gl.canvas === first].join(',');"),
           std::string("true,true,true"));

  // The contract the hand-off leaves behind, spelled out rather than implied.
  // A detached probe is handed the page's own context -- not a refusal and not
  // a context of its own. When a later canvas takes the frame, `gl.canvas`
  // follows the frame and names that canvas, so the probe no longer owns what
  // it was lent. Asking the probe again still answers with the object it was
  // already given: `getContext` is idempotent per canvas, whatever the frame
  // did in between, and a second ask must not mint a second context.
  CHECK_EQ(domEval(runtime,
                   "[probe.getContext('webgl') === probeCtx,"
                   " probe.getContext('webgl') === gl,"
                   " probe.getContext('experimental-webgl') === probeCtx,"
                   " gl.canvas === probe, gl.canvas === first].join(',');"),
           std::string("true,true,true,false,true"));
  // The probe cost nothing: one context, the page's own -- and asking it twice
  // more did not cost anything either.
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 1);

  // The first canvas is in the document now, so it keeps the frame and the next
  // canvas that asks gets a context of its own.
  CHECK_EQ(domEval(runtime,
                   "document.body.appendChild(first);"
                   "globalThis.second = document.createElement('canvas');"
                   "document.body.appendChild(second);"
                   "globalThis.g2 = second.getContext('webgl');"
                   "[g2 !== null, g2 !== gl, g2.contextId !== gl.contextId, g2.canvas === second,"
                   " second.getContext('webgl') === g2,"
                   " second.getContext('experimental-webgl') === g2].join(',');"),
           std::string("true,true,true,true,true,true"));
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 2);

  // A canvas still has one kind of context, each way round.
  CHECK_EQ(domEval(runtime, "[second.getContext('2d'), first.getContext('2d')].join('|');"),
           std::string("|"));

  // A third one is a third context, and the page's own `gl` is untouched by any
  // of it.
  CHECK_EQ(domEval(runtime,
                   "var third = document.createElement('canvas');"
                   "document.body.appendChild(third);"
                   "var g3 = third.getContext('webgl');"
                   "[g3 !== null, g3 !== g2, first.getContext('webgl') === gl,"
                   " gl.canvas === first].join(',');"),
           std::string("true,true,true,true"));
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 3);
  for (const auto& line : capture.lines()) {
    CHECK(line.level != screenkit::LogLevel::Error);
  }
}

// --- matrix row: unknown element ------------------------------------------------

// ============================================================================
// Remaining shims: environment, events, encoding, loaders, fonts, identity.
// Async rows stash a result on globalThis, pump the loop, then read it back --
// the same shape a real bundle's promises take.
// ============================================================================

/// Point the confined reader at the generated assets. Write-once per process,
/// which is fine: every CTest row is its own process.
bool setTestAssetRoot(const std::shared_ptr<screenkit::Runtime>& runtime) {
  const std::string src = "__screenkit.setAssetRoot('" + fixture("assets") + "')";
  const auto r = runtime->evaluateSource(src, "root.js");
  CHECK(r.ok);
  if (!r.ok) std::fprintf(stderr, "  setAssetRoot: %s\n", r.error.c_str());
  return r.ok;
}

std::string pumpThenRead(const std::shared_ptr<screenkit::Runtime>& runtime, const char* expr) {
  pumpUntilIdle(runtime);
  return domEval(runtime, expr);
}

void domEnvironment() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "[window.devicePixelRatio, self === globalThis, location.hash === '',"
      " typeof performance.now, window.document === document].join(',');"),
      std::string("1,true,true,function,true"));
  // monotonic: a later read is never smaller
  CHECK_EQ(domEval(runtime, "var a = performance.now(); performance.now() >= a;"),
           std::string("true"));
}

void domEvents() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var e = new Event('x', { cancelable: true }); e.preventDefault();"
      "var u = new Event('y'); u.preventDefault();"
      "var ce = new CustomEvent('z', { detail: 42 });"
      "[e.defaultPrevented, u.defaultPrevented, ce.detail, ce instanceof Event,"
      " new MutationObserver(function(){}).takeRecords().length,"
      " typeof new ResizeObserver(function(){}).observe].join(',');"),
      std::string("true,false,42,true,0,function"));
}

void domEncoding() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "[atob('aGVsbG8='), btoa('hello'), atob(btoa('Lightning 3')),"
      " new Blob(['abc', 'de']).size, new Blob([new Uint8Array([1,2,3])]).size,"
      " URL.createObjectURL(new Blob(['x'])).indexOf('blob:') === 0].join('|');"),
      std::string("hello|aGVsbG8=|Lightning 3|5|3|true"));
  CHECK_EQ(domEval(runtime,
      "var u = new URL('https://a.com:8080/p/q?x=1&n=2#h');"
      "[u.pathname, u.port, u.hash, u.searchParams.get('n'),"
      " new URL('img.png', 'https://a.com/dir/page.html').href,"
      " new URLSearchParams({a:'1', b:'x y'}).toString(),"
      " new URLSearchParams('q=hello+world').get('q'),"
      " new ImageData(4, 2).data.length].join('|');"),
      std::string("/p/q|8080|#h|2|https://a.com/dir/img.png|a=1&b=x%20y|hello world|32"));
  CHECK_EQ(domEval(runtime,
      "try { new URL('not a url'); 'no' } catch (e) { e instanceof TypeError }"),
      std::string("true"));
  // A string part is UTF-8: two bytes for the e-acute, three for the check mark.
  domEval(runtime,
      "globalThis.__utf8 = 'pending'; var utf8Blob = new Blob(['\\u00e9\\u2713']);"
      "utf8Blob.text().then(function (t) { __utf8 = utf8Blob.size + ' ' + (t === '\\u00e9\\u2713'); }); 'x';");
  CHECK_EQ(pumpThenRead(runtime, "__utf8;"), std::string("5 true"));
  // Relative references: the base's query and fragment take no part (a hash
  // router's `#/intro` in location.href broke every texture URL after it), and
  // `..`, '/', '?', '#' and '//' references resolve as RFC 3986 says.
  CHECK_EQ(domEval(runtime,
      "function r(ref, base) { return new URL(ref, base).href; }"
      "[r('assets/bg.jpg', 'screenkit:/#/intro'),"
      " r('img.png', 'https://a.com/dir/page.html?x=1#/a/b'),"
      " r('../up.png', 'https://a.com/d1/d2/page.html'),"
      " r('./same.png', 'https://a.com/d1/page.html'),"
      " r('/root.png', 'https://a.com/d1/page.html'),"
      " r('?q=2', 'https://a.com/d1/page.html?q=1#h'),"
      " r('#frag', 'https://a.com/d1/page.html#old'),"
      " r('//cdn.b.com/x.js', 'https://a.com/'),"
      " r('x.png', 'https://a.com'),"
      " r('fonts/x.png', 'screenkit:/')].join('\\n');"),
      std::string("screenkit:/assets/bg.jpg\n"
                  "https://a.com/dir/img.png\n"
                  "https://a.com/d1/up.png\n"
                  "https://a.com/d1/same.png\n"
                  "https://a.com/root.png\n"
                  "https://a.com/d1/page.html?q=2\n"
                  "https://a.com/d1/page.html#frag\n"
                  "https://cdn.b.com/x.js\n"
                  "https://a.com/x.png\n"
                  "screenkit:/fonts/x.png"));
}

// Security row. readFile is native, so an escape here reads arbitrary disk.
void domAssetConfinement() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "try { __screenkit.readFile('hello.txt'); 'read-before-root' } catch (e) { 'refused' }"),
      std::string("refused"));
  if (!setTestAssetRoot(runtime)) return;
  const std::string evil = fixture("assets-evil/secret.txt");
  const std::string probe =
      "function esc(p) { try { __screenkit.readFile(p); return 'LEAKED'; } catch (e) { return 'refused'; } }"
      "[__screenkit.readFile('hello.txt').byteLength,"
      " esc('../assets-evil/secret.txt'), esc('/etc/hosts'), esc('" + evil + "'),"
      " (function(){ try { __screenkit.setAssetRoot('/'); return 'WIDENED'; } catch (e) { return 'locked'; } })()"
      "].join(',');";
  CHECK_EQ(domEval(runtime, probe.c_str()), std::string("16,refused,refused,refused,locked"));
}

// The chain Lightning's texture loader actually runs, proven by a pixel.
void domTextureChain() {
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = 'pending';"
      "var x = new XMLHttpRequest(); x.open('GET', 'img/tex.png', true); x.responseType = 'blob';"
      "x.onerror = function () { __r = 'xhr-error ' + x.status; };"
      "x.onload = function () {"
      "  createImageBitmap(x.response, { premultiplyAlpha: 'none' }).then(function (bm) {"
      "    var t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);"
      "    [gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER].forEach(function (k) { gl.texParameteri(gl.TEXTURE_2D, k, gl.NEAREST); });"
      "    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bm);"
      "    var err = gl.getError();"
      "    function sh(k, src) { var s = gl.createShader(k); gl.shaderSource(s, src); gl.compileShader(s); return s; }"
      "    var p = gl.createProgram();"
      "    gl.attachShader(p, sh(gl.VERTEX_SHADER, 'attribute vec2 a; varying vec2 v; void main(){ v=a*0.5+0.5; gl_Position=vec4(a,0.0,1.0); }'));"
      "    gl.attachShader(p, sh(gl.FRAGMENT_SHADER, 'precision mediump float; varying vec2 v; uniform sampler2D s; void main(){ gl_FragColor=texture2D(s,v); }'));"
      "    gl.linkProgram(p); gl.useProgram(p);"
      "    var b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b);"
      "    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,1,-1,-1,1,1,1]), gl.STATIC_DRAW);"
      "    var l = gl.getAttribLocation(p, 'a'); gl.enableVertexAttribArray(l); gl.vertexAttribPointer(l, 2, gl.FLOAT, false, 0, 0);"
      "    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight); gl.clear(gl.COLOR_BUFFER_BIT);"
      "    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);"
      "    var px = new Uint8Array(4); gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "    __r = [x.status, bm.width + 'x' + bm.height, err, px[0] + ',' + px[1] + ',' + px[2] + ',' + px[3]].join('|');"
      "  }).catch(function (e) { __r = 'bitmap-error ' + e.message; });"
      "};"
      "x.send(); 'started';");
  // status | dims | glError | the texture's exact colour read back from the GPU
  CHECK_EQ(pumpThenRead(runtime, "__r;"), std::string("200|17x9|0|200,60,90,255"));
}

void domFetch() {
  auto runtime = domRuntime();
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = [];"
      "fetch('hello.txt').then(function (r) { return r.text(); }).then(function (t) { __r[0] = t; });"
      "fetch('data.json').then(function (r) { return r.json(); }).then(function (j) { __r[1] = j.n; });"
      "fetch('missing.png').then(function (r) { __r[2] = r.status + '/' + r.ok; });"
      // A network URL now goes to the network. Port 1 on loopback refuses the
      // connection, which is a network error -- and reaches nothing outside.
      "fetch('https://127.0.0.1:1/x').catch(function (e) { __r[3] = e instanceof TypeError; });"
      "fetch('utf8.txt').then(function (r) { return r.text(); }).then(function (t) { __r[4] = t === 'caf\\u00e9 \\u2713 \\u65e5\\u672c'; });"
      "fetch('/img/tex.png').then(function (r) { __r[5] = 'rootrel=' + r.status; });"
      "fetch('screenkit://img/tex.png').then(function (r) { __r[6] = 'origin=' + r.status; });"
      "fetch(new URL('img/tex.png', location.href).href).then(function (r) { __r[7] = 'fromLocation=' + r.status; });"
      // after a hash router has navigated, the way Lightning resolves a texture
      "location.hash = '/deep/route';"
      "fetch(new URL('img/tex.png', location.href).href).then(function (r) { __r[8] = 'afterHash=' + r.status; });"
      "'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r.join('|');"),
           std::string("hi from an asset|3|404/false|true|true|rootrel=200|origin=200|fromLocation=200|afterHash=200"));
  // A synchronous XMLHttpRequest reads a package asset in place; over the
  // network it refuses, because network I/O never blocks the JS thread.
  CHECK_EQ(domEval(runtime,
      "var s = new XMLHttpRequest(); s.open('GET', 'hello.txt', false); s.send();"
      "var n = new XMLHttpRequest(); n.open('GET', 'http://127.0.0.1:1/x', false);"
      "var refused; try { n.send(); refused = 'sent'; } catch (e) { refused = e.name; }"
      "[s.readyState, s.status, s.responseText, refused].join('|');"),
      std::string("4|200|hi from an asset|NetworkError"));
  // An async request for a missing asset: DONE, 404, and error then loadend --
  // no load.
  domEval(runtime,
      "globalThis.__missing = []; var m = new XMLHttpRequest(); m.open('GET', 'no-such-asset.txt');"
      "['load', 'error', 'loadend'].forEach(function (t) { m.addEventListener(t, function () {"
      "  __missing.push(t); if (t === 'loadend') __missing.push(m.readyState + '/' + m.status); }); });"
      "m.send(); 'x';");
  CHECK_EQ(pumpThenRead(runtime, "__missing.join(',');"), std::string("error,loadend,4/404"));
}

void domImageElement() {
  auto runtime = domRuntime();
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = 'pending'; globalThis.__events = [];"
      "var i = new Image(), fromHandler = null;"
      "i.onload = function (e) { fromHandler = e; __r = [i.naturalWidth, i.naturalHeight, i.complete, i instanceof HTMLImageElement,"
      "  document.createElement('img') instanceof HTMLImageElement].join(','); };"
      "i.onerror = function () { __r = 'error'; };"
      // load and error are events at the element: addEventListener listeners run
      // too, after the handler and with the same Event.
      "i.addEventListener('load', function (e) {"
      "  __events.push(['load', e === fromHandler, e instanceof Event, e.target === i, this === i].join(' ')); });"
      "var bad = document.createElement('img'), badHandler = null;"
      "bad.onerror = function (e) { badHandler = e; };"
      "bad.addEventListener('error', function (e) {"
      "  __events.push(['error', e === badHandler, e.error instanceof Error, e.target === bad].join(' ')); });"
      "bad.addEventListener('load', function () { __events.push('WRONG load'); });"
      "var plain = new Image();"
      "plain.addEventListener('load', function (e) { __events.push('listener only ' + e.type); });"
      "i.src = 'img/tex.png'; bad.src = 'img/missing.png'; plain.src = 'img/tex.png'; 'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"), std::string("17,9,true,true,true"));
  CHECK_EQ(domEval(runtime, "__events.join(' | ');"),
           std::string("load true true true true | error true true true | listener only load"));
}

void domFonts() {
  auto runtime = domRuntime();
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__r = 'pending';"
      // x.ttf is not an asset: the face really loads now, so it fails.
      "var f = new FontFace('Ubuntu', 'url(x.ttf)'); document.fonts.add(f);"
      "f.load().then(function () { __r = 'loaded'; }, function (e) {"
      "  __r = [e.name, f.status, document.fonts.size, document.fonts.has(f)].join(','); });"
      "'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"), std::string("NetworkError,error,1,true"));
  CHECK_EQ(domEval(runtime,
      "var p = new Path2D(); p.moveTo(0,0); p.arcTo(1,1,2,2,5); p.rect(0,0,4,4); p.closePath(); p._ops.length;"),
      std::string("4"));
}

// Lightning feature-detects these; defining them would route it into a broken
// path. Absence is asserted, so a well-meaning future stub fails this row.

// The completion criterion for the shims, as a row rather than a one-off probe.
// The first 33 are the host globals tools/dom-usage found in the Lightning
// bundle (_bmad-output/implementation-artifacts/m2-lightning-dom-usage.json);
// the last four are what `Architecture.md` 3.2's contract adds for M9's
// `<iframe>` instances, whose evidence is that section rather than the trace.
// Each must be provided, except the two Lightning feature-detects, which must
// be absent. Removing a shim, or stubbing one that should stay absent, fails
// this row.
void domCoverage() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var names = ['Blob', 'Event', 'FontFace', 'HTMLCanvasElement', 'HTMLImageElement', 'Image', 'ImageBitmap', 'ImageData', 'MutationObserver', 'OffscreenCanvas', 'Path2D', 'ResizeObserver', 'URL', 'URLSearchParams', 'WebGL2RenderingContext', 'Worker', 'XMLHttpRequest', 'atob', 'clearInterval', 'clearTimeout', 'console', 'createImageBitmap', 'document', 'fetch', 'location', 'performance', 'queueMicrotask', 'requestAnimationFrame', 'self', 'setInterval', 'setTimeout', 'webkitURL', 'window', 'structuredClone', 'postMessage', 'MessageEvent', 'HTMLIFrameElement'];"
      "var ABSENT = { Worker: 1, OffscreenCanvas: 1 };"
      "var bad = names.filter(function (n) {"
      "  var defined = typeof globalThis[n] !== 'undefined';"
      "  return ABSENT[n] ? defined : !defined;"
      "});"
      "bad.length === 0 ? 'covered ' + names.length : 'UNCOVERED ' + bad.join(',');"),
      std::string("covered 37"));
}

void domIdentityAndAbsence() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var c = document.createElement('canvas');"
      "[c instanceof HTMLCanvasElement, c.getContext('webgl') === gl,"
      " !!self.Worker, typeof OffscreenCanvas,"
      " (function(){ try { new HTMLCanvasElement(); return 'allowed'; } catch (e) { return e instanceof TypeError; } })()"
      "].join(',');"),
      std::string("true,true,false,undefined,true"));
  // Networking's documented absences (spec-runtime-networking.md): a feature
  // check for these must fail honestly rather than find a stub.
  CHECK_EQ(domEval(runtime,
      "[typeof WritableStream, typeof TransformStream, typeof ReadableStream.prototype.pipeTo,"
      " typeof ReadableStream.prototype.pipeThrough, typeof ReadableByteStreamController,"
      " typeof navigator.serviceWorker, typeof document.cookie,"
      " (function(){ try { new ReadableStream({ type: 'bytes' }); return 'allowed'; } catch (e) { return e.name; } })(),"
      " (function(){ try { new ReadableStream().getReader({ mode: 'byob' }); return 'allowed'; } catch (e) { return e.name; } })()"
      "].join(',');"),
      std::string("undefined,undefined,undefined,undefined,undefined,undefined,undefined,RangeError,TypeError"));
  // The video player's (spec-video-player.md): no MediaSource -- a player that
  // needs it must feature-detect and find it missing, @screenkit/shaka runs
  // instead -- no EME (DRM is configured through the Shaka layer), no Web Audio,
  // no picture-in-picture, and no rendering of captions.
  CHECK_EQ(domEval(runtime,
      "var v = document.createElement('video');"
      "[typeof MediaSource, typeof ManagedMediaSource, typeof SourceBuffer, typeof MediaKeys,"
      " typeof navigator.requestMediaKeySystemAccess, typeof AudioContext, typeof webkitAudioContext,"
      " typeof v.requestPictureInPicture, typeof v.captureStream, typeof v.setMediaKeys, v.srcObject].join(',');"),
      std::string("undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined,"));
  // M9's `<iframe>` (spec-m9-iframe-instances): an Instance, not a document this
  // page can reach into -- the other side is a separate runtime with a heap of
  // its own -- and no transfer at all, because nothing can be moved between two
  // runtimes that share no memory. A well-meaning `MessagePort` stub would make
  // a page that feature-detects one take a path this runtime cannot serve.
  CHECK_EQ(domEval(runtime,
      "var f = document.createElement('iframe');"
      "[f instanceof HTMLIFrameElement, f.contentDocument, typeof f.contentWindow.postMessage,"
      " typeof MessagePort, typeof MessageChannel, typeof BroadcastChannel, typeof SharedWorker,"
      " typeof structuredClone, window.parent === window, window.top === window,"
      " typeof f.focus, typeof f.sandbox.add, window.length].join(',');"),
      std::string("true,,function,undefined,undefined,undefined,undefined,function,true,true,"
                  "function,function,0"));
}

// A bundler's module-preload polyfill runs before any app code and calls
// methods -- not just properties -- on a <link> it never renders. Throwing
// there kills the bundle at startup, so this pins the whole call set the
// Lightning bundle's loader actually uses. With a real tree the appended <link>
// is in <head> and getElementsByTagName finds it; `rel` is an ordinary property
// here, not a reflected attribute (only id, class and style reflect), so the
// `[rel=...]` query does not match it -- which leaves the polyfill nothing to
// preload, the right outcome on a runtime with no module loader to warm.
void domLoaderTolerance() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  CHECK_EQ(domEval(runtime,
                   "var l = document.createElement('link');"
                   "l.rel = 'modulepreload';"
                   "l.setAttribute('nonce', 'x');"
                   "l.addEventListener('load', function () {});"
                   "var got = l.getAttribute('nonce');"
                   "document.head.appendChild(l);"
                   "var q = document.querySelectorAll('link[rel=\"modulepreload\"]').length;"
                   "var t = document.getElementsByTagName('link').length;"
                   "[got, q, t, document.querySelector('meta') === null, l.parentNode === document.head].join(',');"),
           std::string("x,0,1,true,true"));
}

void domUnknownElement() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  // Lightning's loader builds a <link> exactly this way. Inert, tolerant of
  // property set/get, and it must never throw.
  CHECK_EQ(domEval(runtime,
                   "var l = document.createElement('link');"
                   "l.rel = 'modulepreload'; l.href = 'assets/index.js'; l.crossOrigin = '';"
                   "[l.tagName, l.rel, l.href, typeof l.style, typeof l.appendChild,"
                   " typeof l.neverSet].join(',');"),
           std::string("LINK,modulepreload,assets/index.js,object,function,undefined"));

  // Nothing but a canvas is backed, so nothing else pretends to be.
  CHECK_EQ(domEval(runtime, "typeof document.createElement('div').getContext;"),
           std::string("undefined"));
}

// --- matrix row: root lookup ----------------------------------------------------
void domRootLookup() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  CHECK_EQ(domEval(runtime,
                   "globalThis.app = document.getElementById('app');"
                   "[typeof app.appendChild, typeof app.style,"
                   " document.getElementById('app') === app].join(',');"),
           std::string("function,object,true"));

  // Every other id is a search of the tree, and a miss is null.
  CHECK_EQ(domEval(runtime,
                   "[document.getElementById('root') === null,"
                   " document.getElementById('') === null,"
                   " document.getElementById('APP') === null].join(',');"),
           std::string("true,true,true"));

  // Mounting is what the root is for: appendChild returns the appended child,
  // which is then in the document under <body>.
  CHECK_EQ(domEval(runtime,
                   "var c = document.createElement('canvas');"
                   "[app.appendChild(c) === c, c.parentNode === app, app.parentNode === document.body,"
                   " c.isConnected].join(',');"),
           std::string("true,true,true,true"));
}

// --- matrix row: unsupported context --------------------------------------------
void domUnsupportedContext() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;

  // null, as the web spec allows -- not undefined, which would turn a missing
  // shim into a crash somewhere unrelated.
  CHECK_EQ(domEval(runtime,
                   "var c = document.createElement('canvas');"
                   "[c.getContext('bitmaprenderer') === null, c.getContext('bitmaprenderer') === null,"
                   " c.getContext('webgpu') === null].join(',');"),
           std::string("true,true,true"));

  // Logged once so the gap is visible, and once is once however many times it
  // is asked.
  int bitmap = 0;
  for (const auto& line : capture.lines()) {
    if (line.message.find("getContext(\"bitmaprenderer\")") != std::string::npos) ++bitmap;
  }
  CHECK_EQ(bitmap, 1);
  if (bitmap != 1) std::fprintf(stderr, "%s", capture.joined().c_str());

  // A different unsupported id gets its own line -- one gap, one warning.
  CHECK(capture.has(screenkit::LogLevel::Warn, "getContext(\"webgpu\")"));
}

// --- the fixture: every row, from JS ---------------------------------------------
void domCanvasFixture() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  // The same bundle the windowed host runs. It throws if any row fails, so an
  // ok result is the assertion; the log lines are what a human reads.
  const screenkit::EvalResult result = runtime->evaluateBundle(fixture("dom-canvas.hbc"));
  CHECK(result.ok);
  if (!result.ok) {
    std::fprintf(stderr, "  %s\n%s", result.error.c_str(), capture.joined().c_str());
    return;
  }
  CHECK_EQ(result.value, std::string("dom-canvas-ok"));
  CHECK(capture.has(screenkit::LogLevel::Log, "9/9 matrix rows ok"));

  // The renderer string, reached through document.createElement("canvas")
  // rather than through the `gl` global. This is the acceptance criterion.
  CHECK(capture.has(screenkit::LogLevel::Log, "dom-canvas: GL_RENDERER = ANGLE"));
  CHECK(capture.has(screenkit::LogLevel::Log, "dom-canvas: drawable = 320x180"));
  for (const auto& line : capture.lines()) {
    CHECK(line.level != screenkit::LogLevel::Error);
  }
}

// --- remote, keyboard and gamepad input ------------------------------------------
//
// SDL events in, DOM keydown/keyup out (screenkit/Input.h). The mapping rows use
// synthetic SDL events, which is exactly what a host feeds the router; the
// dispatch row goes all the way to listeners on `document` and `window`.

std::string describeKey(const screenkit::KeyEvent& e) {
  return std::string(e.down ? "down" : "up") + " " + e.key + " " + e.code + " " +
         std::to_string(e.keyCode) + (e.repeat ? " repeat" : "") +
         (e.modifiers ? " mod" + std::to_string(e.modifiers) : "");
}

std::string keyboard(SDL_Scancode scancode, bool down = true, SDL_Keymod mod = SDL_KMOD_NONE,
                     bool repeat = false) {
  SDL_KeyboardEvent event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.scancode = scancode;
  event.mod = mod;
  event.down = down;
  event.repeat = repeat;
  screenkit::KeyEvent key;
  return screenkit::keyEventFromKeyboard(event, key) ? describeKey(key) : "(none)";
}

void inputKeyboardMap() {
  // The navigation keys every TV UI needs, with the keyCodes Blits maps.
  CHECK_EQ(keyboard(SDL_SCANCODE_UP), std::string("down ArrowUp ArrowUp 38"));
  CHECK_EQ(keyboard(SDL_SCANCODE_DOWN, false), std::string("up ArrowDown ArrowDown 40"));
  CHECK_EQ(keyboard(SDL_SCANCODE_LEFT), std::string("down ArrowLeft ArrowLeft 37"));
  CHECK_EQ(keyboard(SDL_SCANCODE_RIGHT), std::string("down ArrowRight ArrowRight 39"));
  CHECK_EQ(keyboard(SDL_SCANCODE_RETURN), std::string("down Enter Enter 13"));  // tvOS select, Android D-pad centre
  CHECK_EQ(keyboard(SDL_SCANCODE_KP_ENTER), std::string("down Enter NumpadEnter 13"));
  // Android's Back button is back everywhere; Escape is back only on tvOS, where
  // it is the Menu button. This suite runs on macOS.
  CHECK_EQ(keyboard(SDL_SCANCODE_AC_BACK), std::string("down GoBack BrowserBack 8"));
  CHECK_EQ(keyboard(SDL_SCANCODE_ESCAPE), std::string("down Escape Escape 27"));
  CHECK_EQ(keyboard(SDL_SCANCODE_BACKSPACE), std::string("down Backspace Backspace 8"));
  // Media and TV keys.
  CHECK_EQ(keyboard(SDL_SCANCODE_PAUSE), std::string("down Pause Pause 19"));  // tvOS play/pause
  CHECK_EQ(keyboard(SDL_SCANCODE_MEDIA_PLAY_PAUSE), std::string("down MediaPlayPause MediaPlayPause 179"));
  CHECK_EQ(keyboard(SDL_SCANCODE_MEDIA_PLAY), std::string("down MediaPlay MediaPlay 415"));
  CHECK_EQ(keyboard(SDL_SCANCODE_MEDIA_STOP), std::string("down MediaStop MediaStop 413"));
  CHECK_EQ(keyboard(SDL_SCANCODE_CHANNEL_INCREMENT), std::string("down ChannelUp ChannelUp 427"));
  CHECK_EQ(keyboard(SDL_SCANCODE_F5), std::string("down F5 F5 116"));
  // Printable keys: the character typed, shift applied; code and keyCode are physical.
  CHECK_EQ(keyboard(SDL_SCANCODE_A), std::string("down a KeyA 65"));
  CHECK_EQ(keyboard(SDL_SCANCODE_A, true, SDL_KMOD_LSHIFT), std::string("down A KeyA 65 mod1"));
  CHECK_EQ(keyboard(SDL_SCANCODE_1), std::string("down 1 Digit1 49"));
  CHECK_EQ(keyboard(SDL_SCANCODE_0), std::string("down 0 Digit0 48"));
  CHECK_EQ(keyboard(SDL_SCANCODE_KP_0), std::string("down 0 Numpad0 96"));
  CHECK_EQ(keyboard(SDL_SCANCODE_SPACE), std::string("down   Space 32"));
  // The OS's own key repeat is passed through, and so are modifiers.
  CHECK_EQ(keyboard(SDL_SCANCODE_DOWN, true, SDL_KMOD_LCTRL, true), std::string("down ArrowDown ArrowDown 40 repeat mod2"));
  CHECK_EQ(keyboard(SDL_SCANCODE_UNKNOWN), std::string("(none)"));
}

void inputGamepadMap() {
  auto button = [](SDL_GamepadButton b) {
    screenkit::KeyEvent key;
    return screenkit::keyEventFromGamepadButton(b, true, key) ? describeKey(key) : std::string("(none)");
  };
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_DPAD_UP), std::string("down ArrowUp ArrowUp 38"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), std::string("down ArrowDown ArrowDown 40"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), std::string("down ArrowLeft ArrowLeft 37"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), std::string("down ArrowRight ArrowRight 39"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_SOUTH), std::string("down Enter Enter 13"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_EAST), std::string("down GoBack BrowserBack 8"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_BACK), std::string("down GoBack BrowserBack 8"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_START), std::string("down MediaPlayPause MediaPlayPause 179"));
  CHECK_EQ(button(SDL_GAMEPAD_BUTTON_GUIDE), std::string("(none)"));
}

// Input and the event loop. A native key event is a user-agent dispatch, so a
// microtask checkpoint runs after every listener callback: a promise the first
// listener resolves settles before the next listener runs, where a script's
// dispatchEvent runs every listener first. A listener removed by an earlier one
// does not run. A paused runtime receives nothing -- not the press, not a
// repeat, not later on resume -- and repeats pick up again only after it.
void inputEventLoop() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__order = [];"
      "function note(name) {"
      "  __order.push(name);"
      "  Promise.resolve().then(function () { __order.push(name + '.micro'); });"
      "}"
      "function doc1(e) { note('doc1:' + e.key); if (e.key === 'x') document.removeEventListener('keydown', doc2); }"
      "function doc2(e) { note('doc2:' + e.key); }"
      "document.addEventListener('keydown', doc1);"
      "document.addEventListener('keydown', doc2);"
      "window.addEventListener('keydown', function (e) { note('win:' + e.key); });"
      "document.dispatchEvent(new KeyboardEvent('keydown', { key: 'script' }));"
      "__order.push('script-done');"
      "'listening';");

  screenkit::InputRouter router(runtime);
  auto key = [&](SDL_Scancode scancode) {
    SDL_Event e{};
    e.key.type = SDL_EVENT_KEY_DOWN; e.key.scancode = scancode; e.key.down = true;
    CHECK(router.handleEvent(e));
  };
  auto button = [&](bool down) {
    SDL_Event g{};
    g.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    g.gbutton.which = 3; g.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH; g.gbutton.down = down;
    CHECK(router.handleEvent(g));
  };

  key(SDL_SCANCODE_RIGHT);
  key(SDL_SCANCODE_X);  // doc1 removes doc2 before doc2's turn
  CHECK(pumpUntilIdle(runtime));

  const std::uint64_t t0 = 5'000'000'000;
  const std::uint64_t delay = screenkit::InputRouter::kRepeatDelayNs;
  const std::uint64_t interval = screenkit::InputRouter::kRepeatIntervalNs;
  runtime->pause();
  key(SDL_SCANCODE_LEFT);   // dropped
  button(true);             // held, its keydown dropped
  router.tick(t0 + delay);  // due, but paused: no repeat
  runtime->resume();
  router.tick(t0 + delay + 1);  // the paused deadline was pushed on, not banked
  key(SDL_SCANCODE_F2);
  router.tick(t0 + delay + interval);  // now the repeat
  CHECK(pumpUntilIdle(runtime));  // delivered before the next pause, or it drops them too
  runtime->pause();
  button(false);  // dropped
  runtime->resume();

  // A press already queued when the pause lands is dropped too. The JS thread is
  // held busy so the key's task is still waiting when pause() is called.
  {
    std::mutex mutex;
    std::condition_variable cv;
    bool busy = false;
    bool release = false;
    bool finished = false;
    runtime->executor()->invokeAsync([&](facebook::jsi::Runtime&) {
      std::unique_lock<std::mutex> lock(mutex);
      busy = true;
      cv.notify_all();
      cv.wait(lock, [&] { return release; });
      finished = true;
      cv.notify_all();
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&] { return busy; });
    }
    key(SDL_SCANCODE_F3);  // queued behind the busy task: dropped
    runtime->pause();
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    cv.notify_all();
    runtime->resume();
    // The task holds references into this scope until it returns.
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(10), [&] { return finished; }));
  }
  CHECK(pumpUntilIdle(runtime));

  CHECK_EQ(domEval(runtime, "__order.join('\\n');"),
           std::string("doc1:script\n"
                       "doc2:script\n"
                       "script-done\n"
                       "doc1:script.micro\n"
                       "doc2:script.micro\n"
                       "doc1:ArrowRight\n"
                       "doc1:ArrowRight.micro\n"
                       "doc2:ArrowRight\n"
                       "doc2:ArrowRight.micro\n"
                       "win:ArrowRight\n"
                       "win:ArrowRight.micro\n"
                       "doc1:x\n"
                       "doc1:x.micro\n"
                       "win:x\n"
                       "win:x.micro\n"
                       "doc1:F2\n"
                       "doc1:F2.micro\n"
                       "win:F2\n"
                       "win:F2.micro\n"
                       "doc1:Enter\n"
                       "doc1:Enter.micro\n"
                       "win:Enter\n"
                       "win:Enter.micro"));
}

void inputRouterDispatch() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__keys = [];"
      "document.addEventListener('keydown', function (e) {"
      "  __keys.push('doc ' + e.type + ' ' + e.key + ' ' + e.code + ' ' + e.keyCode + ' ' + e.which + (e.repeat ? ' repeat' : '') +"
      "    (e.shiftKey ? ' shift' : '') + ' ' + (e instanceof KeyboardEvent) + ' ' + (e.target === document.body) + ' ' + e.cancelable);"
      "  if (e.key === 'Escape') e.stopPropagation();"
      "});"
      "document.addEventListener('keyup', function (e) { __keys.push('doc keyup ' + e.key); });"
      "window.addEventListener('keydown', function (e) { __keys.push('win keydown ' + e.key + ' ' + (e.target === document.body) + ' ' + (e.currentTarget === window)); });"
      "'listening';");

  screenkit::InputRouter router(runtime);
  auto send = [&](const SDL_Event& event) { CHECK(router.handleEvent(event)); };

  SDL_Event e{};
  // A keyboard (or remote-as-keyboard) press and release.
  e.key.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_RIGHT; e.key.down = true; e.key.mod = SDL_KMOD_LSHIFT;
  send(e);
  e.key.type = SDL_EVENT_KEY_UP; e.key.down = false; e.key.mod = SDL_KMOD_NONE;
  send(e);
  // Escape stops at the document.
  e.key.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_ESCAPE; e.key.down = true;
  send(e);

  // A gamepad button held: one keydown, repeats from tick() after the delay, one keyup.
  SDL_Event g{};
  g.gbutton.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; g.gbutton.which = 7;
  g.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH; g.gbutton.down = true; g.gbutton.timestamp = 1'000'000'000;
  send(g);
  const std::uint64_t t0 = 1'000'000'000;
  router.tick(t0 + screenkit::InputRouter::kRepeatDelayNs - 1);           // not yet
  router.tick(t0 + screenkit::InputRouter::kRepeatDelayNs);               // first repeat
  router.tick(t0 + screenkit::InputRouter::kRepeatDelayNs + 1);           // too soon
  router.tick(t0 + screenkit::InputRouter::kRepeatDelayNs + screenkit::InputRouter::kRepeatIntervalNs);  // second
  g.gbutton.type = SDL_EVENT_GAMEPAD_BUTTON_UP; g.gbutton.down = false;
  send(g);
  router.tick(t0 + 10 * screenkit::InputRouter::kRepeatDelayNs);          // released: nothing

  // The left stick as a D-pad, with hysteresis: past half travel presses, it
  // stays down until back under a quarter.
  SDL_Event a{};
  a.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION; a.gaxis.which = 7; a.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY;
  a.gaxis.value = 12000; send(a);    // under the press threshold: nothing
  a.gaxis.value = 30000; send(a);    // ArrowDown down
  a.gaxis.value = 12000; send(a);    // still held (over release threshold)
  // A marker between the two steps: without hysteresis the keyup lands before
  // it, not after.
  SDL_Event marker{};
  marker.key.type = SDL_EVENT_KEY_DOWN; marker.key.scancode = SDL_SCANCODE_F1; marker.key.down = true;
  send(marker);
  a.gaxis.value = 2000; send(a);     // ArrowDown up

  // Unplugging a gamepad releases what it held.
  g.gbutton.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; g.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_LEFT; g.gbutton.down = true;
  send(g);
  SDL_Event removed{};
  removed.gdevice.type = SDL_EVENT_GAMEPAD_REMOVED; removed.gdevice.which = 7;
  send(removed);

  // Anything else is not input.
  SDL_Event other{};
  other.type = SDL_EVENT_WINDOW_RESIZED;
  CHECK(!router.handleEvent(other));

  const std::string expected =
      "doc keydown ArrowRight ArrowRight 39 39 shift true true true\n"
      "win keydown ArrowRight true true\n"
      "doc keyup ArrowRight\n"
      "doc keydown Escape Escape 27 27 true true true\n"
      "doc keydown Enter Enter 13 13 true true true\n"
      "win keydown Enter true true\n"
      "doc keydown Enter Enter 13 13 repeat true true true\n"
      "win keydown Enter true true\n"
      "doc keydown Enter Enter 13 13 repeat true true true\n"
      "win keydown Enter true true\n"
      "doc keyup Enter\n"
      "doc keydown ArrowDown ArrowDown 40 40 true true true\n"
      "win keydown ArrowDown true true\n"
      "doc keydown F1 F1 112 112 true true true\n"
      "win keydown F1 true true\n"
      "doc keyup ArrowDown\n"
      "doc keydown ArrowLeft ArrowLeft 37 37 true true true\n"
      "win keydown ArrowLeft true true\n"
      "doc keyup ArrowLeft";
  const std::string got = pumpThenRead(runtime, "__keys.join('\\n');");
  CHECK_EQ(got, expected);
  if (got != expected) std::fprintf(stderr, "  got:\n%s\n", got.c_str());
}

// ============================================================================
// Gamepads: the W3C Gamepad API over SDL3 (spec-web-gamepad-api-sdl3.md)
//
// One row per line of the spec's matrix. Each drives a virtual SDL gamepad --
// SDL_AttachVirtualJoystick, typed as a gamepad -- through a real InputRouter
// into the real prelude, so the path is the host's own: SDL's events, the
// registry, the per-frame poll, the connection task and the shim. Where SDL
// cannot attach a virtual joystick the row skips.
// ============================================================================

/// The gamepad subsystem, enumerating virtual joysticks only: a controller
/// plugged into the machine running the suite would take the slots the rows
/// expect.
bool initTestGamepads() {
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    test::skip(std::string("no SDL gamepad subsystem: ") + SDL_GetError());
    return false;
  }
  return true;
}

/// A virtual gamepad. It records every rumble SDL asks of it, which arrives on
/// whichever thread called SDL_RumbleGamepad (the JS thread) or on the thread
/// that pumps events (a rumble's expiry).
class TestPad {
 public:
  struct Rumble {
    Uint16 low;
    Uint16 high;
    std::chrono::steady_clock::time_point at;
  };

  TestPad() = default;
  TestPad(const TestPad&) = delete;
  TestPad& operator=(const TestPad&) = delete;
  ~TestPad() { detach(); }

  /// False, with the row skipped, when SDL cannot attach one here.
  bool attach(const char* name, bool rumble = true, Uint16 vendor = 0xbeef, Uint16 product = 0x0a0b) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    desc.userdata = this;
    if (rumble) desc.Rumble = &TestPad::onRumble;
    id_ = SDL_AttachVirtualJoystick(&desc);
    if (id_ == 0) {
      test::skip(std::string("SDL cannot attach a virtual joystick: ") + SDL_GetError());
      return false;
    }
    joystick_ = SDL_OpenJoystick(id_);
    CHECK(joystick_ != nullptr);
    return joystick_ != nullptr;
  }

  void detach() {
    if (joystick_ != nullptr) SDL_CloseJoystick(joystick_);
    joystick_ = nullptr;
    if (id_ != 0) SDL_DetachVirtualJoystick(id_);
    id_ = 0;
  }

  void button(SDL_GamepadButton button, bool down) {
    CHECK(SDL_SetJoystickVirtualButton(joystick_, button, down));
  }
  void axis(SDL_GamepadAxis axis, Sint16 value) { CHECK(SDL_SetJoystickVirtualAxis(joystick_, axis, value)); }

  std::vector<Rumble> rumbles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rumbles_;
  }

  std::string describeRumbles() const {
    std::string out;
    for (const Rumble& r : rumbles()) out += (out.empty() ? "" : " ") + std::to_string(r.low) + "/" + std::to_string(r.high);
    return out;
  }

 private:
  static bool SDLCALL onRumble(void* userdata, Uint16 low, Uint16 high) {
    auto* self = static_cast<TestPad*>(userdata);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->rumbles_.push_back({low, high, std::chrono::steady_clock::now()});
    return true;
  }

  SDL_JoystickID id_ = 0;
  SDL_Joystick* joystick_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Rumble> rumbles_;
};

/// One host loop iteration's worth of gamepad input: SDL's events to the router,
/// then its per-frame tick, which polls the snapshot. Events are pumped and
/// taken by type, never polled: the runtime's work queue lives on SDL's event
/// queue too.
void gamepadFrame(screenkit::InputRouter& router) {
  SDL_PumpEvents();
  SDL_Event events[64];
  int n = 0;
  while ((n = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION,
                             SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)) > 0) {
    for (int i = 0; i < n; ++i) router.handleEvent(events[i]);
  }
  router.tick(SDL_GetTicksNS());
}

const char* const kPadId = " (STANDARD GAMEPAD Vendor: beef Product: 0a0b)";

std::string padId(const char* name) { return std::string(name) + kPadId; }

// --- matrix row: pad at startup -----------------------------------------------------
void gamepadStartup() {
  if (!initTestGamepads()) return;
  // Attached before the router exists, like a controller plugged in before
  // launch: SDL reports it with the same ADDED event the host's loop gets first.
  TestPad pad;
  if (!pad.attach("Startup Pad")) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__events = [];"
      "window.addEventListener('gamepadconnected', function (e) {"
      "  globalThis.__first = e.gamepad;"
      "  __events.push('listener ' + e.type + ' ' + e.isTrusted + ' ' + (e instanceof GamepadEvent) + ' ' +"
      "    e.gamepad.index + ' ' + e.gamepad.connected + ' ' + (e.target === window) + ' ' + e.bubbles);"
      "});"
      "window.ongamepadconnected = function (e) { __events.push('handler ' + e.gamepad.id); };"
      "'listening';");
  screenkit::InputRouter router(runtime);
  gamepadFrame(router);
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));

  CHECK_EQ(domEval(runtime, "__events.join('\\n');"),
           "listener gamepadconnected true true 0 true true false\nhandler " + padId("Startup Pad"));
  CHECK_EQ(domEval(runtime,
      "var pads = navigator.getGamepads(); var p = pads[0];"
      "[Array.isArray(pads), pads.length, String(pads[1]), String(pads[2]), String(pads[3]),"
      " p === __first, p instanceof Gamepad, p.id, p.index, p.mapping, p.connected,"
      " p.buttons.length, p.buttons[16] instanceof GamepadButton, p.axes.join(','),"
      " p.timestamp >= 0 && p.timestamp <= performance.now() + 1,"
      " p.vibrationActuator instanceof GamepadHapticActuator, p.vibrationActuator.effects.join()].join('|');"),
      "true|4|null|null|null|true|true|" + padId("Startup Pad") +
          "|0|standard|true|17|true|0,0,0,0|true|true|dual-rumble");
  // Once each: more frames announce nothing new.
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "__events.length;"), std::string("2"));
  // Not constructible from script, as in a browser.
  CHECK_EQ(domEval(runtime,
      "function threw(f) { try { f(); return 'no'; } catch (e) { return e.constructor.name; } }"
      "[threw(function () { new Gamepad(); }), threw(function () { new GamepadButton(); }),"
      " threw(function () { new GamepadEvent('gamepadconnected'); }),"
      " threw(function () { new GamepadEvent('gamepadconnected', { gamepad: {} }); }),"
      " new GamepadEvent('x', { gamepad: __first }).gamepad === __first].join(' ');"),
      std::string("TypeError TypeError TypeError TypeError true"));
}

// --- matrix row: button ----------------------------------------------------------------
void gamepadButton() {
  if (!initTestGamepads()) return;
  TestPad pad;
  if (!pad.attach("Button Pad")) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));

  CHECK_EQ(domEval(runtime,
      "globalThis.__pad = navigator.getGamepads()[0];"
      "globalThis.__t0 = __pad.timestamp; globalThis.__south = __pad.buttons[0];"
      "[__south.pressed, __south.touched, __south.value].join();"),
      std::string("false,false,0"));
  // Nothing changed: the same objects, and a timestamp that did not move.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var q = navigator.getGamepads()[0];"
      "[q === __pad, q.buttons[0] === __south, q.timestamp === __t0].join();"),
      std::string("true,true,true"));

  // Held, and read in the next frame's requestAnimationFrame.
  domEval(runtime,
      "requestAnimationFrame(function () {"
      "  var q = navigator.getGamepads()[0];"
      "  globalThis.__inFrame = [q === __pad, q.buttons[0] === __south, q.buttons[0].pressed, q.buttons[0].touched,"
      "    q.buttons[0].value, q.timestamp > __t0, q.timestamp <= performance.now() + 1, q.buttons[1].pressed].join();"
      "});");
  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepadFrame(router);
  CHECK(pumpFrames(runtime));
  CHECK_EQ(domEval(runtime, "__inFrame;"), std::string("true,true,true,true,1,true,true,false"));

  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime, "var b = navigator.getGamepads()[0].buttons[0]; [b.pressed, b.value].join();"),
           std::string("false,0"));

  // Every SDL button lands on its standard index, and on no other.
  const std::pair<SDL_GamepadButton, int> kMapping[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, 0},          {SDL_GAMEPAD_BUTTON_EAST, 1},
      {SDL_GAMEPAD_BUTTON_WEST, 2},           {SDL_GAMEPAD_BUTTON_NORTH, 3},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 4},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5},
      {SDL_GAMEPAD_BUTTON_BACK, 8},           {SDL_GAMEPAD_BUTTON_START, 9},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, 10},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 11},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 12},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 13},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 14},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 15},
      {SDL_GAMEPAD_BUTTON_GUIDE, 16},
  };
  for (const auto& entry : kMapping) {
    pad.button(entry.first, true);
    gamepadFrame(router);
    std::string expected(screenkit::kStandardGamepadButtons, '0');
    expected[entry.second] = '1';
    const std::string got = domEval(runtime,
        "navigator.getGamepads()[0].buttons.map(function (b) { return b.pressed ? 1 : 0; }).join('');");
    CHECK_EQ(got, expected);
    if (got != expected) std::fprintf(stderr, "  SDL button %d\n", static_cast<int>(entry.first));
    pad.button(entry.first, false);
    gamepadFrame(router);
  }
}

// --- matrix row: trigger ---------------------------------------------------------------
void gamepadTrigger() {
  if (!initTestGamepads()) return;
  TestPad pad;
  if (!pad.attach("Trigger Pad")) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));

  // A virtual joystick's trigger travels the whole axis range: its middle is a
  // half-pulled trigger.
  pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var p = navigator.getGamepads()[0]; var rt = p.buttons[7];"
      "[Math.abs(rt.value - 0.5) < 0.01, rt.pressed, rt.touched, p.buttons[6].value, p.buttons[6].pressed].join();"),
      std::string("true,true,true,0,false"));
  // Full travel is 1; a touch under 0.1 is a value but not a press.
  pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
  pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -29768);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var p = navigator.getGamepads()[0];"
      "[p.buttons[7].value, p.buttons[6].value > 0.02 && p.buttons[6].value < 0.1, p.buttons[6].pressed].join();"),
      std::string("1,true,false"));
}

// --- matrix row: stick -------------------------------------------------------------------
void gamepadStick() {
  if (!initTestGamepads()) return;
  TestPad pad;
  if (!pad.attach("Stick Pad")) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));

  domEval(runtime, "globalThis.__axes = navigator.getGamepads()[0].axes; 'x';");
  pad.axis(SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);   // fully up
  pad.axis(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);   // fully right
  pad.axis(SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MIN);  // fully left
  pad.axis(SDL_GAMEPAD_AXIS_RIGHTY, 16384);                  // half down
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var a = navigator.getGamepads()[0].axes;"
      "[a[1] === -1, a[0] === 1, a[2] === -1, Math.abs(a[3] - 0.5) < 0.001, a === __axes, a.length].join();"),
      std::string("true,true,true,true,true,4"));
}

// --- matrix row: slots ---------------------------------------------------------------------
void gamepadSlots() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  auto ids = [&]() {
    return domEval(runtime,
        "navigator.getGamepads().map(function (p, i) {"
        "  return p === null ? 'null' : p.id.slice(0, p.id.indexOf(' (')) + '@' + p.index + (p.index === i ? '' : '!');"
        "}).join(' ');");
  };

  // Pads that share a vendor and product share SDL's mapping, and its name
  // with it, so each of these is a different model.
  TestPad a, b, c;
  if (!a.attach("A", true, 0xbeef, 0xa) || !b.attach("B", true, 0xbeef, 0xb)) return;
  gamepadFrame(router);
  CHECK_EQ(ids(), std::string("A@0 B@1 null null"));
  domEval(runtime, "globalThis.__b = navigator.getGamepads()[1]; 'x';");

  a.detach();
  gamepadFrame(router);
  CHECK_EQ(ids(), std::string("null B@1 null null"));

  if (!c.attach("C", true, 0xbeef, 0xc)) return;
  gamepadFrame(router);
  CHECK_EQ(ids(), std::string("C@0 B@1 null null"));
  CHECK_EQ(domEval(runtime, "navigator.getGamepads()[1] === __b;"), std::string("true"));

  // Past four pads the array grows to the highest index.
  TestPad d, e, f;
  if (!d.attach("D", true, 0xbeef, 0xd) || !e.attach("E", true, 0xbeef, 0xe) || !f.attach("F", true, 0xbeef, 0xf)) return;
  gamepadFrame(router);
  CHECK_EQ(ids(), std::string("C@0 B@1 D@2 E@3 F@4"));
  // Slot 4's state comes through the buffer grown past four slots, and lands on it alone.
  f.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var pads = navigator.getGamepads();"
      "[pads[4].buttons[0].pressed, Number.isFinite(pads[4].timestamp),"
      " pads.map(function (p) { return p.buttons[0].pressed ? 1 : 0; }).join('')].join();"),
      std::string("true,true,00001"));
  f.detach();
  gamepadFrame(router);
  CHECK_EQ(ids(), std::string("C@0 B@1 D@2 E@3"));
  CHECK(pumpUntilIdle(runtime));
}

// --- matrix row: disconnect ------------------------------------------------------------------
void gamepadDisconnect() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__events = [];"
      "window.addEventListener('gamepaddisconnected', function (e) {"
      "  __events.push('listener ' + e.type + ' ' + e.isTrusted + ' ' + e.gamepad.index + ' ' + e.gamepad.connected +"
      "    ' ' + (e.gamepad === __old));"
      "});"
      "window.ongamepaddisconnected = function (e) { __events.push('handler ' + e.gamepad.id); };"
      "'listening';");
  screenkit::InputRouter router(runtime);
  TestPad pad;
  if (!pad.attach("Leaving Pad")) return;
  gamepadFrame(router);
  domEval(runtime, "globalThis.__old = navigator.getGamepads()[0]; 'x';");
  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime, "navigator.getGamepads()[0].buttons[0].pressed;"), std::string("true"));

  pad.detach();
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "__events.join('\\n');"),
           "listener gamepaddisconnected true 0 false true\nhandler " + padId("Leaving Pad"));
  CHECK_EQ(domEval(runtime, "String(navigator.getGamepads()[0]) + ' ' + __old.connected;"),
           std::string("null false"));

  // Its slot goes to the next pad, and the old object keeps what it last showed.
  TestPad next;
  if (!next.attach("Next Pad")) return;
  gamepadFrame(router);
  next.axis(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var now = navigator.getGamepads()[0];"
      "[now !== __old, now.index, now.connected, now.axes[0], now.buttons[0].pressed,"
      " __old.connected, __old.buttons[0].pressed, __old.axes[0], __old.index].join();"),
      std::string("true,0,true,1,false,false,true,0,0"));
  CHECK(pumpUntilIdle(runtime));
}

// --- matrix row: paused runtime ----------------------------------------------------------------
void gamepadPaused() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__events = [];"
      "function note(e) {"
      "  __events.push(e.type + ' ' + e.gamepad.id.slice(0, e.gamepad.id.indexOf(' (')) + '@' + e.gamepad.index + ' ' +"
      "    e.gamepad.connected + ' ' + e.gamepad.buttons[0].pressed + (e.gamepad === globalThis.__last ? ' same' : ''));"
      "  globalThis.__last = e.gamepad;"
      "}"
      "window.addEventListener('gamepadconnected', note);"
      "window.addEventListener('gamepaddisconnected', note);"
      "document.addEventListener('keydown', function (e) { __events.push('keydown ' + e.key); });"
      "'listening';");
  screenkit::InputRouter router(runtime);

  runtime->pause();
  // A stays, holding South; B comes and goes before the runtime resumes.
  TestPad a, b;
  if (!a.attach("A", true, 0xbeef, 0xa) || !b.attach("B", true, 0xbeef, 0xb)) {
    runtime->resume();
    return;
  }
  gamepadFrame(router);
  a.button(SDL_GAMEPAD_BUTTON_SOUTH, true);  // also Enter, which a pause drops
  b.detach();
  gamepadFrame(router);

  // The snapshot is current while JS is frozen.
  std::vector<screenkit::GamepadSlot> slots;
  CHECK_EQ(screenkit::GamepadRegistry::shared().read(slots, false), std::size_t{1});
  CHECK(!slots.empty() && slots[0].id == "A (STANDARD GAMEPAD Vendor: beef Product: 000a)" && slots[0].buttons[0] == 1.0);

  runtime->resume();
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "__events.join('\\n');"),
           std::string("gamepadconnected A@0 true true\n"
                       "gamepadconnected B@1 false false\n"
                       "gamepaddisconnected B@1 false false same"));
  CHECK_EQ(domEval(runtime,
      "var pads = navigator.getGamepads(); [pads[0].buttons[0].pressed, String(pads[1])].join();"),
      std::string("true,null"));

  // A slot reused before JS reads again: the page last saw A in slot 0; while
  // paused A leaves and C, another model, takes the slot.
  domEval(runtime, "globalThis.__old = navigator.getGamepads()[0]; __events.length = 0; 'x';");
  runtime->pause();
  a.detach();
  TestPad c;
  if (!c.attach("C", true, 0xbeef, 0xc)) {
    runtime->resume();
    return;
  }
  gamepadFrame(router);
  runtime->resume();
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "__events.join('\\n');"),
           std::string("gamepaddisconnected A@0 false true\n"
                       "gamepadconnected C@0 true false"));
  CHECK_EQ(domEval(runtime,
      "var now = navigator.getGamepads()[0];"
      "[__old.connected, now !== __old, now.id, now.connected, __last === now].join('|');"),
      std::string("false|true|C (STANDARD GAMEPAD Vendor: beef Product: 000c)|true|true"));
}

// --- matrix row: no gamepad subsystem -------------------------------------------------------------
void gamepadNoSubsystem() {
  // Nothing initialises SDL's gamepads in this process.
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var pads = navigator.getGamepads();"
      "[Array.isArray(pads), pads.length, pads.map(String).join(), typeof navigator.getGamepads,"
      " 'ongamepadconnected' in window, 'ongamepaddisconnected' in window, String(window.ongamepadconnected)].join('|');"),
      std::string("true|4|null,null,null,null|function|true|true|null"));
  // A router with no gamepads is a router with nothing to poll.
  screenkit::InputRouter router(runtime);
  router.tick(SDL_GetTicksNS());
  CHECK_EQ(domEval(runtime, "navigator.getGamepads().map(String).join();"), std::string("null,null,null,null"));

  // A runtime with no prelude, as the headless host runs: the binding answers
  // an empty snapshot and a rumble that goes nowhere.
  auto headless = screenkit::Runtime::create(testConfig());
  CHECK(headless != nullptr);
  if (!headless) return;
  const auto result = headless->evaluateSource(
      "[__screenkit.gamepads.read(new ArrayBuffer(8 * 24 * 4), false), __screenkit.gamepads.id(0),"
      " __screenkit.gamepads.rumble(0, 65535, 65535, 100)].join();",
      "headless.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  %s\n", result.error.c_str());
  CHECK_EQ(result.value, std::string("0,,false"));
}

// --- matrix row: app writes ------------------------------------------------------------------------
void gamepadAppWrites() {
  if (!initTestGamepads()) return;
  TestPad pad;
  if (!pad.attach("Written Pad")) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  gamepadFrame(router);
  // Phaser's disconnectAll, in an ES module's strict mode.
  CHECK_EQ(domEval(runtime,
      "(function () {"
      "  'use strict';"
      "  var p = navigator.getGamepads()[0];"
      "  try { p.connected = false; } catch (e) { return 'threw ' + e; }"
      "  globalThis.__written = p;"
      "  return String(p.connected);"
      "})();"),
      std::string("false"));
  // The write does not detach it: the same object, still refreshed.
  pad.button(SDL_GAMEPAD_BUTTON_NORTH, true);
  gamepadFrame(router);
  CHECK_EQ(domEval(runtime,
      "var p = navigator.getGamepads()[0]; [p === __written, p.buttons[3].pressed, p.connected].join();"),
      std::string("true,true,false"));
  CHECK(pumpUntilIdle(runtime));
}

// --- matrix row: claim -----------------------------------------------------------------------------
void gamepadClaim() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__keys = [];"
      "document.addEventListener('keydown', function (e) { __keys.push('keydown ' + e.key + (e.repeat ? ' repeat' : '')); });"
      "document.addEventListener('keyup', function (e) { __keys.push('keyup ' + e.key); });"
      "window.addEventListener('gamepadconnected', function (e) { __keys.push('connected ' + e.gamepad.index); });"
      "'listening';");
  screenkit::InputRouter router(runtime);
  TestPad pad;
  if (!pad.attach("Claimed Pad")) return;
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));

  // Until the app asks, the D-pad navigates -- a connection event is not asking.
  pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));
  CHECK(!screenkit::GamepadRegistry::shared().claimed());

  CHECK_EQ(domEval(runtime, "navigator.getGamepads()[0].buttons[12].pressed;"), std::string("true"));
  CHECK(screenkit::GamepadRegistry::shared().claimed());
  // The router first sees the claim while the runtime is paused: the keyup
  // waits for the resume rather than being dropped with the key left down.
  runtime->pause();
  gamepadFrame(router);
  runtime->resume();
  gamepadFrame(router);  // the held ArrowUp is released now

  // From here the pad is the app's: no keys, no repeats, no stick navigation.
  pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, false);
  gamepadFrame(router);
  pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  pad.axis(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
  gamepadFrame(router);
  router.tick(SDL_GetTicksNS() + 10 * screenkit::InputRouter::kRepeatDelayNs);
  // A keyboard is not a gamepad.
  SDL_Event key{};
  key.key.type = SDL_EVENT_KEY_DOWN;
  key.key.scancode = SDL_SCANCODE_F1;
  key.key.down = true;
  CHECK(router.handleEvent(key));
  CHECK(pumpUntilIdle(runtime));

  CHECK_EQ(domEval(runtime, "__keys.join('\\n');"),
           std::string("connected 0\nkeydown ArrowUp\nkeyup ArrowUp\nkeydown F1"));
  CHECK_EQ(domEval(runtime,
      "var p = navigator.getGamepads()[0]; [p.buttons[12].pressed, p.buttons[0].pressed, p.axes[0]].join();"),
      std::string("true,true,1"));
}

// --- matrix row: rumble ------------------------------------------------------------------------------
void gamepadRumble() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  screenkit::InputRouter router(runtime);
  TestPad pad, still;
  if (!pad.attach("Rumble Pad") || !still.attach("Still Pad", false, 0xbeef, 0x5)) return;
  gamepadFrame(router);
  domEval(runtime, "globalThis.__r = {}; globalThis.__act = navigator.getGamepads()[0].vibrationActuator; 'x';");
  CHECK_EQ(domEval(runtime, "String(navigator.getGamepads()[1].vibrationActuator);"), std::string("null"));

  // 200 ms at full strength: SDL_RumbleGamepad(pad, 65535, 0, 200). SDL itself
  // ends it, from its event pump, once the 200 ms are up.
  domEval(runtime,
      "__act.playEffect('dual-rumble', { duration: 200, strongMagnitude: 1 }).then(function (r) { __r.first = r; });"
      "'started';");
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0"));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (domEval(runtime, "String(__r.first);") == "undefined" && std::chrono::steady_clock::now() < deadline) {
    gamepadFrame(router);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK_EQ(domEval(runtime, "__r.first;"), std::string("complete"));
  gamepadFrame(router);
  const auto rumbles = pad.rumbles();
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0 0/0"));
  if (rumbles.size() == 2) {
    const auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(rumbles[1].at - rumbles[0].at).count();
    CHECK(ran >= 190 && ran < 5000);
    if (!(ran >= 190 && ran < 5000)) std::fprintf(stderr, "  the rumble ran %lld ms\n", static_cast<long long>(ran));
  }

  // One effect preempts another, reset() preempts that, and magnitudes clamp.
  domEval(runtime,
      "__act.playEffect('dual-rumble', { duration: 1000, strongMagnitude: 2, weakMagnitude: 0.5 })"
      "  .then(function (r) { __r.a = r; });"
      "__act.playEffect('dual-rumble', { duration: 5000, strongMagnitude: 0.25, weakMagnitude: -1 })"
      "  .then(function (r) { __r.b = r; });"
      "__act.reset().then(function (r) { __r.reset = r; });"
      "__act.playEffect('trigger-rumble', { duration: 10 }).then(function () { __r.unsupported = 'resolved'; },"
      "  function (e) { __r.unsupported = e.name + ' ' + (e instanceof DOMException); });"
      "'x';");
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "[__r.a, __r.b, __r.reset, __r.unsupported].join();"),
           std::string("preempted,preempted,complete,NotSupportedError true"));
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0 0/0 65535/32768 16384/0 0/0"));

  // An effect with no duration is a stop: SDL would otherwise rumble forever.
  domEval(runtime,
      "__act.playEffect('dual-rumble', { duration: 1000, strongMagnitude: 0.5 }).then(function (r) { __r.long = r; });"
      "__act.playEffect('dual-rumble', { strongMagnitude: 1 }).then(function (r) { __r.zero = r; });"
      "'x';");
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "[__r.long, __r.zero].join();"), std::string("preempted,complete"));
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0 0/0 65535/32768 16384/0 0/0 32768/0 0/0"));

  // A delayed effect stops the one it preempts at once, and starts when its delay is up.
  domEval(runtime,
      "__act.playEffect('dual-rumble', { duration: 1000, weakMagnitude: 1 }).then(function (r) { __r.running = r; });"
      "__act.playEffect('dual-rumble', { duration: 50, startDelay: 150, strongMagnitude: 1 })"
      "  .then(function (r) { __r.delayed = r; });"
      "'x';");
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0 0/0 65535/32768 16384/0 0/0 32768/0 0/0 0/65535 0/0"));
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "[__r.running, __r.delayed].join();"), std::string("preempted,complete"));
  CHECK_EQ(pad.describeRumbles(), std::string("65535/0 0/0 65535/32768 16384/0 0/0 32768/0 0/0 0/65535 0/0 65535/0"));
  {
    const auto all = pad.rumbles();
    if (all.size() >= 2) {
      const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(all.back().at - all[all.size() - 2].at).count();
      CHECK(delay >= 145);
      if (delay < 145) std::fprintf(stderr, "  the delayed effect started after %lld ms\n", static_cast<long long>(delay));
    }
  }

  // A start delay past 5 s is clamped to 5 s: pending at first, started once 5 s are up.
  const std::size_t before = pad.rumbles().size();
  const auto called = std::chrono::steady_clock::now();
  CHECK_EQ(domEval(runtime,
      "__act.playEffect('dual-rumble', { duration: 50, startDelay: 60000, strongMagnitude: 0.5 })"
      "  .then(function (r) { __r.clamped = r; });"
      "String(__r.clamped);"),
      std::string("undefined"));
  CHECK_EQ(pad.rumbles().size(), before);
  CHECK(pumpUntilIdle(runtime, 15000));
  CHECK_EQ(domEval(runtime, "String(__r.clamped);"), std::string("complete"));
  {
    const auto all = pad.rumbles();
    CHECK_EQ(all.size(), before + 1);
    if (all.size() == before + 1) {
      CHECK_EQ(std::to_string(all.back().low) + "/" + std::to_string(all.back().high), std::string("32768/0"));
      const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(all.back().at - called).count();
      CHECK(waited >= 4900 && waited < 15000);
      if (!(waited >= 4900 && waited < 15000)) {
        std::fprintf(stderr, "  the clamped effect started after %lld ms\n", static_cast<long long>(waited));
      }
    }
  }

  // A pad that leaves mid-effect preempts it when it leaves, not when the effect
  // would have ended (pumpUntilIdle gives up before 5 s); one already gone never starts.
  domEval(runtime, "__act.playEffect('dual-rumble', { duration: 5000, weakMagnitude: 1 }).then(function (r) { __r.mid = r; }); 'x';");
  pad.detach();
  gamepadFrame(router);
  CHECK(pumpUntilIdle(runtime));
  domEval(runtime, "__act.playEffect('dual-rumble', { duration: 100, strongMagnitude: 1 }).then(function (r) { __r.gone = r; }); 'x';");
  CHECK(pumpUntilIdle(runtime));
  CHECK_EQ(domEval(runtime, "[__r.mid, __r.gone].join();"), std::string("preempted,preempted"));
}

// --- matrix rows, as Phaser reads them ---------------------------------------------------------------
//
// Phaser 4's GamepadPlugin, reduced to what it does with the API: enable itself
// on navigator.getGamepads, refresh on window's connection events and poll every
// frame from requestAnimationFrame, build its pads from `id`, `index`,
// `buttons[i].value` and `axes`, skip a pad whose `timestamp` predates it, and
// assign `connected` when it loses them.
void gamepadPhaserPoll() {
  if (!initTestGamepads()) return;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "(function () {"
      "  'use strict';"
      "  var plugin = globalThis.__phaser = { enabled: !!navigator.getGamepads, gamepads: [], queue: [], events: [], pad1: null, frames: 0, stop: false };"
      "  function Button(pad, index, pressed) { this.pad = pad; this.index = index; this.value = 0; this.threshold = 1; this.pressed = pressed; }"
      "  Button.prototype.update = function (value) {"
      "    this.value = value;"
      "    if (value >= this.threshold) { if (!this.pressed) { this.pressed = true; plugin.events.push('down ' + this.index); } }"
      "    else if (this.pressed) { this.pressed = false; plugin.events.push('up ' + this.index); }"
      "  };"
      "  function Axis(index) { this.index = index; this.value = 0; this.threshold = 0.1; }"
      "  Axis.prototype.update = function (value) { this.value = value; };"
      "  Axis.prototype.getValue = function () { return Math.abs(this.value) < this.threshold ? 0 : this.value; };"
      "  function Pad(live) {"
      "    this.pad = live; this.id = live.id; this.index = live.index;"
      "    this.buttons = []; for (var i = 0; i < live.buttons.length; i++) this.buttons.push(new Button(this, i, live.buttons[i].value >= 0.5));"
      "    this.axes = []; for (i = 0; i < live.axes.length; i++) this.axes.push(new Axis(i));"
      "    this.leftStick = { x: 0, y: 0 }; this.rightStick = { x: 0, y: 0 };"
      "    this.vibration = live.vibrationActuator; this._created = performance.now();"
      "  }"
      "  Pad.prototype.update = function (live) {"
      "    if (live.timestamp < this._created) return;"
      "    for (var i = 0; i < this.buttons.length; i++) this.buttons[i].update(live.buttons[i].value);"
      "    for (i = 0; i < this.axes.length; i++) this.axes[i].update(live.axes[i]);"
      "    this.leftStick.x = this.axes[0].getValue(); this.leftStick.y = this.axes[1].getValue();"
      "    this.rightStick.x = this.axes[2].getValue(); this.rightStick.y = this.axes[3].getValue();"
      "  };"
      "  plugin.refreshPads = function () {"
      "    var connected = navigator.getGamepads();"
      "    if (!connected) { plugin.gamepads.forEach(function (p) { if (p) p.pad.connected = false; }); return; }"
      "    for (var i = 0; i < connected.length; i++) {"
      "      var live = connected[i]; if (!live) continue;"
      "      var current = plugin.gamepads[live.index];"
      "      if (!current) { current = plugin.gamepads[live.index] = new Pad(live); if (!plugin.pad1) plugin.pad1 = current; }"
      "      else if (current.id !== live.id) { plugin.gamepads[live.index] = new Pad(live); }"
      "      else { current.update(live); }"
      "    }"
      "  };"
      "  plugin.getPad = function (index) { for (var i = 0; i < plugin.gamepads.length; i++) { if (plugin.gamepads[i] && plugin.gamepads[i].index === index) return plugin.gamepads[i]; } };"
      "  function onGamepad(event) { if (event.defaultPrevented) return; plugin.refreshPads(); plugin.queue.push(event); }"
      "  window.addEventListener('gamepadconnected', onGamepad, false);"
      "  window.addEventListener('gamepaddisconnected', onGamepad, false);"
      "  plugin.update = function () {"
      "    plugin.refreshPads();"
      "    plugin.queue.splice(0).forEach(function (event) {"
      "      plugin.events.push(event.type + ' ' + !!plugin.getPad(event.gamepad.index));"
      "    });"
      "  };"
      "  function frame() { plugin.update(); plugin.frames++; if (!plugin.stop) requestAnimationFrame(frame); }"
      "  requestAnimationFrame(frame);"
      "})();"
      "__phaser.enabled;");
  screenkit::InputRouter router(runtime);

  // One host iteration: events, the router's tick, then the frame, waited on.
  auto frame = [&]() {
    gamepadFrame(router);
    const std::string before = domEval(runtime, "String(__phaser.frames);");
    runtime->tickFrame(static_cast<double>(SDL_GetTicksNS()) / 1.0e6);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (domEval(runtime, "String(__phaser.frames);") == before && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  frame();
  TestPad pad;
  if (!pad.attach("Phaser Pad")) return;
  frame();
  frame();
  CHECK_EQ(domEval(runtime,
      "var p = __phaser.pad1; [p !== null && p.id, p && p.index, __phaser.events.join('/')].join('|');"),
      padId("Phaser Pad") + "|0|gamepadconnected true");

  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  pad.axis(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
  pad.axis(SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
  pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
  frame();
  CHECK_EQ(domEval(runtime,
      "var p = __phaser.pad1;"
      "[p.buttons[0].pressed, p.buttons[0].value, p.buttons[7].value, p.leftStick.x, p.leftStick.y, p.rightStick.x,"
      " typeof p.vibration.playEffect].join();"),
      std::string("true,1,1,1,-1,0,function"));

  pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
  pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
  frame();
  pad.detach();
  frame();
  frame();
  CHECK_EQ(domEval(runtime, "var p = __phaser.pad1; [p.buttons[0].pressed, p.leftStick.x, p.pad.connected, __phaser.events.join('/')].join('|');"),
           std::string("false|0|false|gamepadconnected true/down 0/down 7/up 0/gamepaddisconnected true"));
  domEval(runtime, "__phaser.stop = true; 'x';");
  frame();
  CHECK(pumpUntilIdle(runtime));
}

// --- the blockers a real Blits app hit ---------------------------------------------
//
// The Blits example app (poc/blits-example-app) launched, logged nothing and drew
// nothing. Each row below is one reason, found by running it: a missing
// `navigator`, a `document` with no addEventListener, a canvas with no
// getBoundingClientRect, URLSearchParams without iteration -- all but the first
// invisible to static analysis, because Blits reaches them through aliases.

void domWindowIsGlobal() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "[window === globalThis, window.window === window, self === window,"
      " window.URL === URL, window.document === document, window.navigator === navigator,"
      " typeof window.setTimeout, window.devicePixelRatio, window.innerWidth + 'x' + window.innerHeight,"
      " window.open('https://example.com')].join(' ');"),
      std::string("true true true true true true function 1 320x180 "));
}

void domNavigator() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "[typeof navigator.userAgent, navigator.userAgent.indexOf('Tizen') === -1,"
      " navigator.language, navigator.languages.join('+'), navigator.onLine,"
      " navigator.maxTouchPoints, navigator.hardwareConcurrency].join(' ');"),
      std::string("string true en-US en-US true 0 1"));
}

void domEventTarget() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  // Order, a handleEvent object, `once`, removal, a throwing listener that does
  // not stop the rest, preventDefault's return value, and stopImmediatePropagation.
  CHECK_EQ(domEval(runtime,
      "var log = [];"
      "function a(e) { log.push('a:' + e.type + ':' + (e.target === document) + ':' + (e.currentTarget === document)); }"
      "var b = { handleEvent: function (e) { log.push('b'); } };"
      "function once() { log.push('once'); }"
      "function removed() { log.push('removed'); }"
      "function boom() { throw new Error('listener threw'); }"
      "function cancel(e) { e.preventDefault(); log.push('cancel'); }"
      "document.addEventListener('keydown', a);"
      "document.addEventListener('keydown', a);"  // a duplicate is ignored
      "document.addEventListener('keydown', b);"
      "document.addEventListener('keydown', once, { once: true });"
      "document.addEventListener('keydown', removed);"
      "document.removeEventListener('keydown', removed);"
      "document.addEventListener('keydown', boom);"
      "document.addEventListener('keydown', cancel);"
      "var first = document.dispatchEvent(new Event('keydown', { cancelable: true }));"
      "var second = document.dispatchEvent(new Event('keydown'));"
      "window.addEventListener('resize', function (e) { log.push('resize:' + (e.target === window)); e.stopImmediatePropagation(); });"
      "window.addEventListener('resize', function () { log.push('never'); });"
      "window.dispatchEvent(new Event('resize'));"
      "[log.join(','), first, second].join(' | ');"),
      std::string("a:keydown:true:true,b,once,cancel,a:keydown:true:true,b,cancel,resize:true | false | true"));
}

// Fragment navigation, as hash routers use it: the new hash reads back at
// once, `hashchange` fires later as a task with oldURL / newURL, the same hash
// again fires nothing, and a navigation off the document is ignored.
void domLocationHash() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "var events = [];"
      "window.addEventListener('hashchange', function (e) {"
      "  events.push((e instanceof HashChangeEvent) + ' ' + e.oldURL + ' -> ' + e.newURL + ' @' + location.hash);"
      "});"
      "location.hash = '/intro';"
      "var sync = [location.hash, location.href, String(location), events.length].join(' ');"
      "location.hash = '#/intro';");  // the hash it already has
  CHECK_EQ(pumpThenRead(runtime, "sync + ' | ' + events.join(', ');"),
           std::string("#/intro screenkit:/#/intro screenkit:/#/intro 0 | "
                       "true screenkit:/ -> screenkit:/#/intro @#/intro"));
  domEval(runtime,
      "events = [];"
      "location.assign('#a b');"
      "location.href = 'screenkit:/#/c';"
      "location.replace('https://example.com/');"
      "location.hash = '';");
  CHECK_EQ(pumpThenRead(runtime, "[events.join(', '), location.hash, location.href].join(' | ');"),
           std::string("true screenkit:/#/intro -> screenkit:/#a%20b @, "
                       "true screenkit:/#a%20b -> screenkit:/#/c @, "
                       "true screenkit:/#/c -> screenkit:/# @ |  | screenkit:/#"));
}

void domBoundingRect() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "function r(x) { var b = x.getBoundingClientRect(); return [b.left, b.top, b.width, b.height, b.right, b.bottom].join(','); }"
      "[r(document.createElement('canvas')), r(document.createElement('div')), r(document.body), r(document.documentElement),"
      " document.body.clientWidth + 'x' + document.documentElement.clientHeight, r(document.createElement('body'))].join(' ');"),
      std::string("0,0,320,180,320,180 0,0,0,0,0,0 0,0,320,180,320,180 0,0,320,180,320,180 320x180 0,0,0,0,0,0"));
}

void domUrlSearchParamsIteration() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var p = new URLSearchParams('a=1&b=two&a=3');"
      "var spread = [...p.entries()].map(function (e) { return e.join('='); }).join('&');"
      "var forOf = []; for (var pair of p) forOf.push(pair[0]);"
      "[spread, [...p.keys()].join(''), [...p.values()].join(''), forOf.join(''), p.size].join(' ');"),
      std::string("a=1&b=two&a=3 aba 1two3 aba 3"));
}

// Premultiplied alpha at texture upload, against Chromium's measured behaviour:
// an ImageBitmap uploads premultiplied unless created with premultiplyAlpha
// 'none', whatever UNPACK_PREMULTIPLY_ALPHA_WEBGL says; a typed array or
// ImageData follows the flag. The pixel is 200,100,50 at alpha 128.
void glPremultipliedAlpha() {
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = 'pending';"
      "function readTexel(upload, flag) {"
      "  var t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);"
      "  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, flag);"
      "  upload();"
      "  var fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fb);"
      "  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);"
      "  var px = new Uint8Array(4); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "  gl.bindFramebuffer(gl.FRAMEBUFFER, null);"
      "  return Array.prototype.join.call(px, ',');"
      "}"
      "var x = new XMLHttpRequest(); x.open('GET', 'img/alpha.png', true); x.responseType = 'blob';"
      "x.onerror = function () { __r = 'xhr-error'; };"
      "x.onload = function () {"
      "  var out = [];"
      "  Promise.all(['premultiply', 'none', 'default', undefined].map(function (opt) {"
      "    return createImageBitmap(x.response, opt === undefined ? undefined : { premultiplyAlpha: opt });"
      "  })).then(function (bitmaps) {"
      "    ['premultiply', 'none', 'default', 'unset'].forEach(function (name, i) {"
      "      [false, true].forEach(function (flag) {"
      "        out.push('bitmap ' + name + ' ' + flag + ' ' + readTexel(function () {"
      "          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmaps[i]); }, flag));"
      "      });"
      "    });"
      "    var id = new ImageData(new Uint8ClampedArray([200, 100, 50, 128]), 1, 1);"
      "    [false, true].forEach(function (flag) {"
      "      out.push('typed ' + flag + ' ' + readTexel(function () {"
      "        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([200, 100, 50, 128])); }, flag));"
      "      out.push('imagedata ' + flag + ' ' + readTexel(function () {"
      "        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, id); }, flag));"
      "    });"
      "    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);"
      "    out.push('getParameter ' + gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL));"
      "    __r = out.join('\\n');"
      "  }).catch(function (e) { __r = 'error ' + e.message; });"
      "};"
      "x.send(); 'started';");
  const std::string expected =
      "bitmap premultiply false 100,50,25,128\n"
      "bitmap premultiply true 100,50,25,128\n"
      "bitmap none false 200,100,50,128\n"
      "bitmap none true 200,100,50,128\n"
      "bitmap default false 100,50,25,128\n"
      "bitmap default true 100,50,25,128\n"
      "bitmap unset false 100,50,25,128\n"
      "bitmap unset true 100,50,25,128\n"
      "typed false 200,100,50,128\n"
      "imagedata false 200,100,50,128\n"
      "typed true 100,50,25,128\n"
      "imagedata true 100,50,25,128\n"
      "getParameter true";
  const std::string got = pumpThenRead(runtime, "__r;");
  CHECK_EQ(got, expected);
  if (got != expected) std::fprintf(stderr, "  got:\n%s\n", got.c_str());
}

// A rejection nothing handles is logged with its stack; a handled one is not.
void promiseRejectionLogged() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;
  const auto result = runtime->evaluateSource(
      "Promise.resolve().then(function mount() { throw new Error('lost-in-mount'); });"
      "Promise.reject(new Error('caught-later')).catch(function () {});"
      "'scheduled';",
      "rejections.js");
  CHECK(result.ok);
  pumpUntilIdle(runtime);
  CHECK(capture.has(screenkit::LogLevel::Error, "Unhandled promise rejection: Error: lost-in-mount"));
  CHECK(!capture.has(screenkit::LogLevel::Error, "caught-later"));
  if (!capture.has(screenkit::LogLevel::Error, "lost-in-mount")) std::fprintf(stderr, "%s", capture.joined().c_str());
}

// --- presenting: flush the queued GL, swap only a painted frame -----------------
//
// The two faults that kept Lightning black on tvOS while every readback said the
// scene was right. Neither shows in a JS-only row: `readPixels` is a blocking
// call, so it flushes the queue itself and paints over exactly the bug. So this
// row reads pixels natively, on the JS thread, around `presentFrame`.

/// Run `fn` on the JS thread and wait for it.
void onJsThread(const std::shared_ptr<screenkit::Runtime>& runtime,
                const std::function<void(facebook::jsi::Runtime&)>& fn) {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  runtime->executor()->invokeAsync([&](facebook::jsi::Runtime& js) {
    fn(js);
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
    cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(mutex);
  CHECK(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
}

// --- matrix row: pixelStorei ---------------------------------------------------------
// Every WebGL1 and WebGL2 parameter is taken and reads back; an unknown one, or
// a bad value, is a GL error rather than a log line. three.js resets
// PACK_ALIGNMENT and UNPACK_COLORSPACE_CONVERSION_WEBGL on every renderer.
void glPixelStore() {
  test::LogCapture capture;
  auto runtime = glRuntime(64);
  if (!runtime) return;
  const auto result = runtime->evaluateSource(
      "gl.getError();"
      "var out = [gl.getParameter(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL) === gl.BROWSER_DEFAULT_WEBGL];"
      "gl.pixelStorei(gl.PACK_ALIGNMENT, 1); gl.pixelStorei(gl.UNPACK_ALIGNMENT, 8);"
      "gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 2); gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 3);"
      "gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);"
      "gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true); gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, 1);"
      "out.push(gl.getError(), gl.getParameter(gl.PACK_ALIGNMENT), gl.getParameter(gl.UNPACK_ALIGNMENT),"
      "  gl.getParameter(gl.UNPACK_ROW_LENGTH), gl.getParameter(gl.UNPACK_SKIP_ROWS),"
      "  gl.getParameter(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL) === gl.NONE,"
      "  gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL), gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL));"
      "gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, 5); out.push(gl.getError() === gl.INVALID_ENUM);"
      "gl.pixelStorei(0x1234, 1); out.push(gl.getError() === gl.INVALID_ENUM);"
      "gl.pixelStorei(gl.UNPACK_ALIGNMENT, 3); out.push(gl.getError() === gl.INVALID_VALUE, gl.getParameter(gl.UNPACK_ALIGNMENT));"
      "var threw; try { gl.pixelStorei(gl.PACK_ALIGNMENT); } catch (e) { threw = e instanceof TypeError; }"
      "out.push(threw);"
      "out.join(',');",
      "pixel-store.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  %s\n", result.error.c_str());
  CHECK_EQ(result.value, std::string("true,0,1,8,2,3,true,true,true,true,true,true,8,true"));
  CHECK(!capture.has(screenkit::LogLevel::Log, "doesn't support"));
}

// --- matrix row: shared GL contexts ------------------------------------------------
// Two runtimes, two JS threads, one share group: a texture made in the first
// context is read back through the second, and survives the first surface --
// and the display reference it held -- going away. Every surface is created and
// destroyed on its own JS thread.
void glSharedContext() {
  test::LogCapture capture;
  auto first = screenkit::Runtime::create(testConfig());
  auto second = screenkit::Runtime::create(testConfig());
  CHECK(first != nullptr && second != nullptr);
  if (!first || !second) return;

  std::shared_ptr<screenkit::gfx::GlSurface> a;
  std::shared_ptr<screenkit::gfx::GlSurface> b;
  std::string reason;
  GLuint texture = 0;
  onJsThread(first, [&](facebook::jsi::Runtime&) {
    screenkit::gfx::GlSurface::Desc desc;
    desc.width = 4;
    desc.height = 4;
    a = screenkit::gfx::GlSurface::create(desc, reason);
    if (!a) return;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char pixel[4] = {12, 34, 56, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glFinish();
  });
  CHECK(a != nullptr);
  if (!a) {
    std::fprintf(stderr, "  first surface: %s\n", reason.c_str());
    return;
  }

  // The second context's view of the texture, through a framebuffer of its own.
  auto readShared = [&]() {
    std::string got;
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      unsigned char px[4] = {0, 0, 0, 0};
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      got = std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]) + "," +
            std::to_string(px[3]);
    } else {
      got = "incomplete framebuffer";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    return got;
  };

  std::string viaSecond;
  onJsThread(second, [&](facebook::jsi::Runtime&) {
    screenkit::gfx::GlSurface::Desc desc;
    desc.width = 4;
    desc.height = 4;
    desc.shareWith = a.get();
    b = screenkit::gfx::GlSurface::create(desc, reason);
    if (b) viaSecond = readShared();
  });
  CHECK(b != nullptr);
  if (!b) std::fprintf(stderr, "  shared surface: %s\n", reason.c_str());
  CHECK_EQ(viaSecond, std::string("12,34,56,255"));

  onJsThread(first, [&](facebook::jsi::Runtime&) { a.reset(); });
  std::string afterFirstGone;
  onJsThread(second, [&](facebook::jsi::Runtime&) {
    if (b) afterFirstGone = readShared();
    b.reset();
  });
  CHECK_EQ(afterFirstGone, std::string("12,34,56,255"));
  CHECK(!capture.has(screenkit::LogLevel::Warn, "GL surface destroyed off the thread that created it"));
}

/// A runtime with `gl` over a pbuffer, and the surface itself -- which glRuntime
/// keeps to itself, and presentFrame needs.
std::shared_ptr<screenkit::Runtime> presentRuntime(
    std::shared_ptr<screenkit::gfx::GlSurface>& surface) {
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return nullptr;
  std::string reason;
  onJsThread(runtime, [&](facebook::jsi::Runtime& js) {
    screenkit::gfx::GlSurface::Desc desc;
    desc.width = 32;
    desc.height = 32;
    surface = screenkit::gfx::GlSurface::create(desc, reason);
    if (surface) screenkit::gfx::installVendoredWebGL(js, surface);
  });
  CHECK(surface != nullptr);
  if (!surface) {
    std::fprintf(stderr, "  GL bootstrap failed: %s\n", reason.c_str());
    return nullptr;
  }
  return runtime;
}

/// The centre pixel of the default framebuffer, read with GL directly so the
/// read cannot flush the vendored queue on its way.
std::string centrePixel(const std::shared_ptr<screenkit::Runtime>& runtime) {
  std::string out;
  onJsThread(runtime, [&](facebook::jsi::Runtime&) {
    unsigned char px[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    out = std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]) +
          "," + std::to_string(px[3]);
  });
  return out;
}

bool present(const std::shared_ptr<screenkit::Runtime>& runtime,
             screenkit::gfx::GlSurface& surface) {
  bool swapped = false;
  onJsThread(runtime, [&](facebook::jsi::Runtime& js) {
    swapped = screenkit::gfx::presentFrame(js, surface);
  });
  return swapped;
}

void domPresentFrame() {
  test::LogCapture capture;
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  auto runtime = presentRuntime(surface);
  if (!runtime) return;
  if (!installDomShim(runtime)) return;

  // A frame the way Lightning draws one: nothing in it blocks.
  domEval(runtime, "gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); 'drawn';");

  // The clear has already happened: there is no GL thread and no queue, so a GL
  // call runs on the JS thread as it is made (SKGLNativeContext.h). The back
  // buffer is green before anything presents.
  CHECK_EQ(centrePixel(runtime), std::string("0,255,0,255"));

  // So what the frame end decides is not *whether the work runs* but whether to
  // swap, and this frame painted.
  CHECK(present(runtime, *surface));
  CHECK_EQ(centrePixel(runtime), std::string("0,255,0,255"));

  // An idle frame presents nothing -- the last image stays on screen. This is
  // the assertion the batching used to be needed for: a frame that draws nothing
  // must not swap, however much GL ran before it.
  CHECK(!present(runtime, *surface));

  // Drawing into an offscreen framebuffer is not a painted frame either:
  // presenting after it would show a back buffer nobody drew.
  domEval(runtime,
      "var t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);"
      "gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);"
      "var fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fb);"
      "gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);"
      "gl.clearColor(1, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); 'offscreen';");
  CHECK(!present(runtime, *surface));
  CHECK_EQ(centrePixel(runtime), std::string("0,255,0,255"));

  // Back on the default framebuffer, it counts again.
  domEval(runtime,
      "gl.bindFramebuffer(gl.FRAMEBUFFER, null);"
      "gl.clearColor(0, 0, 1, 1); gl.clear(gl.COLOR_BUFFER_BIT); 'default';");
  CHECK(present(runtime, *surface));
  CHECK_EQ(centrePixel(runtime), std::string("0,0,255,255"));

  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// Without the prelude there is no paint flag, and a bare GL bundle -- the M4
// triangle -- must still present every frame, flushed.
void glPresentWithoutShim() {
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  auto runtime = presentRuntime(surface);
  if (!runtime) return;

  const auto drawn = runtime->evaluateSource(
      "gl.clearColor(1, 0, 1, 1); gl.clear(gl.COLOR_BUFFER_BIT); 'drawn';", "bare.js");
  CHECK(drawn.ok);
  CHECK(present(runtime, *surface));
  CHECK_EQ(centrePixel(runtime), std::string("255,0,255,255"));
  CHECK(present(runtime, *surface));
}

// --- bufferData / bufferSubData argument handling -------------------------------
//
// Every expectation below was measured in Chromium (WebGL2 over ANGLE), not read
// out of the spec -- where they differ, apps were written against the browser.
// The surprising half: bufferData never throws for bad data, because WebIDL
// sends anything that is not null or a buffer to the *size* overload.
//
// Upstream never implemented getBufferSubData, so buffer contents are read back
// natively through `__readArrayBuffer()`, a test-only host function that maps
// the bound ARRAY_BUFFER.
void glBufferData() {
  test::LogCapture capture;
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  auto runtime = presentRuntime(surface);
  if (!runtime) return;

  onJsThread(runtime, [](facebook::jsi::Runtime& js) {
    auto name = facebook::jsi::PropNameID::forAscii(js, "__readArrayBuffer");
    js.global().setProperty(js, name, facebook::jsi::Function::createFromHostFunction(
        js, name, 0,
        [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&, const facebook::jsi::Value*,
           size_t) -> facebook::jsi::Value {
          GLint size = 0;
          glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
          std::string out;
          if (size > 0) {
            auto* bytes = static_cast<const unsigned char*>(
                glMapBufferRange(GL_ARRAY_BUFFER, 0, size, GL_MAP_READ_BIT));
            for (GLint i = 0; bytes != nullptr && i < size; ++i) {
              out += (i ? "." : "") + std::to_string(bytes[i]);
            }
            glUnmapBuffer(GL_ARRAY_BUFFER);
          }
          return facebook::jsi::String::createFromUtf8(rt, out);
        }));
  });

  const auto result = runtime->evaluateSource(R"JS(
    var rows = [];
    // One fresh buffer per case. `setup` runs first; `bytes` asks for contents.
    function probe(name, call, setup, bytes) {
      var b = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, b);
      if (setup) setup();
      gl.getError();
      var outcome;
      try { call(); outcome = 'ok'; } catch (e) { outcome = e.constructor.name; }
      var err = gl.getError();  // blocking: runs every queued call first
      var row = name + ' ' + outcome + ' err=' + err +
                ' size=' + gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE);
      if (bytes) row += ' bytes=' + __readArrayBuffer();
      rows.push(row);
      gl.deleteBuffer(b);
    }
    var A = gl.ARRAY_BUFFER, S = gl.STATIC_DRAW;
    function data(v) { return function () { gl.bufferData(A, v, S); }; }
    function four() { gl.bufferData(A, new Uint8Array(4), S); }
    function u16() { return new Uint16Array([0x0101, 0x0202, 0x0303, 0x0404]); }

    // bufferData, three arguments
    probe('array', data([1, 2, 3]));
    probe('string', data('nope'));
    probe('numericString', data('16'));
    probe('null', data(null));
    probe('undefined', data(undefined));
    probe('duck', data({length: 4}));
    probe('bool', data(true));
    probe('negative', data(-1));
    probe('float', data(7.9));
    probe('dataView', data(new DataView(new ArrayBuffer(8))));
    probe('arrayBuffer', data(new ArrayBuffer(12)));
    probe('typed', data(new Float32Array(3)));
    probe('null.badTarget', function () { gl.bufferData(0x1234, null, S); });
    probe('target.string', function () { gl.bufferData('34962', new Uint8Array([7, 9]), S); }, null, true);
    probe('bigint', data(BigInt(1)));
    probe('symbol', data(Symbol('x')));
    probe('valueOfThrows', data({ valueOf: function () { throw new RangeError('x'); } }));
    probe('twoArgs', function () { gl.bufferData(A, 4); });

    // bufferData, WebGL2 srcOffset / length (in elements)
    probe('offset1', function () { gl.bufferData(A, u16(), S, 1); }, null, true);
    probe('offset1.len2', function () { gl.bufferData(A, u16(), S, 1, 2); }, null, true);
    probe('offset4', function () { gl.bufferData(A, u16(), S, 4); });
    probe('offset5', function () { gl.bufferData(A, u16(), S, 5); });
    probe('offset3.len2', function () { gl.bufferData(A, u16(), S, 3, 2); });
    probe('len0', function () { gl.bufferData(A, u16(), S, 0, 0); }, null, true);
    probe('subarray.offset1', function () {
      gl.bufferData(A, new Uint8Array([9, 8, 7, 6, 5]).subarray(1), S, 1, 2); }, null, true);
    probe('dataView.offset2', function () {
      gl.bufferData(A, new DataView(new Uint8Array([1, 2, 3, 4]).buffer), S, 2); }, null, true);
    probe('arrayBuffer.offset1', function () { gl.bufferData(A, new Uint8Array([1, 2, 3]).buffer, S, 1); });
    probe('size.fourArgs', function () { gl.bufferData(A, 8, S, 1); });

    // bufferSubData, over a four-byte zeroed buffer
    function sub(name, call) { probe(name, call, four, true); }
    sub('sub.array', function () { gl.bufferSubData(A, 0, [1, 2]); });
    sub('sub.string', function () { gl.bufferSubData(A, 0, 'x'); });
    sub('sub.null', function () { gl.bufferSubData(A, 0, null); });
    sub('sub.duck', function () { gl.bufferSubData(A, 0, {length: 2}); });
    sub('sub.typed', function () { gl.bufferSubData(A, 1, new Uint8Array([5, 6])); });
    sub('sub.negOffset', function () { gl.bufferSubData(A, -1, new Uint8Array(2)); });
    sub('sub.offset1', function () { gl.bufferSubData(A, 0, new Uint8Array([5, 6, 7]), 1); });
    sub('sub.offset1.len1', function () { gl.bufferSubData(A, 1, new Uint8Array([5, 6, 7]), 1, 1); });
    sub('sub.offset4', function () { gl.bufferSubData(A, 0, new Uint8Array([5, 6, 7]), 4); });
    sub('sub.len.over', function () { gl.bufferSubData(A, 0, new Uint8Array([5, 6, 7]), 2, 2); });
    sub('sub.arrayBuffer', function () { gl.bufferSubData(A, 2, new Uint8Array([5, 6]).buffer); });
    sub('sub.arrayBuffer.offset', function () { gl.bufferSubData(A, 0, new Uint8Array([5, 6]).buffer, 1); });
    sub('sub.dstOffset.string', function () { gl.bufferSubData(A, '1', new Uint8Array([5])); });
    sub('sub.dstOffset.NaN', function () { gl.bufferSubData(A, 'x', new Uint8Array([5])); });
    sub('sub.overflow', function () { gl.bufferSubData(A, 3, new Uint8Array([5, 6])); });
    sub('sub.twoArgs', function () { gl.bufferSubData(A, 0); });

    // null data raises INVALID_VALUE even with no buffer bound at all.
    gl.bindBuffer(A, null); gl.getError();
    gl.bufferData(A, null, S);
    rows.push('null.unbound err=' + gl.getError());
    rows.join('\n');
  )JS", "buffer-data.js");
  CHECK(result.ok);
  if (!result.ok) {
    std::fprintf(stderr, "  error: %s\n", result.error.c_str());
    return;
  }

  const std::string expected =
      "array ok err=0 size=0\n"
      "string ok err=0 size=0\n"
      "numericString ok err=0 size=16\n"
      "null ok err=1281 size=0\n"
      "undefined ok err=1281 size=0\n"
      "duck ok err=0 size=0\n"
      "bool ok err=0 size=1\n"
      "negative ok err=1281 size=0\n"
      "float ok err=0 size=7\n"
      "dataView ok err=0 size=8\n"
      "arrayBuffer ok err=0 size=12\n"
      "typed ok err=0 size=12\n"
      "null.badTarget ok err=1281 size=0\n"
      "target.string ok err=0 size=2 bytes=7.9\n"
      "bigint TypeError err=0 size=0\n"
      "symbol TypeError err=0 size=0\n"
      "valueOfThrows RangeError err=0 size=0\n"
      "twoArgs TypeError err=0 size=0\n"
      "offset1 ok err=0 size=6 bytes=2.2.3.3.4.4\n"
      "offset1.len2 ok err=0 size=4 bytes=2.2.3.3\n"
      "offset4 ok err=0 size=0\n"
      "offset5 ok err=1281 size=0\n"
      "offset3.len2 ok err=1281 size=0\n"
      "len0 ok err=0 size=8 bytes=1.1.2.2.3.3.4.4\n"
      "subarray.offset1 ok err=0 size=2 bytes=7.6\n"
      "dataView.offset2 ok err=0 size=2 bytes=3.4\n"
      "arrayBuffer.offset1 TypeError err=0 size=0\n"
      "size.fourArgs TypeError err=0 size=0\n"
      "sub.array TypeError err=0 size=4 bytes=0.0.0.0\n"
      "sub.string TypeError err=0 size=4 bytes=0.0.0.0\n"
      "sub.null TypeError err=0 size=4 bytes=0.0.0.0\n"
      "sub.duck TypeError err=0 size=4 bytes=0.0.0.0\n"
      "sub.typed ok err=0 size=4 bytes=0.5.6.0\n"
      "sub.negOffset ok err=1281 size=4 bytes=0.0.0.0\n"
      "sub.offset1 ok err=0 size=4 bytes=6.7.0.0\n"
      "sub.offset1.len1 ok err=0 size=4 bytes=0.6.0.0\n"
      "sub.offset4 ok err=1281 size=4 bytes=0.0.0.0\n"
      "sub.len.over ok err=1281 size=4 bytes=0.0.0.0\n"
      "sub.arrayBuffer ok err=0 size=4 bytes=0.0.5.6\n"
      "sub.arrayBuffer.offset TypeError err=0 size=4 bytes=0.0.0.0\n"
      "sub.dstOffset.string ok err=0 size=4 bytes=0.5.0.0\n"
      "sub.dstOffset.NaN ok err=0 size=4 bytes=5.0.0.0\n"
      "sub.overflow ok err=1281 size=4 bytes=0.0.0.0\n"
      "sub.twoArgs TypeError err=0 size=4 bytes=0.0.0.0\n"
      "null.unbound err=1281";
  CHECK_EQ(result.value, expected);
  if (result.value != expected) std::fprintf(stderr, "  got:\n%s\n", result.value.c_str());
  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// --- drawingBufferWidth has a browser's property shape ---------------------------
// Measured in Chromium: an accessor with no setter on both context prototypes and
// never an own property, so assigning in strict mode is a TypeError and the value
// does not move; a getter called on a foreign object is "Illegal invocation".
// window.innerWidth is [Replaceable]: assigning replaces it rather than throwing.
void domDrawingBufferShape() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "(function () {"
      "  'use strict';"
      "  var d1 = Object.getOwnPropertyDescriptor(WebGLRenderingContext.prototype, 'drawingBufferWidth');"
      "  var d2 = Object.getOwnPropertyDescriptor(WebGL2RenderingContext.prototype, 'drawingBufferHeight');"
      "  var assign; try { gl.drawingBufferWidth = 5; assign = 'no-throw'; } catch (e) { assign = e.constructor.name; }"
      "  var foreign; try { d1.get.call({}); foreign = 'no-throw'; } catch (e) { foreign = e.constructor.name; }"
      "  window.innerWidth = 7;"
      "  return [Object.getOwnPropertyDescriptor(gl, 'drawingBufferWidth') === undefined ? 'no-own' : 'own',"
      "          typeof d1.get, typeof d1.set, d1.enumerable, d1.configurable, typeof d2.get,"
      "          assign, foreign, gl.drawingBufferWidth + 'x' + gl.drawingBufferHeight,"
      "          window.innerWidth, window.innerHeight].join(' ');"
      "})();"),
      std::string("no-own function undefined true true function TypeError TypeError 320x180 7 180"));
}

// --- drawing buffer size follows a resize ---------------------------------------
//
// The one row with a real window: a pbuffer cannot be resized, and whether the
// size reaches JS is exactly what went unverified. The window, view and layer are
// made on this (main) thread and the surface on the JS thread, the way the host
// does it. Events are only pumped, never polled: the runtime's work queue lives on
// SDL's event queue, and polling here would steal from it.
//
// Two facts, both measured. `gl.drawingBufferWidth` was a data property fixed at
// install, so nothing followed a resize. And ANGLE's Metal surface adopts a new
// layer size only when it takes the next drawable, so even a live read of
// eglQuerySurface lagged -- still 320x200 after a presented frame. The size now
// comes from the layer, and this row checks both the numbers JS sees before any
// frame and the drawable ANGLE actually renders into after one.
std::string sizesFromJs(const std::shared_ptr<screenkit::Runtime>& runtime) {
  return domEval(runtime,
      "var c = document.createElement('canvas');"
      "[gl.drawingBufferWidth + 'x' + gl.drawingBufferHeight,"
      " c.width + 'x' + c.height,"
      " window.innerWidth + 'x' + window.innerHeight].join(' ');");
}

void glDrawingBufferResize() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    test::skip(std::string("no window server: ") + SDL_GetError());
    return;
  }
  SDL_Window* window = SDL_CreateWindow("gl-drawing-buffer-resize", 320, 200,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
  CHECK(window != nullptr);
  SDL_MetalView view = window ? SDL_Metal_CreateView(window) : nullptr;
  CHECK(view != nullptr);
  void* layer = view ? SDL_Metal_GetLayer(view) : nullptr;
  CHECK(layer != nullptr);

  auto pixelSize = [window](int& w, int& h) {
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return std::to_string(w) + "x" + std::to_string(h);
  };

  if (layer != nullptr) {
    std::shared_ptr<screenkit::gfx::GlSurface> surface;
    auto runtime = screenkit::Runtime::create(testConfig());
    CHECK(runtime != nullptr);
    int w = 0;
    int h = 0;
    const std::string before = pixelSize(w, h);
    if (runtime) {
      std::string reason;
      onJsThread(runtime, [&](facebook::jsi::Runtime& js) {
        screenkit::gfx::GlSurface::Desc desc;
        desc.nativeLayer = layer;
        surface = screenkit::gfx::GlSurface::create(desc, reason);
        if (surface) screenkit::gfx::installVendoredWebGL(js, surface);
      });
      CHECK(surface != nullptr);
      if (!surface) std::fprintf(stderr, "  surface: %s\n", reason.c_str());
    }

    if (runtime && surface && installDomShim(runtime)) {
      // drawingBufferWidth, canvas.width and innerWidth: one number, three names.
      CHECK_EQ(sizesFromJs(runtime), before + " " + before + " " + before);

      // `resize` at window: after every listener a microtask checkpoint, and
      // the size already the new one when it fires.
      domEval(runtime,
          "globalThis.__resizes = [];"
          "window.addEventListener('resize', function (e) {"
          "  __resizes.push('listener ' + innerWidth + 'x' + innerHeight + ' ' + e.isTrusted + ' ' + (e.target === window));"
          "  Promise.resolve().then(function () { __resizes.push('listener.micro'); });"
          "});"
          "onresize = function () { __resizes.push('onresize ' + canvasSize()); };"
          "function canvasSize() { var c = document.createElement('canvas'); return c.width + 'x' + c.height; }"
          "'listening';");
      screenkit::ViewportEvents viewport(runtime);

      SDL_SetWindowSize(window, 480, 300);
      SDL_SyncWindow(window);
      SDL_PumpEvents();
      const std::string after = pixelSize(w, h);
      CHECK(after != before);

      // The window's own event, not a synthetic one. The poll also sees the
      // runtime's work queue, which goes straight back.
      int sizeEvents = 0;
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (screenkit::reclaimRuntimeEvent(event)) continue;
        if (viewport.handleEvent(event)) ++sizeEvents;
      }
      CHECK(sizeEvents >= 1);
      pumpUntilIdle(runtime);
      CHECK_EQ(domEval(runtime, "__resizes.join(',');"),
               "listener " + after + " true true,listener.micro,onresize " + after);

      // A size event that changes nothing JS can see fires nothing.
      SDL_Event same{};
      same.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
      same.window.data1 = w;
      same.window.data2 = h;
      CHECK(viewport.handleEvent(same));
      CHECK(viewport.handleEvent(same));
      pumpUntilIdle(runtime);
      CHECK_EQ(domEval(runtime, "String(__resizes.length);"), std::string("3"));

      // Before any frame: the new size, not the one ANGLE last drew at.
      CHECK_EQ(sizesFromJs(runtime), after + " " + after + " " + after);

      // And it is the drawable ANGLE really renders into -- from the first frame
      // after the resize. A drawable taken before it (the bootstrap took one) is
      // presented and let go when the resize is handled, so this frame's clear
      // takes a new one and covers a corner that lies outside the old size. Read
      // back natively, so the read cannot flush or resize anything on its way.
      domEval(runtime, "gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); gl.getError(); 'frame 1';");
      std::string corner;
      onJsThread(runtime, [&](facebook::jsi::Runtime&) {
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(w - 2, h - 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        corner = std::to_string(px[0]) + "," + std::to_string(px[1]) + "," +
                 std::to_string(px[2]) + "," + std::to_string(px[3]);
      });
      CHECK_EQ(corner, std::string("0,255,0,255"));
      present(runtime, *surface);
      CHECK_EQ(sizesFromJs(runtime), after + " " + after + " " + after);
    }

    if (runtime) runtime->shutdown();
  }
  if (view) SDL_Metal_DestroyView(view);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
}

// ============================================================================
// M5 finish: a failure JS declares fatal, and the built-ins Hermes lacks
// ============================================================================

// `__screenkit.reportFailure` records the first failure for the host, readable
// from any thread; later reports do not replace it, and it is per runtime.
void reportFailure() {
  test::LogCapture capture;
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;
  CHECK(!runtime->failure().has_value());

  // Reporting is not throwing: the call returns and the script carries on.
  const auto first = runtime->evaluateSource(
      "__screenkit.reportFailure('screenkit bundle: entry \"/a.js\" failed: boom'); 'carried on';", "report.js");
  CHECK(first.ok);
  CHECK_EQ(first.value, std::string("carried on"));
  CHECK(runtime->failure().has_value());
  CHECK_EQ(runtime->failure().value_or(""), std::string("screenkit bundle: entry \"/a.js\" failed: boom"));

  // The first report is the cause; what follows is fallout and does not replace it.
  const auto second = runtime->evaluateSource(
      "__screenkit.reportFailure(new Error('fallout'));"
      "[typeof __screenkit.reportFailure,"
      " (function () { try { __screenkit.reportFailure(); return 'no throw'; } catch (e) { return e.message; } })()"
      "].join(' | ');",
      "again.js");
  CHECK(second.ok);
  CHECK_EQ(second.value, std::string("function | __screenkit.reportFailure requires a message"));
  CHECK_EQ(runtime->failure().value_or(""), std::string("screenkit bundle: entry \"/a.js\" failed: boom"));

  // Per runtime, and reported the way a packed entry reports: from a rejection
  // that settles after the evaluate has returned, read here while the JS thread
  // is still running.
  auto other = screenkit::Runtime::create(testConfig());
  CHECK(other != nullptr);
  if (!other) return;
  CHECK(!other->failure().has_value());
  const auto scheduled = other->evaluateSource(
      "setTimeout(function () {"
      "  Promise.reject(new Error('later')).catch(function (e) { __screenkit.reportFailure(e); });"
      "}, 30); 'scheduled';",
      "later.js");
  CHECK(scheduled.ok);
  CHECK(!other->failure().has_value());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!other->failure() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_EQ(other->failure().value_or("<none>"), std::string("Error: later"));
  other->shutdown();
  // Still there after shutdown: a host reads it on its way out.
  CHECK_EQ(other->failure().value_or("<none>"), std::string("Error: later"));
}

// The exact set of ES2015-ES2023 built-ins the pinned Hermes lacks, probed as
// bytecode, against the list @screenkit/vite-plugin polyfills from. A Hermes pin
// that gains or loses one fails here, and so does an edit to either list alone.
void hermesBuiltins() {
  std::ifstream in(SCREENKIT_HERMES_BUILTINS_JSON);
  CHECK(static_cast<bool>(in));
  if (!in) return;
  std::stringstream text;
  text << in.rdbuf();

  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto recorded = runtime->evaluateSource(
      "(" + text.str() + ").missing.map(function (gap) { return gap.builtin; }).sort().join('\\n');",
      "hermes-builtins.json");
  CHECK(recorded.ok);
  if (!recorded.ok) std::fprintf(stderr, "  %s\n", recorded.error.c_str());

  const auto probed = runtime->evaluateBundle(fixture("hermes-builtins.hbc"));
  CHECK(probed.ok);
  if (!probed.ok) std::fprintf(stderr, "  %s\n", probed.error.c_str());
  CHECK(!probed.value.empty());
  CHECK_EQ(probed.value, recorded.value);
  if (probed.value != recorded.value) {
    std::fprintf(stderr, "  missing from this Hermes:\n%s\n  recorded in hermes-builtins.json:\n%s\n",
                 probed.value.c_str(), recorded.value.c_str());
  }
}

// ============================================================================
// M6: the element tree
// ============================================================================

// Matrix row "Tree move": appending a node that already has a parent moves it.
void domTreeMove() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var a = document.createElement('div'), b = document.createElement('div'), n = document.createElement('p');"
      "b.appendChild(n);"
      "var live = b.childNodes;"
      "var returned = a.appendChild(n);"
      "[returned === n, n.parentNode === a, b.childNodes.length, live.length, b.firstChild === null,"
      " a.childNodes.length, a.childNodes[0] === n, Array.prototype.indexOf.call(b.childNodes, n)].join(',');"),
      std::string("true,true,0,0,true,1,true,-1"));
  // Into the document and back out, and to the end of its own parent.
  CHECK_EQ(domEval(runtime,
      "var m = document.createElement('span');"
      "document.body.appendChild(n); var connected = [n.isConnected, a.childNodes.length, n.parentNode === document.body];"
      "a.appendChild(n); a.appendChild(m); a.appendChild(n);"
      "connected.concat([n.isConnected, a.firstChild === m, a.lastChild === n, a.childNodes.length,"
      " document.body.contains(n)]).join(',');"),
      std::string("true,0,true,false,true,true,2,false"));
}

// Matrix row "Cycle": a node cannot become its own descendant.
void domTreeCycle() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var outer = document.createElement('div'), mid = document.createElement('div'), inner = document.createElement('div');"
      "outer.appendChild(mid); mid.appendChild(inner);"
      "function attempt(f) {"
      "  try { f(); return 'no throw'; }"
      "  catch (e) { return [e.name, e instanceof DOMException, e.code].join(':'); }"
      "}"
      "[attempt(function () { inner.appendChild(outer); }),"
      " attempt(function () { mid.appendChild(mid); }),"
      " attempt(function () { inner.insertBefore(outer, null); }),"
      " attempt(function () { mid.replaceChild(outer, inner); }),"
      " attempt(function () { document.body.appendChild(document.documentElement); }),"
      // Unchanged by every refusal.
      " outer.firstChild === mid, mid.firstChild === inner, inner.childNodes.length, outer.parentNode,"
      " document.documentElement.parentNode === document].join('|');"),
      std::string("HierarchyRequestError:true:3|HierarchyRequestError:true:3|HierarchyRequestError:true:3|"
                  "HierarchyRequestError:true:3|HierarchyRequestError:true:3|true|true|0||true"));
  CHECK_CONTAINS(domEval(runtime, "try { inner.appendChild(outer); } catch (e) { e.message }"),
                 "The new child element contains the parent");
}

void domTreeOperations() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var p = document.createElement('div'), a = document.createElement('a'), b = document.createElement('b'),"
      "    i = document.createElement('i');"
      "function names() { return Array.prototype.map.call(p.childNodes, function (n) { return n.localName; }).join(''); }"
      "var out = [];"
      "p.appendChild(a); p.insertBefore(b, a); p.insertBefore(i, null); out.push(names());"   // bai
      "out.push(p.replaceChild(a, i) === i, names(), i.parentNode);"                            // true, ba, null
      "p.insertBefore(a, a); out.push(names());"                                                // ba
      "out.push(p.removeChild(b) === b, names(), b.parentNode);"                                // true, a, null
      "p.appendChild(b); a.remove(); out.push(names(), a.parentNode); a.remove();"              // b, null
      "p.insertBefore(a, b); out.push(names(), a.nextSibling === b, b.previousSibling === a,"
      "  a.previousSibling, b.nextSibling, p.firstElementChild === a, p.lastElementChild === b,"
      "  p.childElementCount, p.children.length, p.children[1] === b, p.hasChildNodes());"
      "out.push(p.contains(p), p.contains(a), a.contains(p), p.contains(null), a.parentElement === p,"
      "  document.documentElement.parentElement, p.getRootNode() === p, a.getRootNode() === p,"
      "  document.body.getRootNode() === document);"
      "out.join(',');"),
      std::string("bai,true,ba,,ba,true,a,,b,,ab,true,true,,,true,true,2,2,true,true,"
                  "true,true,false,false,true,,true,true,true"));
  // The DOM's exceptions for the wrong arguments.
  CHECK_EQ(domEval(runtime,
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.name; } }"
      "var q = document.createElement('q');"
      "[attempt(function () { p.removeChild(q); }),"
      " attempt(function () { p.insertBefore(q, document.createElement('s')); }),"
      " attempt(function () { p.replaceChild(q, document.createElement('s')); }),"
      " attempt(function () { p.appendChild({}); }),"
      " attempt(function () { p.appendChild(); }),"
      " attempt(function () { p.insertBefore(q); }),"
      " attempt(function () { p.appendChild(document); })].join(',');"),
      std::string("NotFoundError,NotFoundError,NotFoundError,TypeError,TypeError,TypeError,HierarchyRequestError"));
}

// documentElement / head / body are in the tree, lookups walk it, and the
// `app` fallback still answers without markup.
void domDocumentTree() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var html = document.documentElement;"
      "[document.nodeType, document.nodeName, html.tagName, html.parentNode === document,"
      " document.childNodes.length, html.childNodes.length, document.head === html.firstChild,"
      " document.body === html.lastChild, document.head.tagName, document.body.tagName,"
      " document.activeElement === document.body, document.body.ownerDocument === document, document.ownerDocument,"
      " document.defaultView === window, document.body.isConnected, document.createElement('div').isConnected].join(',');"),
      std::string("9,#document,HTML,true,1,2,true,true,HEAD,BODY,true,true,,true,true,false"));
  CHECK_EQ(domEval(runtime,
      "var d = document.createElement('div'); d.id = 'panel'; d.className = 'card big';"
      "var detached = document.createElement('div'); detached.id = 'nowhere';"
      "var before = document.getElementById('panel');"
      "document.body.appendChild(d);"
      "var link = document.createElement('link'); document.head.appendChild(link);"
      "[before, document.getElementById('panel') === d, document.getElementById('nowhere'),"
      " document.getElementsByTagName('link').length, document.getElementsByTagName('LINK')[0] === link,"
      " document.getElementsByTagName('*').length, document.getElementsByClassName('big card')[0] === d,"
      " document.getElementsByClassName('card missing').length, document.body.getElementsByTagName('link').length,"
      " document.getElementsByTagName('div').item(0) === d].join(',');"),
      std::string(",true,,1,true,5,true,0,0,true"));
  // The app fallback: no markup declares #app, so the lookup makes one in <body>
  // -- in the tree, so everything else finds it too.
  CHECK_EQ(domEval(runtime,
      "var app = document.getElementById('app');"
      "[app.tagName, app.id, app.parentNode === document.body, document.getElementById('app') === app,"
      " document.querySelector('#app') === app, document.getElementById('APP')].join(',');"),
      std::string("DIV,app,true,true,true,"));
  // One document, one element child; constructors are the DOM's.
  CHECK_EQ(domEval(runtime,
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.name; } }"
      "[attempt(function () { document.appendChild(document.createElement('html')); }),"
      " attempt(function () { document.createElement('not valid'); }),"
      " attempt(function () { new Node(); }), attempt(function () { new Element(); }),"
      " attempt(function () { new HTMLElement(); }), attempt(function () { new Document(); }),"
      " document instanceof Document, document instanceof Node, document instanceof EventTarget,"
      " document.body instanceof HTMLElement, document.body instanceof Element,"
      " document.createElement('canvas') instanceof HTMLElement, new Image() instanceof HTMLElement,"
      " Node.ELEMENT_NODE, document.body.DOCUMENT_NODE].join(',');"),
      std::string("HierarchyRequestError,InvalidCharacterError,TypeError,TypeError,TypeError,TypeError,"
                  "true,true,true,true,true,true,true,1,9"));
}

void domAttributes() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var e = document.createElement('div');"
      "e.setAttribute('Data-Role', 7);"
      "e.id = 'x'; e.className = 'a b';"
      "[e.getAttribute('data-role'), e.getAttribute('DATA-ROLE'), e.hasAttribute('data-role'),"
      " e.getAttribute('id'), e.getAttribute('class'), e.getAttributeNames().join('+'),"
      " (e.removeAttribute('data-role'), e.hasAttribute('data-role')), e.getAttribute('missing'),"
      " (e.setAttribute('id', 'y'), e.id), (e.removeAttribute('class'), e.className)].join(',');"),
      std::string("7,7,true,x,a b,data-role+id+class,false,,y,"));
  // classList is live and reflects the class attribute both ways.
  CHECK_EQ(domEval(runtime,
      "var c = document.createElement('div'); var list = c.classList;"
      "list.add('a', 'b', 'a'); var r = [c.className, list.length, list[1]];"
      "r.push(list.toggle('a'), list.toggle('z', true), list.contains('z'), c.className);"
      "r.push(list.replace('b', 'c'), c.className, list.remove('z'), c.className);"
      "c.setAttribute('class', '  p  q p '); r.push(list.length, list[0], list[1], list.value, c.classList === list);"
      "r.join(',');"),
      std::string("a b,2,b,false,true,true,b z,true,c z,,c,2,p,q,  p  q p ,true"));
  // replace() into a token already in the set keeps one, where the first of
  // the two stood.
  CHECK_EQ(domEval(runtime,
      "var d = document.createElement('div'); d.className = 'a b c';"
      "var out = [d.classList.replace('a', 'c'), d.className];"
      "d.className = 'a b c'; out.push(d.classList.replace('c', 'a'), d.className, d.classList.length);"
      "out.push(d.classList.replace('zz', 'b'), d.className);"
      "out.join(',');"),
      std::string("true,c b,true,a b,2,false,a b"));
  CHECK_EQ(domEval(runtime,
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.name; } }"
      "var t = document.createElement('div');"
      "[attempt(function () { t.classList.add(''); }), attempt(function () { t.classList.add('a b'); }),"
      " attempt(function () { t.setAttribute('a b', '1'); }), attempt(function () { t.setAttribute('x'); }),"
      " t.hasAttribute('class')].join(',');"),
      std::string("SyntaxError,InvalidCharacterError,InvalidCharacterError,TypeError,false"));
}

// Matrix row "Selector": the subset, first match in document order, or null.
void domSelectors() {
  auto runtime = domRuntime();
  if (!runtime) return;
  domEval(runtime,
      "globalThis.meta = document.createElement('meta');"
      "meta.setAttribute('property', 'csp-nonce'); meta.setAttribute('nonce', 'n0nce');"
      "document.head.appendChild(meta);"
      "globalThis.first = document.createElement('div'); first.id = 'first'; first.className = 'a b';"
      "globalThis.second = document.createElement('div'); second.className = 'a';"
      "globalThis.direct = document.createElement('p');"
      "globalThis.nested = document.createElement('p');"
      "var span = document.createElement('span'); span.appendChild(nested);"
      "first.appendChild(direct); first.appendChild(span);"
      "document.body.appendChild(first); document.body.appendChild(second); 'built';");
  CHECK_EQ(domEval(runtime,
      "var d = document;"
      "[d.querySelector('meta[property=csp-nonce]') === meta, d.querySelector(\"meta[property='csp-nonce']\") === meta,"
      " d.querySelector('meta[property=\"csp-nonce\"]').getAttribute('nonce'), d.querySelector('[nonce]') === meta,"
      " d.querySelector('meta[property=other]'), d.querySelector('#first') === first, d.querySelector('.a.b') === first,"
      " d.querySelector('.a') === first, d.querySelector('div.a') === first,"
      " d.querySelector('.b.c'), d.querySelector('div > p') === direct, d.querySelectorAll('div > p').length,"
      " d.querySelectorAll('div p').length, d.querySelector('div p') === direct, d.querySelector('body span > p') === nested,"
      " d.querySelector('html > body > div#first.a > span p') === nested, d.querySelectorAll('*').length,"
      " d.querySelectorAll('p, meta').length, d.querySelectorAll('p, meta')[0] === meta,"
      " d.querySelectorAll('  .a  ,  #first  ').length, d.querySelector('DIV') === first, d.querySelector('video')].join(',');"),
      std::string("true,true,n0nce,true,,true,true,true,true,,true,1,2,true,true,true,9,3,true,2,true,"));
  // Scoped to an element's descendants, matched against the whole tree -- as in a browser.
  CHECK_EQ(domEval(runtime,
      "var found = first.querySelectorAll('div p');"
      "document.body.appendChild(document.createElement('p'));"
      "[first.querySelector('body p') === direct, found.length, found instanceof NodeList,"
      " span.querySelector('div > span > p') === nested, first.querySelector('#first'),"
      " nested.matches('div p'), nested.matches('div > p'), nested.matches('span > p, .none'),"
      " nested.closest('#first') === first, nested.closest('p') === nested, nested.closest('section'),"
      " direct.closest('body > div') === first].join(',');"),
      std::string("true,2,true,true,,true,false,true,true,true,,true"));
  // Collections iterate the way bundles iterate them: the module-preload
  // polyfill does `for (const e of document.querySelectorAll(...))`.
  CHECK_EQ(domEval(runtime,
      "for (var k = 0; k < 2; k++) {"
      "  var link = document.createElement('link'); link.setAttribute('rel', 'modulepreload');"
      "  document.head.appendChild(link);"
      "}"
      "var names = [];"
      "for (const e of document.querySelectorAll('link[rel=\"modulepreload\"]')) names.push(e.localName);"
      "var tags = 0; for (const e of document.getElementsByTagName('link')) tags++;"
      "var kids = []; for (const e of document.head.childNodes) kids.push(e.localName);"
      "[names.join('+'), [...document.querySelectorAll('link[rel=\"modulepreload\"]')].length,"
      " Array.from(document.querySelectorAll('link'))[1] === document.head.lastChild,"
      " tags, [...document.getElementsByTagName('link')].length,"
      " Array.from(document.getElementsByTagName('meta'))[0] === meta,"
      " kids.join('+'), [...document.head.childNodes][0] === meta, Array.from(document.head.childNodes).length,"
      " [...document.body.children].length, Array.from(first.classList).join('+'),"
      " [...document.querySelectorAll('.nothing')].length].join(',');"),
      std::string("link+link,2,true,2,2,true,meta+link+link,true,3,3,a+b,0"));
  // CSS escapes in identifiers.
  CHECK_EQ(domEval(runtime,
      "var escaped = document.createElement('div'); escaped.id = 'a:b'; escaped.className = '1x';"
      "document.body.appendChild(escaped);"
      "[document.querySelector('#a\\\\:b') === escaped, document.querySelector('#a\\\\3A b') === escaped,"
      " document.querySelector('.\\\\31 x') === escaped, document.querySelector('div.\\\\31x#a\\\\:b') === escaped,"
      " document.querySelector('.\\\\32 x'), escaped.matches('[id=\"a:b\"]')].join(',');"),
      std::string("true,true,true,true,,true"));
}

// Anything outside the subset, or not a selector at all, throws a SyntaxError
// DOMException rather than quietly matching nothing.
void domSelectorSyntax() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var bad = ['', ' ', 'div[', 'div,', ',div', '>div', 'div >', '#1', '.', '[a=1]', 'a b)', '[a=\"b]',"
      "           'a + b', 'a ~ b', 'a:hover', 'p::before', '[a^=b]', '[a|=b]', '[a=b i]', 'svg|rect'];"
      "bad.map(function (s) {"
      "  try { document.querySelector(s); return 'NO THROW ' + JSON.stringify(s); }"
      "  catch (e) { return e instanceof DOMException && e.name === 'SyntaxError' && e.code === 12 ? 'ok' : 'wrong ' + e; }"
      "}).filter(function (r) { return r !== 'ok'; }).join('; ') || 'all threw';"),
      std::string("all threw"));
  CHECK_EQ(domEval(runtime,
      "var e = document.body;"
      "[['querySelectorAll', document], ['matches', e], ['closest', e], ['querySelector', e]].map(function (m) {"
      "  try { m[1][m[0]]('a + b'); return 'no throw'; } catch (x) { return x.name; }"
      "}).join(',');"),
      std::string("SyntaxError,SyntaxError,SyntaxError,SyntaxError"));
  // The message says which: invalid, or valid CSS outside the subset.
  CHECK_CONTAINS(domEval(runtime, "try { document.querySelector('div['); } catch (e) { e.message }"),
                 "'div[' is not a valid selector");
  const std::string unsupported =
      domEval(runtime, "try { document.body.matches('li:first-child'); } catch (e) { e.message }");
  CHECK_CONTAINS(unsupported, "Failed to execute 'matches' on 'Element'");
  CHECK_CONTAINS(unsupported, "a pseudo-class is outside ScreenKit's subset");
}

// Matrix row "Bubbling": capture, target, bubble along the tree to window.
void domEventPropagation() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var log = [];"
      "var child = document.createElement('button'); var box = document.createElement('div');"
      "box.appendChild(child); document.body.appendChild(box);"
      "function note(name) { return function (e) {"
      "  log.push(name + ':' + e.eventPhase + ':' + (e.target === child) + ':' + (e.currentTarget === this));"
      "}; }"
      "document.body.addEventListener('click', note('body'));"
      "window.addEventListener('click', note('window-capture'), true);"
      "document.addEventListener('click', note('document-capture'), { capture: true });"
      "child.addEventListener('click', note('target'));"
      "child.addEventListener('click', note('target-capture'), true);"
      "box.addEventListener('click', note('box'));"
      "window.addEventListener('click', function (e) { log.push('window:' + e.eventPhase + ':' + e.composedPath().length); });"
      "var result = child.dispatchEvent(new Event('click', { bubbles: true }));"
      "log.push('| ' + result + ' ' + e_phase_after());"
      "function e_phase_after() { var e = new Event('click'); child.dispatchEvent(e); return e.eventPhase + ' ' + (e.currentTarget === null) + ' ' + e.composedPath().length; }"
      "log.join(' ');"),
      std::string("window-capture:1:true:true document-capture:1:true:true target-capture:2:true:true "
                  "target:2:true:true box:3:true:true body:3:true:true window:3:6 "
                  "window-capture:1:true:true document-capture:1:true:true target-capture:2:true:true "
                  "target:2:true:true | true 0 true 0"));
  // stopPropagation finishes the current node's listeners and halts the path;
  // stopImmediatePropagation halts at once. A detached node's path ends at its root.
  CHECK_EQ(domEval(runtime,
      "var trail = [];"
      "var leaf = document.createElement('i'); child.appendChild(leaf);"
      "leaf.addEventListener('k', function (e) { trail.push('leaf1'); e.stopPropagation(); });"
      "leaf.addEventListener('k', function () { trail.push('leaf2'); });"
      "child.addEventListener('k', function () { trail.push('child'); });"
      "document.body.addEventListener('k', function () { trail.push('body'); });"
      "leaf.dispatchEvent(new Event('k', { bubbles: true }));"
      "child.addEventListener('j', function (e) { trail.push('j1'); e.stopImmediatePropagation(); });"
      "child.addEventListener('j', function () { trail.push('j2'); });"
      "document.body.addEventListener('j', function () { trail.push('j-body'); });"
      "leaf.dispatchEvent(new Event('j', { bubbles: true }));"
      "var lone = document.createElement('div'), loneChild = document.createElement('span'); lone.appendChild(loneChild);"
      "var path;"
      "loneChild.addEventListener('x', function (e) { path = e.composedPath().length; });"
      "window.addEventListener('x', function () { trail.push('LEAKED TO WINDOW'); });"
      "loneChild.dispatchEvent(new Event('x', { bubbles: true }));"
      "leaf.removeEventListener('k', leaf.__never);"
      "trail.join(',') + ' | ' + path;"),
      std::string("leaf1,leaf2,j1 | 2"));
  // A listener that throws is reported and the rest still run; preventDefault
  // reaches dispatchEvent's return value; an event cannot be dispatched twice at once.
  CHECK_EQ(domEval(runtime,
      "var ran = [];"
      "var node = document.createElement('div'); document.body.appendChild(node);"
      "node.addEventListener('boom', function () { throw new Error('listener threw'); });"
      "document.addEventListener('boom', function (e) { ran.push('document'); e.preventDefault();"
      "  try { node.dispatchEvent(e); ran.push('re-entered'); } catch (x) { ran.push(x.name); } });"
      "var returned = node.dispatchEvent(new Event('boom', { bubbles: true, cancelable: true }));"
      "ran.concat([returned]).join(',');"),
      std::string("document,InvalidStateError,false"));
  // The listeners a node runs are the ones it has when the event reaches it: one
  // removed earlier in the dispatch does not run, one added does not either. And
  // the path is fixed when dispatch starts: moving the target mid-dispatch does
  // not reroute the event to its new ancestors.
  CHECK_EQ(domEval(runtime,
      "var order = [];"
      "var from = document.createElement('div'), to = document.createElement('section'),"
      "    mover = document.createElement('span');"
      "from.appendChild(mover); document.body.appendChild(from); document.body.appendChild(to);"
      "function removedHere() { order.push('REMOVED ON TARGET RAN'); }"
      "function removedAhead() { order.push('REMOVED AHEAD RAN'); }"
      "mover.addEventListener('m', function () {"
      "  order.push('target');"
      "  mover.removeEventListener('m', removedHere); from.removeEventListener('m', removedAhead);"
      "  mover.addEventListener('m', function () { order.push('ADDED DURING RAN'); });"
      "  to.appendChild(mover);"
      "});"
      "mover.addEventListener('m', removedHere);"
      "from.addEventListener('m', removedAhead);"
      "from.addEventListener('m', function (e) {"
      "  order.push('old parent ' + (mover.parentNode === to) + ' ' + e.composedPath().length); });"
      "to.addEventListener('m', function () { order.push('NEW PARENT RAN'); });"
      "mover.dispatchEvent(new Event('m', { bubbles: true }));"
      "order.join(',');"),
      std::string("target,old parent true 6"));
  CHECK(capture.has(screenkit::LogLevel::Error, "Uncaught in boom listener: Error: listener threw"));
}

// Host key events target document.activeElement -- <body> -- and travel the
// whole path, still one listener per step with a microtask checkpoint between.
void domKeyTarget() {
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__keys = [];"
      "function note(name) { return function (e) {"
      "  __keys.push(name + ' ' + e.eventPhase + ' ' + (e.target === document.body) + ' ' + e.isTrusted);"
      "  Promise.resolve().then(function () { __keys.push(name + '.micro'); });"
      "}; }"
      "window.addEventListener('keydown', note('window-capture'), true);"
      "document.body.addEventListener('keydown', note('body'));"
      "document.documentElement.addEventListener('keydown', note('html'));"
      "document.addEventListener('keydown', note('document'));"
      "window.addEventListener('keydown', note('window'));"
      "'listening';");
  screenkit::InputRouter router(runtime);
  SDL_Event e{};
  e.key.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_UP; e.key.down = true;
  CHECK(router.handleEvent(e));
  CHECK_EQ(pumpThenRead(runtime, "__keys.join('\\n');"),
           std::string("window-capture 1 true true\nwindow-capture.micro\n"
                       "body 2 true true\nbody.micro\n"
                       "html 3 true true\nhtml.micro\n"
                       "document 3 true true\ndocument.micro\n"
                       "window 3 true true\nwindow.micro"));
}

// window.close(): how a page asks the host to leave, and the only thing that
// does. Back arrives as a key and nothing more -- exiting on an unhandled Back
// would exit on an overlay that closed without preventDefault(), on a page that
// takes Back on keyup, and on a gamepad's B button, which maps to GoBack too.
void domWindowClose() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  // No host hook -- every host but Android today: close() does nothing and does
  // not throw, as it does for a browser window a script did not open.
  CHECK_EQ(domEval(runtime,
      "var hook = typeof globalThis.__screenkitClose;"
      "window.close(); close();"
      "[hook, typeof window.close, window.close === close].join('|');"),
      std::string("undefined|function|true"));

  // With one, each call reaches it exactly once.
  domEval(runtime,
      "globalThis.__closes = 0;"
      "globalThis.__screenkitClose = function () { __closes++; };"
      "window.close(); 'closed';");
  CHECK_EQ(domEval(runtime, "String(__closes);"), std::string("1"));
  domEval(runtime, "close(); 'closed again';");
  CHECK_EQ(domEval(runtime, "String(__closes);"), std::string("2"));

  // A trusted Back reaches the page and stops there. Leaving is the page's call.
  domEval(runtime,
      "globalThis.__back = 0;"
      "window.addEventListener('keydown', function (e) { if (e.key === 'GoBack') __back++; });"
      "'listening';");
  screenkit::InputRouter router(runtime);
  SDL_Event e{};
  e.key.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_AC_BACK; e.key.down = true;
  CHECK(router.handleEvent(e));
  CHECK_EQ(pumpThenRead(runtime, "__back + '|' + __closes;"), std::string("1|2"));
}

// CSSStyleDeclaration on an element that does not paint: stored, read back, no
// cascade and no parsing beyond the declaration syntax.
void domStyleDeclaration() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var s = document.createElement('div').style;"
      "s.cssText = 'color: red; left: calc(1px + 2px) !important; background: url(\"a;b.png\"); --Brand: X Y; junk; : 1';"
      "[s.length, s.item(0), s.item(3), s.item(9), s.color, s.left, s.getPropertyPriority('left'), s.background,"
      " s.getPropertyValue('--Brand'), s.getPropertyValue('--brand'), s.cssText, s instanceof CSSStyleDeclaration].join('|');"),
      std::string("4|color|--Brand||red|calc(1px + 2px)|important|url(\"a;b.png\")|X Y||"
                  "color: red; left: calc(1px + 2px) !important; background: url(\"a;b.png\"); --Brand: X Y;|true"));
  CHECK_EQ(domEval(runtime,
      "var el = document.createElement('div');"
      "el.style.zIndex = '3'; el.style.setProperty('Margin-Top', '4px'); el.style.setProperty('width', '5px', 'IMPORTANT');"
      "el.style.setProperty('height', '6px', 'bogus');"
      "var r = [el.style.zIndex, el.style.getPropertyValue('z-index'), el.style.marginTop, el.style.cssText,"
      "         el.getAttribute('style')];"
      "r.push(el.style.removeProperty('z-index'), el.style.removeProperty('z-index'), el.style.length);"
      "el.style.marginTop = ''; el.style.width = null; r.push(el.style.cssText, el.getAttribute('style'));"
      "el.style = 'top: 1px'; r.push(el.style.top, el.getAttribute('style'));"
      "el.setAttribute('style', 'bottom: 2px'); r.push(el.style.top, el.style.bottom);"
      "el.removeAttribute('style'); r.push(el.style.length);"
      "var fromMarkup = document.createElement('div'); fromMarkup.setAttribute('style', 'right: 9px');"
      "r.push(fromMarkup.style.right, el.style === el.style);"
      "r.join('|');"),
      std::string("3|3|4px|z-index: 3; margin-top: 4px; width: 5px !important;|"
                  "z-index: 3; margin-top: 4px; width: 5px !important;|3||2|||1px|top: 1px;||2px|0|9px|true"));
  // Nothing about a <div>'s style is a canvas's business: no warnings.
  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Warn);
}

// setAttribute('style') on the canvas goes through the same subset parser as
// `style`, at once: normalised read-back, a rejected value not applied, and the
// one-time warning when it would move the canvas that *is* the page's frame --
// while the attribute keeps the text it was given, as in a browser.
void domCanvasStyleAttribute() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  auto warnings = [&capture](const char* needle) {
    int n = 0;
    for (const auto& line : capture.lines()) {
      if (line.level == screenkit::LogLevel::Warn && line.message.find(needle) != std::string::npos) ++n;
    }
    return n;
  };
  domEval(runtime,
      "globalThis.c = document.createElement('canvas'); c.getContext('webgl');"
      "c.setAttribute('style', 'LEFT: 10PX; Width: 50%; top: banana; z-index: +2'); 'set';");
  // Before anything reads `style`.
  CHECK_EQ(warnings("left: 10px is stored and reads back, but position and size in CSS do not move"), 1);
  CHECK_EQ(warnings("top: banana was not applied"), 1);
  CHECK_EQ(domEval(runtime,
      "[c.style.left, c.style.width, c.style.top, c.style.zIndex, c.getAttribute('style'), c.style.cssText,"
      " c.width + 'x' + c.height].join('|');"),
      std::string("10px|50%||2|LEFT: 10PX; Width: 50%; top: banana; z-index: +2|"
                  "left: 10px; width: 50%; z-index: 2;|320x180"));
  CHECK_EQ(domEval(runtime,
      "c.setAttribute('style', 'left: 0');"
      "var r = [c.style.left, c.style.width, c.style.length];"
      "c.removeAttribute('style'); r.push(c.style.length);"
      "var d = document.createElement('div'); d.setAttribute('style', 'top: banana'); r.push(d.style.top);"
      "r.join('|');"),
      std::string("0px||1|0|banana"));
  CHECK_EQ(warnings("do not move or resize the <canvas>"), 1);
  CHECK_EQ(warnings("was not applied"), 1);
}

// Matrix row "Canvas style": the CSS subset is parsed on the backed canvas and
// reads back normalised. This canvas took the page's frame before anything was
// set on it, so it *is* the drawable: a declaration that would move or resize
// it warns exactly once (a canvas placed before it asks for a context gets a
// layer of its own instead -- canvas-placed).
void domCanvasStyle() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;
  auto warnings = [&capture](const char* needle) {
    int n = 0;
    for (const auto& line : capture.lines()) {
      if (line.level == screenkit::LogLevel::Warn && line.message.find(needle) != std::string::npos) ++n;
    }
    return n;
  };

  // Fullscreen values move nothing and say nothing -- Lightning sets its canvas
  // width and height to the drawable at startup, in floating point.
  CHECK_EQ(domEval(runtime,
      "globalThis.c = document.createElement('canvas'); c.getContext('webgl');"
      "c.style.width = '320.0000016px'; c.style.height = '100%'; c.style.left = '0'; c.style.position = 'ABSOLUTE';"
      "c.style.transform = 'translate(0px) scale(1)'; c.style.zIndex = '2';"
      "c.style.cssText;"),
      std::string("width: 320.0000016px; height: 100%; left: 0px; position: absolute; transform: translate(0px) scale(1); z-index: 2;"));
  CHECK_EQ(warnings("ScreenKit:"), 0);

  CHECK_EQ(domEval(runtime,
      "c.style.cssText = 'left:10px;width:50%';"
      "[c.style.left, c.style.width, c.style.cssText, c.getAttribute('style'),"
      " c.width, c.height, JSON.stringify(c.getBoundingClientRect())].join('|');"),
      std::string("10px|50%|left: 10px; width: 50%;|left: 10px; width: 50%;|320|180|"
                  "{\"x\":0,\"y\":0,\"left\":0,\"top\":0,\"width\":320,\"height\":180,\"right\":320,\"bottom\":180}"));
  CHECK_EQ(warnings("do not move or resize the <canvas>"), 1);

  // Said once, however many more there are.
  domEval(runtime, "c.style.top = '5px'; c.style.height = '20vh'; c.style.transform = 'translateX(4px)'; 'more';");
  CHECK_EQ(warnings("do not move or resize the <canvas>"), 1);

  // Parsed, not just stored: values normalise, and one outside the subset is
  // not applied -- the old value stays -- and says so.
  CHECK_EQ(domEval(runtime,
      "c.style.transform = 'translate(1PX, -2.50%) scaleY(50%)'; c.style.opacity = '50%'; c.style.zIndex = '+7';"
      "c.style.display = 'NONE'; c.style.left = 'calc(1px + 1px)'; c.style.width = '-3px'; c.style.opacity = 'half';"
      "c.style.bottom = 'inherit';"
      "[c.style.transform, c.style.opacity, c.style.zIndex, c.style.display, c.style.left, c.style.width,"
      " c.style.bottom].join('|');"),
      std::string("translate(1px, -2.5%) scaleY(0.5)|0.5|7|none|10px|50%|inherit"));
  CHECK_EQ(warnings("left: calc(1px + 1px) was not applied"), 1);
  CHECK_EQ(warnings("width: -3px was not applied"), 1);
  CHECK_EQ(warnings("opacity: half was not applied"), 1);
  CHECK_EQ(warnings("stays visible"), 1);
  CHECK_EQ(domEval(runtime, "[c.width, c.height].join('x');"), std::string("320x180"));

  // The number grammar: an exponent in either case, a digit after a decimal
  // point, and nothing infinite. `display` takes keywords, not any word.
  CHECK_EQ(domEval(runtime,
      "var r = [];"
      "c.style.width = '1E3px'; r.push(c.style.width);"
      "c.style.width = '1.px'; r.push(c.style.width);"
      "c.style.width = '1e400px'; r.push(c.style.width);"
      "c.style.opacity = '1.'; r.push(c.style.opacity);"
      "c.style.zIndex = '1' + new Array(400).join('0'); r.push(c.style.zIndex);"
      "c.style.display = 'foo'; r.push(c.style.display);"
      "c.style.display = 'Inline  FLEX'; r.push(c.style.display);"
      "r.join('|');"),
      std::string("1000px|1000px|1000px|0.5|7|none|inline flex"));
  CHECK_EQ(warnings("display: foo was not applied"), 1);
  // Names that exist on Object.prototype are unknown names, not lookups into it.
  CHECK_EQ(domEval(runtime,
      "var r = [];"
      "c.style.setProperty('__proto__', 'x'); r.push(c.style.getPropertyValue('__proto__'));"
      "c.style.setProperty('constructor', 'y'); r.push(c.style.getPropertyValue('constructor'));"
      "c.style.transform = 'constructor(1)'; r.push(c.style.transform);"
      "c.style.transform = '__proto__(1)'; r.push(c.style.transform);"
      "c.style.left = '1hasOwnProperty'; r.push(c.style.left);"
      "r.join('|');"),
      std::string("x|y|translate(1px, -2.5%) scaleY(0.5)|translate(1px, -2.5%) scaleY(0.5)|10px"));
  CHECK_EQ(warnings("transform: constructor(1) was not applied"), 1);
  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// ============================================================================
// Runtime networking (spec-runtime-networking.md)
//
// One row per line of the spec's matrix, each against the local fixture server
// ctest starts before them (net-fixture-start, runtime/tests/net/server.mjs):
// plain HTTP, HTTPS behind a generated test CA the runtime is told to trust
// through RuntimeConfig::testTlsAnchors, and three certificates the OS must
// refuse. Every URL a row touches is 127.0.0.1 or localhost -- nothing here
// reaches the public internet.
//
// The rows run the real prelude over the real HTTP client -- NSURLSession on
// Apple, OkHttp on Android, cpp-httplib over the image's OpenSSL on Linux: a JS
// `run(async function () {...})` logs one line per fact, and the row compares
// the whole transcript, so a failure shows every line that differs rather than
// the first boolean that did.
// ============================================================================


struct NetFixture {
  bool ok = false;
  int http = 0;
  int https = 0;
  int untrusted = 0;
  int expired = 0;
  int wrongHost = 0;
  int closedPort = 0;
  std::vector<std::uint8_t> ca;
};

int jsonNumber(const std::string& text, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  auto pos = text.find(needle);
  if (pos == std::string::npos) return 0;
  pos += needle.size();
  while (pos < text.size() && text[pos] == ' ') ++pos;
  return std::atoi(text.c_str() + pos);
}

/// Where ctest's fixture server writes its ports, unless the environment names
/// another directory -- the device-side copy, for the emulator rows reached over
/// `adb reverse` (tools/android/android.sh test). Rows write here too, so this
/// has to be the one answer: the cookie jar's directory comes from it.
std::string netFixtureDir() {
  const char* override_ = std::getenv("SCREENKIT_NET_FIXTURE_DIR");
  if (override_ != nullptr && *override_ != '\0') return override_;
  return SCREENKIT_NET_FIXTURE_DIR;
}

NetFixture netFixture() {
  NetFixture f;
  if (!screenkit::net::networkAvailable()) {
    test::skip("this platform has no HTTP client yet");
    return f;
  }
  const std::string dir = netFixtureDir();
  std::ifstream in(dir + "/servers.json");
  if (!in) {
    test::fail(__FILE__, __LINE__,
               "the networking fixture server is not running: ctest starts it (net-fixture-start); by hand, "
               "node runtime/tests/net/server.mjs --dir " + dir);
    return f;
  }
  std::stringstream text;
  text << in.rdbuf();
  const std::string json = text.str();
  f.http = jsonNumber(json, "http");
  f.https = jsonNumber(json, "https");
  f.untrusted = jsonNumber(json, "httpsUntrusted");
  f.expired = jsonNumber(json, "httpsExpired");
  f.wrongHost = jsonNumber(json, "httpsWrongHost");
  f.closedPort = jsonNumber(json, "closedPort");
  std::ifstream der(dir + "/ca.der", std::ios::binary);
  f.ca.assign(std::istreambuf_iterator<char>(der), std::istreambuf_iterator<char>());
  f.ok = f.http && f.https && f.untrusted && f.expired && f.wrongHost && f.closedPort && !f.ca.empty();
  CHECK(f.ok);
  return f;
}

// Shared by every row: the fixture's addresses and a few helpers the
// transcripts are written with.
const char* kNetPrelude = R"JS(
var H = __net.http, S = __net.https, U = __net.untrusted, E = __net.expired, W = __net.wrongHost;
var CP = __net.closedPort;
globalThis.__r = [];
globalThis.__done = false;
function log(line) { __r.push(String(line)); }
function run(body) {
  body().then(function () { __done = true; },
              function (e) { log('THREW ' + (e && e.name) + ': ' + (e && e.message) + '\n' + (e && e.stack)); __done = true; });
}
function sleep(ms) { return new Promise(function (resolve) { setTimeout(resolve, ms); }); }
function until(test, ms) {
  var deadline = Date.now() + (ms || 10000);
  return new Promise(function (resolve, reject) {
    (function poll() {
      if (test()) return resolve();
      if (Date.now() > deadline) return reject(new Error('timed out waiting'));
      setTimeout(poll, 10);
    })();
  });
}
function stats(id) { return fetch(H + '/stats?id=' + encodeURIComponent(id)).then(function (r) { return r.json(); }); }
var utf8 = new TextDecoder();
function text(bytes) { return utf8.decode(bytes); }
function b64(bytes) {
  var s = '';
  for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}
function readAll(stream) {
  var reader = stream.getReader(), out = '';
  function next() {
    return reader.read().then(function (r) {
      if (r.done) return out;
      out += text(r.value);
      return next();
    });
  }
  return next();
}
// An XMLHttpRequest run to its loadend, with the events it fired in order
// (consecutive progress events collapsed).
function xhr(method, url, options) {
  options = options || {};
  return new Promise(function (resolve) {
    var x = new XMLHttpRequest(), events = [];
    x.open(method, url);
    if (options.responseType) x.responseType = options.responseType;
    if (options.timeout) x.timeout = options.timeout;
    (options.headers || []).forEach(function (h) { x.setRequestHeader(h[0], h[1]); });
    x.onreadystatechange = function () { events.push('readystatechange:' + x.readyState); };
    ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout'].forEach(function (type) {
      x.addEventListener(type, function () {
        if (type !== 'progress' || events[events.length - 1] !== 'progress') events.push(type);
      });
    });
    if (options.upload) {
      ['loadstart', 'progress', 'load', 'loadend'].forEach(function (type) {
        x.upload.addEventListener(type, function (e) {
          if (type !== 'progress' || events[events.length - 1].indexOf('upload.progress') !== 0) events.push('upload.' + type);
          if (type === 'loadend') events.push('upload.total=' + e.total + ',loaded=' + e.loaded);
        });
      });
    }
    x.onloadend = function () { events.push('loadend'); resolve({ xhr: x, events: events.join(',') }); };
    x.send(options.body === undefined ? null : options.body);
    if (options.abortAfter) setTimeout(function () { x.abort(); }, options.abortAfter);
  });
}
// A WebSocket with its events recorded; `closed` settles on the close event.
function ws(url, protocols) {
  var socket = protocols === undefined ? new WebSocket(url) : new WebSocket(url, protocols);
  var events = [];
  var closed = new Promise(function (resolve) {
    socket.onopen = function () { events.push('open'); };
    socket.onerror = function (e) { events.push('error'); };
    socket.onclose = function (e) { events.push('close:' + e.code + ':' + e.reason + ':' + e.wasClean); resolve(); };
  });
  return { socket: socket, events: events, closed: closed };
}
function opened(w) {
  return new Promise(function (resolve) {
    if (w.socket.readyState === 1) return resolve();
    w.socket.addEventListener('open', function () { resolve(); }, { once: true });
  });
}
function nextMessage(socket) {
  return new Promise(function (resolve) {
    socket.addEventListener('message', function (e) { resolve(e.data); }, { once: true });
  });
}
// An EventSource that must fail for good: its events, 300 ms after the first error.
function esFailure(url) {
  return new Promise(function (resolve) {
    var source = new EventSource(url), events = [];
    source.onopen = function () { events.push('open'); };
    source.onerror = function () {
      events.push('error:' + source.readyState);
      if (events.length === 1) setTimeout(function () { source.close(); resolve(events.join(',')); }, 300);
    };
  });
}
'net prelude';
)JS";

/// A runtime with GL, the DOM shim and the fixture's addresses. `trustTestCa`
/// false leaves the generated CA out of the trust anchors -- production trust.
std::shared_ptr<screenkit::Runtime> netRuntime(const NetFixture& f, bool trustTestCa = true) {
  auto config = testConfig();
  config.name = "net";
  if (trustTestCa) config.testTlsAnchors.push_back(f.ca);
#ifdef SCREENKIT_NET_TESTS_ONLY
  // The cross-compiled net binary has no offscreen GL surface to bootstrap
  // (GlSurfaceSdl needs a window), so the rows that do not draw run without one.
  // net-image is the one that does, and it is not in this binary's list.
  auto runtime = screenkit::Runtime::create(std::move(config));
  CHECK(runtime != nullptr);
#else
  auto runtime = glRuntime(64, 64, std::move(config));
#endif
  if (!runtime) return nullptr;
  if (!installDomShim(runtime)) return nullptr;
  const std::string base = "https://127.0.0.1:";
  const std::string addresses =
      "globalThis.__net = { http: 'http://127.0.0.1:" + std::to_string(f.http) + "', https: '" + base +
      std::to_string(f.https) + "', httpsPort: " + std::to_string(f.https) + ", untrusted: '" + base +
      std::to_string(f.untrusted) + "', expired: '" + base + std::to_string(f.expired) + "', wrongHost: '" +
      base + std::to_string(f.wrongHost) + "', closedPort: " + std::to_string(f.closedPort) + " }; 'ok';";
  domEval(runtime, addresses.c_str());
  domEval(runtime, kNetPrelude);
  return runtime;
}

/// Poll a JS expression until it is true, on the runtime's own clock -- the
/// loop keeps running while this waits, as a host's would.
bool waitForJs(const std::shared_ptr<screenkit::Runtime>& runtime, const std::string& expr,
               int timeoutMs = 30000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto r = runtime->evaluateSource(expr, "wait.js");
    if (r.ok && r.value == "true") return true;
    hostWait(10);
  }
  test::fail(__FILE__, __LINE__, "timed out waiting for: " + expr);
  return false;
}

/// Run a transcript (`run(async function () {...})`) and return its lines.
std::string netTranscript(const std::shared_ptr<screenkit::Runtime>& runtime, const char* source,
                          int timeoutMs = 60000) {
  const auto started = runtime->evaluateSource(source, "net-row.js");
  CHECK(started.ok);
  if (!started.ok) {
    std::fprintf(stderr, "  threw: %s\n", started.error.c_str());
    return "";
  }
  waitForJs(runtime, "__done", timeoutMs);
  return domEval(runtime, "__r.join('\\n');");
}

/// One transcript per client, where they genuinely disagree and the difference
/// is a recorded divergence rather than a bug (spec-platform-http-clients.md,
/// spec-linux-http-client.md). Used only by the rows below that carry more than
/// one: everything else has a single transcript that holds on all three.
#if defined(__ANDROID__)
#define NET_PER_PLATFORM(apple, android, linux) (android)
#elif defined(__linux__)
#define NET_PER_PLATFORM(apple, android, linux) (linux)
#else
#define NET_PER_PLATFORM(apple, android, linux) (apple)
#endif

void checkTranscript(const std::string& got, const std::string& expected) {
  CHECK_EQ(got, expected);
  if (got != expected) {
    std::fprintf(stderr, "  --- got ---\n%s\n  --- expected ---\n%s\n", got.c_str(), expected.c_str());
  }
}

// --- matrix row: HTTP GET -------------------------------------------------------
void netHttpGet() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var r = await fetch(H + '/data.json');
  log([r.ok, r.status, r.statusText, r.type, r.redirected, r.url === H + '/data.json',
       r.headers.get('content-type'), r.headers.get('X-Fixture'), r.headers.get('content-length')].join(' '));
  var j = await r.json();
  log(j.hello + ' ' + j.n + ' bodyUsed=' + r.bodyUsed);
  var names = [];
  r.headers.forEach(function (value, name) {
    if (name !== 'date' && name !== 'connection' && name !== 'keep-alive') names.push(name);
  });
  log(names.join(','));
  var a = await (await fetch(H + '/conn')).text();
  var b = await (await fetch(H + '/conn')).text();
  log('keep-alive reuses the connection ' + (a === b));
  var x = await xhr('GET', H + '/data.json', { responseType: 'json' });
  log(['xhr', x.xhr.status, x.xhr.statusText, x.xhr.response.n, x.xhr.getResponseHeader('Content-Type'),
       x.xhr.responseURL === H + '/data.json', x.events].join(' '));
  log(x.xhr.getAllResponseHeaders().split('\r\n').filter(function (l) { return /^(content-type|x-fixture):/.test(l); }).join('|'));
  // A HEAD and a 204 carry no body however their headers read: each ends at its
  // head, and neither leaves the connection desynchronised for what follows.
  // Which connection that is belongs to NSURLSession's and OkHttp's own pools
  // now, so what is asserted is that keep-alive still holds afterwards, not that
  // it is the same socket as before (spec-platform-http-clients.md).
  var started = Date.now();
  var head = await fetch(H + '/data.json', { method: 'HEAD' });
  log('HEAD ' + head.status + ' body=' + head.body + ' text="' + await head.text() + '" content-length=' + head.headers.get('content-length'));
  await (await fetch(H + '/conn')).text();
  var empty = await fetch(H + '/status/204');
  log('204 ' + empty.status + ' body=' + empty.body + ' text="' + await empty.text() + '"');
  var afterEmpty = await (await fetch(H + '/conn')).text();
  var again = await (await fetch(H + '/conn')).text();
  log('keep-alive still reuses after HEAD and 204 ' + (afterEmpty === again) + ', prompt ' + (Date.now() - started < 2000));
});
)JS");
  checkTranscript(got,
      "true 200 OK basic false true application/json screenkit 24\n"
      "world 42 bodyUsed=true\n"
      "content-length,content-type,x-fixture\n"
      "keep-alive reuses the connection true\n"
      "xhr 200 OK 42 application/json true loadstart,readystatechange:2,readystatechange:3,progress,readystatechange:4,load,loadend\n"
      "content-type: application/json|x-fixture: screenkit\n"
      "HEAD 200 body=null text=\"\" content-length=24\n"
      "204 204 body=null text=\"\"\n"
      "keep-alive still reuses after HEAD and 204 true, prompt true");
}

// --- matrix row: HTTPS ----------------------------------------------------------
void netHttps() {
  auto f = netFixture();
  if (!f.ok) return;
  {
    auto runtime = netRuntime(f);
    if (!runtime) return;
    const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var r = await fetch(S + '/data.json');
  log('trusted ' + r.status + ' ' + (await r.json()).n + ' ' + (r.url === S + '/data.json'));
  var byName = await fetch('https://localhost:' + __net.httpsPort + '/data.json');
  log('by host name ' + byName.status + ' ' + (await byName.json()).hello);
  var targets = [['untrusted', U], ['expired', E], ['wrong host', W]];
  for (var i = 0; i < targets.length; i++) {
    var label = targets[i][0], base = targets[i][1];
    try { await fetch(base + '/data.json'); log(label + ' fetch RESOLVED'); }
    catch (e) { log(label + ' fetch ' + e.name + ' ' + e.cause); }
    var x = await xhr('GET', base + '/data.json');
    log(label + ' xhr ' + x.events + ' status=' + x.xhr.status);
    var w = ws(base.replace('https:', 'wss:') + '/ws');
    await w.closed;
    log(label + ' ws ' + w.events.join(','));
    log(label + ' eventsource ' + await esFailure(base + '/events?id=tls-' + i));
  }
  var echo = ws(S.replace('https:', 'wss:') + '/ws');
  await opened(echo);
  echo.socket.send('over tls');
  log('wss ' + await nextMessage(echo.socket));
  echo.socket.close(1000);
  await echo.closed;
  log('wss ' + echo.events.join(','));
});
)JS");
    checkTranscript(got,
        "trusted 200 42 true\n"
        "by host name 200 world\n"
        "untrusted fetch TypeError tls\n"
        "untrusted xhr loadstart,readystatechange:4,error,loadend status=0\n"
        "untrusted ws error,close:1006::false\n"
        "untrusted eventsource error:2\n"
        "expired fetch TypeError tls\n"
        "expired xhr loadstart,readystatechange:4,error,loadend status=0\n"
        "expired ws error,close:1006::false\n"
        "expired eventsource error:2\n"
        "wrong host fetch TypeError tls\n"
        "wrong host xhr loadstart,readystatechange:4,error,loadend status=0\n"
        "wrong host ws error,close:1006::false\n"
        "wrong host eventsource error:2\n"
        "wss over tls\n"
        "wss open,close:1000::true");
  }
  // Without the test anchor the same server is just another untrusted one: the
  // anchor is a RuntimeConfig field, and nothing JS can reach widens trust.
  auto production = netRuntime(f, false);
  if (!production) return;
  const std::string got = netTranscript(production, R"JS(
run(async function () {
  try { await fetch(S + '/data.json'); log('RESOLVED without the anchor'); }
  catch (e) { log('no anchor ' + e.name + ' ' + e.cause); }
  log('no trust knob ' + Object.keys(__screenkit.net).sort().join(','));
});
)JS");
  checkTranscript(got, "no anchor TypeError tls\n"
                       "no trust knob abort,acknowledge,close,decodeImage,finish,getCookies,openSocket,request,send,setCookie,write");
}

// --- matrix row: request body ---------------------------------------------------
void netRequestBody() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var enc = new TextEncoder();
  async function echo(label, init, expected) {
    var reply = await (await fetch(H + '/echo', init)).json();
    log([label, reply.method, reply.body === b64(expected), reply.headers['content-type'],
         reply.headers['content-length'], reply.headers['transfer-encoding']].join(' '));
    return reply;
  }
  await echo('string', { method: 'POST', body: 'héllo ✓' }, enc.encode('héllo ✓'));
  var json = await echo('json', { method: 'POST', body: JSON.stringify({ a: 1 }),
                                  headers: { 'Content-Type': 'application/json', 'X-Custom': 'yes' } }, enc.encode('{"a":1}'));
  log('custom header ' + json.headers['x-custom']);
  await echo('arraybuffer', { method: 'POST', body: new Uint8Array([0, 1, 2, 255]).buffer }, new Uint8Array([0, 1, 2, 255]));
  await echo('view', { method: 'PUT', body: new Uint8Array([9, 8, 7, 6, 5]).subarray(1, 4) }, new Uint8Array([8, 7, 6]));
  await echo('blob', { method: 'POST', body: new Blob(['blob-', 'data'], { type: 'text/x-blob' }) }, enc.encode('blob-data'));
  await echo('params', { method: 'PATCH', body: new URLSearchParams({ q: 'a b', n: '1' }) }, enc.encode('q=a%20b&n=1'));

  var form = new FormData();
  form.append('field', 'value');
  form.append('file', new Blob(['file-bytes'], { type: 'text/plain' }), 'f.txt');
  var reply = await (await fetch(H + '/echo', { method: 'POST', body: form })).json();
  var type = reply.headers['content-type'];
  var raw = Uint8Array.from(atob(reply.body), function (c) { return c.charCodeAt(0); });
  var parsed = await new Response(raw, { headers: { 'content-type': type } }).formData();
  var file = parsed.get('file');
  log(['formdata', /^multipart\/form-data; boundary=/.test(type), reply.length === raw.length, parsed.get('field'),
       file.name, file.type, await file.text()].join(' '));

  // A body whose type the page did not give has no `Content-Type` at all in a
  // browser. The lines above cannot see the difference -- Array.join renders an
  // absent header and an empty one identically -- so it is asked about here.
  var typeless = await (await fetch(H + '/echo', { method: 'POST', body: new Uint8Array([7]).buffer })).json();
  log('a typeless body sends no content-type: ' + !('content-type' in typeless.headers));
  var typelessPut = await (await fetch(H + '/echo', { method: 'PUT', body: new Uint8Array([7]).buffer })).json();
  log('...and as a PUT: ' + !('content-type' in typelessPut.headers));
  var forbidden = await (await fetch(H + '/echo', { headers: { Cookie: 'sneaky=1', Host: 'evil.example', 'X-Ok': '1' } })).json();
  log(['forbidden headers dropped', forbidden.headers.cookie, forbidden.headers.host === H.slice(7), forbidden.headers['x-ok']].join(' '));
  try { await fetch(H + '/echo', { method: 'GET', body: 'no' }); log('GET body RESOLVED'); }
  catch (e) { log('GET with a body ' + e.name); }

  var x = await xhr('POST', H + '/echo', { body: 'from xhr', headers: [['X-Xhr', 'a'], ['x-xhr', 'b']], responseType: 'json', upload: true });
  log(['xhr', x.xhr.response.method, atob(x.xhr.response.body), x.xhr.response.headers['content-type'],
       x.xhr.response.headers['x-xhr']].join(' '));
  log('xhr events ' + x.events);
  var xf = await xhr('POST', H + '/echo', { body: form, responseType: 'json' });
  log('xhr formdata ' + /^multipart\/form-data; boundary=/.test(xf.xhr.response.headers['content-type']));

  // Request objects: fetch(Request), clone, and a body read once.
  var request = new Request(H + '/echo', { method: 'post', body: 'from a Request', headers: [['X-From', 'request']] });
  var copy = request.clone();
  var viaRequest = await (await fetch(request)).json();
  log(['Request', request.method, request.bodyUsed, viaRequest.method, atob(viaRequest.body), viaRequest.headers['x-from'],
       await copy.text()].join(' '));
  try { await fetch(request); log('reused Request RESOLVED'); } catch (e) { log('reused Request ' + e.name); }
  // Headers: case, combining, sorted iteration, guards.
  var headers = new Headers({ 'X-B': '2', 'x-a': '1' });
  headers.append('X-A', 'again');
  log('Headers ' + headers.get('x-A') + ' | ' + Array.from(headers).map(function (p) { return p.join('='); }).join('&'));
  var fetched = await fetch(H + '/data.json');
  try { fetched.headers.set('x', '1'); log('immutable headers CHANGED'); } catch (e) { log('response headers immutable ' + e.name); }
  // Responses built in JS.
  var built = new Response('{"k":1}', { status: 201, statusText: 'Made', headers: { 'Content-Type': 'application/json' } });
  var twin = built.clone();
  log(['Response', built.status, built.ok, built.statusText, (await built.json()).k, await twin.text(), Response.json({ a: 1 }).headers.get('content-type'),
       Response.redirect(H + '/x', 301).headers.get('location') === H + '/x', Response.error().type].join(' '));
  try { new Response('x', { status: 204 }); log('204 with body ALLOWED'); } catch (e) { log('204 with body ' + e.name); }
});
)JS");
  checkTranscript(got,
      "string POST true text/plain;charset=UTF-8 10 \n"
      "json POST true application/json 7 \n"
      "custom header yes\n"
      "arraybuffer POST true  4 \n"
      "view PUT true  3 \n"
      "blob POST true text/x-blob 9 \n"
      "params PATCH true application/x-www-form-urlencoded;charset=UTF-8 11 \n"
      "formdata true true value f.txt text/plain file-bytes\n" +
      // Two of the three send no header at all, which is what a browser does:
      // Android's `UploadBody.contentType()` returns null, and the Linux client
      // puts every request body behind a content provider (cpp-httplib would
      // otherwise write `text/plain` into a typeless one). NSURLSession has no
      // such escape for a **POST** -- CFNetwork invents
      // `application/x-www-form-urlencoded` for a POST with a body and no type
      // of its own, and an empty header value is the one way to stop it -- so
      // Apple alone sends the header with nothing in it there. It does nothing
      // of the kind to any other method, so a typeless PUT goes out with no
      // header on all three (deferred-work.md).
      //
      // The two lines above cannot see this: `Array.join` renders an absent
      // header and an empty one identically, which is why it is asked directly.
      std::string(NET_PER_PLATFORM("a typeless body sends no content-type: false\n",
                                   "a typeless body sends no content-type: true\n",
                                   "a typeless body sends no content-type: true\n")) +
      "...and as a PUT: true\n" +
      "forbidden headers dropped  true 1\n"
      "GET with a body TypeError\n"
      "xhr POST from xhr text/plain;charset=UTF-8 a, b\n"
      "xhr events loadstart,upload.loadstart,upload.progress,upload.load,upload.loadend,upload.total=8,loaded=8,readystatechange:2,readystatechange:3,progress,readystatechange:4,load,loadend\n"
      "xhr formdata true\n"
      "Request POST true POST from a Request request from a Request\n"
      "reused Request TypeError\n"
      "Headers 1, again | x-a=1, again&x-b=2\n"
      "response headers immutable TypeError\n"
      "Response 201 true Made 1 {\"k\":1} application/json true error\n"
      "204 with body TypeError");
}

// --- matrix row: streaming request ----------------------------------------------
void netStreamingRequest() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var enc = new TextEncoder();
  var parts = 0;
  var stream = new ReadableStream({
    pull: function (c) {
      return sleep(30).then(function () {
        if (parts === 3) return c.close();
        c.enqueue(enc.encode('part' + parts++ + ';'));
      });
    }
  });
  var r = await fetch(H + '/upload?id=stream-ok', { method: 'POST', body: stream, duplex: 'half' });
  var j = await r.json();
  log('streamed ' + j.body + ' ' + j.chunked + ' chunks>1=' + ((await stats('stream-ok')).chunks > 1));
  try { await fetch(H + '/upload', { method: 'POST', body: new ReadableStream() }); log('no duplex RESOLVED'); }
  catch (e) { log('no duplex ' + e.name); }
  var failing = new ReadableStream({
    start: function (c) {
      c.enqueue(enc.encode('partial'));
      setTimeout(function () { c.error(new Error('source broke')); }, 100);
    }
  });
  try { await fetch(H + '/upload?id=stream-error', { method: 'POST', body: failing, duplex: 'half' }); log('errored stream RESOLVED'); }
  catch (e) { log('errored stream ' + e.name); }
  await sleep(300);
  var s = await stats('stream-error');
  log('server saw the connection close ' + s.closed + ', body never finished ' + !s.finished);
});
)JS");
  checkTranscript(got,
      "streamed part0;part1;part2; chunked chunks>1=true\n"
      "no duplex TypeError\n"
      "errored stream TypeError\n"
      "server saw the connection close true, body never finished true");
}

// --- matrix row: streaming response ---------------------------------------------
void netStreamingResponse() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var r = await fetch(H + '/slow?id=read&chunks=3&interval=300');
  var reader = r.body.getReader();
  var first = await reader.read();
  log('first read ' + text(first.value) + ' done=' + first.done + ' response finished=' + !!(await stats('read')).finished);
  var rest = '';
  for (;;) {
    var next = await reader.read();
    if (next.done) break;
    rest += text(next.value);
  }
  log('rest ' + rest + ' locked=' + r.body.locked + ' bodyUsed=' + r.bodyUsed);
  // The same for `text/plain`, a line at a time -- which CFNetwork holds back
  // for content sniffing until 512 bytes have arrived unless the client turns
  // sniffing off. A browser's fetch does not sniff, and neither may this.
  var plain = await fetch(H + '/slow?id=plain&chunks=3&interval=300&type=text/plain');
  var plainReader = plain.body.getReader();
  var plainFirst = await plainReader.read();
  log('text/plain first read ' + text(plainFirst.value) + ' response finished=' + !!(await stats('plain')).finished +
      ' type=' + plain.headers.get('content-type'));
  await plainReader.cancel();

  var iterated = await fetch(H + '/slow?id=iterate&chunks=3&interval=30');
  var all = '', count = 0;
  for await (var chunk of iterated.body) { all += text(chunk); count++; }
  log('for await ' + all + ' in ' + count + ' chunks');

  var teed = await fetch(H + '/slow?id=tee&chunks=3&interval=30');
  var branches = teed.body.tee();
  var both = await Promise.all([readAll(branches[0]), readAll(branches[1])]);
  log('tee ' + both[0] + ' | ' + both[1]);

  var original = await fetch(H + '/slow?id=clone&chunks=3&interval=30');
  var copy = original.clone();
  var texts = await Promise.all([original.text(), copy.text()]);
  log('clone ' + texts[0] + ' | ' + texts[1]);
  try { original.clone(); log('clone after read ALLOWED'); } catch (e) { log('clone after read ' + e.name); }

  var cancelled = await fetch(H + '/slow?id=cancel&chunks=40&interval=50');
  var canceller = cancelled.body.getReader();
  await canceller.read();
  await canceller.cancel('enough');
  await sleep(300);
  var s = await stats('cancel');
  log('cancel closed the connection ' + s.closed + ', response never finished ' + !s.finished);

  var broken = new Response(new ReadableStream({ start: function (c) { c.error(new TypeError('stream broke')); } }));
  try { await broken.text(); log('errored body RESOLVED'); } catch (e) { log('errored body ' + e.message); }

  // A stream a page builds itself: pull on demand, the queueing strategy,
  // releaseLock, cancel reaching the source.
  var pulls = 0, cancelledWith = null, desired = [];
  var made = new ReadableStream({
    pull: function (c) { desired.push(c.desiredSize); c.enqueue('n' + pulls++); if (pulls === 3) c.close(); },
    cancel: function (reason) { cancelledWith = reason; }
  }, { highWaterMark: 2 });
  var own = made.getReader();
  var got = [(await own.read()).value, (await own.read()).value];
  own.releaseLock();
  log('source stream ' + got.join(',') + ' locked after release=' + made.locked + ' pulls>=' + (pulls >= 2) + ' desiredSize started at ' + desired[0]);
  var rest2 = [];
  for await (var item of made) rest2.push(item);
  log('then iterated ' + rest2.join(','));
  var endless = new ReadableStream({ pull: function (c) { c.enqueue(1); }, cancel: function (r) { cancelledWith = r; } });
  for await (var one of endless) break;
  log('break in for await cancels the source ' + (cancelledWith === undefined) + ' locked=' + endless.locked);
});
)JS");
  checkTranscript(got,
      "first read chunk-0; done=false response finished=false\n"
      "rest chunk-1;chunk-2; locked=true bodyUsed=true\n"
      "text/plain first read chunk-0; response finished=false type=text/plain\n"
      "for await chunk-0;chunk-1;chunk-2; in 3 chunks\n"
      "tee chunk-0;chunk-1;chunk-2; | chunk-0;chunk-1;chunk-2;\n"
      "clone chunk-0;chunk-1;chunk-2; | chunk-0;chunk-1;chunk-2;\n"
      "clone after read TypeError\n"
      "cancel closed the connection true, response never finished true\n"
      "errored body stream broke\n"
      "source stream n0,n1 locked after release=false pulls>=true desiredSize started at 2\n"
      "then iterated n2\n"
      "break in for await cancels the source true locked=false");
}

// --- matrix row: HTTP error status ----------------------------------------------
void netHttpErrorStatus() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var codes = [404, 500];
  for (var i = 0; i < codes.length; i++) {
    var r = await fetch(H + '/status/' + codes[i]);
    log([r.ok, r.status, r.statusText, await r.text()].join(' '));
  }
  var x = await xhr('GET', H + '/status/404');
  log(['xhr', x.xhr.status, x.xhr.statusText, x.xhr.responseText, x.events].join(' '));
});
)JS");
  checkTranscript(got,
      "false 404 Not Found status 404 body\n"
      "false 500 Internal Server Error status 500 body\n"
      "xhr 404 Not Found status 404 body loadstart,readystatechange:2,readystatechange:3,progress,readystatechange:4,load,loadend");
}

// --- matrix row: redirect -------------------------------------------------------
void netRedirect() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var codes = [301, 302, 303, 307, 308];
  for (var i = 0; i < codes.length; i++) {
    var r = await fetch(H + '/redirect?code=' + codes[i] + '&to=/data.json');
    log([codes[i], r.status, r.redirected, r.url === H + '/data.json', (await r.json()).n].join(' '));
  }
  var twenty = await fetch(H + '/chain/20');
  log('20 redirects ' + twenty.status + ' ' + twenty.redirected + ' ' + (await twenty.json()).chain);
  try { await fetch(H + '/chain/21'); log('21 redirects RESOLVED'); } catch (e) { log('21 redirects ' + e.name + ' ' + e.cause); }
  try { await fetch(H + '/loop'); log('loop RESOLVED'); } catch (e) { log('loop ' + e.name + ' ' + e.cause); }
  var see = await (await fetch(H + '/redirect?code=303&to=/echo', { method: 'POST', body: 'payload' })).json();
  log('303 POST becomes ' + see.method + ' length=' + see.length + ' content-type=' + see.headers['content-type']);
  var found = await (await fetch(H + '/redirect?code=302&to=/echo', { method: 'POST', body: 'payload' })).json();
  log('302 POST becomes ' + found.method);
  var temporary = await (await fetch(H + '/redirect?code=307&to=/echo', { method: 'POST', body: 'payload' })).json();
  log('307 POST stays ' + temporary.method + ' ' + atob(temporary.body) + ' ' + temporary.headers['content-type']);
  var manual = await fetch(H + '/redirect?code=302&to=/data.json', { redirect: 'manual' });
  log('manual ' + manual.type + ' ' + manual.status + ' ok=' + manual.ok + ' ' + manual.headers.get('location') +
      ' body=' + manual.body + ' redirected=' + manual.redirected +
      ' url=' + (manual.url === H + '/redirect?code=302&to=/data.json'));
  try { await fetch(H + '/redirect?code=302&to=/data.json', { redirect: 'error' }); log('error mode RESOLVED'); }
  catch (e) { log('error mode ' + e.name + ' ' + e.cause); }
  var across = await fetch(H + '/redirect?code=302&to=' + encodeURIComponent(S + '/data.json'));
  log('http to https ' + across.status + ' ' + (across.url === S + '/data.json'));
  var x = await xhr('GET', H + '/chain/3', { responseType: 'json' });
  log('xhr follows ' + x.xhr.status + ' ' + (x.xhr.responseURL === H + '/chain/0') + ' ' + x.xhr.response.chain);
  // A relative Location whose query holds a URL is still relative.
  var nested = await fetch(H + '/redirect?code=302&to=' + encodeURIComponent('data.json?next=https://x/y'));
  log('relative with a URL in the query ' + nested.status + ' ' + (nested.url === H + '/data.json?next=https://x/y'));
  // Credentials do not follow a redirect off the origin: without the drop, a
  // bearer token a page wrote for one host is handed to whatever the redirect
  // names. The control is the request that is *not* redirected -- it is what
  // stops the drop being implemented as "never send Authorization at all", and
  // unlike a same-origin redirect it reads the same on every client (Apple
  // strips the header on any redirect, not only a cross-origin one).
  var direct = await (await fetch(H + '/echo',
                                  { headers: { Authorization: 'Bearer direct' } })).json();
  log('Authorization is sent when nothing redirects: ' + direct.headers.authorization);
  var crossed = await (await fetch(H + '/redirect?code=302&to=' + encodeURIComponent(S + '/echo'),
                                   { headers: { Authorization: 'Bearer cross-origin' } })).json();
  log('and dropped across origins: ' + (crossed.headers.authorization === undefined));
});
)JS");
  checkTranscript(got,
      "301 200 true true 42\n"
      "302 200 true true 42\n"
      "303 200 true true 42\n"
      "307 200 true true 42\n"
      "308 200 true true 42\n"
      "20 redirects 200 true end\n"
      "21 redirects TypeError redirect\n"
      "loop TypeError redirect\n"
      "303 POST becomes GET length=0 content-type=undefined\n"
      "302 POST becomes GET\n"
      "307 POST stays POST payload text/plain;charset=UTF-8\n"
      "manual opaqueredirect 0 ok=false null body=null redirected=false url=true\n"
      "error mode TypeError redirect\n"
      "http to https 200 true\n"
      "xhr follows 200 true end\n"
      "relative with a URL in the query 200 true\n"
      "Authorization is sent when nothing redirects: Bearer direct\n"
      "and dropped across origins: true");
}

// --- matrix row: encoded body ---------------------------------------------------
void netEncodedBody() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var TEXT = 'The quick brown fox jumps over the lazy dog. '.repeat(40);
  log('chunked ' + await (await fetch(H + '/chunked')).text());
  // Every client offers `gzip, deflate`, decodes both -- `deflate` zlib-wrapped
  // or raw -- and leaves `Content-Encoding` on what it decoded, as a browser
  // does. NSURLSession does all of that itself; the Android and Linux clients
  // decode in their own code rather than their library's, because OkHttp offers
  // gzip alone and cpp-httplib cannot read raw deflate. Every line below prints
  // what happened rather than asserting one answer, so where the clients still
  // differ -- the malformed bodies -- the transcripts sit side by side.
  var good = ['/gzip', '/deflate', '/deflate-raw', '/gzip-chunked'];
  for (var i = 0; i < good.length; i++) {
    try {
      var r = await fetch(H + good[i]);
      log(good[i] + ' decoded=' + ((await r.text()) === TEXT) + ' ' + r.headers.get('content-encoding'));
    } catch (e) { log(good[i] + ' ' + e.name + ' ' + e.cause); }
  }
  var empties = ['/gzip-empty', '/gzip-empty-chunked'];
  for (var k = 0; k < empties.length; k++) {
    try {
      var e0 = await fetch(H + empties[k]);
      log(empties[k] + ' "' + await e0.text() + '" ' + e0.headers.get('content-encoding'));
    } catch (e) { log(empties[k] + ' ' + e.name + ' ' + e.cause); }
  }
  // Truncated bodies are the platform client's judgement, and NSURLSession's is
  // the lenient one: it fails a body cut short of its Content-Length, but
  // accepts a chunked body whose terminating chunk never arrives -- and a gzip
  // stream cut short inside one -- as complete, and says nothing through any API
  // that would let this client tell. Android and Linux refuse both. Recorded in
  // deferred-work.md; the line prints what happened either way.
  var bad = ['/gzip-truncated', '/gzip-cut', '/chunked-truncated', '/length-truncated', '/conflicting-length'];
  for (var j = 0; j < bad.length; j++) {
    try {
      var resp = await fetch(H + bad[j]);
      var body = await resp.text();
      log(bad[j] + ' RESOLVED short=' + (body.length < TEXT.length));
    } catch (e) {
      log(bad[j] + ' ' + e.name + ' ' + e.cause);
    }
  }
  var x = await xhr('GET', H + '/gzip');
  log('xhr gzip decoded=' + (x.xhr.responseText === TEXT) + ' ' + x.events.split(',').pop());
  var y = await xhr('GET', H + '/chunked-truncated');
  log('xhr truncated ' + y.events + ' status=' + y.xhr.status);
});
)JS");
  checkTranscript(got, NET_PER_PLATFORM(
      "chunked one-two-three\n"
      "/gzip decoded=true gzip\n"
      "/deflate decoded=true deflate\n"
      "/deflate-raw decoded=true deflate\n"
      "/gzip-chunked decoded=true gzip\n"
      "/gzip-empty \"\" gzip\n"
      "/gzip-empty-chunked \"\" gzip\n"
      "/gzip-truncated TypeError network\n"
      "/gzip-cut RESOLVED short=true\n"
      "/chunked-truncated RESOLVED short=true\n"
      "/length-truncated TypeError network\n"
      "/conflicting-length TypeError protocol\n"
      "xhr gzip decoded=true loadend\n"
      "xhr truncated loadstart,readystatechange:2,readystatechange:3,progress,readystatechange:4,load,loadend status=200",
      // Android. OkHttp frames the body and this client decodes it
      // (HttpClient.decoded): it asks for `gzip, deflate` itself, which turns
      // OkHttp's own gzip off, so every good line now reads as Apple's does --
      // `deflate` in either form decoded, the headers left as they came, an
      // empty gzip body empty. OkHttp alone would have handed `deflate` over
      // compressed, stripped `Content-Encoding` and failed the empty ones. Where
      // it still differs is the malformed bodies, and there it is the strict
      // one: every truncation fails, a gzip stream cut short inside a clean
      // chunked body included, and two disagreeing `Content-Length` lines are
      // refused by the client before OkHttp can read the first.
      "chunked one-two-three\n"
      "/gzip decoded=true gzip\n"
      "/deflate decoded=true deflate\n"
      "/deflate-raw decoded=true deflate\n"
      "/gzip-chunked decoded=true gzip\n"
      "/gzip-empty \"\" gzip\n"
      "/gzip-empty-chunked \"\" gzip\n"
      "/gzip-truncated TypeError protocol\n"
      "/gzip-cut TypeError network\n"
      "/chunked-truncated TypeError network\n"
      "/length-truncated TypeError protocol\n"
      "/conflicting-length TypeError protocol\n"
      "xhr gzip decoded=true loadend\n"
      "xhr truncated loadstart,readystatechange:2,readystatechange:3,progress,readystatechange:4,error,loadend status=0",
      // Linux, cpp-httplib for the framing and this client for the decoding
      // (NetServiceLinux.cpp, `BodyDecoder`): cpp-httplib's own decoder reads
      // zlib and gzip through zlib's header detection but not a *raw* deflate
      // stream, which browsers accept under the same name, so it is not used.
      // Good bodies read as Apple's do. On the malformed ones it is as strict as
      // Android -- a body short of its `Content-Length`, a chunked body with no
      // terminating chunk, a compressed stream that stops before its end and two
      // disagreeing `Content-Length` lines all fail. cpp-httplib refuses the
      // last before any callback sees the head and reports it as a read
      // failure, so here it is `network` where the other two say `protocol`.
      "chunked one-two-three\n"
      "/gzip decoded=true gzip\n"
      "/deflate decoded=true deflate\n"
      "/deflate-raw decoded=true deflate\n"
      "/gzip-chunked decoded=true gzip\n"
      "/gzip-empty \"\" gzip\n"
      "/gzip-empty-chunked \"\" gzip\n"
      "/gzip-truncated TypeError network\n"
      "/gzip-cut TypeError network\n"
      "/chunked-truncated TypeError network\n"
      "/length-truncated TypeError network\n"
      "/conflicting-length TypeError network\n"
      "xhr gzip decoded=true loadend\n"
      "xhr truncated loadstart,readystatechange:2,readystatechange:3,progress,readystatechange:4,error,loadend status=0"));
}

// --- retired: cookie domains that are public suffixes -------------------------------
// The jar refused a `Domain` attribute naming a single-label public suffix, so
// one site could not set a cookie for `com`. That rule left with the jar: the
// store is the platform's now (NSHTTPCookieStorage, android.webkit.CookieManager)
// and each brings its own answer -- Apple's has no public-suffix list at all,
// Chromium's on Android does -- so there is no one transcript for both. Retired
// under the cookie decision in spec-platform-http-clients.md, with the leak the
// row really guarded against still covered by `net-cookies`: a cookie set with
// `Domain=example.com` is never sent to 127.0.0.1.

// --- matrix row: flow control and the origin limit ----------------------------------
// A body nobody reads stops downloading once a window of it is queued, and
// resumes as the page reads; at most six requests to one origin are on the wire
// at once, and one waiting its turn can be aborted without holding a slot.
//
// The first of those holds on all three clients, each its own way: OkHttp is
// pull-based and cpp-httplib is synchronous, so the reader simply not reading
// *is* backpressure; NSURLSession pushes through its delegate and the task is
// suspended, which holds the server back only while the delegate is caught up
// -- so that delegate does nothing but count, suspend and hand the bytes on
// (NetServiceApple.mm, the file comment). Asserted at under 24 MiB of a 64 MiB
// body written as fast as the socket takes it, in the release and ASan builds
// alike; before that change the ASan build read all 64 MiB into memory.
void netFlowControl() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var size = 64 * 1024 * 1024;
  var r = await fetch(H + '/firehose?id=flow-1&bytes=' + size);
  await sleep(1500);
  var stalled = await stats('flow-1');
  log('unread body stalls ' + (stalled.written < 24 * 1024 * 1024) + ' finished=' + stalled.finished);
  var reader = r.body.getReader(), total = 0;
  for (;;) {
    var step = await reader.read();
    if (step.done) break;
    total += step.value.byteLength;
  }
  log('read in full ' + (total === size) + ' finished=' + (await stats('flow-1')).finished);
  var body = await (await fetch(H + '/firehose?id=flow-2&bytes=' + 3 * 1024 * 1024)).arrayBuffer();
  log('arrayBuffer ' + body.byteLength);

  // A second unread body, this one from a server that honours backpressure but
  // offers at most 128 KiB every 10 ms. Left alone it would hand over all 16 MiB
  // in about 1.25 s; nobody reads it, so after 1.5 s it must still be short of
  // the end on **both** clients -- this is the part of the window that holds
  // even where the stall itself is only best effort. Measured over three runs
  // each: 3.2-6.9 MiB through in the release build, 6.0-9.6 MiB under ASan,
  // against a 16 MiB body. Deleting Apple's `[task suspend]` or Android's wait
  // in the reader loop finishes the body and fails this line.
  var paced = await fetch(H + '/firehose?id=flow-3&bytes=' + (16 * 1024 * 1024) + '&rate=' + 131072);
  await sleep(1500);
  log('a paced server is held short of the end ' + !(await stats('flow-3')).finished);
  // ...and aborting it tears the connection down whether the window had stopped
  // it or the body had already arrived. Either is an answer; still being open is
  // not, which is what this fails on.
  var pacedReader = paced.body.getReader();
  await pacedReader.cancel('enough');
  await sleep(400);
  var pacedEnd = await stats('flow-3');
  log('aborted mid-window, connection gone ' + (pacedEnd.closed || !!pacedEnd.finished));

  var parallel = [];
  for (var i = 0; i < 10; i++) {
    parallel.push(fetch(H + '/slot?id=slot-1&ms=300').then(function (res) { return res.json(); }));
  }
  await Promise.all(parallel);
  var slots = await stats('slot-1');
  log('origin limit max=' + slots.maxActive + ' served=' + slots.served);

  var controller = new AbortController(), hung = [];
  for (var j = 0; j < 8; j++) {
    hung.push(fetch(H + '/hang?id=slot-hang-' + j, { signal: controller.signal })
      .then(function () { return 'RESOLVED'; }, function (e) { return e.name; }));
  }
  await sleep(100);
  controller.abort();
  var outcomes = await Promise.all(hung);
  var next = await Promise.race([fetch(H + '/data.json').then(function (res) { return res.status; }),
                                 sleep(3000).then(function () { return 'still waiting'; })]);
  log('aborted ' + outcomes.join(',') + ' then ' + next);
});
)JS");
  const std::string expected =
      "unread body stalls true finished=false\n"
      "read in full true finished=true\n"
      "arrayBuffer 3145728\n"
      "a paced server is held short of the end true\n"
      "aborted mid-window, connection gone true\n"
      "origin limit max=6 served=10\n"
      "aborted AbortError,AbortError,AbortError,AbortError,AbortError,AbortError,AbortError,AbortError then 200";
  checkTranscript(got, expected);
}

// --- matrix row: abort / timeout ------------------------------------------------
void netAbortTimeout() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var controller = new AbortController(), order = [];
  controller.signal.onabort = function (e) { order.push('onabort:' + e.type); };
  controller.signal.addEventListener('abort', function () { order.push('listener'); });
  var pending = fetch(H + '/hang?id=abort-1', { signal: controller.signal });
  setTimeout(function () { controller.abort(); }, 50);
  try { await pending; log('abort RESOLVED'); }
  catch (e) { log(['abort', e.name, e instanceof DOMException, e.code, controller.signal.aborted, order.join(',')].join(' ')); }

  var reasoned = new AbortController();
  var second = fetch(H + '/hang?id=abort-2', { signal: reasoned.signal });
  setTimeout(function () { reasoned.abort('because'); }, 20);
  try { await second; } catch (e) { log('abort with a reason rejects with it: ' + e + ' ' + reasoned.signal.reason); }

  try { await fetch(H + '/hang?id=abort-3', { signal: AbortSignal.timeout(80) }); log('timeout RESOLVED'); }
  catch (e) { log(['timeout', e.name, e instanceof DOMException, e.code].join(' ')); }

  try { await fetch(H + '/hang?id=abort-4', { signal: AbortSignal.abort() }); log('pre-aborted RESOLVED'); }
  catch (e) { log('already aborted ' + e.name); }

  var any = new AbortController();
  var fifth = fetch(H + '/hang?id=abort-5', { signal: AbortSignal.any([any.signal, AbortSignal.timeout(60000)]) });
  setTimeout(function () { any.abort(); }, 20);
  try { await fifth; } catch (e) { log('AbortSignal.any ' + e.name); }

  var mid = new AbortController();
  var streaming = await fetch(H + '/slow?id=abort-6&chunks=40&interval=50', { signal: mid.signal });
  var reader = streaming.body.getReader();
  await reader.read();
  mid.abort();
  try { await reader.read(); log('mid-body RESOLVED'); } catch (e) { log('aborted mid-body ' + e.name); }

  var aborted = await xhr('GET', H + '/hang?id=abort-7', { abortAfter: 50 });
  log('xhr abort ' + aborted.events + ' readyState=' + aborted.xhr.readyState + ' status=' + aborted.xhr.status);
  var timedOut = await xhr('GET', H + '/hang?id=abort-8', { timeout: 80 });
  log('xhr timeout ' + timedOut.events + ' readyState=' + timedOut.xhr.readyState);

  await sleep(300);
  var ids = ['abort-1', 'abort-2', 'abort-3', 'abort-5', 'abort-6', 'abort-7', 'abort-8'], closed = [];
  for (var i = 0; i < ids.length; i++) closed.push((await stats(ids[i])).closed);
  log('connections closed ' + closed.join(','));
});
)JS");
  checkTranscript(got,
      "abort AbortError true 20 true onabort:abort,listener\n"
      "abort with a reason rejects with it: because because\n"
      "timeout TimeoutError true 23\n"
      "already aborted AbortError\n"
      "AbortSignal.any AbortError\n"
      "aborted mid-body AbortError\n"
      "xhr abort loadstart,readystatechange:4,abort,loadend readyState=0 status=0\n"
      "xhr timeout loadstart,readystatechange:4,timeout,loadend readyState=4\n"
      "connections closed true,true,true,true,true,true,true");
}

// --- matrix row: unreachable ----------------------------------------------------
void netUnreachable() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var refused = 'http://127.0.0.1:' + CP;
  try { await fetch(refused + '/x'); log('refused RESOLVED'); } catch (e) { log('refused ' + e.name + ' ' + e.cause); }
  try { await fetch('https://127.0.0.1:' + CP + '/x'); log('refused https RESOLVED'); } catch (e) { log('refused https ' + e.name + ' ' + e.cause); }
  // A name the resolver refuses before asking anyone: a 64-octet label cannot be
  // encoded in a DNS message (RFC 1035 2.3.4), so no query can leave the machine
  // -- unlike a reserved .invalid name, which the system resolver may forward.
  // ...and the clients disagree about what to call it: Apple asks the resolver
  // and is told the name is bad (`dns`), while OkHttp's URL parser and the Linux
  // client both refuse a label over 63 octets before anything is resolved
  // (`url`). All of them mean "it never reached a server", which is what the row
  // is for.
  var unsendable = 'http://' + new Array(65).join('a') + '.invalid/x';
  try { await fetch(unsendable); log('dns RESOLVED'); }
  catch (e) { log('unresolvable ' + e.name + ' ' + (e.cause === 'dns' || e.cause === 'url')); }
  var x = await xhr('GET', refused + '/x');
  log('xhr ' + x.events + ' status=' + x.xhr.status);
  var w = ws('ws://127.0.0.1:' + CP + '/ws');
  await w.closed;
  log('ws ' + w.events.join(',') + ' readyState=' + w.socket.readyState);
});
)JS");
  checkTranscript(got,
      "refused TypeError connect\n"
      "refused https TypeError connect\n"
      "unresolvable TypeError true\n"
      "xhr loadstart,readystatechange:4,error,loadend status=0\n"
      "ws error,close:1006::false readyState=3");
}

// --- matrix row: cookies --------------------------------------------------------
void netCookies() {
  auto f = netFixture();
  if (!f.ok) return;
  {
    auto runtime = netRuntime(f);
    if (!runtime) return;
    const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  async function cookies(base, path, init) { return (await (await fetch(base + (path || '/cookies'), init)).json()).cookie; }
  function setting(base, list) {
    return base + '/set-cookie?' + list.map(function (c) { return 'c=' + encodeURIComponent(c); }).join('&');
  }
  await fetch(setting(H, ['session=1', 'persist=2; Max-Age=3600', 'secure=3; Secure', 'http=4; HttpOnly',
                          'deep=5; Path=/cookies/sub', 'gone=6; Expires=Thu, 01 Jan 1970 00:00:01 GMT',
                          'strict=7; SameSite=Strict', 'host=8; Domain=127.0.0.1', 'foreign=9; Domain=example.com']));
  log('http: ' + await cookies(H));
  log('path: ' + await cookies(H, '/cookies/sub'));
  await fetch(setting(S, ['secure=3; Secure; Path=/']));
  log('https: ' + await cookies(S));
  log('http, after a Secure cookie: ' + await cookies(H));
  log('JS view hides HttpOnly: ' + __screenkit.net.getCookies(S + '/cookies'));
  log('JS may not set HttpOnly: ' + __screenkit.net.setCookie(H + '/', 'jsonly=1; HttpOnly') +
      ', may set plain: ' + __screenkit.net.setCookie(H + '/', 'fromjs=10'));
  await fetch(setting(H, ['session=; Max-Age=0']));
  log('after Max-Age=0: ' + await cookies(H));
  await fetch(setting(H, ['omitted=11']), { credentials: 'omit' });
  log('omit sends none: ' + JSON.stringify(await cookies(H, '/cookies', { credentials: 'omit' })) +
      ', stored none: ' + ((await cookies(H)).indexOf('omitted') < 0));
  var x = await xhr('GET', H + '/cookies', { responseType: 'json' });
  log('xhr: ' + x.xhr.response.cookie);
  // The handshake is an ordinary request and carries the jar's cookies.
  var w = ws(H.replace('http:', 'ws:') + '/ws');
  await opened(w);
  w.socket.send('cookie');
  log('websocket: ' + await nextMessage(w.socket));
  w.socket.close();
  await w.closed;
  var source = new EventSource(H + '/events?id=cookie-es', { withCredentials: true });
  await new Promise(function (resolve) { source.onmessage = function () { source.close(); resolve(); }; });
  log('eventsource: ' + (await stats('cookie-es')).cookies[0] + ' withCredentials=' + source.withCredentials);
  // A Path with control characters in it: there is no cookies.txt to forge a
  // line in any more, and the platform store refuses the whole cookie rather
  // than dropping the Path -- stricter than the jar was, and recorded as such.
  log('forged path refused outright: ' + !__screenkit.net.setCookie(H + '/',
      'lined=3; Max-Age=3600; Path=/\nforged\tx\t127.0.0.1\t/\t99999999999999\t0\tHO\t'));
  await fetch(setting(H, ['expiring=12; Expires=Wed, 01 Jan 2070 00:00:00 GMT']));
  var now = await cookies(H);
  log('future Expires sent: ' + (now.indexOf('expiring=12') >= 0) + ', forged absent: ' + (now.indexOf('forged') < 0) +
      ', nothing from the forged line: ' + (now.indexOf('lined=3') < 0));
});
)JS");
    // Everything about a cookie beyond "it is stored and sent" is the platform
    // store's now, and the two stores are different products: the order of the
    // `Cookie:` header, whether 127.0.0.1 counts as a secure origin (Chromium
    // says yes, so a `Secure` cookie is sent over http to it), and what a Path
    // with control characters in it does. Both transcripts are here rather than
    // one weakened one (spec-platform-http-clients.md).
    checkTranscript(got, NET_PER_PLATFORM(
        "http: host=8; http=4; persist=2; session=1; strict=7\n"
        "path: deep=5; host=8; http=4; persist=2; session=1; strict=7\n"
        "https: host=8; http=4; persist=2; secure=3; session=1; strict=7\n"
        "http, after a Secure cookie: host=8; http=4; persist=2; session=1; strict=7\n"
        "JS view hides HttpOnly: host=8; persist=2; secure=3; session=1; strict=7\n"
        "JS may not set HttpOnly: false, may set plain: true\n"
        "after Max-Age=0: fromjs=10; host=8; http=4; persist=2; strict=7\n"
        "omit sends none: \"\", stored none: true\n"
        "xhr: fromjs=10; host=8; http=4; persist=2; strict=7\n"
        "websocket: cookie:fromjs=10; host=8; http=4; persist=2; strict=7\n"
        "eventsource: fromjs=10; host=8; http=4; persist=2; strict=7 withCredentials=true\n"
        "forged path refused outright: true\n"
        "future Expires sent: true, forged absent: true, nothing from the forged line: true",
        // Android: `android.webkit.CookieManager`, which is Chromium's store.
        // It keeps the order cookies were stored in, and it treats 127.0.0.1 as
        // a trustworthy origin -- so a `Secure` cookie set over https is sent
        // back over http to it, which is Chromium's rule and not a mistake here.
        // `Cookie.parse` accepts a Path with control characters in it rather
        // than refusing the cookie, which costs nothing now that there is no
        // cookies.txt for such a Path to forge a line in. HttpOnly is kept out
        // of the JS view by this client, not by the store: `getCookie` returns
        // HttpOnly cookies with no flags on them, so the JS view is an
        // allow-list of the cookies `HttpClient` saw stored without the flag
        // (ScreenKitCookieJar), and the harness empties the store before each
        // row so the header below cannot pick up a previous run's cookies.
        "http: session=1; persist=2; http=4; strict=7; host=8\n"
        "path: deep=5; session=1; persist=2; http=4; strict=7; host=8\n"
        "https: session=1; persist=2; http=4; strict=7; host=8; secure=3\n"
        "http, after a Secure cookie: session=1; persist=2; http=4; strict=7; host=8; secure=3\n"
        "JS view hides HttpOnly: session=1; persist=2; strict=7; host=8; secure=3\n"
        "JS may not set HttpOnly: false, may set plain: true\n"
        "after Max-Age=0: persist=2; http=4; strict=7; host=8; secure=3; fromjs=10\n"
        "omit sends none: \"\", stored none: true\n"
        "xhr: persist=2; http=4; strict=7; host=8; secure=3; fromjs=10\n"
        "websocket: cookie:persist=2; http=4; strict=7; host=8; secure=3; fromjs=10\n"
        "eventsource: persist=2; http=4; strict=7; host=8; secure=3; fromjs=10 withCredentials=true\n"
        "forged path refused outright: false\n"
        "future Expires sent: true, forged absent: true, nothing from the forged line: false",
        // Linux: `LinuxCookieJar`, the one store in the tree that is ours,
        // because this platform has none to point at. It is RFC 6265 read
        // straight -- the `Cookie:` order is 5.4's (longer paths first, then
        // oldest first, and an update keeps its original creation time), a
        // `Secure` cookie is only ever sent over a secure origin, and a
        // `Set-Cookie` with a control character anywhere in it is refused
        // whole.
        //
        // Two of these lines are the jar's alone. `secure=3` is offered first
        // over **http**, where this jar refuses it outright (a plaintext origin
        // may not plant a cookie the site marked Secure), so the later https one
        // is a new cookie and takes its place at the end -- the same answer
        // Chromium gives, for the same reason. And `http, after a Secure cookie`
        // does not carry it: a Secure cookie goes only over a secure origin,
        // with no exception for 127.0.0.1, which is Apple's rule.
        "http: session=1; persist=2; http=4; strict=7; host=8\n"
        "path: deep=5; session=1; persist=2; http=4; strict=7; host=8\n"
        "https: session=1; persist=2; http=4; strict=7; host=8; secure=3\n"
        "http, after a Secure cookie: session=1; persist=2; http=4; strict=7; host=8\n"
        "JS view hides HttpOnly: session=1; persist=2; strict=7; host=8; secure=3\n"
        "JS may not set HttpOnly: false, may set plain: true\n"
        "after Max-Age=0: persist=2; http=4; strict=7; host=8; fromjs=10\n"
        "omit sends none: \"\", stored none: true\n"
        "xhr: persist=2; http=4; strict=7; host=8; fromjs=10\n"
        "websocket: cookie:persist=2; http=4; strict=7; host=8; fromjs=10\n"
        "eventsource: persist=2; http=4; strict=7; host=8; fromjs=10 withCredentials=true\n"
        "forged path refused outright: true\n"
        "future Expires sent: true, forged absent: true, nothing from the forged line: true"));
    runtime->shutdown();
  }
  // A second runtime in the same process: on Apple nothing carries over. The jar
  // and its cookies.txt are gone -- each runtime's cookies live in a store of
  // its own inside the platform's (spec-platform-http-clients.md, the cookie
  // decision), so "persistent cookies survive a runtime restart" is retired and
  // this pins what replaced it. A shared store would leak one app's cookies into
  // the next runtime in the process and into the user's own cookie file.
  auto restarted = netRuntime(f);
  if (!restarted) return;
  const std::string got = netTranscript(restarted, R"JS(
run(async function () {
  var after = (await (await fetch(H + '/cookies')).json()).cookie;
  log('after restart: persistent=' + (after.indexOf('persist=2') >= 0) +
      ' session=' + (after.indexOf('strict=7') >= 0));
});
)JS");
  // Membership, not the whole header: what the answer depends on is the store,
  // and on Android that is the device's, shared with every WebView on it. On
  // Linux the jar is this runtime's own and lives only as long as it does, so
  // a second runtime starts empty -- the same answer Apple gives, reached the
  // same way (spec-linux-http-client.md, the cookie decision).
  checkTranscript(got, NET_PER_PLATFORM("after restart: persistent=false session=false",
                                        "after restart: persistent=true session=true",
                                        "after restart: persistent=false session=false"));
}

// --- matrix row: WebSocket ------------------------------------------------------
void netWebSocket() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var url = H.replace('http:', 'ws:') + '/ws';
  var w = ws(url, ['chat', 'superchat']);
  log('connecting readyState=' + w.socket.readyState + ' url=' + (w.socket.url === url) + ' OPEN=' + WebSocket.OPEN);
  try { w.socket.send('too early'); log('send while connecting ALLOWED'); } catch (e) { log('send while connecting ' + e.name); }
  await opened(w);
  log('open protocol=' + w.socket.protocol + ' extensions="' + w.socket.extensions + '" readyState=' +
      w.socket.readyState + ' binaryType=' + w.socket.binaryType);
  w.socket.send('hello');
  log('text ' + await nextMessage(w.socket));
  w.socket.binaryType = 'arraybuffer';
  w.socket.send(new Uint8Array([1, 2, 3]));
  var buffer = await nextMessage(w.socket);
  log('arraybuffer ' + (buffer instanceof ArrayBuffer) + ' ' + Array.from(new Uint8Array(buffer)).join(','));
  w.socket.binaryType = 'blob';
  w.socket.send(new Blob(['as a blob']));
  var blob = await nextMessage(w.socket);
  log('blob ' + (blob instanceof Blob) + ' ' + await blob.text());
  var big = new Uint8Array(1 << 20);
  w.socket.send(big);
  log('bufferedAmount counts it ' + (w.socket.bufferedAmount >= big.length));
  var echoed = await nextMessage(w.socket);
  await until(function () { return w.socket.bufferedAmount === 0; });
  log('big echo ' + echoed.size + ', bufferedAmount drained to ' + w.socket.bufferedAmount);
  w.socket.send('ping:heartbeat');
  log('server ping answered: ' + await nextMessage(w.socket));
  w.socket.send('close:4001:bye');
  await w.closed;
  log('server close ' + w.events[w.events.length - 1] + ' readyState=' + w.socket.readyState);

  var c = ws(url);
  await opened(c);
  c.socket.close(4000, 'done');
  log('closing readyState=' + c.socket.readyState);
  await c.closed;
  log('client close ' + c.events.join(',') + ' protocol="' + c.socket.protocol + '"');

  var d = ws(url);
  await opened(d);
  d.socket.send('drop');
  await d.closed;
  log('dropped ' + d.events.join(','));

  var refused = ws(H.replace('http:', 'ws:') + '/ws-refuse');
  await refused.closed;
  log('handshake refused ' + refused.events.join(','));

  try { new WebSocket('ftp://127.0.0.1/'); log('ftp ALLOWED'); } catch (e) { log('bad scheme ' + e.name); }
  try { c.socket.close(1001); log('close(1001) ALLOWED'); } catch (e) { log('bad close code ' + e.name); }

  // close() while CONNECTING fails the connection, whatever the handshake had
  // reached natively: no open, error, then close 1006.
  var early = ws(url);
  // Busy long enough for the handshake to finish natively, so its open is
  // already on its way when close() is called.
  for (var busyUntil = Date.now() + 300; Date.now() < busyUntil;) {}
  early.socket.close();
  await early.closed;
  log('closed while connecting ' + early.events.join(',') + ' readyState=' + early.socket.readyState);

  // readyState is CLOSED by the time error is dispatched.
  var stateAtError = -1;
  var failing = ws(H.replace('http:', 'ws:') + '/ws-refuse');
  failing.socket.addEventListener('error', function () { stateAtError = failing.socket.readyState; });
  await failing.closed;
  log('readyState at error ' + stateAtError);

  // A reason without a code goes out with 1000.
  var reasoned = ws(url);
  await opened(reasoned);
  reasoned.socket.close(undefined, 'why');
  await reasoned.closed;
  log('reason without a code ' + reasoned.events[reasoned.events.length - 1]);

  // Two messages that arrive together: a promise the first resolves has settled
  // before the second is dispatched, as with a task per message.
  var pair = ws(url), order = [];
  await opened(pair);
  await new Promise(function (resolve) {
    pair.socket.addEventListener('message', function (e) {
      order.push(e.data);
      if (e.data === 'first') Promise.resolve().then(function () { order.push('microtask after first'); });
      else resolve();
    });
    pair.socket.send('pair');
  });
  log('pair ' + order.join(','));
  pair.socket.close();
  await pair.closed;

  // Handshakes the client must refuse (RFC 6455 4.1): a wrong accept key, a
  // subprotocol it never offered, an extension it never offered. None of them
  // may reach `open`. OkHttp checks only the first and cpp-httplib only the
  // first, so the other two are checked by those clients themselves
  // (HttpClient.java, NetServiceLinux.cpp); NSURLSession checks all three.
  var refusals = ['/ws-bad-accept', '/ws-bad-protocol', '/ws-unknown-extension'];
  for (var r = 0; r < refusals.length; r++) {
    var bad = ws(H.replace('http:', 'ws:') + refusals[r], ['chat']);
    await bad.closed;
    log(refusals[r] + ' ' + bad.events.join(','));
  }
  // permessage-deflate is offered by NSURLSession and by OkHttp, and a server
  // that accepts it has negotiated it: `extensions` says so, as in a browser.
  // The Linux client offers no extension at all, so the same answer is one it
  // must refuse.
  var deflate = ws(H.replace('http:', 'ws:') + '/ws-extension');
  await Promise.race([opened(deflate), deflate.closed]);
  log('/ws-extension ' + deflate.events.join(',') + ' extensions="' + deflate.socket.extensions + '"');
  deflate.socket.close();
  await deflate.closed;

  // Fragmented messages with a ping between fragments, reassembled once each.
  var frag = ws(url), received = [];
  frag.socket.binaryType = 'arraybuffer';
  await opened(frag);
  await new Promise(function (resolve) {
    frag.socket.addEventListener('message', function (e) {
      if (e.data === 'fragments-done') return resolve();
      received.push(typeof e.data === 'string' ? 'text:' + e.data : 'binary:' + Array.from(new Uint8Array(e.data)).join(','));
    });
    frag.socket.send('fragments');
  });
  log('fragments ' + received.join(' ') + ' count=' + received.length);
  frag.socket.close();
  await frag.closed;
});
)JS");
  checkTranscript(got,
      "connecting readyState=0 url=true OPEN=1\n"
      "send while connecting InvalidStateError\n"
      "open protocol=superchat extensions=\"\" readyState=1 binaryType=blob\n"
      "text hello\n"
      "arraybuffer true 1,2,3\n"
      "blob true as a blob\n"
      "bufferedAmount counts it true\n"
      "big echo 1048576, bufferedAmount drained to 0\n"
      "server ping answered: pong:heartbeat\n"
      // A close the *peer* sent, with its own code and reason. cpp-httplib
      // discards a Close frame's payload upstream, so on Linux this line rests
      // on the one patch tools/vendor/httplib.rules makes to it.
      "server close close:4001:bye:true readyState=3\n"
      "closing readyState=2\n"
      "client close open,close:4000:done:true protocol=\"\"\n"
      "dropped open,error,close:1006::false\n"
      "handshake refused error,close:1006::false\n"
      "bad scheme SyntaxError\n"
      "bad close code InvalidAccessError\n"
      "closed while connecting error,close:1006::false readyState=3\n"
      "readyState at error 3\n"
      "reason without a code close:1000:why:true\n"
      "pair first,microtask after first,second\n"
      "/ws-bad-accept error,close:1006::false\n"
      "/ws-bad-protocol error,close:1006::false\n"
      "/ws-unknown-extension error,close:1006::false\n" +
      std::string(NET_PER_PLATFORM("/ws-extension open extensions=\"permessage-deflate\"\n",
                                   "/ws-extension open extensions=\"permessage-deflate\"\n",
                                   "/ws-extension error,close:1006::false extensions=\"\"\n")) +
      "fragments text:fragmented binary:1,2,3 count=2");
}

// --- matrix row: EventSource ----------------------------------------------------
void netEventSource() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var source = new EventSource(H + '/events?id=es-main');
  var seen = [];
  log('initial readyState=' + source.readyState + ' url=' + (source.url === H + '/events?id=es-main') +
      ' withCredentials=' + source.withCredentials);
  source.onopen = function () { seen.push('open readyState=' + source.readyState); };
  source.onmessage = function (e) {
    seen.push('message ' + JSON.stringify(e.data) + ' lastEventId=' + e.lastEventId + ' origin=' + (e.origin === H));
  };
  source.addEventListener('custom', function (e) { seen.push('custom ' + e.data + ' lastEventId=' + e.lastEventId); });
  var droppedAt = 0, reconnectedIn = -1, opens = 0;
  source.addEventListener('error', function () { if (!droppedAt) droppedAt = Date.now(); });
  source.addEventListener('open', function () { if (++opens === 2) reconnectedIn = Date.now() - droppedAt; });
  source.onerror = function () { seen.push('error readyState=' + source.readyState); };
  await until(function () { return seen.some(function (s) { return s.indexOf('again') >= 0; }); });
  source.close();
  log(seen.join('\n'));
  log('closed readyState=' + source.readyState);
  log('reconnected after retry: 150, not the 3 s default: ' + (reconnectedIn >= 100 && reconnectedIn < 1500));
  await sleep(400);
  var s = await stats('es-main');
  log('connections=' + s.attempts + ' Last-Event-ID sent=' + JSON.stringify(s.lastEventIds) + ' close() closed it=' + s.closed);
  var failing = ['/events-404', '/events-wrong-type'];
  for (var i = 0; i < failing.length; i++) {
    var bad = new EventSource(H + failing[i] + '?id=bad-' + i), events = [];
    bad.onopen = function () { events.push('open'); };
    await new Promise(function (resolve) {
      bad.onerror = function () { events.push('error readyState=' + bad.readyState); setTimeout(resolve, 400); };
    });
    log(failing[i] + ' ' + events.join(',') + ' connections=' + (await stats('bad-' + i)).attempts);
  }
});
)JS");
  checkTranscript(got,
      "initial readyState=0 url=true withCredentials=false\n"
      "open readyState=1\n"
      "message \"first\" lastEventId= origin=true\n"
      "custom named lastEventId=7\n"
      "message \"line1\\nline2\" lastEventId=7 origin=true\n"
      "error readyState=0\n"
      "open readyState=1\n"
      "message \"again 7\" lastEventId=7 origin=true\n"
      "closed readyState=2\n"
      "reconnected after retry: 150, not the 3 s default: true\n"
      "connections=2 Last-Event-ID sent=[null,\"7\"] close() closed it=true\n"
      "/events-404 error readyState=2 connections=1\n"
      "/events-wrong-type error readyState=2 connections=1");
}

// --- matrix row: network image --------------------------------------------------
void netImage() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  // Three texels of the fixture PNG: the green origin, the red far corner (12,6)
  // -- a crop, flip or short upload would miss it -- and the half-transparent
  // (6,3), which shows the alpha state the upload used.
  function texel(upload) {
    var t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    upload();
    var error = gl.getError();
    var fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
    var out = [[0, 0], [12, 6], [6, 3]].map(function (at) {
      var px = new Uint8Array(4);
      gl.readPixels(at[0], at[1], 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
      return Array.prototype.join.call(px, ',');
    });
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return out.join(' | ') + ' glError=' + error;
  }
  var img = new Image();
  await new Promise(function (resolve) {
    img.onload = resolve;
    img.onerror = function (e) { log('img error ' + (e.error && e.error.message)); resolve(); };
    img.src = S + '/image.png';
  });
  img.width = 1;  // what the page sets does not change the pixels
  log(['img', img.complete, img.naturalWidth + 'x' + img.naturalHeight,
       texel(function () { gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img); })].join(' '));
  var blob = await (await fetch(S + '/image.png')).blob();
  var bitmap = await createImageBitmap(blob, { premultiplyAlpha: 'none' });
  log(['fetch blob bitmap', blob.type, bitmap.width + 'x' + bitmap.height,
       texel(function () { gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmap); })].join(' '));
  // Lightning's loader, exactly: XHR blob, then the cropped createImageBitmap form.
  var x = await xhr('GET', S + '/image.png', { responseType: 'blob' });
  var lightning = await createImageBitmap(x.xhr.response, 0, 0, 13, 7,
      { premultiplyAlpha: 'premultiply', colorSpaceConversion: 'none', imageOrientation: 'none' });
  log(['xhr blob bitmap', x.xhr.response instanceof Blob, x.xhr.response.type, lightning.width + 'x' + lightning.height,
       texel(function () { gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, lightning); })].join(' '));
  var fromObjectUrl = new Image();
  await new Promise(function (resolve) {
    fromObjectUrl.onload = resolve;
    fromObjectUrl.onerror = function (e) { log('object url error ' + (e.error && e.error.message)); resolve(); };
    fromObjectUrl.src = URL.createObjectURL(blob);
  });
  log('object URL of a download ' + fromObjectUrl.naturalWidth + 'x' + fromObjectUrl.naturalHeight);
  var bad = new Image();
  await new Promise(function (resolve) {
    bad.onload = function () { log('undecodable LOADED'); resolve(); };
    bad.onerror = function (e) { log('undecodable img error ' + (e.error instanceof DOMException) + ' ' + e.error.name); resolve(); };
    bad.src = S + '/not-image.png';
  });
  var badBlob = await (await fetch(S + '/not-image.png')).blob();
  try { await createImageBitmap(badBlob); log('undecodable bitmap RESOLVED'); } catch (e) { log('undecodable bitmap ' + e.name); }
  var missing = new Image();
  await new Promise(function (resolve) {
    missing.onload = function () { log('404 LOADED'); resolve(); };
    missing.onerror = function () { log('404 img error'); resolve(); };
    missing.src = S + '/status/404';
  });
});
)JS");
  checkTranscript(got,
      "img true 13x7 12,200,80,255 | 250,10,30,255 | 200,100,50,128 glError=0\n"
      "fetch blob bitmap image/png 13x7 12,200,80,255 | 250,10,30,255 | 200,100,50,128 glError=0\n"
      "xhr blob bitmap true image/png 13x7 12,200,80,255 | 250,10,30,255 | 100,50,25,128 glError=0\n"
      "object URL of a download 13x7\n"
      "undecodable img error true InvalidStateError\n"
      "undecodable bitmap InvalidStateError\n"
      "404 img error");
}

// --- matrix row: shutdown / idle ------------------------------------------------
void netShutdownIdle() {
  auto f = netFixture();
  if (!f.ok) return;
  {
    auto runtime = netRuntime(f);
    if (!runtime) return;
    CHECK(pumpUntilIdle(runtime));

    // An in-flight request holds the runtime busy; aborting it lets go.
    domEval(runtime, "globalThis.__hang = new AbortController();"
                     "fetch(H + '/hang?id=idle-fetch', { signal: __hang.signal }).catch(function () {}); 'x';");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!runtime->idle());
    domEval(runtime, "__hang.abort(); 'x';");
    CHECK(pumpUntilIdle(runtime));

    // So does an open WebSocket, until its close event.
    domEval(runtime, "globalThis.__ws = new WebSocket(H.replace('http:', 'ws:') + '/ws');"
                     "__ws.onopen = function () { globalThis.__wsOpen = true; }; 'x';");
    waitForJs(runtime, "globalThis.__wsOpen === true");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!runtime->idle());
    domEval(runtime, "__ws.close(); 'x';");
    CHECK(pumpUntilIdle(runtime));

    // And an open EventSource -- across its reconnect, too -- until close().
    domEval(runtime, "globalThis.__es = new EventSource(H + '/events?id=idle-es');"
                     "__es.onmessage = function (e) { if (e.data.indexOf('again') === 0) globalThis.__esAgain = true; }; 'x';");
    waitForJs(runtime, "globalThis.__esAgain === true");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!runtime->idle());
    domEval(runtime, "__es.close(); 'x';");
    CHECK(pumpUntilIdle(runtime));

    // A completion waits behind the freeze gate: the response arrives while the
    // runtime is paused, and the callback runs only after resume.
    domEval(runtime, "globalThis.__late = 'pending'; globalThis.__lateAt = 0;"
                     "fetch(H + '/slow?id=paused&chunks=1&interval=10').then(function (r) { return r.text(); })"
                     ".then(function (t) { __late = t; __lateAt = Date.now(); }); 'x';");
    runtime->pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(!runtime->idle());
    const double resumedAt = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    runtime->resume();
    CHECK(pumpUntilIdle(runtime));
    CHECK_EQ(domEval(runtime, "__late;"), std::string("chunk-0;"));
    const std::string ranAt = domEval(runtime, "String(__lateAt);");
    CHECK(std::atof(ranAt.c_str()) >= resumedAt - 5);

    // A paused runtime's backlog is not bounded by the task queue: 70,000
    // WebSocket messages -- more than SDL's 65,535-event queue holds -- arrive
    // while paused, and every one is delivered, in order, after resume.
    domEval(runtime, R"JS(
      globalThis.__burst = { next: 0, bad: 0, done: false };
      globalThis.__burstSocket = new WebSocket(H.replace('http:', 'ws:') + '/ws');
      __burstSocket.onmessage = function (e) {
        if (e.data === 'burst-done') { __burst.done = true; __burstSocket.close(); return; }
        if (Number(e.data) !== __burst.next) __burst.bad++;
        __burst.next++;
      };
      __burstSocket.onopen = function () { globalThis.__burstOpen = true; };
      'x';
    )JS");
    waitForJs(runtime, "globalThis.__burstOpen === true");
    // The request leaves before the pause; the server's answer arrives during it.
    domEval(runtime, "__burstSocket.send('burst:70000'); 'x';");
    runtime->pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(!runtime->idle());
    runtime->resume();
    waitForJs(runtime, "__burst.done === true", 60000);
    CHECK_EQ(domEval(runtime, "__burst.next + ' ' + __burst.bad;"), std::string("70000 0"));
    CHECK(pumpUntilIdle(runtime, 10000));
  }
  {
    // Shutdown with everything open: a hanging request, a streaming response
    // half read, an open WebSocket and an open EventSource. No crash, no
    // callback, and the server sees every connection close.
    test::LogCapture capture;
    auto runtime = netRuntime(f);
    if (!runtime) return;
    domEval(runtime, R"JS(
      globalThis.__socket = new WebSocket(H.replace('http:', 'ws:') + '/ws');
      __socket.onopen = function () { globalThis.__socketOpen = true; };
      fetch(H + '/hang?id=shutdown-fetch').catch(function () {});
      fetch(H + '/slow?id=shutdown-stream&chunks=100&interval=50').then(function (r) {
        var reader = r.body.getReader();
        function next() { return reader.read().then(function (x) { if (!x.done) return next(); }); }
        return next();
      });
      // ...and one the flow window has stopped reading, which nothing else here
      // covers: teardown has to reach a request the client was told to hold back,
      // not only ones it is actively reading.
      fetch(H + '/firehose?id=shutdown-unread&bytes=' + (64 * 1024 * 1024) + '&rate=65536')
        .then(function (r) { globalThis.__unreadHead = true; return r; })
        .catch(function () {});
      globalThis.__source = new EventSource(H + '/events?id=shutdown-es');
      __source.onmessage = function (e) { if (e.data.indexOf('again') === 0) globalThis.__sourceAgain = true; };
      'started';
    )JS");
    waitForJs(runtime,
              "globalThis.__socketOpen === true && globalThis.__sourceAgain === true &&"
              " globalThis.__unreadHead === true");
    // Long enough for the window to have stopped the unread body.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!runtime->idle());
    runtime->shutdown();
    runtime.reset();
    for (const auto& line : capture.lines()) {
      if (line.level == screenkit::LogLevel::Error) std::fprintf(stderr, "  error logged: %s\n", line.message.c_str());
      CHECK(line.level != screenkit::LogLevel::Error);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto observer = netRuntime(f);
  if (!observer) return;
  const std::string got = netTranscript(observer, R"JS(
run(async function () {
  log('hanging request closed ' + (await stats('shutdown-fetch')).closed);
  var stream = await stats('shutdown-stream');
  log('stream closed ' + stream.closed + ', never finished ' + !stream.finished);
  var unread = await stats('shutdown-unread');
  log('unread body closed ' + unread.closed + ', never finished ' + !unread.finished);
  log('eventsource closed ' + (await stats('shutdown-es')).closed);
});
)JS");
  checkTranscript(got, "hanging request closed true\n"
                       "stream closed true, never finished true\n"
                       "unread body closed true, never finished true\n"
                       "eventsource closed true");
}

// --- matrix row: the seam's own contract, on every client ----------------------------
/// What `NetService.h` promises and no JS-level row can check -- above the seam a
/// torn-down runtime has no JS thread left to deliver to, so a late callback
/// there proves nothing. Here, directly at the seam:
///
///   - `shutdown` returns only once no sink call is running, and nothing is
///     delivered after it. **Staged, not timed**: the thread that delivers is
///     held inside a sink call while the server keeps streaming a chunk every
///     5 ms, and `shutdown` is called while it is held -- so every event the
///     client produces meanwhile is queued behind a call that has not returned.
///     A client may deliver those or drop them, but only before `shutdown`
///     returns. Checked by sabotage on macOS: without the drain of the delivery
///     queue in NetServiceApple.mm's `shutdown`, it returns with the held call
///     still running and the row fails.
///   - a request id that is already in use is refused, and the request that
///     holds it is untouched -- still running, and still the one an abort of
///     that id reaches.
void netSeamContract() {
  auto f = netFixture();
  if (!f.ok) return;
  namespace net = screenkit::net;
  using std::chrono::milliseconds;

  struct Recorder final : net::HttpSink {
    int holdMs = 0;
    std::atomic<bool> holding{false};
    std::atomic<bool> released{false};
    std::atomic<bool> stopped{false};
    std::atomic<int> afterStop{0};
    std::atomic<int> heads{0};
    std::atomic<int> data{0};
    std::atomic<int> ends{0};
    std::atomic<int> errors{0};
    std::atomic<int> lastError{0};
    std::mutex bodyMutex;
    std::string body;
    void note() {
      if (stopped.load()) afterStop.fetch_add(1);
    }
    void onHead(net::ResponseHead) override {
      note();
      heads.fetch_add(1);
    }
    void onData(net::Bytes chunk) override {
      note();
      {
        std::lock_guard<std::mutex> lock(bodyMutex);
        body.append(chunk.begin(), chunk.end());
      }
      if (data.fetch_add(1) == 0 && holdMs > 0) {
        holding.store(true);
        std::this_thread::sleep_for(milliseconds(holdMs));
        released.store(true);
      }
    }
    void onEnd() override {
      note();
      ends.fetch_add(1);
    }
    void onError(net::NetError kind, std::string) override {
      note();
      lastError.store(static_cast<int>(kind));
      errors.fetch_add(1);
    }
    void onUploadProgress(std::uint64_t, std::int64_t, bool) override { note(); }
  };
  const auto waitUntil = [](const std::function<bool()>& done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(timeoutMs);
    while (!done()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(milliseconds(5));
    }
    return true;
  };

  net::NetConfig config;
  config.name = "net.seam";
  const std::string base = "http://127.0.0.1:" + std::to_string(f.http);

  // Part 1: shutdown with the delivering thread held inside a sink call.
  {
    auto service = net::NetService::create(config);
    CHECK(service != nullptr);
    if (!service) return;
    auto recorder = std::make_shared<Recorder>();
    recorder->holdMs = 500;
    net::HttpRequestSpec spec;
    std::string error;
    CHECK(net::parseUrl(base + "/slow?id=seam-stop&chunks=400&interval=5", "http", spec.url, error));
    service->startRequest(1, spec, recorder);
    CHECK(waitUntil([&] { return recorder->holding.load(); }, 20000));
    // Held. The server goes on writing and the client goes on reading, and what
    // it reads it hands to a delivery thread that is not coming back yet.
    std::this_thread::sleep_for(milliseconds(100));
    service->shutdown();
    recorder->stopped.store(true);
    std::fprintf(stderr, "  held call finished before shutdown returned: %d, chunks delivered: %d\n",
                 recorder->released.load() ? 1 : 0, recorder->data.load());
    CHECK(recorder->released.load());
    // Long enough for every chunk that was queued behind the stop to have run,
    // and for any the client was still reading to arrive.
    std::this_thread::sleep_for(milliseconds(500));
    CHECK_EQ(recorder->afterStop.load(), 0);
    service.reset();
    std::this_thread::sleep_for(milliseconds(100));
    CHECK_EQ(recorder->afterStop.load(), 0);
  }

  // Part 2: a duplicate id.
  {
    auto service = net::NetService::create(config);
    CHECK(service != nullptr);
    if (!service) return;
    auto first = std::make_shared<Recorder>();
    auto second = std::make_shared<Recorder>();
    net::HttpRequestSpec spec;
    std::string error;
    CHECK(net::parseUrl(base + "/hang?id=seam-dup", "http", spec.url, error));
    service->startRequest(5, spec, first);
    service->startRequest(5, spec, second);
    CHECK(waitUntil([&] { return second->errors.load() == 1; }, 5000));
    CHECK_EQ(second->lastError.load(), static_cast<int>(net::NetError::Url));
    std::this_thread::sleep_for(milliseconds(200));
    CHECK_EQ(first->heads.load() + first->errors.load() + first->ends.load(), 0);

    // The id still names the first: aborting it closes that connection, which
    // the server sees, and the first sink hears nothing.
    service->abortRequest(5);
    std::this_thread::sleep_for(milliseconds(300));
    auto stats = std::make_shared<Recorder>();
    net::HttpRequestSpec statsSpec;
    CHECK(net::parseUrl(base + "/stats?id=seam-dup", "http", statsSpec.url, error));
    service->startRequest(6, statsSpec, stats);
    CHECK(waitUntil([&] { return stats->ends.load() + stats->errors.load() > 0; }, 5000));
    {
      std::lock_guard<std::mutex> lock(stats->bodyMutex);
      std::fprintf(stderr, "  server's view of the first request: %s\n", stats->body.c_str());
      CHECK(stats->body.find("\"closed\":true") != std::string::npos);
    }
    CHECK_EQ(first->heads.load() + first->errors.load() + first->ends.load(), 0);
    service->shutdown();
  }
}

#if defined(__linux__) && !defined(__ANDROID__)
// ============================================================================
// Rows that exist only where the client is ours.
//
// Apple and Android hand trust, cookies and upload buffering to a vendor
// client, and what those do is that vendor's business. On Linux each one is
// code in this repo -- `trustPaths()`, `LinuxCookieJar`, the upload cap in
// `NetServiceLinux.cpp` -- so each one needs a row that fails when it is
// deleted. None of them touches the shared transcripts.
// ============================================================================

// --- Linux only: https verifies against the *image's* CA bundle -----------------
/// `netRuntime(f, false)` is production trust: no test anchors at all. The only
/// way the fixture's certificate can be accepted here is if the client really
/// did go and find the image's roots, so deleting `trustPaths()` -- or letting
/// it fall through to OpenSSL's compiled-in paths -- fails this row rather than
/// passing quietly. `SSL_CERT_FILE` is how every other OpenSSL program on the
/// image is pointed at a different bundle, and this client honours it.
void netSystemCa() {
  auto f = netFixture();
  if (!f.ok) return;
  // Read once per process, and every row runs in one of its own.
  const std::string bundle = netFixtureDir() + "/ca.pem";
  setenv("SSL_CERT_FILE", bundle.c_str(), 1);
  auto runtime = netRuntime(f, false);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var r = await fetch(S + '/data.json');
  log('the image roots are what https trusts: ' + r.status + ' ' + (await r.json()).n);
  // ...and all that it trusts: a certificate this bundle did not sign is still
  // refused, so the row cannot pass by verification having been turned off.
  try { await fetch(U + '/data.json'); log('untrusted RESOLVED'); }
  catch (e) { log('untrusted still refused: ' + e.name + ' ' + e.cause); }
  try { await fetch(E + '/data.json'); log('expired RESOLVED'); }
  catch (e) { log('expired still refused: ' + e.name + ' ' + e.cause); }
  // Three different wrongnesses, three different reasons: the verify result is
  // folded into the message (`describe`), or all three read "SSL server
  // verification failed" and a developer on the device cannot tell an expired
  // certificate from a missing root. The wording is OpenSSL's, so only its
  // distinctness and the host-name one -- which is this client's own -- are
  // asserted.
  var reasons = [];
  var refused = [U, E, W];
  for (var i = 0; i < refused.length; i++) {
    try { await fetch(refused[i] + '/data.json'); reasons.push('RESOLVED'); }
    catch (e) { reasons.push(e.message.replace(/^Failed to fetch \S+: /, '')); }
  }
  log('three refusals, three reasons: ' + (reasons[0] !== reasons[1] && reasons[1] !== reasons[2] && reasons[0] !== reasons[2]));
  log('the wrong host says so: ' + /not valid for this host name/.test(reasons[2]));
});
)JS");
  checkTranscript(got,
      "the image roots are what https trusts: 200 42\n"
      "untrusted still refused: TypeError tls\n"
      "expired still refused: TypeError tls\n"
      "three refusals, three reasons: true\n"
      "the wrong host says so: true");
}

// --- Linux only: the jar's own rules --------------------------------------------
/// Every one of these is a rule `LinuxCookieJar` enforces and no fixture host
/// can reach: the fixture is 127.0.0.1 and localhost, and the interesting cases
/// are about registrable domains and secure origins. `__screenkit.net.setCookie`
/// takes the URL as an argument, so they are reachable without a server.
void netCookieRules() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var set = __screenkit.net.setCookie, get = __screenkit.net.getCookies;
  // A single-label `Domain` is refused. There is no public-suffix list here, so
  // this is the cheap half of that rule: without it one site writes a cookie
  // for every site under a TLD.
  log('Domain=com refused: ' + !set('http://shop.example.com/', 'wide=1; Domain=com'));
  log('nothing reached another site: ' + JSON.stringify(get('http://other.example.org/')));
  log('nor the site that tried: ' + JSON.stringify(get('http://shop.example.com/')));
  // A domain the host is actually under is still allowed, or the rule above
  // would be indistinguishable from refusing every Domain attribute.
  log('Domain=example.com kept: ' + set('http://shop.example.com/', 'narrow=2; Domain=example.com'));
  log('and reaches the parent: ' + JSON.stringify(get('http://example.com/')));
  // `Secure` may not be *set* from a plaintext origin. Refusing to send it
  // later is no help: the damage is the value that is now stored under a name
  // the site believes only https could have written.
  log('Secure over http refused: ' + !set('http://shop.example.com/', 'tok=plain; Secure'));
  log('Secure over https kept: ' + set('https://shop.example.com/', 'tok=tls; Secure'));
  log('and is not readable over http: ' + JSON.stringify(get('http://shop.example.com/')));
  log('but is over https: ' + JSON.stringify(get('https://shop.example.com/')));
  // The cookie prefixes, which mean nothing unless they are enforced -- and
  // `setCookie` is reachable from page JS, so a page could otherwise forge the
  // one name a server is entitled to trust.
  log('__Secure- without Secure refused: ' + !set('https://shop.example.com/', '__Secure-a=1'));
  log('__Host- with a Domain refused: ' + !set('https://shop.example.com/', '__Host-b=1; Secure; Path=/; Domain=example.com'));
  log('__Host- with a narrower Path refused: ' + !set('https://shop.example.com/', '__Host-c=1; Secure; Path=/admin'));
  log('__Host- over http refused: ' + !set('http://shop.example.com/', '__Host-d=1; Secure; Path=/'));
  log('__Host- done properly kept: ' + set('https://shop.example.com/', '__Host-e=1; Secure; Path=/'));
  // A `Max-Age` past the end of the clock means "never", and must not be added
  // to it: 2^63-1 seconds from now is signed overflow, which wrapped negative
  // and deleted the very cookie that asked to live for ever.
  // (Hosts outside example.com, which `narrow=2` above is scoped to.)
  log('Max-Age=2^63-1 kept: ' + set('http://life.example.net/', 'forever=1; Max-Age=9223372036854775807') +
      ' and alive: ' + JSON.stringify(get('http://life.example.net/')));
  log('Max-Age=-1 deletes it: ' + set('http://life.example.net/', 'forever=; Max-Age=-1') +
      ' ' + JSON.stringify(get('http://life.example.net/')));
  // RFC 6265 6.1's floor is 4096 bytes of name and value, and this jar's
  // ceiling is the same: every cookie is held in RAM on a 908 MB device.
  log('4096 bytes of name and value kept: ' + set('http://size.example.net/', 'b=' + 'x'.repeat(4095)));
  log('4097 refused: ' + !set('http://size.example.net/', 'c=' + 'x'.repeat(4096)) +
      ', only the first there: ' + (get('http://size.example.net/').indexOf('c=') < 0));
});
)JS");
  checkTranscript(got,
      "Domain=com refused: true\n"
      "nothing reached another site: \"\"\n"
      "nor the site that tried: \"\"\n"
      "Domain=example.com kept: true\n"
      "and reaches the parent: \"narrow=2\"\n"
      "Secure over http refused: true\n"
      "Secure over https kept: true\n"
      "and is not readable over http: \"narrow=2\"\n"
      "but is over https: \"narrow=2; tok=tls\"\n"
      "__Secure- without Secure refused: true\n"
      "__Host- with a Domain refused: true\n"
      "__Host- with a narrower Path refused: true\n"
      "__Host- over http refused: true\n"
      "__Host- done properly kept: true\n"
      "Max-Age=2^63-1 kept: true and alive: \"forever=1\"\n"
      "Max-Age=-1 deletes it: true \"\"\n"
      "4096 bytes of name and value kept: true\n"
      "4097 refused: true, only the first there: true");
}


// --- Linux only: names, and what a cancel reaches -----------------------------------
/// Two things only the Linux client owns. Name resolution: every other row
/// connects to 127.0.0.1, so this is the one that resolves a name -- through
/// the client's own abandonable resolver and `set_hostname_addr_map`, with TLS
/// still checked against the *name*. And cancellation of a connect in progress:
/// cpp-httplib holds its socket mutex through a whole connect, so its `stop()`
/// waited one out, on the I/O queue, stalling every other request behind it.
/// 192.0.2.1 is TEST-NET-1 (RFC 5737) and never routed, so a SYN to it is
/// dropped and a connect waits for its timeout -- 30 s here, and measured at the
/// full 5 s `--connect-timeout` with curl from the Pi.
void netCancelReach() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var named = await fetch(H.replace('127.0.0.1', 'localhost') + '/data.json');
  log('by name ' + named.status + ' ' + (await named.json()).n);
  var secure = await fetch(S.replace('127.0.0.1', 'localhost') + '/data.json');
  log('by name over https ' + secure.status);
  var w = ws(H.replace('http://127.0.0.1', 'ws://localhost') + '/ws');
  await opened(w);
  w.socket.close();
  await w.closed;
  log('a socket by name ' + w.events[0]);

  var controller = new AbortController();
  var pending = fetch('http://192.0.2.1/x', { signal: controller.signal })
    .then(function () { return 'RESOLVED'; }, function (e) { return e.name; });
  await sleep(300);
  controller.abort();
  log('aborted mid-connect ' + await pending);
  var started = Date.now();
  var next = await fetch(H + '/data.json');
  log('the next request is not held behind it ' + next.status + ' ' + (Date.now() - started < 2000));
});
)JS");
  checkTranscript(got,
      "by name 200 42\n"
      "by name over https 200\n"
      "a socket by name open\n"
      "aborted mid-connect AbortError\n"
      "the next request is not held behind it 200 true");

  // ...and a teardown with a request and a socket both still connecting
  // returns at once: `shutdown` joins every worker, and before the socket copy
  // each of these joins waited out a 30 s connect.
  domEval(runtime, "fetch('http://192.0.2.1/y').catch(function () {});"
                   "new WebSocket('ws://192.0.2.1/z'); 'ok';");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto started = std::chrono::steady_clock::now();
  runtime->shutdown();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr, "  shutdown with two connects in flight: %lld ms\n", static_cast<long long>(took));
  CHECK(took < 2000);
}

// --- Linux only: a request body that outruns the server -------------------------
/// The 8 MiB cap in `appendRequestBody`. `__screenkit.net.write` is
/// fire-and-forget -- there is no way to push back on the page -- so a server
/// that stops reading leaves the choice between a bound and an OOM. Nothing
/// else reaches it: every other upload row has a server that reads.
///
/// The second claim is the one that needed the code moved: the failure is
/// reported by the worker on its way out, never from the I/O queue, so it can
/// not arrive after an `onEnd` that was already queued.
void netUploadOverflow() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = netRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var pushed = 0, events = [];
  // `sleep(0)` rather than a bare enqueue: the shim's upload pump reads this
  // stream and writes in a promise chain, so a `pull` that resolves
  // synchronously never lets the event loop run a *task* -- and the error this
  // row is waiting for is delivered as one. Without the yield the page spins,
  // pushing megabytes, and never hears the answer.
  var stream = new ReadableStream({
    pull: function (c) { pushed++; c.enqueue(new Uint8Array(1024 * 1024)); return sleep(0); }
  });
  var response = await fetch(H + '/sink-stalled?id=overflow',
                             { method: 'POST', body: stream, duplex: 'half' })
      .then(function (r) { events.push('RESOLVED ' + r.status); },
            function (e) { events.push(e.name + ' ' + e.cause); });
  log('outran the server: ' + events.join(','));
  log('and it took more than the cap to do it: ' + (pushed > 8));
  // Not "the server saw it close": it cannot. The client shuts the socket, but
  // the FIN queues behind megabytes this server is deliberately not reading, so
  // node sees nothing until it drains -- which is the whole point of the route.
  // What *is* observable, and is what the failure has to leave behind, is that
  // the origin still works: the slot and the worker thread were given back.
  await sleep(300);
  var after = await (await fetch(H + '/data.json')).json();
  log('the origin is usable afterwards: ' + (after.n === 42));
});
)JS", 90000);
  checkTranscript(got,
      "outran the server: TypeError network\n"
      "and it took more than the cap to do it: true\n"
      "the origin is usable afterwards: true");
}
#endif  // __linux__ && !__ANDROID__

#ifdef SCREENKIT_ANDROID_NET_ROWS
// --- matrix row: no JavaVM (Android) --------------------------------------------
/// The client is built but nothing ever gave it a JavaVM, so it never resolved
/// `dev.screenkit.net.HttpClient`. Every request has to fail with `unsupported`,
/// as on a platform with no client at all, and nothing may crash.
///
/// The harness is what arranges it, not the client: `NetTests` sets
/// SCREENKIT_NET_TESTS_NO_BACKEND before it loads this library, and this
/// library's JNI_OnLoad then skips `setJavaVm` and `prepareAndroidNetwork`
/// entirely. Nothing in `NetServiceAndroid.cpp` knows the row exists.
void netNoJavaVm() {
  // If this fails the harness did not do its half, and everything below would
  // pass for the wrong reason.
  CHECK(!screenkit::net::networkAvailable());
  if (screenkit::net::networkAvailable()) return;

  auto config = testConfig();
  config.name = "net";
  auto runtime = screenkit::Runtime::create(std::move(config));
  CHECK(runtime != nullptr);
  if (!runtime) return;
  if (!installDomShim(runtime)) return;
  // Addresses nothing listens on: the point is that no connection is attempted.
  domEval(runtime, "globalThis.__net = { http: 'http://127.0.0.1:9', https: 'https://127.0.0.1:9',"
                   " httpsPort: 9, untrusted: 'https://127.0.0.1:9', expired: 'https://127.0.0.1:9',"
                   " wrongHost: 'https://127.0.0.1:9', closedPort: 9 }; 'ok';");
  domEval(runtime, kNetPrelude);

  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  try { await fetch(H + '/x'); log('fetch RESOLVED'); } catch (e) { log('fetch ' + e.name + ' ' + e.cause); }
  var x = await xhr('GET', H + '/x');
  log('xhr ' + x.events + ' status=' + x.xhr.status);
  var w = ws(H.replace('http:', 'ws:') + '/ws');
  await w.closed;
  log('ws ' + w.events.join(',') + ' readyState=' + w.socket.readyState);
  // Fatal, not retried for ever: a platform with no client is not a blip.
  log('eventsource ' + await esFailure(H + '/events'));
});
)JS");
  checkTranscript(got,
      "fetch TypeError unsupported\n"
      "xhr loadstart,readystatechange:4,error,loadend status=0\n"
      "ws error,close:1006::false readyState=3\n"
      "eventsource error:2");

  // Nothing is left hanging, no HttpClient was ever built, and the runtime tears
  // down like any other.
  CHECK(pumpUntilIdle(runtime, 10000));
  CHECK_EQ(screenkit::net::liveJavaRefCount(), static_cast<std::size_t>(0));
  runtime->shutdown();
  runtime.reset();
}

// --- matrix row: shutdown mid-call (Android) -------------------------------------
/// Teardown while a response is streaming and a WebSocket is open. Two claims,
/// neither of which `net-shutdown-idle` makes: that no sink call arrives once
/// the seam has been told to stop, and that the JNI references the client held
/// are gone afterwards.
///
/// The second is the reason this row exists on Android. A global reference the
/// client forgets to delete fails nothing and logs nothing -- the Java object
/// and its connection simply never go -- so `net::liveJavaRefCount()` is the
/// only way to see it.
void netShutdownMidCall() {
  auto f = netFixture();
  if (!f.ok) return;
  namespace net = screenkit::net;

  // Part 1: the seam's own guarantee, checked at the seam. Above it a torn-down
  // runtime has no JS thread left to call, so a missing callback there proves
  // nothing; here a late one is counted.
  {
    /// Every event notes whether the request had already been aborted.
    struct Recorder final : net::HttpSink {
      std::atomic<bool> aborted{false};
      std::atomic<int> afterAbort{0};
      std::atomic<int> heads{0};
      std::atomic<int> data{0};
      void note() {
        if (aborted.load()) afterAbort.fetch_add(1);
      }
      void onHead(net::ResponseHead) override { note(); heads.fetch_add(1); }
      void onData(net::Bytes) override { note(); data.fetch_add(1); }
      void onEnd() override { note(); }
      void onError(net::NetError, std::string) override { note(); }
      void onUploadProgress(std::uint64_t, std::int64_t, bool) override { note(); }
    };

    net::NetConfig config;
    config.name = "net.seam";
    config.testTlsAnchors.push_back(f.ca);
    auto service = net::NetService::create(config);
    CHECK(service != nullptr);
    if (!service) return;

    auto recorder = std::make_shared<Recorder>();
    net::HttpRequestSpec spec;
    std::string error;
    const std::string base = "http://127.0.0.1:" + std::to_string(f.http);
    CHECK(net::parseUrl(base + "/slow?id=seam-close&chunks=200&interval=20", "http", spec.url, error));
    service->startRequest(1, spec, recorder);

    // A response really in flight: the row is about stopping mid-stream.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (recorder->data.load() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(recorder->heads.load() == 1);
    CHECK(recorder->data.load() > 0);
    // The client object and the one call it is running.
    CHECK(net::liveJavaRefCount() >= 2);

    recorder->aborted.store(true);
    service->abortRequest(1);
    // Long enough for the server's next chunks -- one every 20 ms -- and for a
    // read already under way to come back through Java and find its slot gone.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_EQ(recorder->afterAbort.load(), 0);
    // The call is retired by the abort itself; only the client itself is left.
    CHECK_EQ(net::liveJavaRefCount(), static_cast<std::size_t>(1));
    service->shutdown();
    service.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(recorder->afterAbort.load(), 0);
    CHECK_EQ(net::liveJavaRefCount(), static_cast<std::size_t>(0));
  }

  // Part 2: the matrix scenario. Three times, because what would go wrong here
  // is a race between the teardown and the Java threads still reading.
  for (int attempt = 0; attempt < 3; ++attempt) {
    test::LogCapture capture;
    auto runtime = netRuntime(f);
    if (!runtime) return;
    const std::string id = "midcall-" + std::to_string(attempt);
    domEval(runtime, ("globalThis.__chunks = 0;"
                      "fetch(H + '/slow?id=" + id + "&chunks=200&interval=20').then(function (r) {"
                      "  var reader = r.body.getReader();"
                      "  function next() { return reader.read().then(function (x) {"
                      "    if (x.done) return; __chunks++; return next(); }); }"
                      "  return next();"
                      "}).catch(function () {});"
                      "globalThis.__socket = new WebSocket(H.replace('http:', 'ws:') + '/ws');"
                      "__socket.onopen = function () { globalThis.__socketOpen = true; };"
                      "'started';").c_str());
    // Both genuinely in flight before the teardown, or the row proves nothing.
    waitForJs(runtime, "globalThis.__socketOpen === true && globalThis.__chunks > 0");
    CHECK(!runtime->idle());
    CHECK(net::liveJavaRefCount() >= 3);

    runtime->shutdown();
    runtime.reset();
    // Everything the client held is released by shutdown itself, which is
    // synchronous: no later turn of any queue is needed.
    CHECK_EQ(net::liveJavaRefCount(), static_cast<std::size_t>(0));
    for (const auto& line : capture.lines()) {
      if (line.level == screenkit::LogLevel::Error) {
        std::fprintf(stderr, "  error logged: %s\n", line.message.c_str());
      }
      CHECK(line.level != screenkit::LogLevel::Error);
    }
  }

  // The server's side of the same story: every connection torn down mid-stream
  // was actually closed, and none of the streams ran to completion.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto observer = netRuntime(f);
  if (!observer) return;
  const std::string got = netTranscript(observer, R"JS(
run(async function () {
  for (var i = 0; i < 3; i++) {
    var s = await stats('midcall-' + i);
    log('stream ' + i + ' closed ' + s.closed + ', never finished ' + !s.finished);
  }
});
)JS");
  checkTranscript(got, "stream 0 closed true, never finished true\n"
                       "stream 1 closed true, never finished true\n"
                       "stream 2 closed true, never finished true");
}
#endif  // SCREENKIT_ANDROID_NET_ROWS

// Event handler IDL attributes: a handler is a listener, placed where it was
// first set, invoked in the target and bubble passes; false cancels.
void domEventHandlers() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  CHECK_EQ(domEval(runtime,
      "var el = document.createElement('div'); document.body.appendChild(el);"
      "var order = [];"
      "el.addEventListener('click', function () { order.push('first'); });"
      "el.onclick = function (e) { order.push('handler ' + (this === el) + ' ' + e.eventPhase); };"
      "el.addEventListener('click', function () { order.push('last'); });"
      "el.dispatchEvent(new Event('click'));"
      // Replacing keeps the position; returning false cancels.
      "el.onclick = function () { order.push('replaced'); return false; };"
      "var cancelable = new Event('click', { cancelable: true });"
      "order.push('dispatch ' + el.dispatchEvent(cancelable) + ' ' + cancelable.defaultPrevented);"
      // null unregisters; setting again goes to the end.
      "el.onclick = null; el.dispatchEvent(new Event('click'));"
      "el.onclick = function () { order.push('re-added'); }; el.dispatchEvent(new Event('click'));"
      "order.join(',');"),
      std::string("first,handler true 2,last,first,replaced,last,dispatch false true,first,last,first,last,re-added"));

  // Not a function is null; the attributes are where a browser has them, and a
  // TV reports no touch.
  CHECK_EQ(domEval(runtime,
      "var probe = document.createElement('span'); probe.onclick = 'alert(1)';"
      "[probe.onclick === null, el.onkeydown === null, 'onclick' in el, 'onkeydown' in document,"
      " 'onhashchange' in window, 'onresize' in window, 'onvisibilitychange' in document,"
      " 'onhashchange' in el, 'ontouchstart' in window, 'ontouchstart' in el,"
      " 'onload' in new XMLHttpRequest(), 'onmessage' in WebSocket.prototype,"
      " 'onabort' in new AbortController().signal].join(',');"),
      std::string("true,true,true,true,true,true,true,false,false,false,true,true,true"));

  // A key from the host bubbles to document.onkeydown and window.onkeydown; a
  // throwing handler does not stop the rest; capture does not invoke handlers.
  CHECK_EQ(domEval(runtime,
      "var keys = [];"
      "document.body.onkeydown = function () { throw new Error('body handler broke'); };"
      "document.onkeydown = function (e) { keys.push('document ' + e.key + ' ' + e.eventPhase); };"
      "window.onkeydown = function (e) { keys.push('window ' + e.key + ' ' + (this === window)); };"
      "window.addEventListener('keydown', function () { keys.push('window listener'); });"
      "var step = __screenkitKey('keydown', 'ArrowUp', 'ArrowUp', 38, false, 0);"
      "while (step()) {}"
      "var parent = document.createElement('div'), child = document.createElement('div');"
      "parent.appendChild(child);"
      "parent.onfocus = function () { keys.push('WRONG parent onfocus'); };"
      "child.dispatchEvent(new Event('focus'));"
      "keys.join(',');"),
      std::string("document ArrowUp 3,window ArrowUp true,window listener"));
  CHECK(capture.has(screenkit::LogLevel::Error, "body handler broke"));

  // A bare global assignment is window's handler, as in a browser.
  domEval(runtime,
      "globalThis.__hash = 'pending';"
      "onhashchange = function (e) { __hash = e.type + ' ' + e.newURL.slice(e.newURL.indexOf('#')); };"
      "location.hash = '/next'; 'set';");
  CHECK_EQ(pumpThenRead(runtime, "__hash;"), std::string("hashchange #/next"));
}

// MutationObserver: records for tree and attribute changes, delivered from a
// microtask, with the DOM's options, transient registrations and errors.
void domMutationObserver() {
  test::LogCapture capture;
  auto runtime = domRuntime(320, 180);
  if (!runtime) return;

  domEval(runtime,
      "function name(n) { return n ? n.localName : '-'; }"
      "function describe(records) {"
      "  return records.map(function (r) {"
      "    if (r.type === 'attributes') return 'attr ' + name(r.target) + ' ' + r.attributeName + ' ' + r.oldValue;"
      "    return 'tree ' + name(r.target) + ' +' + Array.prototype.map.call(r.addedNodes, name).join('+') +"
      "      ' -' + Array.prototype.map.call(r.removedNodes, name).join('-') +"
      "      ' ' + name(r.previousSibling) + ' ' + name(r.nextSibling);"
      "  }).join(' ; ');"
      "}"
      "globalThis.log = [];"
      "globalThis.root = document.createElement('div'); document.body.appendChild(root);"
      "globalThis.a = document.createElement('p');"
      "globalThis.b = document.createElement('span');"
      "globalThis.c = document.createElement('i');"
      "globalThis.mo = new MutationObserver(function (records, observer) {"
      "  log.push((observer === mo && this === mo && records[0] instanceof MutationRecord) + ' ' + describe(records));"
      "});"
      "mo.observe(root, { childList: true, attributeOldValue: true, subtree: true });"
      "root.appendChild(a); root.appendChild(b); a.setAttribute('id', 'x'); a.id = 'y';"
      "globalThis.syncCount = log.length; 'observing';");
  CHECK_EQ(pumpThenRead(runtime, "syncCount + ' | ' + log.join(' | ');"),
           std::string("0 | true tree div +p - - - ; tree div +span - p - ; attr p id null ; attr p id x"));

  // A move is a removal and an insertion; replaceChild is one record; a node
  // removed from the observed subtree stays observed until the delivery.
  domEval(runtime,
      "log.length = 0;"
      "root.insertBefore(b, a);"
      "root.replaceChild(c, a);"
      "root.removeChild(b); b.setAttribute('class', 'gone');"
      "c.style.left = '5px';"
      "'mutated';");
  CHECK_EQ(pumpThenRead(runtime, "log.join(' | ');"),
           std::string("true tree div + -span p - ; tree div +span - - p ; tree div +i -p span - ; "
                       "tree div + -span - i ; attr span class null ; attr i style null"));

  // After the delivery the transient registration is gone. A second observer
  // with a filter; observers are notified in creation order even when the first
  // throws; takeRecords empties the queue; observing again replaces options;
  // disconnect stops everything.
  domEval(runtime,
      "log.length = 0;"
      "b.setAttribute('class', 'later');"
      "globalThis.filtered = new MutationObserver(function (records) { log.push('filtered ' + describe(records)); });"
      "filtered.observe(c, { attributeFilter: ['title'] });"
      "globalThis.thrower = new MutationObserver(function () { throw new Error('observer broke'); });"
      "thrower.observe(c, { attributes: true });"
      "c.setAttribute('title', 't'); c.setAttribute('lang', 'en');"
      "'mutated';");
  CHECK_EQ(pumpThenRead(runtime, "log.join(' | ');"),
           std::string("true attr i title null ; attr i lang null | filtered attr i title null"));
  CHECK(capture.has(screenkit::LogLevel::Error, "observer broke"));

  CHECK_EQ(domEval(runtime,
      "log.length = 0;"
      "root.appendChild(document.createElement('b'));"
      "var taken = mo.takeRecords();"
      "mo.observe(root, { attributes: true });"
      "root.appendChild(document.createElement('u'));"
      "root.setAttribute('title', 'r');"
      "taken.length + ' ' + describe(taken);"),
      std::string("1 tree div +b - i -"));
  CHECK_EQ(pumpThenRead(runtime, "log.join(' | ');"), std::string("true attr div title null"));
  CHECK_EQ(domEval(runtime,
      "log.length = 0; mo.disconnect(); filtered.disconnect(); thrower.disconnect();"
      "root.setAttribute('title', 'again'); c.setAttribute('title', 'again'); 'disconnected';"),
      std::string("disconnected"));
  CHECK_EQ(pumpThenRead(runtime, "log.length + ' ' + mo.takeRecords().length;"), std::string("0 0"));

  CHECK_EQ(domEval(runtime,
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.name + ': ' + e.message; } }"
      "[attempt(function () { mo.observe(root, {}); }),"
      " attempt(function () { mo.observe(root, { childList: true, attributes: false, attributeOldValue: true }); }),"
      " attempt(function () { mo.observe({}, { childList: true }); }),"
      " attempt(function () { new MutationObserver(); })].join(' | ');"),
      std::string("TypeError: Failed to execute 'observe' on 'MutationObserver': The options object must set at least "
                  "one of 'attributes', 'characterData', or 'childList' to true. | "
                  "TypeError: Failed to execute 'observe' on 'MutationObserver': The options object may only set "
                  "'attributeOldValue' to true when 'attributes' is true or not present. | "
                  "TypeError: Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'. | "
                  "TypeError: Failed to construct 'MutationObserver': parameter 1 is not of type 'Function'."));
}

// createImageBitmap(source, sx, sy, sw, sh): the rectangle, uploaded and read
// back from the GPU, from every kind of source a crop can take.
void domImageBitmapCrop() {
  test::LogCapture capture;
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = 'pending';"
      // A bitmap's pixels as the GPU has them: uploaded, attached to a
      // framebuffer and read back, one 'r,g,b,a' per pixel.
      "function gpu(bm) {"
      "  var t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);"
      "  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bm);"
      "  var fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fb);"
      "  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);"
      "  var px = new Uint8Array(bm.width * bm.height * 4);"
      "  gl.readPixels(0, 0, bm.width, bm.height, gl.RGBA, gl.UNSIGNED_BYTE, px);"
      "  gl.bindFramebuffer(gl.FRAMEBUFFER, null);"
      "  var out = [];"
      "  for (var i = 0; i < px.length; i += 4) out.push(px[i] + ',' + px[i + 1] + ',' + px[i + 2] + ',' + px[i + 3]);"
      "  return bm.width + 'x' + bm.height + ' ' + out.join(' ');"
      "}"
      "function settle(p) { return p.then(gpu, function (e) { return e.name + ': ' + e.message; }); }"
      "var img = new Image();"
      "img.onload = function () {"
      "  fetch('img/sheet.png').then(function (r) { return r.blob(); }).then(function (blob) {"
      "    return createImageBitmap(img).then(function (whole) {"
      "      var data = new ImageData(new Uint8ClampedArray([1,2,3,255, 4,5,6,255, 7,8,9,255, 10,11,12,255]), 2, 2);"
      "      return Promise.all(["
      "        settle(createImageBitmap(blob, 1, 0, 2, 2)),"
      "        settle(createImageBitmap(blob, 3, 1, -2, 1, { premultiplyAlpha: 'none' })),"
      "        settle(createImageBitmap(blob, 3, 1, 2, 2)),"
      "        settle(createImageBitmap(img, 2, 1, 1, 1)),"
      "        settle(createImageBitmap(whole, 0, 1, 1, 1)),"
      "        settle(createImageBitmap(data, 1, 1, 1, 1)),"
      "        settle(createImageBitmap(blob, 0, 0, 0, 1)),"
      "        settle(createImageBitmap(blob, 0, 0, 1, 0)),"
      "        settle(createImageBitmap(blob))"
      "      ]);"
      "    });"
      "  }).then(function (all) { __r = all.join('\\n'); }, function (e) { __r = 'failed ' + e; });"
      "};"
      "img.onerror = function (e) { __r = 'img error ' + e.error; };"
      "img.src = 'img/sheet.png'; 'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"),
           std::string("2x2 70,20,7,255 130,20,7,255 70,120,7,255 130,120,7,255\n"
                       "2x1 70,120,7,255 130,120,7,255\n"
                       "2x2 190,120,7,255 0,0,0,0 0,0,0,0 0,0,0,0\n"
                       "1x1 130,120,7,255\n"
                       "1x1 10,120,7,255\n"
                       "1x1 10,11,12,255\n"
                       "RangeError: Failed to execute 'createImageBitmap' on 'Window': The crop rect width is 0.\n"
                       "RangeError: Failed to execute 'createImageBitmap' on 'Window': The crop rect height is 0.\n"
                       "4x2 10,20,7,255 70,20,7,255 130,20,7,255 190,20,7,255 10,120,7,255 70,120,7,255 130,120,7,255 190,120,7,255"));
}

// --- matrix row: log sink swap -------------------------------------------------
// setLogSink while the JS thread logs flat out. Each sink writes into its own
// heap object, and the test deletes the old object the moment the swap returns
// -- so a call still inside the old sink would write into freed memory, which
// is what the ASan build is there to catch.
void logSinkSwap() {
  struct Counter {
    std::atomic<long> lines{0};
    std::string last;
    std::mutex mutex;
  };
  auto sinkFor = [](Counter* counter) {
    return [counter](screenkit::LogLevel, const std::string&, const std::string& message) {
      std::lock_guard<std::mutex> lock(counter->mutex);
      counter->last = message;
      counter->lines.fetch_add(1);
    };
  };

  auto current = std::make_unique<Counter>();
  screenkit::setLogSink(sinkFor(current.get()));
  auto runtime = screenkit::Runtime::create(testConfig());
  CHECK(runtime != nullptr);
  if (!runtime) {
    screenkit::setLogSink(nullptr);
    return;
  }
  const auto started = runtime->evaluateSource(
      "var n = 0;"
      "setInterval(function () { for (var i = 0; i < 200; i++) console.log('spin ' + n++); }, 0);"
      "'spinning';",
      "spin.js");
  CHECK(started.ok);

  long total = 0;
  int swaps = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline) {
    auto next = std::make_unique<Counter>();
    screenkit::setLogSink(sinkFor(next.get()));
    total += current->lines.load();
    current = std::move(next);  // the old sink's object is freed here
    ++swaps;
    // Room for the JS thread to take the sink lock between swaps.
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }

  runtime->shutdown();
  screenkit::setLogSink(nullptr);
  total += current->lines.load();
  current.reset();
  // The JS thread really was logging throughout, and the swaps really happened.
  CHECK(swaps > 1000);
  CHECK(total > 10000);
  std::fprintf(stderr, "  %d swaps, %ld lines\n", swaps, total);
}

// Bytes in memory decode off the JS thread: a 0 ms timer set after
// createImageBitmap runs before the bitmap arrives. Decoded on the JS thread, the
// promise would already be settled and its callback would run first. An
// undecodable Blob still rejects with InvalidStateError.
void domImageDecodeAsync() {
  test::LogCapture capture;
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;
  domEval(runtime,
      "globalThis.__r = 'pending';"
      "fetch('img/large.png').then(function (r) { return r.arrayBuffer(); }).then(function (buffer) {"
      "  var order = [];"
      "  var inMemory = new Blob([buffer], { type: 'image/png' });"
      "  var decoded = createImageBitmap(inMemory).then(function (bm) {"
      "    order.push('bitmap ' + bm.width + 'x' + bm.height + ' ' + Array.prototype.slice.call(bm.data, 0, 4).join(','));"
      "  });"
      "  setTimeout(function () { order.push('timer'); }, 0);"
      "  order.push('sync');"
      "  var broken = createImageBitmap(new Blob([new Uint8Array([1, 2, 3])])).then(function () { return 'RESOLVED'; },"
      "    function (e) { return e.name; });"
      "  return Promise.all([decoded, broken]).then(function (all) { __r = order.join(',') + ' | ' + all[1]; });"
      "}).catch(function (e) { __r = 'failed ' + e; });"
      "'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"), std::string("sync,timer,bitmap 2048x2048 30,60,90,255 | InvalidStateError"));
}

// document.readyState and the load events: 'loading' while the app first runs,
// then DOMContentLoaded (bubbling to window) and, a task later, load -- once.
void domDocumentReady() {
  auto runtime = domRuntime();
  if (!runtime) return;
  domEval(runtime,
      "globalThis.__events = [document.readyState];"
      "document.addEventListener('readystatechange', function () { __events.push('readystatechange:' + document.readyState); });"
      "document.addEventListener('DOMContentLoaded', function (e) { __events.push('DOMContentLoaded:' + document.readyState + ':' + e.isTrusted); });"
      "window.addEventListener('DOMContentLoaded', function () { __events.push('window saw DOMContentLoaded'); });"
      "window.onload = function (e) { __events.push('load:' + document.readyState + ':' + (e.target === window)); };"
      "__screenkitDocumentLoaded(); __events.push('sync:' + document.readyState); __screenkitDocumentLoaded(); 'ok';");
  CHECK_EQ(pumpThenRead(runtime, "__events.join(', ') + ' | ' + new DOMParser().parseFromString('<a/>', 'text/xml').readyState;"),
           std::string("loading, readystatechange:interactive, DOMContentLoaded:interactive:true, window saw DOMContentLoaded, "
                       "sync:interactive, readystatechange:complete, load:complete:true | complete"));
}

// The software 2D context: exact pixels through fill, clear, image data,
// compositing, transforms and drawImage from every kind of source, a canvas as a
// WebGL texture, and the unsupported half drawing nothing without throwing.
void domCanvas2d() {
  test::LogCapture capture;
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;

  CHECK_EQ(domEval(runtime,
      "var c = document.createElement('canvas'); c.width = 8; c.height = 4;"
      "var x = c.getContext('2d', { willReadFrequently: true });"
      "function px(ctx, a, b) { return Array.prototype.join.call(ctx.getImageData(a, b, 1, 1).data, ','); }"
      "var out = [x instanceof CanvasRenderingContext2D, x.canvas === c, c.getContext('2d') === x, px(x, 0, 0)];"
      // The same probe Phaser runs at import: a half-transparent colour survives
      // getImageData -> putImageData -> getImageData.
      "x.fillStyle = 'rgba(10, 20, 30, 0.5)'; x.fillRect(0, 0, 1, 1);"
      "var got = x.getImageData(0, 0, 1, 1); x.putImageData(got, 1, 0);"
      "out.push(px(x, 0, 0), px(x, 1, 0));"
      "var colors = ['red', '#0f08', 'rgb(1 2 3 / 50%)', 'hsl(240, 100%, 50%)', 'transparent', 'nonsense'].map(function (v) {"
      "  x.fillStyle = '#123456'; x.fillStyle = v; return x.fillStyle; });"
      "out.push(colors.join(';'));"
      // Source-over: half red over opaque blue.
      "x.fillStyle = 'blue'; x.fillRect(2, 0, 1, 1); x.globalAlpha = 0.5; x.fillStyle = 'red'; x.fillRect(2, 0, 1, 1); x.globalAlpha = 1;"
      "out.push(px(x, 2, 0));"
      // multiply (Phaser's blend-mode probe) and destination-out.
      "x.fillStyle = '#ff00ff'; x.fillRect(3, 0, 1, 1); x.globalCompositeOperation = 'multiply'; x.fillStyle = '#ffff00'; x.fillRect(3, 0, 1, 1);"
      "x.globalCompositeOperation = 'destination-out'; x.fillRect(4, 0, 1, 1); x.globalCompositeOperation = 'source-over';"
      "out.push(px(x, 3, 0), x.globalCompositeOperation);"
      // Transforms, save/restore, clearRect.
      "x.save(); x.translate(5, 1); x.scale(2, 2); x.fillStyle = 'lime'; x.fillRect(0, 0, 1, 1); x.restore();"
      "out.push(px(x, 5, 1), px(x, 6, 2), px(x, 7, 3), x.fillStyle, x.getTransform().isIdentity);"
      "x.clearRect(5, 1, 1, 1); out.push(px(x, 5, 1));"
      "x.save(); x.translate(4, 0); x.rotate(Math.PI / 2); x.fillStyle = 'yellow'; x.fillRect(0, 0, 2, 1); x.restore();"
      "out.push(px(x, 3, 1), px(x, 3, 0));"
      "out.join(' | ');"),
      std::string("true | true | true | 0,0,0,0 | 10,20,30,128 | 10,20,30,128 | "
                  "#ff0000;rgba(0, 255, 0, 0.533);rgba(1, 2, 3, 0.5);#0000ff;rgba(0, 0, 0, 0);#123456 | "
                  "128,0,128,255 | 255,0,0,255 | source-over | 0,255,0,255 | 0,255,0,255 | 0,0,0,0 | #ffff00 | true | 0,0,0,0 | "
                  "255,255,0,255 | 255,255,0,255"));

  // drawImage: a canvas scaled with nearest sampling, a cropped nine-argument
  // draw, an asset <img>, an ImageBitmap and a data: URL image.
  domEval(runtime,
      "globalThis.__r = 'pending';"
      "var src = document.createElement('canvas'); src.width = 2; src.height = 1;"
      "var sx = src.getContext('2d'); sx.fillStyle = 'red'; sx.fillRect(0, 0, 1, 1); sx.fillStyle = 'blue'; sx.fillRect(1, 0, 1, 1);"
      "var dst = document.createElement('canvas'); dst.width = 16; dst.height = 16;"
      "var d = dst.getContext('2d'); d.imageSmoothingEnabled = false;"
      "d.drawImage(src, 0, 0, 4, 2); d.drawImage(src, 1, 0, 1, 1, 6, 0, 2, 2);"
      "function pd(a, b) { return Array.prototype.join.call(d.getImageData(a, b, 1, 1).data, ','); }"
      "var draws = [pd(0, 0), pd(1, 1), pd(2, 0), pd(3, 1), pd(6, 0), pd(7, 1)];"
      "var img = new Image();"
      "img.onload = function () {"
      "  d.drawImage(img, 0, 4); draws.push(pd(3, 5), img.naturalWidth);"
      "  createImageBitmap(dst).then(function (bm) {"
      "    draws.push(bm.width + 'x' + bm.height, Array.prototype.slice.call(bm.data, 24, 28).join(','));"
      "    var png = new Image();"
      "    png.onload = function () {"
      "      d.drawImage(png, 10, 10); draws.push('data url ' + png.naturalWidth + 'x' + png.naturalHeight + ' ' + pd(10, 10));"
      // The canvas as a WebGL texture, read back through a framebuffer.
      "      var t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);"
      "      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, dst);"
      "      var fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fb);"
      "      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);"
      "      var g = new Uint8Array(4); gl.readPixels(6, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, g); gl.bindFramebuffer(gl.FRAMEBUFFER, null);"
      "      draws.push('texture ' + Array.prototype.join.call(g, ','));"
      // Resizing resets the buffer; the unsupported half draws nothing and does not throw.
      "      dst.width = 16; draws.push('reset ' + pd(6, 0));"
      "      d.beginPath(); d.arc(4, 4, 3, 0, 7); d.fill(); draws.push('paths ' + pd(4, 4));"
      "      var threw; try { d.getImageData(0, 0, 0, 1); } catch (e) { threw = e.name; } draws.push(threw);"
      "      __r = draws.join(' | ');"
      "    };"
      "    png.onerror = function (e) { __r = 'data url error ' + (e.error && e.error.message); };"
      "    png.src = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGP4z8AAQv8BD/kD/YURmXYAAAAASUVORK5CYII=';"
      "  }, function (e) { __r = 'bitmap error ' + e; });"
      "};"
      "img.onerror = function (e) { __r = 'img error ' + (e.error && e.error.message); };"
      "img.src = 'img/sheet.png'; 'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"),
           std::string("255,0,0,255 | 255,0,0,255 | 0,0,255,255 | 0,0,255,255 | 0,0,255,255 | 0,0,255,255 | "
                       "190,120,7,255 | 4 | 16x16 | 0,0,255,255 | data url 2x1 255,0,0,255 | texture 0,0,255,255 | "
                       "reset 0,0,0,0 | paths 0,0,0,0 | IndexSizeError"));
  CHECK(capture.has(screenkit::LogLevel::Warn, "CanvasRenderingContext2D.arc draws nothing here"));
}

// Canvas text: the font shorthand, measureText with real metrics, fillText and
// strokeText drawing glyphs, alignment and baselines, a gradient's first colour,
// and FontFace loading -- from the installed fonts through local(), and failing
// cleanly for a URL that does not exist.
void domCanvasText() {
  test::LogCapture capture;
  auto runtime = domRuntime(64, 64);
  if (!runtime) return;
  if (!setTestAssetRoot(runtime)) return;

  CHECK_EQ(domEval(runtime,
      "var c = document.createElement('canvas'); c.width = 120; c.height = 40;"
      "var x = c.getContext('2d');"
      "var out = [x.font];"
      "x.font = 'italic bold 20pt \"Helvetica\", sans-serif'; out.push(x.font);"
      "x.font = 'bold 20px Arial'; x.font = 'nonsense'; out.push(x.font);"
      "var m = x.measureText('Hello');"
      "out.push(m instanceof TextMetrics, m.width > 30 && m.width < 80, m.actualBoundingBoxAscent > 10,"
      "         m.fontBoundingBoxAscent >= m.actualBoundingBoxAscent, m.actualBoundingBoxDescent >= 0);"
      "x.textBaseline = 'top'; out.push(Math.abs(x.measureText('Hello').fontBoundingBoxAscent) < 5);"
      "x.textBaseline = 'alphabetic'; x.textAlign = 'right';"
      "out.push(Math.round(x.measureText('Hello').actualBoundingBoxRight));"
      "x.textAlign = 'start';"
      "out.push('letterSpacing' in CanvasRenderingContext2D.prototype, x.measureText('').width);"
      "out.join(' | ');"),
      std::string("10px sans-serif | italic bold 26.666666666666664px Helvetica, sans-serif | bold 20px Arial | "
                  "true | true | true | true | true | true | 0 | false | 0"));

  // Drawing: red text sits on its baseline, centred text is centred, a stroke
  // and a gradient's first colour draw, and nothing lands above the ascent.
  CHECK_EQ(domEval(runtime,
      "function ink(ctx, x0, x1) { var d = ctx.getImageData(0, 0, 120, 40).data, r = { n: 0, left: 1e9, right: -1, top: 1e9, bottom: -1, rgb: '' };"
      "  for (var y = 0; y < 40; y++) for (var x = x0; x < x1; x++) { var i = (y * 120 + x) * 4; if (d[i + 3] === 0) continue;"
      "    r.n++; r.left = Math.min(r.left, x); r.right = Math.max(r.right, x); r.top = Math.min(r.top, y); r.bottom = Math.max(r.bottom, y);"
      "    if (d[i + 3] === 255) r.rgb = d[i] + ',' + d[i + 1] + ',' + d[i + 2]; }"
      "  return r; }"
      "x.font = '20px Helvetica'; x.fillStyle = '#ff0000'; x.fillText('Hi', 4, 30);"
      "var a = ink(x, 0, 40);"
      "var r = [a.rgb, a.n > 40, a.bottom === 29, a.top >= 10 && a.top <= 16, a.left >= 4 && a.left <= 7];"
      "x.clearRect(0, 0, 120, 40); x.textAlign = 'center'; x.fillText('Hi', 60, 30);"
      "var b = ink(x, 0, 120); r.push(Math.abs((b.left + b.right + 1) / 2 - 60) <= 2);"
      "x.clearRect(0, 0, 120, 40); x.textAlign = 'start'; x.strokeStyle = 'blue'; x.lineWidth = 2; x.strokeText('Hi', 4, 30);"
      "r.push(ink(x, 0, 40).rgb);"
      "x.clearRect(0, 0, 120, 40); var g = x.createLinearGradient(0, 0, 10, 0); g.addColorStop(1, 'lime'); g.addColorStop(0, 'yellow');"
      "x.fillStyle = g; x.fillText('Hi', 4, 30); r.push(ink(x, 0, 40).rgb);"
      "x.clearRect(0, 0, 120, 40); x.fillStyle = 'black'; x.save(); x.scale(2, 2); x.fillText('Hi', 2, 15); x.restore();"
      "var s = ink(x, 0, 120); r.push(s.bottom >= 29 && s.bottom <= 30, s.right - s.left > (a.right - a.left) * 1.6);"
      "r.join(' | ');"),
      std::string("255,0,0 | true | true | true | true | true | 0,0,255 | 255,255,0 | true | true"));

  // FontFace: an installed face under a new family name draws exactly like the
  // original; a missing URL fails with NetworkError; an unknown family falls
  // back to sans-serif rather than drawing nothing.
  domEval(runtime,
      "globalThis.__r = 'pending';"
      "var face = new FontFace('Brand', 'local(Helvetica-Bold)', { weight: 'bold' });"
      "var missing = new FontFace('Missing', 'url(/fonts/nope.ttf)');"
      "Promise.all([face.load(), missing.load().then(function () { return 'loaded'; }, function (e) { return e.name; })]).then(function (results) {"
      "  document.fonts.add(face);"
      "  x.font = 'bold 20px Brand'; var brand = x.measureText('Hello').width;"
      "  x.font = 'bold 20px Helvetica'; var helvetica = x.measureText('Hello').width;"
      "  x.font = '20px NoSuchFamily'; var fallback = x.measureText('Hello').width;"
      "  __r = [face.status, results[1], missing.status, brand === helvetica, fallback > 0, document.fonts.has(face)].join(' | ');"
      "}, function (e) { __r = 'error ' + e; }); 'started';");
  CHECK_EQ(pumpThenRead(runtime, "__r;"), std::string("loaded | NetworkError | error | true | true | true"));
}

// DOMParser for XML: a BMFont description as PixiJS and Phaser read it, text
// and CDATA, entities, namespaces, case-sensitive names, and a <parsererror>
// document for malformed input. HTML parsing stays absent.
void domXmlParser() {
  test::LogCapture capture;
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
      "var xml = '<?xml version=\"1.0\"?>\\n<!-- a bitmap font -->\\n<font>'"
      "  + '<info face=\"Lato-Black\" size=\"64\"/><common lineHeight=\"77\" scaleW=\"512\"/>'"
      "  + '<distanceField fieldType=\"msdf\" distanceRange=\"4\"/>'"
      "  + '<chars count=\"2\"><char id=\"65\" xoffset=\"-2\"/><char id=\"66\" xoffset=\"1\"/></chars>'"
      "  + '<note lang=\"en\">fish &amp; chips &#x263A; <![CDATA[<raw> & ok]]></note></font>';"
      "var doc = new DOMParser().parseFromString(xml, 'text/xml');"
      "var info = doc.getElementsByTagName('info')[0], chars = doc.getElementsByTagName('char');"
      "var df = doc.getElementsByTagName('distanceField')[0], note = doc.getElementsByTagName('note')[0];"
      "[doc instanceof XMLDocument, doc.documentElement.tagName, info.getAttribute('face'), doc.getElementsByTagName('common')[0].getAttribute('lineHeight'),"
      " doc.getElementsByTagName('common')[0].getAttribute('lineheight'), df.getAttribute('fieldType'), chars.length, chars[1].getAttribute('xoffset'),"
      " note.textContent, note.childNodes.length, note.firstChild.nodeType, doc.getElementsByTagName('parsererror').length,"
      " info.ownerDocument === doc, info.isConnected, doc.getElementsByTagName('DISTANCEFIELD').length].join('|');"),
      std::string("true|font|Lato-Black|77||msdf|2|1|fish & chips \u263A <raw> & ok|1|3|0|true|false|0"));

  // Namespaces: a default namespace, a prefix, and an unbound prefix as an error.
  CHECK_EQ(domEval(runtime,
      "var ns = new DOMParser().parseFromString('<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:x=\"urn:x\"><x:thing/><rect/></svg>', 'image/svg+xml');"
      "var thing = ns.documentElement.firstChild;"
      "[ns.documentElement.namespaceURI, thing.tagName, thing.localName, thing.prefix, thing.namespaceURI, ns.documentElement.lastChild.namespaceURI].join('|');"),
      std::string("http://www.w3.org/2000/svg|x:thing|thing|x|urn:x|http://www.w3.org/2000/svg"));

  // Malformed XML is a document, not a throw, with the reason inside.
  CHECK_EQ(domEval(runtime,
      "function bad(text) { var d = new DOMParser().parseFromString(text, 'application/xml');"
      "  return d.documentElement.localName + ':' + (d.getElementsByTagName('parsererror').length === 1); }"
      "[bad('<a><b></a>'), bad('<a>&nope;</a>'), bad('<a/><b/>'), bad('<a x=\"1\" x=\"2\"/>'), bad('<p:a/>'), bad('')].join('|');"),
      std::string("parsererror:true|parsererror:true|parsererror:true|parsererror:true|parsererror:true|parsererror:true"));

  // HTML parsing stays absent, and an unknown type is a TypeError, as in a browser.
  CHECK_EQ(domEval(runtime,
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.name; } }"
      "[attempt(function () { new DOMParser().parseFromString('<p>', 'text/html'); }),"
      " attempt(function () { new DOMParser().parseFromString('<p/>', 'text/plain'); })].join('|');"),
      std::string("NotSupportedError|TypeError"));

  // Text nodes in the page: textContent reads and replaces, createTextNode
  // appends, characterData records arrive, and text cannot go into a document.
  domEval(runtime,
      "globalThis.__records = [];"
      "var host = document.createElement('div'); document.body.appendChild(host);"
      "host.appendChild(document.createElement('span')); host.firstChild.textContent = 'one';"
      "host.appendChild(document.createTextNode(' two'));"
      "globalThis.__text = host.lastChild;"
      "new MutationObserver(function (records) { records.forEach(function (r) { __records.push(r.type + ':' + r.oldValue); }); })"
      "  .observe(host, { characterData: true, characterDataOldValue: true, subtree: true });"
      "__text.data = ' three';"
      "globalThis.__summary = [host.textContent, host.childNodes.length, host.children.length, __text.nodeName, __text.length,"
      " (function () { try { document.appendChild(document.createTextNode('x')); return 'no throw'; } catch (e) { return e.name; } })()];"
      "host.textContent = 'reset';"
      "__summary.push(host.childNodes.length, host.firstChild.nodeType, host.textContent, document.textContent);"
      "'ok';");
  CHECK_EQ(pumpThenRead(runtime, "__summary.join('|') + ' / ' + __records.join(',');"),
           std::string("one three|2|1|#text|6|HierarchyRequestError|1|3|reset| / characterData: two"));
}

// --- matrix rows: JSI object model --------------------------------------------
// The native half of a module: objects whose lifetime JS owns, emitters native
// code talks through, and modules that are not built until JS touches them. Each
// row drives it through `Runtime::registerModule`, the one door a host uses.

namespace jsi = facebook::jsi;

/// A native object whose destructor the test thread can count.
class ProbeObject final : public screenkit::SharedObject {
 public:
  explicit ProbeObject(std::shared_ptr<std::atomic<int>> destroyed)
      : destroyed_(std::move(destroyed)) {}
  ~ProbeObject() override { destroyed_->fetch_add(1); }
  std::string nativeTypeName() const override { return "ProbeObject"; }

 private:
  std::shared_ptr<std::atomic<int>> destroyed_;
};

struct ProbeCounters {
  std::shared_ptr<std::atomic<int>> built = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<int>> destroyed = std::make_shared<std::atomic<int>>(0);
  std::shared_ptr<std::atomic<int>> refDestroyed = std::make_shared<std::atomic<int>>(0);
};

/// `screenkit.modules.probe`: hands out native objects, forces a full collection,
/// reports the registry's size and emits from native code.
screenkit::ModuleFactory probeModule(const ProbeCounters& counters) {
  return [counters](jsi::Runtime& rt) -> jsi::Value {
    counters.built->fetch_add(1);
    jsi::Object module = screenkit::createNativeModule(rt, "probe");
    screenkit::jsiutils::defineMethod(
        rt, module, "make", 0,
        [counters](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
          return screenkit::wrapSharedObject(r, std::make_shared<ProbeObject>(counters.destroyed));
        });
    screenkit::jsiutils::defineMethod(
        rt, module, "makeRef", 0,
        [counters](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
          auto texture = std::shared_ptr<void>(new int(7), [destroyed = counters.refDestroyed](void* p) {
            delete static_cast<int*>(p);
            destroyed->fetch_add(1);
          });
          return screenkit::wrapSharedRef(
              r, std::make_shared<screenkit::SharedRef>(std::move(texture), "probe-texture"));
        });
    screenkit::jsiutils::defineMethod(
        rt, module, "collect", 0,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
          r.instrumentation().collectGarbage("object-model test");
          return jsi::Value::undefined();
        });
    screenkit::jsiutils::defineMethod(
        rt, module, "live", 0,
        [](jsi::Runtime& r, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
          auto registry = screenkit::sharedObjectRegistry(r);
          return jsi::Value(static_cast<double>(registry ? registry->size() : 0));
        });
    // Emits on the module itself, the way a player or a socket would report
    // something: through `emitEvent`, not through JS `emit`.
    screenkit::jsiutils::defineMethod(
        rt, module, "emitNative", 2,
        [](jsi::Runtime& r, const jsi::Value& self, const jsi::Value* args, size_t count) -> jsi::Value {
          jsi::Value target = screenkit::unwrapObjectIfNecessary(r, self);
          if (count < 1 || !args[0].isString() || !target.isObject()) return jsi::Value::undefined();
          screenkit::emitEvent(r, target.getObject(r), args[0].getString(r).utf8(r),
                               count > 1 ? args + 1 : nullptr, count > 1 ? count - 1 : 0);
          return jsi::Value::undefined();
        });
    return module;
  };
}

std::shared_ptr<screenkit::Runtime> probeRuntime(const ProbeCounters& counters) {
  auto runtime = screenkit::Runtime::create(testConfig());
  if (runtime && !runtime->registerModule("probe", probeModule(counters))) return nullptr;
  return runtime;
}

// A native object JS drops is destroyed by the collector exactly once; one JS
// still holds is not; teardown does not destroy anything a second time.
void objectModelGc() {
  test::LogCapture capture;
  ProbeCounters counters;
  auto runtime = probeRuntime(counters);
  CHECK(runtime != nullptr);
  if (!runtime) return;

  // Made and dropped inside a function, so no live register still points at it
  // when the collection runs. Hermes' collector finalizes concurrently, so one
  // forced collection is not guaranteed to have run every finalizer by the time
  // it returns; `settle` collects until the registry is down to `expected`, or
  // gives up after twenty collections and lets the check fail.
  const auto made = runtime->evaluateSource(
      "var probe = screenkit.modules.probe;"
      "function settle(expected) {"
      "  for (var i = 0; i < 20 && probe.live() !== expected; i++) probe.collect();"
      "  return probe.live();"
      "}"
      "(function () { var p = probe.make(); return [p.nativeId() > 0, probe.live(), p.nativeTypeName()].join(' '); })();",
      "gc-make.js");
  CHECK(made.ok);
  CHECK_EQ(made.value, std::string("true 1 ProbeObject"));
  CHECK_EQ(counters.destroyed->load(), 0);

  const auto collected = runtime->evaluateSource(
      "String(settle(0));", "gc-collect.js");
  CHECK(collected.ok);
  CHECK_EQ(collected.value, std::string("0"));
  CHECK_EQ(counters.destroyed->load(), 1);

  // A reference JS keeps is a reference the collector honours.
  const auto kept = runtime->evaluateSource(
      "globalThis.kept = probe.make(); for (var i = 0; i < 5; i++) probe.collect();"
      "[probe.live(), kept.nativeId() > 0].join(' ');",
      "gc-kept.js");
  CHECK(kept.ok);
  CHECK_EQ(kept.value, std::string("1 true"));
  CHECK_EQ(counters.destroyed->load(), 1);

  const auto dropped = runtime->evaluateSource(
      "kept = null; String(settle(0));", "gc-drop.js");
  CHECK(dropped.ok);
  CHECK_EQ(dropped.value, std::string("0"));
  CHECK_EQ(counters.destroyed->load(), 2);

  // Released explicitly, then dropped and collected: the collector's finalizer
  // must find nothing left to destroy.
  const auto released = runtime->evaluateSource(
      "(function () { var p = probe.make(); p.release(); })(); for (var i = 0; i < 5; i++) probe.collect(); String(probe.live());",
      "gc-released.js");
  CHECK(released.ok);
  CHECK_EQ(released.value, std::string("0"));
  CHECK_EQ(counters.destroyed->load(), 3);

  // One still alive at shutdown is destroyed by teardown, once.
  CHECK(runtime->evaluateSource("globalThis.survivor = probe.make(); 1;", "gc-survivor.js").ok);
  runtime->shutdown();
  runtime.reset();
  CHECK_EQ(counters.destroyed->load(), 4);
}

// `release()` destroys the native object now; every later use of the wrapper is
// a JS error naming the cause, never a native crash.
void objectModelRelease() {
  test::LogCapture capture;
  ProbeCounters counters;
  auto runtime = probeRuntime(counters);
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "var probe = screenkit.modules.probe;"
      "function attempt(f) { try { f(); return 'no throw'; } catch (e) { return e.message; } }"
      "var p = probe.make();"
      "var facts = [p instanceof screenkit.SharedObject, probe.live()];"
      "p.release();"
      "facts.push(probe.live(),"
      "  attempt(function () { p.nativeId(); }),"
      "  attempt(function () { p.release(); }),"
      "  attempt(function () { new screenkit.SharedObject(); }),"
      "  attempt(function () { screenkit.SharedObject.prototype.nativeId.call({}); }),"
      "  attempt(function () { screenkit.SharedObject.prototype.nativeId.call(42); }));"
      "facts.join(' | ');",
      "release.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  %s\n", result.error.c_str());
  const std::string released =
      "this SharedObject has been released; its native object is gone and the wrapper cannot be used again";
  CHECK_EQ(result.value, "true | 1 | 0 | " + released + " | " + released +
                             " | SharedObject is not constructible from JavaScript"
                             " | SharedObject method called on an object that is not one"
                             " | SharedObject method called on a non-object");
  // Destroyed at release, not at some later collection.
  CHECK_EQ(counters.destroyed->load(), 1);

  // A SharedRef is a SharedObject that names the reference it holds, and gives
  // that reference up the same way.
  const auto ref = runtime->evaluateSource(
      "var r = probe.makeRef();"
      "var refFacts = [r instanceof screenkit.SharedRef, r instanceof screenkit.SharedObject,"
      "  r.nativeRefType(), r.nativeTypeName()];"
      "r.release();"
      "refFacts.push(attempt(function () { r.nativeRefType(); }) === '" + released + "',"
      "  attempt(function () { new screenkit.SharedRef(); }));"
      "refFacts.join(' | ');",
      "ref.js");
  CHECK(ref.ok);
  if (!ref.ok) std::fprintf(stderr, "  %s\n", ref.error.c_str());
  CHECK_EQ(ref.value, std::string("true | true | probe-texture | probe-texture | true"
                                  " | SharedRef is not constructible from JavaScript"));
  CHECK_EQ(counters.refDestroyed->load(), 1);
}

// Three listeners, the middle one throws: the other two still run, `emit` does
// not throw, and the error is logged naming the listener.
void objectModelListenerIsolation() {
  test::LogCapture capture;
  ProbeCounters counters;
  auto runtime = probeRuntime(counters);
  CHECK(runtime != nullptr);
  if (!runtime) return;

  const auto result = runtime->evaluateSource(
      "var probe = screenkit.modules.probe;"
      "var seen = [];"
      "probe.addListener('tick', function (n) { seen.push('first:' + n); });"
      "probe.addListener('tick', function () { throw new Error('middle broke'); });"
      "var last = probe.addListener('tick', function (n) { seen.push('last:' + n); });"
      "var threw = 'no throw';"
      "try { probe.emit('tick', 1); probe.emitNative('tick', 2); } catch (e) { threw = e.message; }"
      "var count = probe.listenerCount('tick');"
      "last.remove();"
      "probe.emit('tick', 3);"
      // A listener that removes itself mid-emit does not disturb the others.
      "var emitter = new screenkit.EventEmitter();"
      "var order = [];"
      "var once = emitter.addListener('x', function () { order.push('once'); once.remove(); });"
      "emitter.addListener('x', function () { order.push('after'); });"
      "emitter.emit('x'); emitter.emit('x');"
      "function named() { order.push('named'); }"
      "emitter.addListener('y', named); emitter.removeListener('y', named); emitter.emit('y');"
      "emitter.removeAllListeners();"
      "[seen.join(','), threw, count, order.join(','), emitter.listenerCount('x'),"
      " probe instanceof Object, probe.name].join(' | ');",
      "listeners.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  %s\n", result.error.c_str());
  CHECK_EQ(result.value, std::string("first:1,last:1,first:2,last:2,first:3 | no throw | 3 | "
                                     "once,after,after | 0 | true | probe"));

  // One error line per throw, naming the listener and the event.
  std::size_t logged = 0;
  for (const auto& line : capture.lines()) {
    if (line.level == screenkit::LogLevel::Error &&
        line.message.find("listener 1 for \"tick\" threw: middle broke") != std::string::npos) {
      ++logged;
    }
  }
  CHECK_EQ(logged, std::size_t{3});
}

// A registered module is not built until JS touches it, and is built once.
void objectModelLazyModule() {
  test::LogCapture capture;
  ProbeCounters counters;
  auto runtime = probeRuntime(counters);
  CHECK(runtime != nullptr);
  if (!runtime) return;
  CHECK(runtime->registerModule("plain", nullptr));

  const auto untouched = runtime->evaluateSource(
      "[typeof screenkit.modules, 'probe' in screenkit.modules, typeof screenkit.modules.probe,"
      " typeof screenkit.NativeModule, typeof screenkit.EventEmitter].join(' ');",
      "lazy-untouched.js");
  CHECK(untouched.ok);
  CHECK_EQ(untouched.value, std::string("object true object function function"));
  CHECK_EQ(counters.built->load(), 0);

  const auto touched = runtime->evaluateSource(
      "[screenkit.modules.probe.name, typeof screenkit.modules.probe.addListener,"
      " screenkit.modules.probe.listenerCount('none'), screenkit.modules.plain.name,"
      " Object.keys(screenkit.modules.plain).join(',')].join(' ');",
      "lazy-touched.js");
  CHECK(touched.ok);
  if (!touched.ok) std::fprintf(stderr, "  %s\n", touched.error.c_str());
  CHECK_EQ(touched.value, std::string("probe function 0 plain name"));
  CHECK_EQ(counters.built->load(), 1);
}

// ============================================================================
// Media (spec-video-player.md): one row per line of the I/O matrix, through the
// element and through the Shaka API, against the media the fixture server
// generates with this machine's ffmpeg (runtime/tests/net/media.mjs) and serves
// on loopback. No row reaches the public internet.
// ============================================================================

/// The fixture's media root, from servers.json: null when the server was started
/// with --no-media, which fails the row rather than skipping it.
bool mediaFixtureReady() {
  std::ifstream in(netFixtureDir() + "/servers.json");
  std::stringstream text;
  text << in.rdbuf();
  const bool ready = text.str().find("\"media\": null") == std::string::npos &&
                     text.str().find("\"media\":") != std::string::npos;
  if (!ready) {
    test::fail(__FILE__, __LINE__,
               "the fixture server has no media: it generates it with ffmpeg at start (runtime/tests/net/media.mjs)");
  }
  return ready;
}

// What every media row is written with, on top of the net prelude: the media
// and licence URLs, and waiting on element events.
const char* kMediaPrelude = R"JS(
var MEDIA = H + '/media', LICENCE = H + '/licence';
var CLEARKEY_KID = 'a7e61c373e219033c21091fa607bf3b8', CLEARKEY_KEY = '76a6c65c5ea762046bd749a2e632ccbb';
// The next `type` event at `target`, or a rejection after `ms`.
function next(target, type, ms) {
  return new Promise(function (resolve, reject) {
    var timer = setTimeout(function () {
      target.removeEventListener(type, on);
      reject(new Error('no ' + type + ' event within ' + (ms || 20000) + ' ms'));
    }, ms || 20000);
    function on(e) { clearTimeout(timer); target.removeEventListener(type, on); resolve(e); }
    target.addEventListener(type, on);
  });
}
// Every event of `types` at `target`, in order, as `type` strings.
function record(target, types) {
  var seen = [];
  types.forEach(function (type) { target.addEventListener(type, function () { seen.push(type); }); });
  return seen;
}
'media prelude';
)JS";

#if defined(__linux__) && !defined(__ANDROID__)
/// Linux shows video on a Wayland subsurface of the app's window, so the rows
/// there need a window to hang it from: one SDL window, made once per process
/// and registered as the video host, as the windowed host registers its own
/// (host/Host.cpp). With no Wayland -- X11, KMS, or no display at all -- there is
/// none, and the player refuses every load (media-no-wayland).
void ensureMediaWindow() {
  static SDL_Window* window = nullptr;
  if (window != nullptr) return;
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  if (wayland == nullptr || *wayland == '\0') return;
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "  no SDL video for the media window: %s\n", SDL_GetError());
    return;
  }
  window = SDL_CreateWindow("screenkit media rows", 640, 360, 0);
  if (window == nullptr) {
    std::fprintf(stderr, "  SDL_CreateWindow for the media window failed: %s\n", SDL_GetError());
    return;
  }
  screenkit::media::VideoHost host;
  host.window = window;
  screenkit::media::setVideoHost(host);
  // The compositor maps the window once it has content and a round trip.
  for (int i = 0; i < 10; ++i) {
    SDL_PumpEvents();
    SDL_Delay(10);
  }
}
#endif

std::shared_ptr<screenkit::Runtime> mediaRuntime(const NetFixture& f) {
  if (!mediaFixtureReady()) return nullptr;
#if defined(__linux__) && !defined(__ANDROID__)
  ensureMediaWindow();
#endif
  auto runtime = netRuntime(f);
  if (!runtime) return nullptr;
  domEval(runtime, kMediaPrelude);
  return runtime;
}

// --- media: the seam, straight through __screenkit.media ----------------------------
// Below the element: one load of the two-variant HLS stream, as the platform
// player reports it -- metadata, tracks, readiness, time advancing once played,
// frames decoded -- and a clean release.
void mediaSeam() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = mediaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var api = __screenkit.media;
  var caps = api.capabilities();
  log('available=' + caps.available + ' hls=' + caps.hls);
  var target = { last: {}, types: [] };
  var previous = api.onevent;
  api.onevent = function (t, type, payload) {
    if (t !== target) return previous && previous.apply(this, arguments);
    t.types.push(type);
    t.last[type] = payload;
  };
  var id = api.create(target);
  var serial = api.load(id, { url: MEDIA + '/vod/master.m3u8' });
  await until(function () { return target.last.state && target.last.state.state === 'ready'; }, 30000);
  var m = target.last.metadata;
  log('metadata serial=' + (m.serial === serial) + ' duration=' + Math.round(m.duration) + ' live=' + m.live +
      ' manifest=' + m.manifest + ' video=' + m.hasVideo + ' audio=' + m.hasAudio);
  await until(function () { return target.last.tracks; }, 10000);
  log('variants=' + target.last.tracks.variants.length + ' text=' + target.last.tracks.text.length);
  api.play(id);
  await until(function () { return target.last.time && target.last.time.position > 1; }, 30000);
  await until(function () { return target.last.stats && target.last.stats.decodedFrames > 0; }, 10000);
  log('played: time advanced, frames decoded');
  api.pause(id);
  api.destroy(id);
  api.onevent = previous;
  log('destroyed');
});
)JS");
  // libvlc lists no variants: Linux reports the one playing (a recorded divergence).
  checkTranscript(got, std::string(
                  "available=true hls=true\n"
                  "metadata serial=true duration=10 live=false manifest=hls video=true audio=true\n") +
                  NET_PER_PLATFORM("variants=2 text=1\n", "variants=2 text=1\n", "variants=1 text=1\n") +
                  "played: time advanced, frames decoded\n"
                  "destroyed");
  CHECK(pumpUntilIdle(runtime));
}


/// Evaluate @screenkit/shaka -- the Shaka-shaped player -- as the build compiled
/// it (runtime/CMakeLists.txt, tests/ShakaScript.cmake), unless the environment
/// names the copy a device run pushed beside the binary.
bool installShaka(const std::shared_ptr<screenkit::Runtime>& runtime) {
  const char* override_ = std::getenv("SCREENKIT_SHAKA_HBC");
  const std::string path = override_ != nullptr && *override_ != '\0' ? override_ : SCREENKIT_SHAKA_HBC;
  const screenkit::EvalResult result = runtime->evaluateBundle(path);
  CHECK(result.ok);
  if (!result.ok) {
    std::fprintf(stderr, "  @screenkit/shaka failed: %s\n", result.error.c_str());
    return false;
  }
  CHECK_EQ(result.value, std::string("screenkit-shaka"));
  return true;
}

// Helpers every element and Shaka row is written with.
const char* kMediaRowPrelude = R"JS(
var RT = shaka.net.NetworkingEngine.RequestType;
function video(css) {
  var v = document.createElement('video');
  v.style.cssText = css || 'position: absolute; left: 0; top: 0; width: 64px; height: 36px; z-index: -1';
  document.body.appendChild(v);
  return v;
}
function player(v) {
  var p = new shaka.Player();
  return p.attach(v).then(function () { return p; });
}
// A shaka.util.Error as category/code/severity.
function code(e) {
  return e && e.code !== undefined && e.category !== undefined ? e.category + '/' + e.code + '/' + e.severity
                                                                 : 'not a Shaka error: ' + e;
}
function outcome(promise) {
  return promise.then(function () { return 'resolved'; }, function (e) { return 'rejected ' + code(e); });
}
function advanced(v, seconds, ms) {
  var from = v.currentTime;
  return until(function () { return v.currentTime > from + seconds; }, ms || 30000);
}
function once(target, type, ms) { return next(target, type, ms); }
// `promise`, or null after `ms` -- with the timer cleared either way, so a row
// that finishes leaves nothing scheduled behind it.
function within(promise, ms) {
  var timer;
  return Promise.race([promise, new Promise(function (resolve) { timer = setTimeout(function () { resolve(null); }, ms); })])
    .then(function (value) { clearTimeout(timer); return value; });
}
'media row prelude';
)JS";

std::shared_ptr<screenkit::Runtime> shakaRuntime(const NetFixture& f) {
  auto runtime = mediaRuntime(f);
  if (!runtime) return nullptr;
  if (!installShaka(runtime)) return nullptr;
  domEval(runtime, kMediaRowPrelude);
  return runtime;
}

// --- media: load and play through the element --------------------------------------
// `src` on a <video>, then play(): HTMLMediaElement's events in the order a
// browser fires them, readyState climbing, time advancing, and canPlayType
// answering for what this platform's player plays.
void mediaElement() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var probe = document.createElement('video');
  log('canPlayType mp4=' + probe.canPlayType('video/mp4') + ' hls=' + probe.canPlayType('application/vnd.apple.mpegurl') +
      ' dash=' + probe.canPlayType('application/dash+xml') + ' avc=' + probe.canPlayType('video/mp4; codecs="avc1.4d401e, mp4a.40.2"') +
      ' audio=' + JSON.stringify(document.createElement('audio').canPlayType('audio/mp4')));
  var v = video();
  var seen = record(v, ['loadstart', 'durationchange', 'loadedmetadata', 'loadeddata', 'canplay', 'canplaythrough',
                        'play', 'playing', 'pause', 'error']);
  log('empty: networkState=' + v.networkState + ' readyState=' + v.readyState + ' paused=' + v.paused +
      ' duration=' + v.duration);
  v.src = MEDIA + '/vod/nosubs.m3u8';
  log('src: ' + (v.src === MEDIA + '/vod/nosubs.m3u8') + ' networkState=' + v.networkState);
  await once(v, 'canplaythrough', 30000);
  log('ready: ' + seen.join(',') + ' readyState=' + v.readyState + ' duration=' + Math.round(v.duration) +
      ' currentSrc=' + (v.currentSrc === v.src));
  seen.length = 0;
  var updates = 0;
  v.addEventListener('timeupdate', function () { updates++; });
  await v.play();
  await advanced(v, 1);
  log('playing: ' + seen.join(',') + ' paused=' + v.paused + ' timeupdates=' + (updates >= 3) +
      ' buffered=' + (v.buffered.length > 0 && v.buffered.end(0) > v.currentTime) +
      ' seekable=' + v.seekable.length + ':' + v.seekable.start(0) + '-' + Math.round(v.seekable.end(0)) +
      ' size=' + (v.videoWidth > 0 && v.videoHeight > 0) + ' frames=' + (v.getVideoPlaybackQuality().totalVideoFrames > 0));
  seen.length = 0;
  v.pause();
  await once(v, 'pause');
  var at = v.currentTime;
  await sleep(600);
  log('paused: ' + seen.join(',') + ' held=' + (Math.abs(v.currentTime - at) < 0.3) + ' played=' + (v.played.length > 0));
  v.removeAttribute('src');
  v.load();
  await sleep(50);
  log('emptied: networkState=' + v.networkState + ' readyState=' + v.readyState + ' currentTime=' + v.currentTime);
});
)JS");
  checkTranscript(got,
      std::string(NET_PER_PLATFORM(
          "canPlayType mp4=maybe hls=maybe dash= avc=probably audio=\"\"\n",
          "canPlayType mp4=maybe hls=maybe dash=maybe avc=probably audio=\"\"\n",
          "canPlayType mp4=maybe hls=maybe dash=maybe avc=probably audio=\"\"\n")) +
      "empty: networkState=0 readyState=0 paused=true duration=NaN\n"
      "src: true networkState=2\n"
      "ready: loadstart,durationchange,loadedmetadata,loadeddata,canplay,canplaythrough readyState=4 duration=10 currentSrc=true\n"
      "playing: play,playing paused=false timeupdates=true buffered=true seekable=1:0-10 size=true frames=true\n"
      "paused: pause held=true played=true\n"
      "emptied: networkState=0 readyState=0 currentTime=0");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: load and play HLS through Shaka -------------------------------------------
// The Blits PlayerManager's calls: attach, load, play. load() resolves once
// loadedmetadata has fired; the two variants are listed; after a second of
// playback frames have been decoded (the acceptance criterion's stat).
void mediaHls() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  shaka.polyfill.installAll();
  log('supported ' + shaka.Player.isBrowserSupported());
  var v = video();
  var p = await player(v);
  var events = [];
  ['loading', 'manifestparsed', 'streaming', 'trackschanged', 'loaded'].forEach(function (type) {
    p.addEventListener(type, function () { if (events.indexOf(type) < 0) events.push(type); });
  });
  var metadataFirst = null;
  v.addEventListener('loadedmetadata', function () { if (metadataFirst === null) metadataFirst = true; });
  await p.load(MEDIA + '/vod/master.m3u8');
  if (metadataFirst === null) metadataFirst = false;
  log('loaded: ' + events.join(',') + ' after loadedmetadata=' + metadataFirst + ' mode=' + p.getLoadMode() +
      ' type=' + p.getManifestType() + ' uri=' + (p.getAssetUri() === MEDIA + '/vod/master.m3u8'));
  var variants = p.getVariantTracks();
  // Linux lists only the variant VLC is playing, whichever rung that is.
  var heights = variants.map(function (t) { return t.height; }).sort().join('/');
  if (variants.length === 1 && /^(180|360)$/.test(heights)) heights = 'a rung';
  log('variants=' + variants.length + ' heights=' + heights +
      ' active=' + variants.filter(function (t) { return t.active; }).length + ' bandwidth=' + (variants[0].bandwidth > 0) +
      ' text=' + p.getTextTracks().length + ' live=' + p.isLive() + ' range=' + JSON.stringify(p.seekRange()).replace(/\.\d+/g, ''));
  await v.play();
  await advanced(v, 1.2);
  var s = p.getStats();
  log('stats: decoded=' + (s.decodedFrames > 0) + ' dropped=' + (s.droppedFrames >= 0) + ' size=' + (s.width > 0 && s.height > 0) +
      ' streamBandwidth=' + (s.streamBandwidth > 0) + ' loadLatency=' + (s.loadLatency > 0) + ' playTime=' + (s.playTime > 0) +
      ' state=' + s.stateHistory[s.stateHistory.length - 1].state + ' buffering=' + p.isBuffering());
  await p.destroy();
  log('destroyed: mode=' + p.getLoadMode() + ' networkState=' + v.networkState + ' readyState=' + v.readyState);
  log('after destroy: ' + await outcome(p.load(MEDIA + '/vod/master.m3u8')));
});
)JS");
  checkTranscript(got,
                  "supported true\n"
                  "loaded: loading,manifestparsed,streaming,trackschanged,loaded after loadedmetadata=true mode=3 type=HLS uri=true\n" +
                  std::string(NET_PER_PLATFORM("variants=2 heights=180/360", "variants=2 heights=180/360",
                                               "variants=1 heights=a rung")) +
                  " active=1 bandwidth=true text=1 live=false range={\"start\":0,\"end\":10}\n"
                  "stats: decoded=true dropped=true size=true streamBandwidth=true loadLatency=true playTime=true state=playing buffering=false\n"
                  "destroyed: mode=0 networkState=0 readyState=0\n"
                  "after destroy: rejected 7/7003/2");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: progressive MP4 -------------------------------------------------------------
void mediaMp4() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  await p.load(MEDIA + '/mp4/progressive.mp4');
  var variants = p.getVariantTracks();
  log('live=' + p.isLive() + ' variants=' + variants.length + ' height=' + variants[0].height +
      ' duration=' + Math.round(v.duration) + ' type=' + p.getManifestType());
  await v.play();
  await advanced(v, 1.2);
  log('played: decoded=' + (p.getStats().decodedFrames > 0) + ' size=' + v.videoWidth + 'x' + v.videoHeight);
  await p.destroy();
});
)JS");
  checkTranscript(got,
                  "live=false variants=1 height=360 duration=10 type=null\n"
                  "played: decoded=true size=640x360");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: DASH ---------------------------------------------------------------------------
// Plays on Android and Linux. Apple's player has no DASH: a recorded divergence,
// rejected as Shaka rejects a manifest it cannot place (4000), CRITICAL.
void mediaDash() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var errors = [];
  p.addEventListener('error', function (e) { errors.push(code(e.detail)); });
  try {
    await p.load(MEDIA + '/dash/manifest.mpd');
    log('loaded: type=' + p.getManifestType() + ' variants=' + p.getVariantTracks().length + ' live=' + p.isLive() +
        ' duration=' + Math.round(v.duration));
    await v.play();
    await advanced(v, 1.2);
    log('played: decoded=' + (p.getStats().decodedFrames > 0));
  } catch (e) {
    log('rejected ' + code(e) + ' error events ' + errors.join(','));
  }
  await p.destroy();
});
)JS");
  checkTranscript(got, NET_PER_PLATFORM(
      "rejected 4/4000/2 error events 4/4000/2",
      "loaded: type=DASH variants=2 live=false duration=10\nplayed: decoded=true",
      "loaded: type=DASH variants=1 live=false duration=10\nplayed: decoded=true"));
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: live HLS -----------------------------------------------------------------------
// A sliding-window playlist: isLive, an infinite duration, and a seek range
// that moves forward as the window does.
void mediaLive() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  await p.load(MEDIA + '/live/live.m3u8');
  var first = p.seekRange();
  log('live=' + p.isLive() + ' duration=' + v.duration + ' window=' + (first.end > first.start) +
      ' seekable=' + (v.seekable.length === 1 && v.seekable.end(0) === first.end));
  await v.play();
  await until(function () { return p.seekRange().end >= first.end + 3; }, 30000);
  var later = p.seekRange();
  log('seekRange advanced: end=' + (later.end > first.end) + ' start=' + (later.start > first.start) +
      ' playing inside it=' + (v.currentTime >= later.start - 2 && v.currentTime <= later.end + 0.5) +
      ' decoded=' + (p.getStats().decodedFrames > 0));
  await p.destroy();
});
)JS", 90000);
  checkTranscript(got,
                  "live=true duration=Infinity window=true seekable=true\n"
                  "seekRange advanced: end=true start=true playing inside it=true decoded=true");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: seek -----------------------------------------------------------------------------
// currentTime = t: `seeking`, then `seeked` at t; a time past the end is clamped
// to the seekable range, as it is before the element reports it.
void mediaSeek() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  v.src = MEDIA + '/vod/nosubs.m3u8';
  await once(v, 'canplaythrough', 30000);
  var seen = record(v, ['seeking', 'seeked']);
  v.currentTime = 6;
  log('set: seeking=' + v.seeking + ' currentTime=' + v.currentTime);
  await once(v, 'seeked', 20000);
  log('seeked: ' + seen.join(',') + ' seeking=' + v.seeking + ' at=' + v.currentTime.toFixed(1));
  seen.length = 0;
  v.currentTime = 100;
  log('clamped on set: ' + (v.currentTime === v.duration));
  await once(v, 'seeked', 20000);
  log('clamped: ' + seen.join(',') + ' at end=' + (Math.abs(v.currentTime - v.duration) < 0.5));
  seen.length = 0;
  v.currentTime = -5;
  await once(v, 'seeked', 20000);
  log('clamped low: at=' + v.currentTime.toFixed(1));
  var p = await player(video());
  await p.load(MEDIA + '/vod/master.m3u8', 4);
  log('start time: at=' + p.getMediaElement().currentTime.toFixed(0) + ' range=' + JSON.stringify(p.seekRange()).replace(/\.\d+/g, ''));
  await p.destroy();
  v.removeAttribute('src');
  v.load();
});
)JS");
  checkTranscript(got,
                  "set: seeking=true currentTime=6\n"
                  "seeked: seeking,seeked seeking=false at=6.0\n"
                  "clamped on set: true\n"
                  "clamped: seeking,seeked at end=true\n"
                  "clamped low: at=0.0\n"
                  "start time: at=4 range={\"start\":0,\"end\":10}");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: end of stream ----------------------------------------------------------------------
void mediaEnded() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var completes = 0;
  p.addEventListener('complete', function () { completes++; });
  await p.load(MEDIA + '/mp4/progressive.mp4');
  v.currentTime = 8.5;
  await once(v, 'seeked', 20000);
  var seen = record(v, ['pause', 'ended']);
  await v.play();
  await once(v, 'ended', 20000);
  log('ended=' + v.ended + ' paused=' + v.paused + ' events=' + seen.join(',') + ' at end=' +
      (Math.abs(v.currentTime - v.duration) < 0.5) + ' complete=' + completes + ' isEnded=' + p.isEnded());
  seen.length = 0;
  await v.play();
  log('play again from the start: ended=' + v.ended + ' near 0=' + (v.currentTime < 1));
  await p.destroy();
});
)JS");
  checkTranscript(got,
                  "ended=true paused=true events=pause,ended at end=true complete=1 isEnded=true\n"
                  "play again from the start: ended=false near 0=true");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: variant, audio and text selection -------------------------------------------------------
// selectVariantTrack -> variantchanged and the other variant active;
// selectAudioLanguage -> variantchanged; selectTextTrack -> textchanged, the
// TextTrack showing and its activeCues (VTTCue) updating with cuechange.
// Linux's recorded divergences: libvlc lists only the variant it plays, so the
// one listed is selected again and answered as such; and VLC draws the selected
// text into the picture, so no cue ever reaches the page.
void mediaTracks() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var cues = __screenkit.media.capabilities().platform !== 'linux';
  var v = video();
  var p = await player(v);
  p.configure({ abr: { enabled: false } });
  await p.load(MEDIA + '/vod/master.m3u8');
  await v.play();
  await advanced(v, 0.5);
  var variants = p.getVariantTracks();
  var active = variants.filter(function (t) { return t.active; })[0];
  var other = variants.filter(function (t) { return !t.active; })[0];
  log('variants=' + variants.length + ' one active=' + (!!active && !!other));
  var changed = new Promise(function (resolve) { p.addEventListener('variantchanged', resolve); });
  if (other) {
    p.selectVariantTrack(other, true);
    var e = await within(changed, 20000);
    await until(function () {
      var now = p.getVariantTracks().filter(function (t) { return t.active; })[0];
      return now && now.id === other.id;
    }, 20000);
    log('variantchanged: new=' + !!(e && e.newTrack && e.newTrack.id === other.id) + ' old=' +
        !!(e && e.oldTrack && e.oldTrack.id === active.id) + ' switched=true');
  } else {
    p.selectVariantTrack(active, true);
    var same = await within(changed, 20000);
    log('the one variant selected again: variantchanged=' + !!(same && same.newTrack && same.newTrack.id === active.id));
  }

  var audioChanged = new Promise(function (resolve) { p.addEventListener('variantchanged', resolve, { once: true }); });
  p.selectAudioLanguage(p.getAudioLanguages()[0]);
  await audioChanged;
  log('audio language selected: variantchanged');

  var text = p.getTextTracks();
  log('text tracks=' + text.length + ' language=' + text[0].language + ' kind=' + text[0].kind +
      ' element tracks=' + v.textTracks.length + ' mode=' + v.textTracks[0].mode);
  var textChanged = new Promise(function (resolve) { p.addEventListener('textchanged', resolve, { once: true }); });
  p.selectTextTrack(text[0]);
  await textChanged;
  var track = v.textTracks[0];
  log('textchanged: mode=' + track.mode + ' visible=' + p.isTextTrackVisible() + ' active=' + p.getTextTracks()[0].active);
  var changes = 0;
  track.addEventListener('cuechange', function () { changes++; });
  if (cues) {
    await until(function () { return track.activeCues && track.activeCues.length > 0; }, 20000);
    var cue = track.activeCues[0];
    log('cue: ' + /^Cue \d+$/.test(cue.text) + ' VTTCue=' + (cue instanceof VTTCue) + ' in cues=' +
        (Array.prototype.indexOf.call(track.cues, cue) >= 0) + ' track=' + (cue.track === track));
    var first = cue.text;
    await until(function () { return track.activeCues.length > 0 && track.activeCues[0].text !== first; }, 20000);
    log('next cue: cuechange=' + (changes > 0) + ' ' + /^Cue \d+$/.test(track.activeCues[0].text));
  } else {
    // Two cues' worth of playback (they change every 2 s): drawn by VLC, never reported.
    await advanced(v, 4.5);
    log('cues drawn by the player: cuechange=' + changes + ' active cues=' + track.activeCues.length);
  }
  var hidden = new Promise(function (resolve) { p.addEventListener('texttrackvisibility', resolve, { once: true }); });
  p.setTextTrackVisibility(false);
  await hidden;
  log('hidden: mode=' + track.mode + ' visible=' + p.isTextTrackVisible());
  var off = new Promise(function (resolve) { p.addEventListener('textchanged', resolve, { once: true }); });
  p.selectTextTrack(null);
  await off;
  log('off: mode=' + track.mode + ' cues=' + track.cues + ' active=' + p.getTextTracks()[0].active);
  await p.destroy();
  log('destroyed: element tracks=' + v.textTracks.length);
});
)JS", 90000);
  const std::string variantsAndCues =
      "variants=2 one active=true\n"
      "variantchanged: new=true old=true switched=true\n"
      "audio language selected: variantchanged\n"
      "text tracks=1 language=en kind=subtitle element tracks=1 mode=disabled\n"
      "textchanged: mode=showing visible=true active=true\n"
      "cue: true VTTCue=true in cues=true track=true\n"
      "next cue: cuechange=true true\n";
  const std::string linux =
      "variants=1 one active=false\n"
      "the one variant selected again: variantchanged=true\n"
      "audio language selected: variantchanged\n"
      "text tracks=1 language=en kind=subtitle element tracks=1 mode=disabled\n"
      "textchanged: mode=showing visible=true active=true\n"
      "cues drawn by the player: cuechange=0 active cues=0\n";
  checkTranscript(got, NET_PER_PLATFORM(variantsAndCues, variantsAndCues, linux) +
                  "hidden: mode=hidden visible=false\n"
                  "off: mode=disabled cues=null active=false\n"
                  "destroyed: element tracks=0");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: ClearKey ---------------------------------------------------------------------------
// drm.clearKeys and a CENC stream: plays on Android. Apple's player has no
// ClearKey, and libvlc 3 takes no CENC keys: 6001 on both, recorded divergences.
void mediaClearKey() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var keys = {};
  keys[CLEARKEY_KID] = CLEARKEY_KEY;
  p.configure({ drm: { clearKeys: keys } });
  try {
    await p.load(MEDIA + '/cenc/clearkey.mpd');
    log('loaded: keySystem=' + p.keySystem() + ' drmInfo=' + (p.drmInfo() && p.drmInfo().keySystem));
    await v.play();
    await advanced(v, 1.2);
    log('played: decoded=' + (p.getStats().decodedFrames > 0) + ' error=' + v.error);
  } catch (e) {
    log('rejected ' + code(e));
  }
  await p.destroy();
});
)JS");
  checkTranscript(got, NET_PER_PLATFORM(
      "rejected 6/6001/2",
      "loaded: keySystem=org.w3.clearkey drmInfo=org.w3.clearkey\nplayed: decoded=true error=null",
      "rejected 6/6001/2"));
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: Widevine / FairPlay licences ------------------------------------------------------------
// A key system the platform lacks is 6001. A licence request goes out through
// the player's networking engine -- request filters applied, response filters
// run -- to the configured server (the fixture's fake one, which records what
// reached it), and its answer goes back to the key system; a licence server
// that fails is 6007. The fake server's answer is no real licence, so where the
// platform's key system gets that far it refuses it.
void mediaLicence() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var platform = __screenkit.media.capabilities().platform;
  var v = video();
  var p = await player(v);
  var seen = { licence: 0, certificate: 0, responses: 0 };
  p.getNetworkingEngine().registerRequestFilter(function (type, request) {
    if (type === RT.LICENSE) {
      request.headers['x-screenkit-filter'] = 'licence';
      seen.licence++;
    }
    if (type === RT.SERVER_CERTIFICATE) seen.certificate++;
  });
  p.getNetworkingEngine().registerResponseFilter(function (type) { if (type === RT.LICENSE) seen.responses++; });
  var errors = [];
  p.addEventListener('error', function (e) { errors.push(code(e.detail)); });

  // A key system this platform does not have.
  var missing = platform === 'android' ? 'com.apple.fps' : 'com.widevine.alpha';
  var servers = {};
  servers[missing] = LICENCE + '/ok?id=missing';
  p.configure({ drm: { servers: servers } });
  log('unsupported key system: ' + await outcome(p.load(MEDIA + (platform === 'android' ? '/cenc/widevine.mpd'
                                                                                          : '/vod/master.m3u8'))));
  p.resetConfiguration();

  async function exchange(keySystem, manifest, id, extra) {
    var config = { drm: { servers: {}, advanced: {} } };
    config.drm.servers[keySystem] = LICENCE + (id.indexOf('fail') === 0 ? '/fail' : '/ok') + '?id=' + id;
    config.drm.retryParameters = { maxAttempts: 1, baseDelay: 10, backoffFactor: 1, fuzzFactor: 0, timeout: 10000 };
    if (extra) config.drm.advanced[keySystem] = extra;
    p.resetConfiguration();
    p.configure(config);
    var result = await outcome(p.load(MEDIA + manifest));
    var received = await (await fetch(LICENCE + '/log?id=' + id)).json();
    return { result: result, received: received };
  }

  if (platform === 'android') {
    var ok = await exchange('com.widevine.alpha', '/cenc/widevine.mpd', 'wv-ok');
    var r = ok.received[0] || {};
    log('widevine: reached=' + ok.received.length + ' filtered=' + (r.headers && r.headers['x-screenkit-filter']) +
        ' challenge=' + (r.length > 0) + ' method=' + r.method + ' responses filtered=' + seen.responses +
        ' then ' + ok.result);
    var failed = await exchange('com.widevine.alpha', '/cenc/widevine.mpd', 'fail-wv');
    log('licence server fails: reached=' + failed.received.length + ' ' + failed.result);
  } else if (platform === 'apple') {
    var fairplay = await exchange('com.apple.fps', '/fairplay/master.m3u8', 'fp-ok',
                                  { serverCertificateUri: MEDIA + '/fairplay/cert.der' });
    log('fairplay: certificate fetched=' + (seen.certificate > 0) + ' licence requests=' + fairplay.received.length +
        ' then ' + fairplay.result);
    var noCertificate = await exchange('com.apple.fps', '/fairplay/master.m3u8', 'fp-nocert',
                                       { serverCertificateUri: MEDIA + '/fairplay/missing.der' });
    log('certificate fetch fails: ' + noCertificate.result + ' licence requests=' + noCertificate.received.length);
  } else {
    var fps = {};
    fps['com.apple.fps'] = LICENCE + '/ok?id=fps';
    p.resetConfiguration();
    p.configure({ drm: { servers: fps } });
    log('fairplay: ' + await outcome(p.load(MEDIA + '/vod/master.m3u8')));
  }
  log('error events: ' + errors.filter(function (e, i) { return errors.indexOf(e) === i; }).join(','));
  await p.destroy();
});
)JS", 90000);
  checkTranscript(got, NET_PER_PLATFORM(
      "unsupported key system: rejected 6/6001/2\n"
      "fairplay: certificate fetched=true licence requests=0 then rejected 6/6007/2\n"
      "certificate fetch fails: rejected 6/6007/2 licence requests=0\n"
      "error events: 6/6001/2,6/6007/2",
      "unsupported key system: rejected 6/6001/2\n"
      "widevine: reached=1 filtered=licence challenge=true method=POST responses filtered=1 then rejected 6/6007/2\n"
      "licence server fails: reached=1 rejected 6/6007/2\n"
      "error events: 6/6001/2,6/6007/2",
      "unsupported key system: rejected 6/6001/2\n"
      "fairplay: rejected 6/6001/2\n"
      "error events: 6/6001/2"));
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: bad URL ------------------------------------------------------------------------------
// A manifest that 404s: load rejects with 1001 BAD_HTTP_STATUS, CRITICAL; the
// player fires `error`; the element's error is MEDIA_ERR_NETWORK; and the
// player plays the next load.
void mediaBadUrl() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var errors = [];
  p.addEventListener('error', function (e) { errors.push(code(e.detail)); });
  var elementErrors = 0;
  v.addEventListener('error', function () { elementErrors++; });
  try {
    await p.load(MEDIA + '/vod/missing.m3u8');
    log('resolved');
  } catch (e) {
    log('rejected ' + code(e) + ' status=' + e.data[1] + ' uri=' + (e.data[0] === MEDIA + '/vod/missing.m3u8'));
  }
  log('error event: ' + errors.join(',') + ' video.error=' + (v.error && v.error.code) + ' (MEDIA_ERR_NETWORK=' +
      MediaError.MEDIA_ERR_NETWORK + ') element error events=' + elementErrors + ' networkState=' + v.networkState);
  await p.load(MEDIA + '/mp4/progressive.mp4');
  await v.play();
  await advanced(v, 0.5);
  log('usable again: error=' + v.error + ' playing=' + !v.paused);
  var e2 = await outcome(p.load('http://127.0.0.1:' + __net.closedPort + '/x.m3u8'));
  log('refused connection: ' + e2);
  await p.destroy();
});
)JS");
  checkTranscript(got,
                  "rejected 1/1001/2 status=404 uri=true\n"
                  "error event: 1/1001/2 video.error=2 (MEDIA_ERR_NETWORK=2) element error events=1 networkState=3\n"
                  "usable again: error=null playing=true\n"
                  "refused connection: rejected 1/1002/2");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: unplayable media ------------------------------------------------------------------------
void mediaUnplayable() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var errors = [];
  p.addEventListener('error', function (e) { errors.push(code(e.detail)); });
  try {
    await p.load(MEDIA + '/corrupt/corrupt.mp4');
    log('resolved');
  } catch (e) {
    log('rejected category=' + e.category + ' severity=' + e.severity + ' code=' + e.code);
  }
  log('error event: ' + errors.length + ' video.error=' + !!v.error);
  await p.load(MEDIA + '/mp4/progressive.mp4');
  log('usable again: ' + (v.readyState >= 1) + ' error=' + v.error);
  await p.destroy();
});
)JS");
  checkTranscript(got,
                  "rejected category=3 severity=2 code=3016\n"
                  "error event: 1 video.error=true\n"
                  "usable again: true error=null");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: interrupted load ----------------------------------------------------------------------------
// load(a) then load(b), unload() or destroy() before a resolves: a rejects with
// 7000 LOAD_INTERRUPTED and the second proceeds. The element's own src does the
// same by HTML's rules: abort and emptied for the first.
void mediaInterrupted() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  var p = await player(v);
  var errors = [];
  p.addEventListener('error', function (e) { errors.push(code(e.detail)); });
  var a = outcome(p.load(MEDIA + '/vod/master.m3u8'));
  var b = outcome(p.load(MEDIA + '/mp4/progressive.mp4'));
  log('load, load: ' + await a + ' / ' + await b + ' playing b=' + (p.getAssetUri() === MEDIA + '/mp4/progressive.mp4'));
  var c = outcome(p.load(MEDIA + '/vod/master.m3u8'));
  var unloaded = p.unload();
  log('load, unload: ' + await c + ' mode=' + (await unloaded, p.getLoadMode()));
  var d = outcome(p.load(MEDIA + '/vod/master.m3u8'));
  var destroyed = p.destroy();
  log('load, destroy: ' + await d + ' mode=' + (await destroyed, p.getLoadMode()));
  log('no error events: ' + (errors.length === 0));
  var w = video();
  var seen = record(w, ['loadstart', 'abort', 'emptied', 'loadedmetadata']);
  w.src = MEDIA + '/vod/master.m3u8';
  await sleep(0);
  w.src = MEDIA + '/mp4/progressive.mp4';
  await once(w, 'loadedmetadata', 30000);
  log('element src, src: ' + seen.join(',') + ' duration=' + Math.round(w.duration));
  w.removeAttribute('src');
  w.load();
});
)JS");
  checkTranscript(got,
                  "load, load: rejected 7/7000/2 / resolved playing b=true\n"
                  "load, unload: rejected 7/7000/2 mode=1\n"
                  "load, destroy: rejected 7/7000/2 mode=0\n"
                  "no error events: true\n"
                  "element src, src: loadstart,abort,emptied,loadstart,loadedmetadata duration=10");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: the element leaves the document -----------------------------------------------------------
// The plane follows the element's CSS rect; removing the element pauses it and
// hides its plane, and putting it back shows the plane again, still paused.
// The planes are read off __screenkit.media.setPlane as the shim calls it.
void mediaRemove() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var api = __screenkit.media, setPlane = api.setPlane, planes = [];
  api.setPlane = function (id, x, y, w, h, visible, z) {
    planes.push([Math.round(x), Math.round(y), Math.round(w), Math.round(h), visible, z].join(' '));
    return setPlane.apply(api, arguments);
  };
  function plane() { return planes[planes.length - 1]; }
  var v = video('position: absolute; left: 10px; top: 20px; width: 320px; height: 180px; z-index: -1');
  v.src = MEDIA + '/mp4/progressive.mp4';
  await v.play();
  await advanced(v, 0.5);
  log('plane: ' + plane());
  v.style.left = '40px';
  v.style.transform = 'translate(5px, 6px)';
  await sleep(0);
  log('moved: ' + plane());
  v.removeAttribute('style');
  v.width = 160;
  v.height = 90;
  await sleep(0);
  log('attributes: ' + plane());
  v.style.cssText = 'position: absolute; right: 0; bottom: 0; z-index: -1';
  await sleep(0);
  log('intrinsic size: ' + plane().split(' ').slice(2).join(' '));
  var paused = once(v, 'pause');
  v.remove();
  await paused;
  await sleep(0);
  log('removed: paused=' + v.paused + ' plane visible=' + plane().split(' ')[4]);
  document.body.appendChild(v);
  await sleep(0);
  log('back: paused=' + v.paused + ' plane visible=' + plane().split(' ')[4]);
  v.style.display = 'none';
  await sleep(0);
  log('display none: plane visible=' + plane().split(' ')[4]);
  var p = document.createElement('div');
  document.body.appendChild(p);
  v.style.display = '';
  p.appendChild(v);
  await v.play();
  var paused2 = once(v, 'pause');
  p.remove();
  await paused2;
  await sleep(0);
  log('ancestor removed: paused=' + v.paused + ' plane visible=' + plane().split(' ')[4]);
  var moved = video();
  moved.src = MEDIA + '/mp4/progressive.mp4';
  await moved.play();
  var pauses = 0;
  moved.addEventListener('pause', function () { pauses++; });
  document.body.insertBefore(moved, document.body.firstChild);
  await sleep(200);
  log('moved within the document: paused=' + moved.paused + ' pauses=' + pauses);
  api.setPlane = setPlane;
  [v, moved].forEach(function (m) { m.removeAttribute('src'); m.load(); });
});
)JS");
  checkTranscript(got,
                  "plane: 10 20 320 180 true -1\n"
                  "moved: 45 26 320 180 true -1\n"
                  "attributes: 0 0 160 90 true 0\n"
                  "intrinsic size: 160 90 true -1\n"
                  "removed: paused=true plane visible=false\n"
                  "back: paused=true plane visible=true\n"
                  "display none: plane visible=false\n"
                  "ancestor removed: paused=true plane visible=false\n"
                  "moved within the document: paused=false pauses=0");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: two players ------------------------------------------------------------------------------
void mediaTwoPlayers() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var api = __screenkit.media, setPlane = api.setPlane, planes = {};
  api.setPlane = function (id, x) { planes[id] = Math.round(x); return setPlane.apply(api, arguments); };
  var v1 = video('position: absolute; left: 0; top: 0; width: 32px; height: 18px; z-index: -1');
  var v2 = video('position: absolute; left: 32px; top: 0; width: 32px; height: 18px; z-index: -2');
  var p1 = await player(v1), p2 = await player(v2);
  await Promise.all([p1.load(MEDIA + '/vod/nosubs.m3u8'), p2.load(MEDIA + '/mp4/progressive.mp4')]);
  await Promise.all([v1.play(), v2.play()]);
  await Promise.all([advanced(v1, 1.2), advanced(v2, 1.2)]);
  log('both play: ' + (p1.getStats().decodedFrames > 0) + ' ' + (p2.getStats().decodedFrames > 0) +
      ' planes=' + Object.keys(planes).map(function (id) { return planes[id]; }).sort().join(','));
  v1.pause();
  await once(v1, 'pause');
  await sleep(300);
  var t1 = v1.currentTime, t2 = v2.currentTime;
  await sleep(1200);
  log('independent: first held=' + (Math.abs(v1.currentTime - t1) < 0.3) + ' second advanced=' + (v2.currentTime > t2 + 0.5));
  await Promise.all([p1.destroy(), p2.destroy()]);
  api.setPlane = setPlane;
});
)JS");
  checkTranscript(got,
                  "both play: true true planes=0,32\n"
                  "independent: first held=true second advanced=true");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// --- media: the runtime pauses, and shuts down -------------------------------------------------------
// A paused runtime's players pause -- the video stops, not just the page's view
// of it -- and play again on resume without the page seeing its own `paused`
// change. Shutdown mid-playback releases every player; nothing is delivered
// once it returns (leaks-media-shutdown runs this under leaks(1)).
void mediaRuntimePause() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  CHECK_EQ(domEval(runtime, "globalThis.pv = video(); pv.src = MEDIA + '/mp4/progressive.mp4';"
                            "globalThis.pauses = 0; pv.addEventListener('pause', function () { pauses++; });"
                            "pv.play(); 'x';"),
           std::string("x"));
  waitForJs(runtime, "pv.currentTime > 1");
  const double before = std::atof(domEval(runtime, "String(pv.currentTime);").c_str());
  runtime->pause();
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  runtime->resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const double after = std::atof(domEval(runtime, "String(pv.currentTime);").c_str());
  // Playing through the pause would have moved it three seconds.
  CHECK(after - before < 1.5);
  if (!(after - before < 1.5)) std::fprintf(stderr, "  advanced %.2f s across a 3 s pause\n", after - before);
  CHECK_EQ(domEval(runtime, "String(pv.paused) + ' ' + pauses;"), std::string("false 0"));
  // And it plays on after the resume.
  waitForJs(runtime, "pv.currentTime > " + std::to_string(after + 0.8));
}

// --- media: released with its element ---------------------------------------------------------
// A player lives as long as its element: an element nothing refers to any more
// -- never inserted, or removed -- is collected, and its player goes with it,
// so the runtime can go idle again. (Unloading releases it at once; this is the
// case where nobody unloads.)
void mediaCollected() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  await (async function () {
    var v = document.createElement('video');
    v.src = MEDIA + '/mp4/progressive.mp4';
    await once(v, 'loadedmetadata', 30000);
    var w = video();
    w.src = MEDIA + '/mp4/progressive.mp4';
    await w.play();
    w.remove();
  })();
  log('dropped two elements with players');
});
)JS");
  checkTranscript(got, "dropped two elements with players");
  // Holding players, the runtime is busy; once the collector has taken the
  // elements, it is not.
  CHECK(!runtime->idle());
  bool idle = false;
  for (int i = 0; i < 20 && !idle; ++i) {
    onJsThread(runtime, [](facebook::jsi::Runtime& rt) { rt.instrumentation().collectGarbage("media-collected"); });
    idle = pumpUntilIdle(runtime, 500);
  }
  CHECK(idle);
}

void mediaShutdown() {
  auto f = netFixture();
  if (!f.ok) return;
  {
    auto runtime = shakaRuntime(f);
    if (!runtime) return;
    domEval(runtime, "globalThis.sv = video(); sv.src = MEDIA + '/vod/master.m3u8'; sv.play();"
                     "globalThis.sp = null; player(video()).then(function (p) { sp = p;"
                     "  return p.load(MEDIA + '/mp4/progressive.mp4'); }).then(function () { return sp.getMediaElement().play(); });"
                     "'x';");
    waitForJs(runtime, "sv.currentTime > 0.5 && sp !== null && sp.getMediaElement().currentTime > 0.5");
    // Mid-playback, with both players producing time and stats events.
    runtime->shutdown();
    // Whatever was in flight is dropped, not delivered to a runtime that is gone
    // (an ASan build would report a use after free here).
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  }
  // A second runtime in the same process starts clean.
  auto again = shakaRuntime(f);
  if (!again) return;
  const std::string got = netTranscript(again, R"JS(
run(async function () {
  var v = video();
  v.src = MEDIA + '/mp4/progressive.mp4';
  await v.play();
  await advanced(v, 0.5);
  log('a later runtime plays');
});
)JS");
  checkTranscript(got, "a later runtime plays");
}

#if defined(__linux__) && !defined(__ANDROID__)
// --- media: the device's hardware decoder keeps up ------------------------------------------------------
// The acceptance criterion the small clips cannot make: a picture size the
// probe says this board decodes in hardware really is decoded in real time.
// 1920x1080 at 30, played whole, with the ScreenKit VLC plugin on the decoder
// (`/dev/video10`, bcm2835-codec, on a Pi 3) -- under 1% of frames dropped.
//
// SCREENKIT_MEDIA_SOAK=1 replays it until 60 s have played, which is the
// measurement the spec asks for; the row itself plays it once so the suite
// stays quick.
void mediaHardware() {
  auto f = netFixture();
  if (!f.ok) return;
  const char* soak = std::getenv("SCREENKIT_MEDIA_SOAK");
  const bool longRun = soak != nullptr && *soak != '\0' && std::strcmp(soak, "0") != 0;
  test::LogCapture capture;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string source = std::string("globalThis.__seconds = ") + (longRun ? "60" : "11") + "; 'ok';";
  domEval(runtime, source.c_str());
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var v = video();
  v.src = MEDIA + '/hw/1080p30.mp4';
  await once(v, 'canplaythrough', 60000);
  // Steady playback, measured apart from the start of each pass: priming the
  // decoder and the first pictures cost a handful of frames every time
  // playback begins, and replaying a short clip to fill 60 s would charge that
  // five times where a 60 s stream pays it once.
  var played = 0, passes = 0, base = null, steadyFrames = 0, steadyDropped = 0;
  function quality() { var q = v.getVideoPlaybackQuality(); return { f: q.totalVideoFrames, d: q.droppedVideoFrames }; }
  function closePass() {
    if (base === null) return;
    var q = quality();
    steadyFrames += q.f - base.f;
    steadyDropped += q.d - base.d;
    base = null;
  }
  v.addEventListener('timeupdate', function () { if (base === null && v.currentTime > 2) base = quality(); });
  v.addEventListener('ended', function () {
    closePass();
    played += v.duration;
    if (played < __seconds) { passes++; v.currentTime = 0; v.play(); }
  });
  await v.play();
  await until(function () { return played >= __seconds; }, __seconds * 1000 + 30000);
  var q = v.getVideoPlaybackQuality();
  var dropped = steadyDropped / steadyFrames;
  log('1080p30 ' + Math.round(played) + 's: size=' + v.videoWidth + 'x' + v.videoHeight +
      ' frames=' + (q.totalVideoFrames >= 25 * played) + ' dropped<1%=' + (dropped < 0.01));
  globalThis.__hw = 'steady ' + steadyFrames + ' frames, ' + steadyDropped + ' dropped (' +
                   (dropped * 100).toFixed(2) + '%); with the starts ' + q.totalVideoFrames + '/' +
                   q.droppedVideoFrames + ' over ' + Math.round(played) + ' s in ' + (passes + 1) + ' pass(es)';
  v.pause();
});
)JS", 180000);
  // Printed whatever the row does: the measurement is the point of it.
  std::fprintf(stderr, "  media-hardware: %s\n", domEval(runtime, "__hw").c_str());
  checkTranscript(got, std::string("1080p30 ") + (longRun ? "60" : "12") +
                           "s: size=1920x1080 frames=true dropped<1%=true");
  // ...and it was the hardware, not VLC's software decoder.
  const bool hardware = capture.has(screenkit::LogLevel::Log, "ScreenKit decoder: h264 on /dev/video");
  CHECK(hardware);
  if (!hardware) std::fprintf(stderr, "  no ScreenKit decoder line in the log\n");
}

// --- media: no Wayland on Linux ------------------------------------------------------------------------
// Video on Linux is a Wayland subsurface; with X11, KMS or no display there is
// nowhere to put it, and load says so: 3016 VIDEO_ERROR, naming Wayland.
void mediaNoWayland() {
  auto f = netFixture();
  if (!f.ok) return;
  // No Wayland display for this process: the rows' window is never made.
  unsetenv("WAYLAND_DISPLAY");
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var caps = __screenkit.media.capabilities();
  log('videoOutput=' + caps.videoOutput + ' names Wayland=' + /Wayland/.test(caps.videoOutputProblem));
  var v = video();
  var p = await player(v);
  try {
    await p.load(MEDIA + '/mp4/progressive.mp4');
    log('resolved');
  } catch (e) {
    log('rejected ' + code(e) + ' names Wayland=' + /Wayland/.test(e.message) + ' video.error=' + (v.error && v.error.code));
  }
  await p.destroy();
});
)JS");
  checkTranscript(got,
                  "videoOutput=false names Wayland=true\n"
                  "rejected 3/3016/2 names Wayland=true video.error=4");
}
#endif  // __linux__ && !__ANDROID__

// --- media: HTMLMediaElement against a scripted player ---------------------------------------------------
// The element's state machine on its own, with __screenkit.media replaced by a
// script that answers exactly as the seam's contract says a player may: the
// event order HTML defines around load, play, seek, stall, end and error,
// play() promises, and the plane. Runs on every platform, no media needed.
void mediaElementModel() {
  auto runtime = domRuntime();
  if (!runtime) return;
  domEval(runtime, R"JS(
globalThis.__r = []; globalThis.__done = false;
function log(line) { __r.push(String(line)); }
var real = __screenkit.media, handler = real.onevent, calls = [], players = {}, nextId = 1, nextSerial = 1;
function emit(id, type, payload) {
  var p = players[id];
  payload.serial = p.serial;
  setTimeout(function () { handler(p.target, type, payload); }, 0);
}
__screenkit.media = {
  onevent: handler,
  capabilities: function () {
    return { available: true, platform: 'script', hls: true, dash: false, progressive: true, keySystems: [],
             containers: ['video/mp4'], codecs: ['avc1', 'mp4a'], videoOutput: true, videoOutputProblem: '' };
  },
  create: function (target) { var id = nextId++; players[id] = { target: target, serial: 0 }; calls.push('create'); return id; },
  load: function (id, o) {
    var p = players[id];
    p.serial = nextSerial++;
    calls.push('load ' + o.url.split('/').pop());
    if (/missing/.test(o.url)) { emit(id, 'error', { kind: 'network', httpStatus: 404, message: 'HTTP 404' }); return p.serial; }
    emit(id, 'state', { state: 'loading' });
    var live = /live/.test(o.url);
    emit(id, 'metadata', { duration: live ? Infinity : 10, live: live, seekStart: live ? 30 : 0,
                           seekEnd: live ? 40 : 10, width: 640, height: 360,
                           hasVideo: true, hasAudio: true, manifest: 'hls' });
    emit(id, 'state', { state: 'ready' });
    return p.serial;
  },
  play: function (id) { calls.push('play'); emit(id, 'time', { position: 1, seekStart: 0, seekEnd: 10 }); },
  pause: function () { calls.push('pause'); },
  seek: function (id, t) {
    calls.push('seek ' + t);
    emit(id, 'state', { state: 'buffering' });
    emit(id, 'seeked', { position: t });
    emit(id, 'state', { state: 'ready' });
  },
  setRate: function (id, r) { calls.push('rate ' + r); }, setVolume: function (id, v) { calls.push('volume ' + v); },
  setMuted: function (id, m) { calls.push('muted ' + m); },
  setPlane: function (id, x, y, w, h, visible) { calls.push('plane ' + [x, y, w, h, visible].join(',')); },
  selectVariant: function () {}, setAbr: function () {}, selectAudioLanguage: function () {},
  selectText: function () {}, provideLicence: function () {},
  unload: function (id) { calls.push('unload'); return ++nextSerial; },
  destroy: function (id) { calls.push('destroy'); delete players[id]; }
};
globalThis.fake = { calls: calls, emit: emit, players: players, restore: function () { __screenkit.media = real; } };
'fake player';
)JS");
  const std::string got = netTranscript(runtime, R"JS(
function sleep(ms) { return new Promise(function (resolve) { setTimeout(resolve, ms); }); }
function run(body) {
  body().then(function () { __done = true; },
              function (e) { log('THREW ' + (e && e.name) + ': ' + (e && e.message)); __done = true; });
}
run(async function () {
  var v = document.createElement('video');
  var seen = [];
  ['loadstart', 'durationchange', 'loadedmetadata', 'resize', 'loadeddata', 'canplay', 'canplaythrough', 'play',
   'playing', 'waiting', 'pause', 'seeking', 'seeked', 'timeupdate', 'ended', 'emptied', 'abort', 'error',
   'volumechange', 'ratechange'].forEach(function (type) {
    v.addEventListener(type, function () { if (type !== 'timeupdate' || seen[seen.length - 1] !== 'timeupdate') seen.push(type); });
  });
  document.body.appendChild(v);
  v.src = 'http://fixture/a.m3u8';
  var played = v.play().then(function () { return 'resolved'; });
  await sleep(20);
  log('load and play: ' + seen.join(',') + ' readyState=' + v.readyState + ' ' + await played);
  seen.length = 0;
  v.currentTime = 5;
  await sleep(20);
  log('seek: ' + seen.join(',') + ' at=' + v.currentTime);
  seen.length = 0;
  fake.emit(1, 'state', { state: 'buffering' });
  fake.emit(1, 'state', { state: 'ready' });
  await sleep(20);
  log('stall: ' + seen.join(','));
  seen.length = 0;
  var interrupted = v.play().then(function () { return 'resolved'; });
  v.pause();
  log('pause: ' + await interrupted + ' paused=' + v.paused);
  var resolved = v.play().then(function () { return 'resolved'; });
  v.pause();
  log('play, pause when ready: ' + await resolved.catch(function (e) { return 'rejected ' + e.name; }));
  await sleep(20);
  seen.length = 0;
  v.play();
  await sleep(20);
  seen.length = 0;
  fake.emit(1, 'state', { state: 'ended' });
  await sleep(20);
  log('end: ' + seen.join(',') + ' ended=' + v.ended + ' paused=' + v.paused + ' at=' + v.currentTime);
  seen.length = 0;
  v.volume = 0.5; v.muted = true; v.playbackRate = 2;
  await sleep(20);
  log('volume, rate: ' + seen.join(',') + (function () { try { v.volume = 2; return ' no throw'; } catch (e) { return ' ' + e.name; } })());
  seen.length = 0;
  v.src = 'http://fixture/missing.m3u8';
  var failed = v.play().catch(function (e) { return 'rejected ' + e.name; });
  await sleep(20);
  log('error: ' + seen.join(',') + ' code=' + v.error.code + ' networkState=' + v.networkState + ' ' + await failed +
      ' then ' + await v.play().then(function () { return 'resolved'; }, function (e) { return 'rejected ' + e.name; }));
  log('calls: ' + fake.calls.join(' | '));
  var w = document.createElement('video');
  w.src = 'http://fixture/b.m3u8';
  var early = w.play().then(function () { return 'resolved'; }, function (e) { return 'rejected ' + e.name; });
  w.pause();
  log('play, pause before data: ' + await early);

  // `loop`: the end goes back to the start and keeps playing, and `ended` does
  // not fire -- except on a live stream, which has no start to go back to.
  function newest() { return Math.max.apply(null, Object.keys(fake.players).map(Number)); }
  async function element(url, set) {
    var e = document.createElement('video');
    if (set) set(e);
    e.src = url;
    await sleep(30);
    return e;
  }
  var looped = await element('http://fixture/loop.m3u8', function (e) { e.loop = true; });
  var loopId = newest();
  var loopSeen = [];
  ['seeking', 'seeked', 'ended', 'pause', 'playing'].forEach(function (t) {
    looped.addEventListener(t, function () { loopSeen.push(t); });
  });
  await looped.play();
  await sleep(20);
  loopSeen.length = 0;
  fake.emit(loopId, 'state', { state: 'ended' });
  await sleep(30);
  log('loop: ' + loopSeen.join(',') + ' ended=' + looped.ended + ' paused=' + looped.paused +
      ' at=' + looped.currentTime);
  var liveLoop = await element('http://fixture/live.m3u8', function (e) { e.loop = true; });
  var liveId = newest();
  var liveSeen = [];
  ['ended', 'pause'].forEach(function (t) { liveLoop.addEventListener(t, function () { liveSeen.push(t); }); });
  await liveLoop.play();
  await sleep(20);
  liveSeen.length = 0;
  fake.emit(liveId, 'state', { state: 'ended' });
  await sleep(30);
  log('loop on live: ' + liveSeen.join(',') + ' ended=' + liveLoop.ended + ' isLive=' + (liveLoop.duration === Infinity));

  // `autoplay`: it starts once, by itself, and a pause by hand ends it.
  var autoSeen = [];
  var auto = document.createElement('video');
  ['play', 'playing', 'pause'].forEach(function (t) {
    auto.addEventListener(t, function () { autoSeen.push(t); });
  });
  auto.autoplay = true;
  auto.src = 'http://fixture/auto.m3u8';
  await sleep(40);
  log('autoplay: ' + autoSeen.join(',') + ' paused=' + auto.paused);
  autoSeen.length = 0;
  auto.pause();
  fake.emit(newest(), 'state', { state: 'ready' });
  await sleep(30);
  log('autoplay after a pause: ' + autoSeen.join(',') + ' paused=' + auto.paused);

  // Every kind the seam can report, as the element's own code
  // (MediaError: 2 NETWORK, 3 DECODE, 4 SRC_NOT_SUPPORTED).
  var codes = [];
  var kinds = ['network', 'manifest', 'media', 'video-output', 'key-system', 'licence', 'unavailable'];
  for (var i = 0; i < kinds.length; i++) {
    var e = await element('http://fixture/kind' + i + '.m3u8');
    fake.emit(newest(), 'error', { kind: kinds[i], message: kinds[i] });
    await sleep(20);
    codes.push(kinds[i] + '=' + (e.error && e.error.code));
  }
  log('error codes: ' + codes.join(' '));
  fake.restore();
});
)JS");
  checkTranscript(got,
      "load and play: loadstart,play,waiting,durationchange,loadedmetadata,resize,loadeddata,canplay,playing,"
      "canplaythrough,timeupdate readyState=4 resolved\n"
      "seek: seeking,waiting,timeupdate,seeked,canplay,playing,canplaythrough at=5\n"
      "stall: timeupdate,waiting,canplay,playing,canplaythrough\n"
      "pause: resolved paused=true\n"
      "play, pause when ready: resolved\n"
      "end: timeupdate,pause,ended ended=true paused=true at=10\n"
      "volume, rate: volumechange,volumechange,ratechange IndexSizeError\n"
      "error: abort,emptied,timeupdate,durationchange,loadstart,play,waiting,error code=2 networkState=3 rejected NotSupportedError "
      "then rejected NotSupportedError\n"
      // The plane takes the default 300x150 until the video's own size is known;
      // a new load keeps the volume and mute, and resets the rate.
      "calls: create | load a.m3u8 | plane 0,0,300,150,true | play | plane 0,0,640,360,true | seek 5 | play | "
      "pause | play | pause | play | pause | volume 0.5 | muted true | rate 2 | load missing.m3u8 | volume 0.5 | "
      "muted true | plane 0,0,300,150,true | play\n"
      "play, pause before data: rejected AbortError\n"
      // `loop` seeks back and keeps playing, with no `ended`; a live stream ends.
      "loop: seeking,seeked,playing ended=false paused=false at=0\n"
      "loop on live: pause,ended ended=true isLive=true\n"
      "autoplay: play,playing paused=false\n"
      "autoplay after a pause: pause paused=true\n"
      "error codes: network=2 manifest=4 media=3 video-output=4 key-system=4 licence=3 unavailable=4");
}


// --- media: the licence exchange, against a scripted key system ------------------------------------
// What the licence flow promises on every platform, whatever its key system
// can do with the fixture: a scripted player raises the licence request a
// platform key system would (FairPlay's SPC with its skd:// id, Widevine's
// challenge) and waits for the answer. The request goes out through the
// player's networking engine -- filters applied, FairPlay's certificate fetched
// first, the body the challenge -- reaches the fixture's licence server, and the
// server's bytes go back through the response filters to the key system; a
// server that fails is 6007, and the key system hears null.
void mediaLicenceExchange() {
  auto f = netFixture();
  if (!f.ok) return;
  auto runtime = shakaRuntime(f);
  if (!runtime) return;
  const std::string got = netTranscript(runtime, R"JS(
run(async function () {
  var real = __screenkit.media, handler = real.onevent, loads = [], provided = [], players = {}, nextId = 1, serials = 1;
  function emit(id, type, payload) {
    payload.serial = players[id].serial;
    setTimeout(function () { handler(players[id].target, type, payload); }, 0);
  }
  __screenkit.media = {
    onevent: handler,
    capabilities: function () {
      return { available: true, platform: 'script', hls: true, dash: true, progressive: true,
               keySystems: ['com.apple.fps', 'com.widevine.alpha'], containers: ['video/mp4'], codecs: ['avc1'],
               videoOutput: true, videoOutputProblem: '' };
    },
    create: function (target) { var id = nextId++; players[id] = { target: target, serial: 0 }; return id; },
    load: function (id, o) {
      players[id].serial = serials++;
      players[id].drm = o.drm;
      loads.push(o.drm.keySystem + ' certificate=' + (o.drm.serverCertificate ? o.drm.serverCertificate.byteLength : 0) +
                 ' server=' + (o.drm.licenceServer.indexOf(LICENCE) === 0));
      var fps = o.drm.keySystem === 'com.apple.fps';
      emit(id, 'state', { state: 'loading' });
      emit(id, 'licence', { requestId: 40 + id, keySystem: o.drm.keySystem,
                            challenge: new TextEncoder().encode(fps ? 'SPC-bytes' : 'widevine-challenge').buffer,
                            contentId: fps ? 'skd://asset-1' : '' });
      return players[id].serial;
    },
    provideLicence: function (id, requestId, bytes) {
      provided.push(requestId + ':' + (bytes ? new TextDecoder().decode(new Uint8Array(bytes)) : 'null'));
      if (!bytes) { emit(id, 'error', { kind: 'licence', httpStatus: 0, message: 'no licence' }); return; }
      emit(id, 'metadata', { duration: 10, live: false, seekStart: 0, seekEnd: 10, width: 64, height: 36,
                             hasVideo: true, hasAudio: true, manifest: 'hls' });
      emit(id, 'tracks', { variants: [{ id: 0, bandwidth: 1, width: 64, height: 36, active: true }], audio: [], text: [] });
      emit(id, 'variant', { id: 0 });
      emit(id, 'state', { state: 'ready' });
    },
    play: function () {}, pause: function () {}, seek: function () {}, setRate: function () {},
    setVolume: function () {}, setMuted: function () {}, setPlane: function () {}, selectVariant: function () {},
    setAbr: function () {}, selectAudioLanguage: function () {}, selectText: function () {},
    unload: function (id) { return ++serials; }, destroy: function (id) { delete players[id]; }
  };
  try {
    var v = video();
    var p = await player(v);
    var requests = [];
    p.getNetworkingEngine().registerRequestFilter(function (type, request) {
      if (type === RT.SERVER_CERTIFICATE) requests.push('certificate');
      if (type !== RT.LICENSE) return;
      request.headers['x-screenkit-filter'] = 'licence';
      requests.push('licence ' + request.method + ' ' + request.initDataType + ' ' +
                    (request.initData ? shaka.util.StringUtils.fromUTF16(request.initData, true) : '-') +
                    ' drmInfo=' + request.drmInfo.keySystem);
    });
    p.getNetworkingEngine().registerResponseFilter(function (type, response) {
      if (type === RT.LICENSE) requests.push('response ' + response.status);
    });
    var errors = [];
    p.addEventListener('error', function (e) { errors.push(code(e.detail)); });
    var drmUpdates = 0;
    p.addEventListener('drmsessionupdate', function () { drmUpdates++; });
    async function received(id) { return (await fetch(LICENCE + '/log?id=' + id)).json(); }

    p.configure({ drm: { servers: { 'com.apple.fps': LICENCE + '/ok?id=fx' },
                         advanced: { 'com.apple.fps': { serverCertificateUri: MEDIA + '/fairplay/cert.der' } } } });
    log('fairplay: ' + await outcome(p.load(MEDIA + '/fairplay/master.m3u8')) + ' keySystem=' + p.keySystem());
    var fx = (await received('fx'))[0] || {};
    log('  reached: ' + fx.method + ' filtered=' + (fx.headers && fx.headers['x-screenkit-filter']) +
        ' body=' + atob(fx.body || '') + ' | ' + requests.join(', ') + ' | provided ' + provided.join(','));
    requests.length = 0;

    p.resetConfiguration();
    p.configure({ drm: { servers: { 'com.widevine.alpha': LICENCE + '/ok?id=wx' } } });
    log('widevine: ' + await outcome(p.load(MEDIA + '/cenc/widevine.mpd')) + ' keySystem=' + p.keySystem());
    var wx = (await received('wx'))[0] || {};
    log('  reached: ' + wx.method + ' filtered=' + (wx.headers && wx.headers['x-screenkit-filter']) +
        ' body=' + atob(wx.body || '') + ' | ' + requests.join(', '));
    requests.length = 0;

    p.resetConfiguration();
    p.configure({ drm: { servers: { 'com.widevine.alpha': LICENCE + '/fail?id=wf' },
                         retryParameters: { maxAttempts: 1, baseDelay: 10, backoffFactor: 1, fuzzFactor: 0, timeout: 10000 } } });
    var failure = null;
    try { await p.load(MEDIA + '/cenc/widevine.mpd'); } catch (e) { failure = e; }
    log('server fails: ' + code(failure) + ' cause=' + code(failure && failure.data[0]) + ' status=' +
        (failure && failure.data[0] && failure.data[0].data[1]) + ' reached=' + (await received('wf')).length +
        ' | ' + requests.join(', '));

    p.resetConfiguration();
    p.configure({ drm: { servers: { 'com.microsoft.playready': LICENCE + '/ok?id=pr' } } });
    log('no such key system: ' + await outcome(p.load(MEDIA + '/vod/master.m3u8')) + ' reached=' + (await received('pr')).length);
    log('loads: ' + loads.join(' / ') + ' | provided ' + provided.join(','));
    log('drmsessionupdate=' + drmUpdates + ' error events=' + errors.join(','));
    await p.destroy();
  } finally {
    __screenkit.media = real;
  }
});
)JS");
  checkTranscript(got,
                  "fairplay: resolved keySystem=com.apple.fps\n"
                  "  reached: POST filtered=licence body=SPC-bytes | certificate, licence POST skd skd://asset-1 "
                  "drmInfo=com.apple.fps, response 200 | provided 41:screenkit-fixture-licence\n"
                  "widevine: resolved keySystem=com.widevine.alpha\n"
                  "  reached: POST filtered=licence body=widevine-challenge | licence POST cenc - "
                  "drmInfo=com.widevine.alpha, response 200\n"
                  // Shaka's 1001 for a 5xx is RECOVERABLE (retried); the licence failure it causes is CRITICAL.
                  "server fails: 6/6007/2 cause=1/1001/1 status=500 reached=1 | licence POST cenc - drmInfo=com.widevine.alpha\n"
                  "no such key system: rejected 6/6001/2 reached=0\n"
                  "loads: com.apple.fps certificate=32 server=true / com.widevine.alpha certificate=0 server=true / "
                  "com.widevine.alpha certificate=0 server=true | provided 41:screenkit-fixture-licence,"
                  "42:screenkit-fixture-licence,43:null\n"
                  "drmsessionupdate=2 error events=6/6007/2,6/6001/2");
  CHECK(pumpUntilIdle(runtime, 10000));
}

// ============================================================================
// <iframe> instances (spec-m9-iframe-instances, Architecture.md 5)
//
// One iframe = one Instance = a second app on its own runtime, thread and GL
// context, drawing into a texture this page composites at the element's CSS
// rect. Every row here drives a real launcher page with a real child package
// (fixtures/iframe-child.js, packaged as iframe-child.skpkg) over a real ANGLE
// context, because the three things that can go wrong -- the share group, the
// freeze gate and the composite -- are none of them visible from JS alone.
//
// The child reports what it sees through `postMessage`, because that is the
// only channel between the two: the row cannot read the child's variables, and
// the child cannot read the row's.
// ============================================================================

struct IframeHost {
  std::shared_ptr<screenkit::Runtime> runtime;
  std::shared_ptr<screenkit::InstanceRegistry> instances;
  /// How many frames this page has actually presented. Counted inside the
  /// present itself, so it can be read from this thread while the page is
  /// frozen -- which is the one way to see that a launcher that froze itself is
  /// still compositing the game that took the remote.
  std::shared_ptr<std::atomic<int>> presents = std::make_shared<std::atomic<int>>(0);
  explicit operator bool() const { return runtime != nullptr; }
};

/// The page a launcher is: `gl`, the prelude, an instance registry, and an asset
/// root that is the fixture package directory -- so `src` names the child
/// package exactly as a launcher names a game it ships.
IframeHost iframeHost(int width = 320, int height = 180) {
  IframeHost host;
  host.instances = screenkit::makeInstanceRegistry();
  const char* override_ = std::getenv("SCREENKIT_DOM_SHIM_HBC");
  screenkit::setInstanceEnvironment(
      *host.instances, override_ != nullptr && *override_ != '\0' ? override_ : SCREENKIT_DOM_SHIM_HBC);
  screenkit::RuntimeConfig config = testConfig();
  config.instances = host.instances;
  host.runtime = glRuntime(width, height, config);
  if (!host.runtime) return host;
  if (!installDomShim(host.runtime)) {
    host.runtime.reset();
    return host;
  }
  const auto root =
      host.runtime->evaluateSource("__screenkit.setAssetRoot('" + fixture("packages") + "')", "root.js");
  CHECK(root.ok);
  if (!root.ok) {
    std::fprintf(stderr, "  setAssetRoot: %s\n", root.error.c_str());
    host.runtime.reset();
    return host;
  }
  // The present, exactly as `startGraphics` hangs one off the host's frame:
  // the composite runs inside it, so without it nothing is ever composited and
  // the default framebuffer keeps whatever the page last drew straight into it.
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  onJsThread(host.runtime, [&](facebook::jsi::Runtime& js) { surface = screenkit::gfx::surfaceFor(js); });
  CHECK(surface != nullptr);
  if (!surface) {
    host.runtime.reset();
    return host;
  }
  auto presents = host.presents;
  CHECK(host.runtime->setFrameFinishedCallback([surface, presents](facebook::jsi::Runtime& js) {
    if (screenkit::gfx::presentFrame(js, *surface)) presents->fetch_add(1);
  }));
  return host;
}

/// The launcher's own page: a blue frame every tick, an `<iframe>` helper, and
/// somewhere to record what the child says.
const char* kIframePrelude = R"JS(
globalThis.__log = [];
globalThis.__msgs = [];
window.addEventListener('message', function (e) { __msgs.push(e.data); });
globalThis.__page = document.createElement('canvas');
document.body.appendChild(__page);
globalThis.__pg = __page.getContext('webgl');
globalThis.__pageFrames = 0;
(function paintPage() {
  __pg.clearColor(0, 0, 1, 1);
  __pg.clear(__pg.COLOR_BUFFER_BIT);
  __pageFrames++;
  requestAnimationFrame(paintPage);
})();
globalThis.embed = function (options) {
  var o = options || {};
  var f = document.createElement('iframe');
  f.style.cssText = o.css || 'position: absolute; left: 0; top: 0; width: 160px; height: 90px';
  if (o.sandbox !== undefined) f.sandbox = o.sandbox;
  f.addEventListener('load', function () { __log.push('load'); });
  f.addEventListener('error', function () { __log.push('error'); });
  document.body.appendChild(f);
  f.src = o.src === undefined ? 'iframe-child.skpkg' : o.src;
  return f;
};
globalThis.said = function (key) {
  var found = null;
  for (var i = 0; i < __msgs.length; i++) {
    if (__msgs[i] && __msgs[i][key] !== undefined) found = __msgs[i];
  }
  return found;
};
globalThis.saidCount = function (key, value) {
  var n = 0;
  for (var i = 0; i < __msgs.length; i++) {
    if (__msgs[i] && __msgs[i][key] === value) n++;
  }
  return n;
};
'iframe prelude';
)JS";

bool iframePrelude(const IframeHost& host) {
  const auto result = host.runtime->evaluateSource(kIframePrelude, "iframe-prelude.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  prelude: %s\n", result.error.c_str());
  return result.ok;
}

/// One frame for the whole tree, as the windowed host's loop drives it: every
/// live instance, then the page.
void tickTree(const IframeHost& host, double& tick) {
  for (const std::shared_ptr<screenkit::Instance>& instance : screenkit::liveInstances(*host.instances)) {
    instance->tickFrame(tick);
  }
  host.runtime->tickFrame(tick);
  tick += 1000.0 / 60.0;
}

void tickTree(const IframeHost& host, int frames) {
  static double tick = 0;
  for (int i = 0; i < frames; ++i) {
    tickTree(host, tick);
    hostWait(2);
  }
}

/// Drive the tree until `expr` reads true in the launcher. Never evaluates while
/// the launcher is frozen -- a focused child pauses it, and an evaluation would
/// wait behind the very gate the row is testing.
bool pumpTree(const IframeHost& host, const std::string& expr, int timeoutMs = 20000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  double tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!host.runtime->paused()) {
      const auto r = host.runtime->evaluateSource(expr, "wait.js");
      if (r.ok && r.value == "true") return true;
    }
    tickTree(host, tick);
    hostWait(2);
  }
  test::fail(__FILE__, __LINE__, "timed out waiting for: " + expr);
  return false;
}

/// One pixel of the launcher's default framebuffer, in GL coordinates (y up),
/// read on its JS thread where its context is current.
std::string pixelAt(const IframeHost& host, int x, int y) {
  std::string out;
  onJsThread(host.runtime, [&](facebook::jsi::Runtime&) {
    unsigned char px[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    out = std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]);
  });
  return out;
}

// --- matrix row: embed and paint ------------------------------------------------
// A launcher page with an <iframe> whose src is a game package: the instance
// loads, runs and paints at the element's CSS rect, `load` fires on the element,
// and the launcher's own UI is still around it. The pixels are the assertion --
// the composite is the one part of this that no JS-visible state can prove.
void iframeEmbed() {
  test::LogCapture capture;
  IframeHost host = iframeHost(320, 180);
  if (!host) return;
  if (!iframePrelude(host)) return;

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.f = embed();"
                   "[f instanceof HTMLIFrameElement, f.localName, typeof f.contentWindow.postMessage,"
                   " f.contentWindow.parent === window, f.contentDocument].join(',');"),
           std::string("true,iframe,function,true,"));

  CHECK(pumpTree(host, "__log.indexOf('load') >= 0 && said('ready') !== null"));
  CHECK_EQ(domEval(host.runtime, "__log.join(',');"), std::string("load"));
  CHECK_EQ(domEval(host.runtime, "String(window.length) + ' ' + String(window.frames.length);"),
           std::string("1 1"));

  // Enough frames for the child to paint and the launcher to composite it.
  tickTree(host, 12);
  // Inside the iframe's rect: the child's green. CSS (40, 20) with a 180-high
  // drawable is GL row 159.
  CHECK_EQ(pixelAt(host, 40, 159), std::string("0,255,0"));
  // Outside it: the launcher's own blue, still being drawn around the game.
  CHECK_EQ(pixelAt(host, 240, 40), std::string("0,0,255"));
  CHECK(!capture.has(screenkit::LogLevel::Error, "failed"));
}

// --- matrix row: messages both ways ---------------------------------------------
// A message is a copy -- mutating the sender's object afterwards does not change
// the receiver's -- the `source` can reply, and a value the clone cannot carry
// throws DataCloneError at the sender rather than arriving as something else.
void iframeMessage() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.list = [{ n: 1 }];"
                   "globalThis.sent = { ask: 'mutate', list: list, bytes: new Uint8Array([7, 8]).buffer };"
                   "sent.self = sent;"
                   "f.contentWindow.postMessage(sent);"
                   "String(list.length);"),
           std::string("1"));
  CHECK(pumpTree(host, "said('sawLength') !== null"));
  // The child saw its own copy grow to 2 and the sender's is still 1; the
  // nested object came with it, the cycle arrived as a cycle, and the bytes
  // came by value.
  CHECK_EQ(domEval(host.runtime,
                   "var m = said('sawLength');"
                   "[m.sawLength, list.length, m.sawNested, m.cyclic, m.bytes].join(',');"),
           std::string("2,1,1,true,7"));

  // The source of a message can reply, which is how a child answers without
  // knowing anything about who embedded it.
  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'counters', tag: 'reply' });");
  CHECK(pumpTree(host, "said('tag') !== null"));
  CHECK_EQ(domEval(host.runtime, "String(said('tag').tag);"), std::string("reply"));

  // Anything the subset cannot carry is refused at the sender.
  CHECK_EQ(domEval(host.runtime,
                   "function refuse(v) {"
                   "  try { f.contentWindow.postMessage(v); return 'allowed'; } catch (e) { return e.name; }"
                   "}"
                   "[refuse(function () {}), refuse(Symbol('x')), refuse(new Map()), refuse(document.body),"
                   " refuse({ ok: 1 })].join(',');"),
           std::string("DataCloneError,DataCloneError,DataCloneError,DataCloneError,allowed"));

  // Depth is capped at both ends -- the clone recurses, so a message nested a
  // hundred thousand deep would be a stack overflow rather than an error the
  // page can catch.
  CHECK_EQ(domEval(host.runtime,
                   "var deep = {}; var at = deep;"
                   "for (var i = 0; i < 2000; i++) { at.next = {}; at = at.next; }"
                   "function tooDeep(run) { try { run(); return 'allowed'; } catch (e) { return e.name; } }"
                   "[tooDeep(function () { structuredClone(deep); }),"
                   " tooDeep(function () { f.contentWindow.postMessage(deep); })].join(',');"),
           std::string("DataCloneError,DataCloneError"));

  // The same subset, and the same refusals, through structuredClone.
  CHECK_EQ(domEval(host.runtime,
                   "var cyc = { n: 1 }; cyc.self = cyc;"
                   "var copy = structuredClone(cyc);"
                   "var threw = ''; try { structuredClone(function () {}); } catch (e) { threw = e.name; }"
                   "[copy !== cyc, copy.self === copy, copy.n, threw].join(',');"),
           std::string("true,true,1,DataCloneError"));

  // And back the other way, with `window.parent` as the destination.
  CHECK_EQ(domEval(host.runtime, "String(said('ready').ready);"), std::string("true"));
}

// --- matrix row: paused and resumed ---------------------------------------------
// `Architecture.md` 14.7's lifecycle proof, measured rather than asserted by
// eye: the counters the child reports inside its own `pause` and `resume`
// handlers are the same number, across many frames of the tree being ticked.
void iframeLifecycle() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));
  // Running: the counters move.
  tickTree(host, 10);
  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'counters', tag: 'before' });");
  CHECK(pumpTree(host, "said('tag') !== null && said('tag').tag === 'before'"));
  const int before = std::atoi(domEval(host.runtime, "String(said('tag').frames);").c_str());
  CHECK(before > 0);

  // The instance id this page's binding gave the first `<iframe>` it made.
  domEval(host.runtime, "__screenkit.instances.setPaused(1, true);");
  CHECK(pumpTree(host, "said('lifecycle') !== null && said('lifecycle').lifecycle === 'pause'"));
  const int atPause = std::atoi(domEval(host.runtime, "String(said('lifecycle').frames);").c_str());
  const int timersAtPause = std::atoi(domEval(host.runtime, "String(said('lifecycle').timers);").c_str());

  // Thirty frames and a third of a second of wall clock with the gate closed.
  tickTree(host, 30);
  hostWait(300);
  tickTree(host, 30);

  domEval(host.runtime, "__screenkit.instances.setPaused(1, false);");
  CHECK(pumpTree(host, "said('lifecycle') !== null && said('lifecycle').lifecycle === 'resume'"));
  const int atResume = std::atoi(domEval(host.runtime, "String(said('lifecycle').frames);").c_str());
  const int timersAtResume = std::atoi(domEval(host.runtime, "String(said('lifecycle').timers);").c_str());
  CHECK_EQ(atResume, atPause);
  CHECK_EQ(timersAtResume, timersAtPause);
  if (atResume != atPause || timersAtResume != timersAtPause) {
    std::fprintf(stderr, "  paused instance ran: rAF %d -> %d, timers %d -> %d\n", atPause, atResume,
                 timersAtPause, timersAtResume);
  }
  // Its last frame keeps showing while it is frozen: a paused instance is not an
  // invisible one, which is what makes a launcher's tile free.
  CHECK_EQ(pixelAt(host, 40, 159), std::string("0,255,0"));

  // And it runs again, within a frame of the resume.
  tickTree(host, 10);
  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'counters', tag: 'after' });");
  CHECK(pumpTree(host, "said('tag') !== null && said('tag').tag === 'after'"));
  CHECK(std::atoi(domEval(host.runtime, "String(said('tag').frames);").c_str()) > atResume);
}

// --- matrix row: focus ------------------------------------------------------------
// `focus()` is atomic: the outgoing context is paused before the incoming one
// resumes. A launcher that gives the remote to a game freezes itself, and the
// game hands it back with `parent.focus()` -- from its own thread, because the
// parent is frozen and a task would wait behind the gate it means to open.
void iframeFocus() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));
  CHECK_EQ(domEval(host.runtime, "document.activeElement.localName;"), std::string("body"));
  CHECK(!host.runtime->paused());
  // Input follows focus rather than the element tree: until something is
  // focused it goes to this page, which is what `focusTarget` answers with.
  CHECK(screenkit::focusTarget(*host.instances)->current() == nullptr);

  // The launcher tells the game to hand the remote back after twenty of the
  // game's own frames, then hands it over and freezes itself. The delay is what
  // makes "frozen" observable: a pause and a resume asked for in the same turn
  // are allowed to cancel out, and do.
  domEval(host.runtime,
          "f.contentWindow.postMessage({ ask: 'focus-parent-after', frames: 200 });"
          "f.focus();"
          "globalThis.activeWhileFocused = document.activeElement.localName;");
  // Nothing may be evaluated in this page from here until it thaws: an
  // evaluation waits behind the freeze gate, which is the whole point of it.
  double tick = 0;
  const auto frozenBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!host.runtime->paused() && std::chrono::steady_clock::now() < frozenBy) {
    tickTree(host, tick);
    hostWait(2);
  }
  CHECK(host.runtime->paused());

  // Frozen, and still compositing. `Paused` stops the page, not the window: the
  // game that took the remote is the one thing on screen, and a launcher that
  // stopped presenting would show its own last frame instead.
  const int presentsWhenFrozen = host.presents->load();
  for (int i = 0; i < 40 && host.runtime->paused(); ++i) {
    tickTree(host, tick);
    hostWait(2);
  }
  // Forty ticks is nowhere near the two hundred frames the game was asked to
  // wait for, so the launcher is still frozen -- and has still presented.
  CHECK(host.runtime->paused());
  const int whileFrozen = host.presents->load() - presentsWhenFrozen;
  CHECK(whileFrozen > 0);
  if (whileFrozen <= 0) {
    std::fprintf(stderr, "  a frozen launcher presented nothing: the game would be invisible\n");
  }

  // The child asks for it back, and the launcher thaws without anything on its
  // own thread having to run first.
  const auto thawedBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (host.runtime->paused() && std::chrono::steady_clock::now() < thawedBy) {
    tickTree(host, tick);
    hostWait(2);
  }
  CHECK(!host.runtime->paused());
  // Focus came back to the page, so the body holds it again -- moved by the
  // instance's own `window.parent.focus()`, from its own thread, while this page
  // had nothing running that could have moved it.
  CHECK(pumpTree(host, "document.activeElement.localName === 'body'"));
  CHECK(screenkit::focusTarget(*host.instances)->current() == nullptr);
  // And while the game had it, the iframe was `document.activeElement`.
  CHECK_EQ(domEval(host.runtime, "activeWhileFocused;"), std::string("iframe"));
  // And what it was compositing all along is the game's frame, not its own.
  tickTree(host, 6);
  CHECK_EQ(pixelAt(host, 40, 159), std::string("0,255,0"));
}

// --- matrix row: removal terminates ------------------------------------------------
// `iframe.remove()` terminates the instance: thread joined, heap and GL freed,
// layer gone. A launch/remove loop of ten leaves nothing behind -- no live
// instance, and a runtime that can go idle again (Architecture.md 14.7).
void iframeRemove() {
  test::LogCapture capture;
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;

  for (int i = 0; i < 10; ++i) {
    domEval(host.runtime, "globalThis.__msgs = []; globalThis.__log = []; globalThis.f = embed();");
    CHECK(pumpTree(host, "said('ready') !== null"));
    CHECK_EQ(static_cast<int>(screenkit::liveInstances(*host.instances).size()), 1);
    domEval(host.runtime, "f.remove(); f = null;");
    // The removal is deferred to a microtask, as HTML's removal steps are, so a
    // node moved within the document is not torn down and rebuilt.
    const auto gone = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!screenkit::liveInstances(*host.instances).empty() &&
           std::chrono::steady_clock::now() < gone) {
      tickTree(host, 1);
    }
    CHECK(screenkit::liveInstances(*host.instances).empty());
    if (!screenkit::liveInstances(*host.instances).empty()) break;
  }

  // Nothing of the ten is still holding the loop open.
  bool idle = false;
  for (int i = 0; i < 20 && !idle; ++i) {
    onJsThread(host.runtime, [](facebook::jsi::Runtime& rt) {
      rt.instrumentation().collectGarbage("iframe-remove");
    });
    idle = pumpUntilIdle(host.runtime, 500);
  }
  CHECK(!capture.has(screenkit::LogLevel::Warn, "GL surface destroyed off the thread"));
}

// --- matrix row: a src that is refused ---------------------------------------------
// The existing package gates decide, and an `http(s)` src or a path escaping the
// parent's package is refused by the asset root exactly as a <video>'s is.
// `error` fires on the element, no instance is left behind, and the parent keeps
// running.
void iframeBadSrc() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;

  const char* kSources[] = {"https://cdn.example/games/tetris.skpkg", "../assets/hello.txt",
                            "no-such-game.skpkg", "bad-manifest.skpkg", "runtime-too-new.skpkg",
                            "bytecode-mismatch.skpkg"};
  for (const char* src : kSources) {
    domEval(host.runtime, (std::string("globalThis.__log = []; globalThis.g = embed({ src: '") + src +
                           "' });")
                              .c_str());
    CHECK(pumpTree(host, "__log.length > 0"));
    const std::string log = domEval(host.runtime, "__log.join(',');");
    CHECK_EQ(log, std::string("error"));
    if (log != "error") std::fprintf(stderr, "  src %s gave %s\n", src, log.c_str());
    domEval(host.runtime, "g.remove(); g = null;");
    tickTree(host, 2);
  }
  // The parent kept running through all six.
  CHECK(pumpTree(host, "__pageFrames > 0"));
  CHECK(screenkit::liveInstances(*host.instances).empty());
}

// --- matrix row: one level of nesting ------------------------------------------------
// An `<iframe>` inside an instance is refused with a clear error, and the
// instance keeps running. `Architecture.md` 5 allows a tree; that this build
// does not is a recorded divergence, not a silently inert element.
void iframeNested() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));
  tickTree(host, 8);
  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'nest' });");
  CHECK(pumpTree(host, "said('nested') !== null"));
  CHECK_EQ(domEval(host.runtime, "said('nested').nested;"), std::string("error"));
  const int atRefusal = std::atoi(domEval(host.runtime, "String(said('nested').frames);").c_str());
  CHECK(atRefusal > 0);
  // The instance keeps running: a refused nested <iframe> is one element that
  // failed, not an app that stopped.
  tickTree(host, 8);
  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'counters', tag: 'after-nest' });");
  CHECK(pumpTree(host, "said('tag') !== null && said('tag').tag === 'after-nest'"));
  CHECK(std::atoi(domEval(host.runtime, "String(said('tag').frames);").c_str()) > atRefusal);
}

// --- matrix row: sandbox ------------------------------------------------------------
// `sandbox` gates capabilities where the binding is installed: a child without
// `allow-network` has no `__screenkit.net` at all, so `fetch` fails in the
// documented shape rather than crashing, and the refusal is said once.
void iframeSandbox() {
  test::LogCapture capture;
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed({ sandbox: 'allow-media allow-storage allow-nonsense' });");
  CHECK(pumpTree(host, "said('ready') !== null"));
  CHECK_EQ(domEval(host.runtime, "f.sandbox.contains('allow-media') + ',' + f.sandbox.length + ',' + f.sandbox.value;"),
           std::string("true,3,allow-media allow-storage allow-nonsense"));

  domEval(host.runtime, "f.contentWindow.postMessage({ ask: 'capabilities' });");
  CHECK(pumpTree(host, "said('sandboxed') !== null"));
  CHECK_EQ(domEval(host.runtime,
                   "var c = said('sandboxed');"
                   "[c.sandboxed, c.net, c.media, c.canEmbed, c.hasParent, c.parentIsSelf, c.topIsParent,"
                   " c.cloneThrows].join(',');"),
           std::string("true,false,true,false,true,false,true,DataCloneError"));
  CHECK_CONTAINS(domEval(host.runtime, "String(said('sandboxed').fetch);"), std::string("__screenkit.net"));
  // Each assertion names the wording of the message it means: every one of
  // these tokens appears in the unknown-token warning's own list, so matching a
  // token name alone would pass with the message it is about deleted.
  CHECK(capture.has(screenkit::LogLevel::Warn, "unknown sandbox token \"allow-nonsense\""));
  CHECK(capture.has(screenkit::LogLevel::Warn, "accepted and reserved"));
  domEval(host.runtime, "f.remove(); f = null;");
  tickTree(host, 2);

  // ...and the other side of the gate: a sandbox that withholds allow-media
  // leaves the child no media binding at all.
  domEval(host.runtime, "__msgs.length = 0; globalThis.g = embed({ sandbox: 'allow-network' });");
  CHECK(pumpTree(host, "said('ready') !== null"));
  domEval(host.runtime, "g.contentWindow.postMessage({ ask: 'capabilities' });");
  CHECK(pumpTree(host, "said('sandboxed') !== null"));
  CHECK_EQ(domEval(host.runtime, "var c = said('sandboxed'); [c.sandboxed, c.net, c.media].join(',');"),
           std::string("true,true,false"));
}

// --- matrix row: two instances -------------------------------------------------------
// Both load, only the focused one runs, and both layers composite in z-order.
void iframeTwo() {
  IframeHost host = iframeHost(320, 180);
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime,
          "globalThis.a = embed({ css: 'position: absolute; left: 0; top: 0; width: 160px; height: 90px; z-index: 1' });"
          "globalThis.b = embed({ css: 'position: absolute; left: 0; top: 0; width: 320px; height: 180px; z-index: 0' });");
  CHECK(pumpTree(host, "__log.length === 2"));
  CHECK_EQ(domEval(host.runtime, "__log.join(',');"), std::string("load,load"));
  CHECK_EQ(static_cast<int>(screenkit::liveInstances(*host.instances).size()), 2);
  CHECK_EQ(domEval(host.runtime, "String(window.length);"), std::string("2"));
  tickTree(host, 12);
  // Both are the same green, so what the z-order proves here is that the lower
  // one does not paint over the higher one's corner: the whole drawable is the
  // child's, and the page's blue is gone.
  CHECK_EQ(pixelAt(host, 40, 159), std::string("0,255,0"));
  CHECK_EQ(pixelAt(host, 240, 40), std::string("0,255,0"));
}

// --- matrix row: shutdown ---------------------------------------------------------
// The host shuts down mid-play: every instance is terminated before it returns,
// and nothing is delivered afterwards.
void iframeShutdown() {
  test::LogCapture capture;
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));
  tickTree(host, 6);
  CHECK_EQ(static_cast<int>(screenkit::liveInstances(*host.instances).size()), 1);

  screenkit::terminateInstances(*host.instances);
  CHECK(screenkit::liveInstances(*host.instances).empty());
  host.runtime->shutdown();
  host.runtime.reset();
  CHECK(screenkit::liveInstances(*host.instances).empty());
  CHECK(!capture.has(screenkit::LogLevel::Error, "uncaught"));
}

// --- matrix row: a page with no iframe ---------------------------------------------
// The present path, and its cost, are what they were: no compositor is built, no
// second context is made, and a frame that painted nothing still presents
// nothing.
void iframeNoneCosts() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  tickTree(host, 6);
  // The page's own blue, straight out of the default framebuffer: with no
  // compositor there is no frame texture in between.
  CHECK_EQ(pixelAt(host, 160, 90), std::string("0,0,255"));
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  onJsThread(host.runtime, [&](facebook::jsi::Runtime& js) { surface = screenkit::gfx::surfaceFor(js); });
  CHECK(surface != nullptr);
  if (!surface) return;
  CHECK(!surface->compositing());
  CHECK_EQ(static_cast<int>(surface->defaultFramebuffer()), 0);
  CHECK(!surface->layersDirty());
}

// --- matrix row: the child fails at runtime ------------------------------------------
// A package whose entry fails -- at once, or two hundred milliseconds later
// when a dynamic import settles -- has already been evaluated by then: the
// packed wrapper catches it and reports it, so `load` has fired and the failure
// arrives afterwards as `error` on the element. The launcher is untouched
// either way: its own frames keep coming, and what is reported is the child's
// failure, not the page's.
void iframeChildFails() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;

  for (const char* src : {"entry-throws.skpkg", "entry-rejects.skpkg"}) {
    domEval(host.runtime, (std::string("globalThis.__log = []; globalThis.g = embed({ src: '") + src +
                           "' });")
                              .c_str());
    CHECK(pumpTree(host, "__log.indexOf('load') >= 0"));
    const int framesAtLoad = std::atoi(domEval(host.runtime, "String(__pageFrames);").c_str());
    CHECK(pumpTree(host, "__log.indexOf('error') >= 0"));
    const std::string log = domEval(host.runtime, "__log.join(',');");
    CHECK_EQ(log, std::string("load,error"));
    if (log != "load,error") std::fprintf(stderr, "  %s gave %s\n", src, log.c_str());
    // The launcher kept painting through its child's failure.
    CHECK(pumpTree(host, "__pageFrames > " + std::to_string(framesAtLoad)));
    domEval(host.runtime, "g.remove(); g = null;");
    tickTree(host, 2);
  }
  CHECK(screenkit::liveInstances(*host.instances).empty());
}

// --- matrix row: no compositor ---------------------------------------------------
// A runtime with nowhere to put a layer -- a headless one, and every runtime
// that is itself an instance -- refuses the src instead of half-loading it, and
// says which it is. The page keeps painting: an `<iframe>` that cannot be
// embedded is one element that failed, not a page that stopped.
void iframeNoCompositor() {
  auto runtime = domRuntime();
  if (!runtime) return;
  CHECK_EQ(domEval(runtime,
                   "String(__screenkit.instances ? __screenkit.instances.capabilities().canEmbed : 'no binding');"),
           std::string("false"));
  domEval(runtime, R"JS(
globalThis.__log = [];
globalThis.__errors = [];
var f = document.createElement('iframe');
f.addEventListener('load', function () { __log.push('load'); });
f.addEventListener('error', function () { __log.push('error'); });
document.body.appendChild(f);
f.src = 'iframe-child.skpkg';
'embedded';
)JS");
  CHECK(waitForJs(runtime, "__log.length > 0", 10000));
  CHECK_EQ(domEval(runtime, "__log.join(',');"), std::string("error"));
  // The page is still there, and so is the element.
  CHECK_EQ(domEval(runtime, "document.querySelector('iframe') === null ? 'gone' : 'still here';"),
           std::string("still here"));
}

// --- matrix row: input follows focus -------------------------------------------------
// The milestone's own headline: a launcher hands the remote to the game it
// embeds and the game gets the keys -- not the launcher, which is frozen by
// then and would drop them. The router is the shipping one, wired to the same
// focus target the host wires (`Host.cpp`), so a key that stops following focus
// fails here rather than on a television.
void iframeInput() {
  IframeHost host = iframeHost();
  if (!host) return;
  if (!iframePrelude(host)) return;
  domEval(host.runtime,
          "globalThis.__pageKeys = [];"
          "window.addEventListener('keydown', function (e) { __pageKeys.push(e.key); });"
          "'listening';");

  screenkit::InputRouter router(host.runtime);
  router.setFocusTarget(screenkit::focusTarget(*host.instances));
  const auto press = [&](SDL_Scancode scancode) {
    SDL_Event down{};
    down.type = SDL_EVENT_KEY_DOWN;
    down.key.scancode = scancode;
    down.key.down = true;
    CHECK(router.handleEvent(down));
    SDL_Event up = down;
    up.type = SDL_EVENT_KEY_UP;
    up.key.down = false;
    CHECK(router.handleEvent(up));
  };

  // Unfocused: the launcher's own page hears it.
  press(SDL_SCANCODE_UP);
  CHECK(pumpTree(host, "__pageKeys.length > 0"));
  CHECK_EQ(domEval(host.runtime, "__pageKeys.join(',');"), std::string("ArrowUp"));

  domEval(host.runtime, "globalThis.f = embed();");
  CHECK(pumpTree(host, "said('ready') !== null"));
  tickTree(host, 4);

  // The game is asked to hand the remote back after sixty of its own frames,
  // and only then does the launcher hand it over and freeze itself -- nothing
  // in this page may be evaluated again until it thaws (`iframe-focus`).
  domEval(host.runtime,
          "f.contentWindow.postMessage({ ask: 'focus-parent-after', frames: 60 });"
          "f.focus();");
  double tick = 0;
  const auto frozenBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!host.runtime->paused() && std::chrono::steady_clock::now() < frozenBy) {
    tickTree(host, tick);
    hostWait(2);
  }
  CHECK(host.runtime->paused());
  router.tick(SDL_GetTicksNS());

  // Focused: this key belongs to the game. The launcher is frozen, so a key it
  // received instead would be dropped and lost rather than merely misplaced.
  press(SDL_SCANCODE_DOWN);

  const auto thawedBy = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (host.runtime->paused() && std::chrono::steady_clock::now() < thawedBy) {
    tickTree(host, tick);
    hostWait(2);
  }
  CHECK(!host.runtime->paused());
  router.tick(SDL_GetTicksNS());

  // What the game saw, reported through the only channel there is.
  CHECK(pumpTree(host, "said('key') !== null"));
  CHECK_EQ(domEval(host.runtime, "said('key').key;"), std::string("ArrowDown"));
  // ...and what the launcher saw: only its own key, from before the switch.
  CHECK_EQ(domEval(host.runtime, "__pageKeys.join(',');"), std::string("ArrowUp"));

  // ...and back: the launcher has the remote again.
  press(SDL_SCANCODE_LEFT);
  CHECK(pumpTree(host, "__pageKeys.length > 1"));
  CHECK_EQ(domEval(host.runtime, "__pageKeys.join(',');"), std::string("ArrowUp,ArrowLeft"));
}

// ============================================================================
// Composited canvases (spec-m6-composited-canvases.md, Architecture.md 3.1)
//
// Every canvas that takes a WebGL context gets a real GL context of its own,
// and -- unless it is the page's frame -- an FBO-backed layer the page's own
// present composites at the element's CSS rect, through the same `LayerList` an
// `<iframe>` instance's layer goes through.
//
// Every row runs against a real ANGLE context on an offscreen pbuffer, the same
// way the `iframe-*` rows do, and the ones that matter most are pixel reads:
// the composite, the z-order and the blend are none of them visible from JS.
// ============================================================================

struct CanvasHost {
  std::shared_ptr<screenkit::Runtime> runtime;
  /// How many frames this page has actually presented, counted inside the
  /// present itself.
  std::shared_ptr<std::atomic<int>> presents = std::make_shared<std::atomic<int>>(0);
  explicit operator bool() const { return runtime != nullptr; }
};

/// A page: `gl`, the prelude, and the present `startGraphics` hangs off the end
/// of the frame -- without which nothing is ever composited.
CanvasHost canvasHost(int width = 320, int height = 180) {
  CanvasHost host;
  host.runtime = domRuntime(width, height);
  if (!host.runtime) return host;
  std::shared_ptr<screenkit::gfx::GlSurface> surface;
  onJsThread(host.runtime, [&](facebook::jsi::Runtime& js) { surface = screenkit::gfx::surfaceFor(js); });
  CHECK(surface != nullptr);
  if (!surface) {
    host.runtime.reset();
    return host;
  }
  auto presents = host.presents;
  CHECK(host.runtime->setFrameFinishedCallback([surface, presents](facebook::jsi::Runtime& js) {
    if (screenkit::gfx::presentFrame(js, *surface)) presents->fetch_add(1);
  }));
  return host;
}

void tickCanvas(const CanvasHost& host, int frames) {
  static double tick = 0;
  for (int i = 0; i < frames; ++i) {
    host.runtime->tickFrame(tick);
    tick += 1000.0 / 60.0;
    hostWait(2);
  }
}

/// One pixel of what the page presented, in GL coordinates (y up), read on the
/// JS thread where its context is current.
std::string canvasPixel(const CanvasHost& host, int x, int y) {
  std::string out;
  onJsThread(host.runtime, [&](facebook::jsi::Runtime& js) {
    std::shared_ptr<screenkit::gfx::GlSurface> surface = screenkit::gfx::surfaceFor(js);
    screenkit::gfx::makeContextCurrent(screenkit::gfx::primaryContext(js));
    unsigned char px[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    // Put the page's own frame back: once it composites, the page draws into a
    // texture, and leaving 0 bound would send its next paint to the drawable.
    if (surface) glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(surface->defaultFramebuffer()));
    out = std::to_string(px[0]) + "," + std::to_string(px[1]) + "," + std::to_string(px[2]);
  });
  return out;
}

/// The page a launcher with a HUD is: a blue frame every tick, and helpers for
/// making a canvas the CSS subset places and painting it.
const char* kCanvasPrelude = R"JS(
globalThis.page = document.createElement('canvas');
document.body.appendChild(page);
globalThis.pg = page.getContext('webgl');
globalThis.pageFrames = 0;
globalThis.paintPage = true;
(function paint() {
  if (paintPage) {
    pg.clearColor(0, 0, 1, 1);
    pg.clear(pg.COLOR_BUFFER_BIT);
    pageFrames++;
  }
  requestAnimationFrame(paint);
})();
globalThis.place = function (css) {
  var c = document.createElement('canvas');
  c.style.cssText = css;
  document.body.appendChild(c);
  return c;
};
// Clear the whole buffer transparent, then fill `half` of it (0 = none,
// 1 = the left half in CSS terms, 2 = all of it) with `colour`.
globalThis.paintLayer = function (g, colour, half) {
  g.clearColor(0, 0, 0, 0);
  g.clear(g.COLOR_BUFFER_BIT);
  if (half === 0) return;
  if (half === 1) {
    g.enable(g.SCISSOR_TEST);
    g.scissor(0, 0, Math.round(g.drawingBufferWidth / 2), g.drawingBufferHeight);
  }
  g.clearColor(colour[0], colour[1], colour[2], 1);
  g.clear(g.COLOR_BUFFER_BIT);
  g.disable(g.SCISSOR_TEST);
};
// Stops rescheduling, rather than merely stopping painting: a rAF callback that
// keeps re-arming is a root, and it would hold the context -- and through
// `gl.canvas` the canvas -- alive for good.
globalThis.keepPainting = function (g, colour, half) {
  (function again() {
    if (g.__stop === true) return;
    paintLayer(g, colour, half);
    requestAnimationFrame(again);
  })();
};
// A linked program, so `CURRENT_PROGRAM` is a real answer rather than an
// INVALID_OPERATION that leaves the state alone.
globalThis.program = function (g) {
  var v = g.createShader(g.VERTEX_SHADER);
  g.shaderSource(v, 'attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }');
  g.compileShader(v);
  var f = g.createShader(g.FRAGMENT_SHADER);
  g.shaderSource(f, 'precision mediump float; void main(){ gl_FragColor = vec4(1.0); }');
  g.compileShader(f);
  var p = g.createProgram();
  g.attachShader(p, v);
  g.attachShader(p, f);
  g.linkProgram(p);
  return p;
};
'canvas prelude';
)JS";

bool canvasPrelude(const CanvasHost& host) {
  const auto result = host.runtime->evaluateSource(kCanvasPrelude, "canvas-prelude.js");
  CHECK(result.ok);
  if (!result.ok) std::fprintf(stderr, "  prelude: %s\n", result.error.c_str());
  return result.ok;
}

// --- matrix row: state isolation -----------------------------------------------
// Two canvases, two real GL contexts: each keeps the program, the texture and
// the blend state it bound, and neither sees the other's. Read back, not drawn:
// a pixel could be right for the wrong reason.
void canvasIsolation() {
  test::LogCapture capture;
  CanvasHost host = canvasHost();
  if (!host) return;
  if (!canvasPrelude(host)) return;

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.a = place('position:absolute; left:0; top:0; width:100px; height:100px');"
                   "globalThis.b = place('position:absolute; left:120px; top:0; width:100px; height:100px');"
                   "globalThis.ga = a.getContext('webgl');"
                   "globalThis.gb = b.getContext('webgl');"
                   "[ga !== null, gb !== null, ga !== gb, ga !== pg, gb !== pg].join(',');"),
           std::string("true,true,true,true,true"));

  // Each binds its own. The interleaving is deliberate: every one of these
  // calls is a context switch, which is the thing that could go wrong.
  CHECK_EQ(domEval(host.runtime,
                   "globalThis.pa = program(ga); globalThis.pb = program(gb);"
                   "globalThis.ta = ga.createTexture(); globalThis.tb = gb.createTexture();"
                   "ga.useProgram(pa); gb.useProgram(pb);"
                   "ga.bindTexture(ga.TEXTURE_2D, ta); gb.bindTexture(gb.TEXTURE_2D, tb);"
                   "ga.enable(ga.BLEND); gb.disable(gb.BLEND);"
                   "ga.clearColor(1, 0, 0, 1); gb.clearColor(0, 1, 0, 1);"
                   "[ga.getParameter(ga.CURRENT_PROGRAM).id === pa.id,"
                   " gb.getParameter(gb.CURRENT_PROGRAM).id === pb.id,"
                   " ga.getParameter(ga.TEXTURE_BINDING_2D).id === ta.id,"
                   " gb.getParameter(gb.TEXTURE_BINDING_2D).id === tb.id,"
                   " ga.getParameter(ga.BLEND), gb.getParameter(gb.BLEND),"
                   " ga.getParameter(ga.COLOR_CLEAR_VALUE).join(' '),"
                   " gb.getParameter(gb.COLOR_CLEAR_VALUE).join(' ')].join('|');"),
           // getParameter(BLEND) answers 1/0 here, not true/false.
           std::string("true|true|true|true|1|0|1 0 0 1|0 1 0 1"));

  // ...and the page's own frame, which is a third context, kept its own too.
  CHECK_EQ(domEval(host.runtime, "pg.getParameter(pg.COLOR_CLEAR_VALUE).join(' ');"),
           std::string("0 0 1 1"));
  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// --- matrix row: placed by CSS, and a size write -----------------------------------
// The CSS subset places a canvas instead of warning that it cannot: the rect is
// where it composites, and its pixels are what `canvas.width` reads. A size
// write is honoured for it, and still ignored for the canvas that is the frame.
void canvasPlaced() {
  test::LogCapture capture;
  CanvasHost host = canvasHost(320, 180);
  if (!host) return;
  if (!canvasPrelude(host)) return;

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.hud = place('position:absolute; left:10%; top:10%; width:50%; height:50%');"
                   "globalThis.hg = hud.getContext('webgl');"
                   "var r = hud.getBoundingClientRect();"
                   "[hud.width + 'x' + hud.height, hg.drawingBufferWidth + 'x' + hg.drawingBufferHeight,"
                   " r.x + ',' + r.y + ',' + r.width + ',' + r.height].join('|');"),
           std::string("160x90|160x90|32,18,160,90"));

  // A size write on a placed canvas gives it that drawing buffer, and
  // `gl.drawingBufferWidth` agrees -- they are the same number here too.
  CHECK_EQ(domEval(host.runtime,
                   "hud.width = 640; hud.height = 360;"
                   "var r = hud.getBoundingClientRect();"
                   "[hud.width + 'x' + hud.height, hg.drawingBufferWidth + 'x' + hg.drawingBufferHeight,"
                   " r.width + 'x' + r.height].join('|');"),
           std::string("640x360|640x360|160x90"));

  // Resizing a drawing buffer does not reset the GL state the page set, and
  // leaves the framebuffer binding where it was -- a browser's canvas does not
  // even reset the viewport when it is resized.
  //
  // The resize has to build a new frame target, which means binding a
  // framebuffer and clearing it past whatever the page left set, so the state
  // it has to put back is more than the clear colour: a framebuffer the page
  // itself bound (render-to-texture is the common case), the colour mask and
  // the scissor enable are all touched on the way through and all have to come
  // out the way they went in. `getParameter` mints a fresh wrapper for a bound
  // object, so the framebuffer is compared by id, as `canvas-isolation` does
  // for the program and texture bindings; the writemask comes back as 1/0 for
  // the same reason `BLEND` does there.
  CHECK_EQ(domEval(host.runtime,
                   "hg.clearColor(0.25, 0.5, 0.75, 1); hg.viewport(1, 2, 3, 4);"
                   "globalThis.fb = hg.createFramebuffer();"
                   "hg.bindFramebuffer(hg.FRAMEBUFFER, fb);"
                   "hg.colorMask(true, true, true, false); hg.enable(hg.SCISSOR_TEST);"
                   "hud.width = 320;"
                   "var bound = hg.getParameter(hg.FRAMEBUFFER_BINDING);"
                   "[hg.getParameter(hg.COLOR_CLEAR_VALUE).join(' '),"
                   " hg.getParameter(hg.VIEWPORT).join(' '), hg.drawingBufferWidth,"
                   " bound !== null && bound.id === fb.id,"
                   " hg.getParameter(hg.COLOR_WRITEMASK).join(' '),"
                   " hg.isEnabled(hg.SCISSOR_TEST)].join('|');"),
           std::string("0.25 0.5 0.75 1|1 2 3 4|320|true|1 1 1 0|true"));
  // Back to the canvas's own frame, so nothing below here draws or reads
  // through the page's framebuffer by accident.
  domEval(host.runtime,
          "hg.bindFramebuffer(hg.FRAMEBUFFER, null);"
          "hg.colorMask(true, true, true, true); hg.disable(hg.SCISSOR_TEST); 'restored';");

  // The canvas that is the frame is the drawable: the write is accepted,
  // changes nothing, and says so once -- exactly as before this feature.
  CHECK_EQ(domEval(host.runtime, "page.width = 640; page.width + 'x' + page.height;"),
           std::string("320x180"));
  CHECK(capture.has(screenkit::LogLevel::Warn, "was ignored"));

  // A rect that computes to nothing composites nothing, and allocates nothing
  // beyond the smallest framebuffer a context needs to be complete.
  CHECK_EQ(domEval(host.runtime,
                   "globalThis.zero = place('position:absolute; left:0; top:0; width:0px; height:50%');"
                   "globalThis.zg = zero.getContext('webgl');"
                   "[zg !== null, zero.width + 'x' + zero.height].join(',');"),
           std::string("true,1x90"));
  tickCanvas(host, 4);
  CHECK_EQ(canvasPixel(host, 2, 178), std::string("0,0,255"));
}

// --- matrix row: two canvases composited in z-order --------------------------------
// The pixels are the assertion. A low canvas, a high one overlapping it, and the
// high one's transparent half showing the low one through -- with the page's own
// frame under both.
void canvasComposite() {
  test::LogCapture capture;
  CanvasHost host = canvasHost(320, 180);
  if (!host) return;
  if (!canvasPrelude(host)) return;

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.low = place('position:absolute; left:0; top:0; width:160px; height:90px; z-index:1');"
                   "globalThis.high = place('position:absolute; left:80px; top:0; width:160px; height:90px; z-index:2');"
                   "globalThis.lg = low.getContext('webgl');"
                   "globalThis.hg = high.getContext('webgl');"
                   // The high one is made second, so only `z-index` can put it
                   // on top -- but it would be on top by insertion order too.
                   // Give the low one the higher document position to be sure.
                   "keepPainting(lg, [1, 0, 0], 2);"
                   "keepPainting(hg, [0, 1, 0], 1);"
                   "[lg !== null, hg !== null].join(',');"),
           std::string("true,true"));
  tickCanvas(host, 10);

  // The low canvas alone: its red.
  CHECK_EQ(canvasPixel(host, 40, 135), std::string("255,0,0"));
  // Both, and the high one painted there: its green wins.
  CHECK_EQ(canvasPixel(host, 120, 135), std::string("0,255,0"));
  // The high one's transparent half, with nothing under it but the page.
  CHECK_EQ(canvasPixel(host, 200, 135), std::string("0,0,255"));
  // Neither: the page's own blue, still drawn around them.
  CHECK_EQ(canvasPixel(host, 280, 40), std::string("0,0,255"));
  CHECK(host.presents->load() > 0);

  // A layer's plane follows CSS after `getContext` has already run. Every other
  // row here styles the canvas before asking for the context, so the plane is
  // only ever computed once; this moves one that is already compositing, which
  // is the shape a HUD that slides or reflows actually takes.
  domEval(host.runtime, "high.style.left = '160px'; 'moved';");
  tickCanvas(host, 6);
  // Where the high one was: its green is gone, and the low one's red -- which
  // was under it all along -- is what shows.
  CHECK_EQ(canvasPixel(host, 120, 135), std::string("255,0,0"));
  // Where it is now: its painted half, over nothing but the page.
  CHECK_EQ(canvasPixel(host, 200, 135), std::string("0,255,0"));

  // `display:none` takes the plane out of the composite entirely; the layer is
  // still there and still painting, so this is the visibility flag and not a
  // release.
  domEval(host.runtime, "high.style.display = 'none'; 'hidden';");
  tickCanvas(host, 6);
  CHECK_EQ(canvasPixel(host, 200, 135), std::string("0,0,255"));

  // Opacity is the compositor's `alpha` uniform, and nothing else asserts it:
  // a half-transparent canvas over the page's blue has to come out as the
  // blend of the two, not as the colour the canvas painted.
  domEval(host.runtime,
          "globalThis.fade = place('position:absolute; left:240px; top:0; width:80px; height:90px;"
          " z-index:3; opacity:0.5');"
          "globalThis.fg = fade.getContext('webgl');"
          "keepPainting(fg, [1, 0, 0], 2);");
  tickCanvas(host, 8);
  // 0.5 * red over 0.5 * the page's blue. Raw red here would mean the uniform
  // never reached the shader.
  CHECK_EQ(canvasPixel(host, 280, 135), std::string("128,0,128"));

  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// --- matrix row: an idle canvas ------------------------------------------------------
// One canvas paints this frame and the other does not. The idle one keeps its
// last image -- "present only a frame that painted" is per layer -- and the
// frame still presents.
void canvasIdle() {
  CanvasHost host = canvasHost(320, 180);
  if (!host) return;
  if (!canvasPrelude(host)) return;

  domEval(host.runtime,
          "globalThis.hud = place('position:absolute; left:0; top:0; width:160px; height:90px');"
          "globalThis.hg = hud.getContext('webgl');"
          "keepPainting(hg, [0, 1, 0], 2);");
  tickCanvas(host, 8);
  CHECK_EQ(canvasPixel(host, 40, 135), std::string("0,255,0"));

  // It stops. The page keeps painting, so frames keep being presented, and the
  // idle canvas's last image is still in them.
  domEval(host.runtime, "hg.__stop = true; 'stopped';");
  const int before = host.presents->load();
  tickCanvas(host, 20);
  CHECK(host.presents->load() > before);
  CHECK_EQ(canvasPixel(host, 40, 135), std::string("0,255,0"));
  CHECK_EQ(canvasPixel(host, 280, 40), std::string("0,0,255"));

  // The mirror, which nothing covered: the page goes idle and the canvas keeps
  // painting. A frame where the page drew nothing still has to present, because
  // a layer is dirty -- the `!painted && layersDirty()` arm of presentFrame.
  // Drop that and a HUD-only update is silently thrown away every frame the
  // page happens not to redraw.
  domEval(host.runtime,
          "paintPage = false; hg.__stop = false; keepPainting(hg, [1, 0, 1], 2); 'swapped';");
  const int quiet = host.presents->load();
  const std::string stoppedAt = domEval(host.runtime, "pageFrames;");
  tickCanvas(host, 12);
  // The page really did go idle over those frames, so the presents below are
  // the layer's doing and not the page quietly still painting.
  CHECK_EQ(domEval(host.runtime, "pageFrames;"), stoppedAt);
  CHECK(host.presents->load() > quiet);
  // The canvas's new colour reached the page's framebuffer...
  CHECK_EQ(canvasPixel(host, 40, 135), std::string("255,0,255"));
  // ...and the page's own last frame is still under it, retained rather than
  // recleared, because the page never painted again.
  CHECK_EQ(canvasPixel(host, 280, 40), std::string("0,0,255"));
}

// --- matrix row: removal ---------------------------------------------------------
// A canvas removed from the document and dropped takes its layer and its GL
// context with it, and the page keeps painting.
void canvasRemove() {
  test::LogCapture capture;
  CanvasHost host = canvasHost(320, 180);
  if (!host) return;
  if (!canvasPrelude(host)) return;

  // Five times over, not once: a context or a layer that leaks only on the
  // second cycle onwards -- a registry entry the release does not erase, say --
  // reads as a clean pass when the row makes exactly one canvas. The count has
  // to come back to the page's own every single time.
  for (int cycle = 0; cycle < 5; ++cycle) {
    domEval(host.runtime,
            "globalThis.hud = place('position:absolute; left:0; top:0; width:160px; height:90px');"
            "globalThis.hg = hud.getContext('webgl');"
            "keepPainting(hg, [0, 1, 0], 2);");
    tickCanvas(host, 8);
    CHECK_EQ(canvasPixel(host, 40, 135), std::string("0,255,0"));
    CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 2);

    // Removed: the layer is not composited any more, and the page is what shows.
    domEval(host.runtime, "hg.__stop = true; hud.remove(); 'removed';");
    tickCanvas(host, 6);
    CHECK_EQ(canvasPixel(host, 40, 135), std::string("0,0,255"));

    // Dropped: the context goes with it. Hades finalizes concurrently, so this
    // collects until the count falls rather than trusting one collectGarbage.
    domEval(host.runtime, "globalThis.hud = null; globalThis.hg = null; 'dropped';");
    for (int i = 0; i < 40 && screenkit::gfx::contextCount() > 1; ++i) {
      onJsThread(host.runtime, [](facebook::jsi::Runtime& js) {
        js.instrumentation().collectGarbage("canvas-remove");
      });
      tickCanvas(host, 2);
    }
    CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 1);
  }
  // ...and the page is still drawing, on its own context, into its own frame.
  const int before = host.presents->load();
  tickCanvas(host, 6);
  CHECK(host.presents->load() > before);
  CHECK_EQ(canvasPixel(host, 40, 135), std::string("0,0,255"));
  CHECK(!capture.has(screenkit::LogLevel::Warn, "destroyed off the thread"));
  for (const auto& line : capture.lines()) CHECK(line.level != screenkit::LogLevel::Error);
}

// --- matrix row: context loss on teardown ----------------------------------------
// A runtime that shuts down with three canvases releases every context and
// every layer, on the thread that owns them, before shutdown returns.
void canvasTeardown() {
  test::LogCapture capture;
  CanvasHost host = canvasHost(320, 180);
  if (!host) return;
  if (!canvasPrelude(host)) return;

  domEval(host.runtime,
          "globalThis.one = place('position:absolute; left:0; top:0; width:80px; height:45px');"
          "globalThis.two = place('position:absolute; left:80px; top:0; width:80px; height:45px');"
          "globalThis.g1 = one.getContext('webgl');"
          "globalThis.g2 = two.getContext('webgl');"
          "keepPainting(g1, [1, 0, 0], 2); keepPainting(g2, [0, 1, 0], 2);");
  tickCanvas(host, 6);
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 3);

  host.runtime->shutdown();
  host.runtime.reset();
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), 0);
  // ~GlSurface says so when it runs anywhere but on the thread its context was
  // current on, which is the failure this ordering exists to prevent.
  CHECK(!capture.has(screenkit::LogLevel::Warn, "destroyed off the thread"));
}

// --- matrix row: a HUD canvas over an <iframe> ------------------------------------
// One layer list: a canvas layer and an instance layer sort together by
// `z-index`, so a launcher may put its own HUD over the game it embeds.
void canvasOverIframe() {
  test::LogCapture capture;
  IframeHost host = iframeHost(320, 180);
  if (!host) return;
  if (!iframePrelude(host)) return;

  domEval(host.runtime,
          "globalThis.f = embed({ css: 'position: absolute; left: 0; top: 0; width: 160px; "
          "height: 90px; z-index: 1' });");
  CHECK(pumpTree(host, "__log.indexOf('load') >= 0 && said('ready') !== null"));

  CHECK_EQ(domEval(host.runtime,
                   "globalThis.hud = document.createElement('canvas');"
                   "hud.style.cssText = 'position:absolute; left:0; top:0; width:80px; height:90px; z-index:2';"
                   "document.body.appendChild(hud);"
                   "globalThis.hg = hud.getContext('webgl');"
                   "(function paint() {"
                   "  hg.clearColor(1, 0, 0, 1); hg.clear(hg.COLOR_BUFFER_BIT);"
                   "  requestAnimationFrame(paint);"
                   "})();"
                   "hg !== null;"),
           std::string("true"));
  tickTree(host, 14);

  // The HUD, over the game.
  CHECK_EQ(pixelAt(host, 40, 159), std::string("255,0,0"));
  // The game, where the HUD is not.
  CHECK_EQ(pixelAt(host, 120, 159), std::string("0,255,0"));
  // The launcher's own blue around both.
  CHECK_EQ(pixelAt(host, 240, 40), std::string("0,0,255"));

  // An instance's plane follows the element's CSS after it has loaded and is
  // already compositing, and it follows a declaration being *removed* as well
  // as one being set -- `removeProperty` has to put the element back on its
  // static position rather than leave the last value cached in the plane.
  // Nothing else in the suite removes a declaration from an <iframe>.
  domEval(host.runtime, "f.style.left = '160px'; 'moved';");
  tickTree(host, 8);
  // The game moved off the HUD's half and onto the right-hand side.
  CHECK_EQ(pixelAt(host, 200, 159), std::string("0,255,0"));
  CHECK_EQ(pixelAt(host, 120, 159), std::string("0,0,255"));

  domEval(host.runtime, "f.style.removeProperty('left'); 'back';");
  tickTree(host, 8);
  // ...and back where it started, with the HUD over it again.
  CHECK_EQ(pixelAt(host, 120, 159), std::string("0,255,0"));
  CHECK_EQ(pixelAt(host, 200, 159), std::string("0,0,255"));
  CHECK_EQ(pixelAt(host, 40, 159), std::string("255,0,0"));

  CHECK(!capture.has(screenkit::LogLevel::Error, "failed"));
}

// --- matrix row: z-order with video --------------------------------------------
// A page with two canvas layers and a <video>. The plane is a platform plane
// beneath every canvas whatever its z-index -- so a video asked to sit above one
// still says so, and now `aboveACanvas` has to see canvases that are layers of
// their own, not just the page's drawable.
//
// The player is scripted (`media-element-model` does the same): a real one would
// need a stream, and what is under test is the geometry, not playback.
void canvasOverVideo() {
  test::LogCapture capture;
  CanvasHost host = canvasHost();
  if (!host) return;
  if (!canvasPrelude(host)) return;

  CHECK_EQ(domEval(host.runtime, R"JS(
globalThis.__planes = [];
__screenkit.media = {
  onevent: __screenkit.media ? __screenkit.media.onevent : function () {},
  capabilities: function () {
    return { available: true, platform: 'script', hls: false, dash: false, progressive: true,
             keySystems: [], containers: ['video/mp4'], codecs: ['avc1'], videoOutput: true,
             videoOutputProblem: '' };
  },
  create: function () { return 7; },
  load: function () { return 1; },
  setPlane: function (id, x, y, w, h, visible, z) {
    __planes.push([Math.round(x), Math.round(y), Math.round(w), Math.round(h), visible, z].join(' '));
  },
  play: function () {}, pause: function () {}, seek: function () {}, release: function () {},
  setVolume: function () {}, setMuted: function () {}, setLoop: function () {}, setRate: function () {},
};
// A HUD canvas of its own, and the video beneath everything, as it must be.
globalThis.hud = place('position:absolute; left:0; top:0; width:50%; height:50%; z-index: 4');
globalThis.hg = hud.getContext('webgl');
globalThis.clip = document.createElement('video');
clip.style.cssText = 'position:absolute; left:0; top:0; width:100%; height:100%; z-index: -1';
document.body.appendChild(clip);
clip.src = 'clip.mp4';
'staged';
)JS"),
           std::string("staged"));
  CHECK(waitForJs(host.runtime, "__planes.length > 0", 4000));
  // Beneath both canvases: nothing to say.
  CHECK_EQ(domEval(host.runtime, "__planes[__planes.length - 1];"), std::string("0 0 320 180 true -1"));
  CHECK(!capture.has(screenkit::LogLevel::Warn, "composited beneath"));

  // Sent above the HUD, which is a layer and not the drawable: still beneath,
  // and said once.
  CHECK_EQ(domEval(host.runtime, "clip.style.zIndex = '9'; 'raised';"), std::string("raised"));
  CHECK(waitForJs(host.runtime, "__planes[__planes.length - 1].indexOf(' 9') > 0", 4000));
  CHECK(capture.has(screenkit::LogLevel::Warn, "composited beneath"));

  // The plane never became a layer: the canvases are the only ones, and the
  // composite is still the HUD over the page.
  CHECK_EQ(domEval(host.runtime,
                   "paintLayer(hg, [1, 0, 0], 2); "
                   "[typeof __screenkit.canvas.create, clip.getContext === undefined].join(',');"),
           std::string("function,true"));
  tickCanvas(host, 6);
  CHECK_EQ(canvasPixel(host, 40, 178), std::string("255,0,0"));
  CHECK_EQ(canvasPixel(host, 240, 40), std::string("0,0,255"));
}

// --- matrix row: a 2D canvas ---------------------------------------------------
// `getContext('2d')` on a second canvas is what it was before this feature: CPU
// pixels, usable as a texture source, and no layer -- the compositor knows only
// about WebGL canvases (a frozen decision of this spec).
void canvas2dNoLayer() {
  test::LogCapture capture;
  CanvasHost host = canvasHost();
  if (!host) return;
  if (!canvasPrelude(host)) return;

  const int before = static_cast<int>(screenkit::gfx::contextCount());
  CHECK_EQ(domEval(host.runtime,
                   "globalThis.flat = place('position:absolute; left:0; top:0; width:50%; height:50%');"
                   "globalThis.fx = flat.getContext('2d');"
                   "fx.fillStyle = '#00ff00'; fx.fillRect(0, 0, flat.width, flat.height);"
                   "var px = fx.getImageData(1, 1, 1, 1).data;"
                   "[fx !== null, flat.getContext('webgl') === null,"
                   " [px[0], px[1], px[2], px[3]].join(' ')].join('|');"),
           std::string("true|true|0 255 0 255"));
  // No GL context was made for it, so there is no layer either.
  CHECK_EQ(static_cast<int>(screenkit::gfx::contextCount()), before);

  // Still a texture source for a context that does draw.
  CHECK_EQ(domEval(host.runtime,
                   "var t = pg.createTexture();"
                   "pg.bindTexture(pg.TEXTURE_2D, t);"
                   "pg.texImage2D(pg.TEXTURE_2D, 0, pg.RGBA, pg.RGBA, pg.UNSIGNED_BYTE, flat);"
                   "String(pg.getError());"),
           std::string("0"));

  // And it composites nothing: where it sits, the page's own blue is what the
  // screen gets.
  tickCanvas(host, 6);
  CHECK_EQ(canvasPixel(host, 40, 178), std::string("0,0,255"));
  CHECK(!capture.has(screenkit::LogLevel::Error, "canvas"));
}

// --- matrix row: too many ------------------------------------------------------
// The web's answer to a driver that will not give another context is `null` from
// `getContext`, not an exception. Two halves: the native refusal (a runtime with
// no drawable to composite over is the one refusal a test can arrange), and the
// shim's own handling of a null, which is what a page actually meets.
void canvasRefused() {
  {
    test::LogCapture capture;
    CanvasHost host = canvasHost();
    if (!host) return;
    if (!canvasPrelude(host)) return;

    // The driver, refusing.
    CHECK_EQ(domEval(host.runtime,
                     "__screenkit.canvas.create = function () { return null; };"
                     "globalThis.a = place('position:absolute; left:0; top:0; width:50%; height:50%');"
                     "globalThis.b = place('position:absolute; left:50%; top:0; width:50%; height:50%');"
                     "var ga, gb, threw = 'no';"
                     "try { ga = a.getContext('webgl'); gb = b.getContext('webgl'); }"
                     "catch (e) { threw = String(e); }"
                     "[threw, ga === null, gb === null].join(',');"),
             std::string("no,true,true"));
    // Said once, however many canvases ask.
    int said = 0;
    for (const auto& line : capture.lines()) {
      if (line.level == screenkit::LogLevel::Warn && line.message.find("returned null") != std::string::npos) ++said;
    }
    CHECK_EQ(said, 1);

    // The page itself is untouched: it keeps its context and keeps painting.
    tickCanvas(host, 6);
    CHECK_EQ(domEval(host.runtime, "pg !== null && pageFrames > 0 ? 'painting' : 'stopped';"),
             std::string("painting"));
    CHECK_EQ(canvasPixel(host, 160, 90), std::string("0,0,255"));
  }

  // The native side, refusing for real: no drawable, so no second context to
  // give -- and `create` answers null rather than throwing.
  test::LogCapture capture;
  auto config = testConfig();
  config.name = "canvas";
  auto runtime = screenkit::Runtime::create(std::move(config));
  CHECK(runtime != nullptr);
  if (!runtime) return;
  if (!installDomShim(runtime)) return;
  CHECK_EQ(domEval(runtime,
                   "var c = document.createElement('canvas');"
                   "document.body.appendChild(c);"
                   "var made = 'threw';"
                   "try { made = String(__screenkit.canvas.create(c, 32, 32)); } catch (e) { made = 'threw: ' + e; }"
                   "made;"),
           std::string("null"));
  CHECK(capture.has(screenkit::LogLevel::Warn, "no drawable"));
  // A page in a host with no graphics at all is a different thing, and keeps the
  // answer it had before this feature: `getContext` throws, loudly, because the
  // host is misconfigured -- a driver that will not give one more context is not.
  CHECK_EQ(domEval(runtime,
                   "var threw = 'no';"
                   "try { c.getContext('webgl'); } catch (e) { threw = 'threw'; }"
                   "threw;"),
           std::string("threw"));
}

const std::map<std::string, std::function<void()>>& cases() {
  static const std::map<std::string, std::function<void()>> kCases = {
      {"valid-bytecode", validBytecode},
      {"source-fallback", sourceFallback},
      {"block-scoping", blockScoping},
      {"float-bit-patterns", floatBitPatterns},
      {"garbage-source", garbageSource},
      {"version-mismatch", versionMismatch},
      {"corrupt-bytecode", corruptBytecode},
      {"truncated-header", truncatedHeader},
      {"js-throws", jsThrows},
      {"teardown-race", teardownRace},
      {"evaluate-cancelled-by-shutdown", evaluateCancelledByShutdown},
      {"evaluate-from-js-thread", evaluateFromJsThread},
      {"callback-throws", callbackThrows},
      {"console-levels", consoleLevels},
      {"bytecode-version-pin", bytecodeVersionPin},
      {"timers-ordering", timersOrdering},
      {"timers-cancel", timersCancel},
      {"timers-freeze", timersFreeze},
      {"missing-bundle", missingBundle},
      {"gl-context", glContext},
      {"gl-shader-error", glShaderError},
      {"gl-link-error", glLinkError},
      {"gl-typed-array", glTypedArray},
      {"gl-typed-array-shadowed", glTypedArrayShadowed},
      {"gl-error-passthrough", glErrorPassthrough},
      {"gl-triangle", glTriangle},
      {"gl-teardown", glTeardown},
      {"gl-shared-context", glSharedContext},
      {"gl-pixel-store", glPixelStore},
      {"gl-frame-stats", glFrameStats},
      {"frame-pacing-interval", framePacingInterval},
      {"frame-pacing-deadline", framePacingDeadline},
      {"dom-canvas-create", domCanvasCreate},
      {"dom-gl-handoff", domGlHandoff},
      {"dom-webgl2", domWebgl2},
      {"dom-dimensions", domDimensions},
      {"dom-dimension-write", domDimensionWrite},
      {"dom-second-canvas", domSecondCanvas},
      {"dom-unknown-element", domUnknownElement},
      {"dom-loader-tolerance", domLoaderTolerance},
      {"dom-environment", domEnvironment},
      {"dom-events", domEvents},
      {"dom-encoding", domEncoding},
      {"dom-asset-confinement", domAssetConfinement},
      {"dom-texture-chain", domTextureChain},
      {"dom-fetch", domFetch},
      {"dom-image-element", domImageElement},
      {"dom-fonts", domFonts},
      {"dom-identity-and-absence", domIdentityAndAbsence},
      {"dom-coverage", domCoverage},
      {"dom-root-lookup", domRootLookup},
      {"dom-unsupported-context", domUnsupportedContext},
      {"dom-canvas-fixture", domCanvasFixture},
      {"dom-present-frame", domPresentFrame},
      {"input-keyboard-map", inputKeyboardMap},
      {"input-gamepad-map", inputGamepadMap},
      {"input-router-dispatch", inputRouterDispatch},
      {"input-event-loop", inputEventLoop},
      {"gamepad-startup", gamepadStartup},
      {"gamepad-button", gamepadButton},
      {"gamepad-trigger", gamepadTrigger},
      {"gamepad-stick", gamepadStick},
      {"gamepad-slots", gamepadSlots},
      {"gamepad-disconnect", gamepadDisconnect},
      {"gamepad-paused", gamepadPaused},
      {"gamepad-no-subsystem", gamepadNoSubsystem},
      {"gamepad-app-writes", gamepadAppWrites},
      {"gamepad-claim", gamepadClaim},
      {"gamepad-rumble", gamepadRumble},
      {"gamepad-phaser-poll", gamepadPhaserPoll},
      {"dom-window-is-global", domWindowIsGlobal},
      {"dom-navigator", domNavigator},
      {"dom-event-target", domEventTarget},
      {"dom-location-hash", domLocationHash},
      {"dom-bounding-rect", domBoundingRect},
      {"dom-url-search-params-iteration", domUrlSearchParamsIteration},
      {"gl-premultiplied-alpha", glPremultipliedAlpha},
      {"promise-rejection-logged", promiseRejectionLogged},
      {"gl-present-without-shim", glPresentWithoutShim},
      {"gl-buffer-data", glBufferData},
      {"dom-drawing-buffer-shape", domDrawingBufferShape},
      {"gl-drawing-buffer-resize", glDrawingBufferResize},
      {"report-failure", reportFailure},
      {"hermes-builtins", hermesBuiltins},
      {"dom-tree-move", domTreeMove},
      {"dom-tree-cycle", domTreeCycle},
      {"dom-tree-operations", domTreeOperations},
      {"dom-document-tree", domDocumentTree},
      {"dom-attributes", domAttributes},
      {"dom-selectors", domSelectors},
      {"dom-selector-syntax", domSelectorSyntax},
      {"dom-event-propagation", domEventPropagation},
      {"dom-key-target", domKeyTarget},
      {"dom-window-close", domWindowClose},
      {"dom-style-declaration", domStyleDeclaration},
      {"dom-canvas-style", domCanvasStyle},
      {"dom-canvas-style-attribute", domCanvasStyleAttribute},
      {"dom-event-handlers", domEventHandlers},
      {"dom-mutation-observer", domMutationObserver},
      {"dom-image-bitmap-crop", domImageBitmapCrop},
      {"dom-image-decode-async", domImageDecodeAsync},
      {"dom-xml-parser", domXmlParser},
      {"dom-canvas-2d", domCanvas2d},
      {"dom-canvas-text", domCanvasText},
      {"dom-document-ready", domDocumentReady},
      {"net-http-get", netHttpGet},
      {"net-https", netHttps},
      {"net-request-body", netRequestBody},
      {"net-streaming-request", netStreamingRequest},
      {"net-streaming-response", netStreamingResponse},
      {"net-http-error-status", netHttpErrorStatus},
      {"net-redirect", netRedirect},
      {"net-encoded-body", netEncodedBody},
      {"net-abort-timeout", netAbortTimeout},
      {"net-flow-control", netFlowControl},
      {"net-unreachable", netUnreachable},
      {"net-cookies", netCookies},
      {"net-websocket", netWebSocket},
      {"net-eventsource", netEventSource},
      {"net-image", netImage},
      {"net-shutdown-idle", netShutdownIdle},
      {"net-seam-contract", netSeamContract},
      {"media-seam", mediaSeam},
      {"media-element", mediaElement},
      {"media-element-model", mediaElementModel},
      {"media-hls", mediaHls},
      {"media-mp4", mediaMp4},
      {"media-dash", mediaDash},
      {"media-live", mediaLive},
      {"media-seek", mediaSeek},
      {"media-ended", mediaEnded},
      {"media-tracks", mediaTracks},
      {"media-clearkey", mediaClearKey},
      {"media-licence", mediaLicence},
      {"media-licence-exchange", mediaLicenceExchange},
      {"media-bad-url", mediaBadUrl},
      {"media-unplayable", mediaUnplayable},
      {"media-interrupted", mediaInterrupted},
      {"media-remove", mediaRemove},
      {"media-two-players", mediaTwoPlayers},
      {"media-runtime-pause", mediaRuntimePause},
      {"media-shutdown", mediaShutdown},
      {"media-collected", mediaCollected},
      {"iframe-embed", iframeEmbed},
      {"iframe-message", iframeMessage},
      {"iframe-lifecycle", iframeLifecycle},
      {"iframe-focus", iframeFocus},
      {"iframe-remove", iframeRemove},
      {"iframe-bad-src", iframeBadSrc},
      {"iframe-nested", iframeNested},
      {"iframe-sandbox", iframeSandbox},
      {"iframe-two", iframeTwo},
      {"iframe-shutdown", iframeShutdown},
      {"iframe-none-costs", iframeNoneCosts},
      {"iframe-child-fails", iframeChildFails},
      {"iframe-no-compositor", iframeNoCompositor},
      {"iframe-input", iframeInput},
      {"canvas-isolation", canvasIsolation},
      {"canvas-placed", canvasPlaced},
      {"canvas-composite", canvasComposite},
      {"canvas-idle", canvasIdle},
      {"canvas-remove", canvasRemove},
      {"canvas-teardown", canvasTeardown},
      {"canvas-over-iframe", canvasOverIframe},
      {"canvas-over-video", canvasOverVideo},
      {"canvas-2d-no-layer", canvas2dNoLayer},
      {"canvas-refused", canvasRefused},
#if defined(__linux__) && !defined(__ANDROID__)
      {"media-hardware", mediaHardware},
      {"media-no-wayland", mediaNoWayland},
#endif
#ifdef SCREENKIT_ANDROID_NET_ROWS
      {"net-no-javavm", netNoJavaVm},
      {"net-shutdown-mid-call", netShutdownMidCall},
#endif
#if defined(__linux__) && !defined(__ANDROID__)
      {"net-system-ca", netSystemCa},
      {"net-cookie-rules", netCookieRules},
      {"net-upload-overflow", netUploadOverflow},
      {"net-cancel-reach", netCancelReach},
#endif
      {"log-sink-swap", logSinkSwap},
      {"object-model-gc", objectModelGc},
      {"object-model-release", objectModelRelease},
      {"object-model-listener-isolation", objectModelListenerIsolation},
      {"object-model-lazy-module", objectModelLazyModule},
  };
  return kCases;
}

/// One row, with CTest's own exit codes: 0 ran, 77 skipped, 1 failed, 64 unknown.
int runCase(const std::string& name) {
  const auto it = cases().find(name);
  if (it == cases().end()) {
    std::fprintf(stderr, "unknown case \"%s\"\n", name.c_str());
    return 64;
  }

  std::fprintf(stderr, "== %s ==\n", name.c_str());
  it->second();
  if (test::failures() == 0 && !test::skipped().empty()) {
    std::fprintf(stderr, "== %s: skipped -- %s ==\n", name.c_str(), test::skipped().c_str());
    return 77;
  }
  if (test::failures() > 0) {
    std::fprintf(stderr, "== %s: %d failure(s) ==\n", name.c_str(), test::failures());
    return 1;
  }
  std::fprintf(stderr, "== %s: ok ==\n", name.c_str());
  return 0;
}

}  // namespace

#if defined(__linux__) && !defined(__ANDROID__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

namespace {
/// A device row that crashes is otherwise just `rc=139` in a log: the Pi has no
/// debugger and no core dumps. This prints the stack, as `binary(+offset)`
/// lines that `addr2line -e screenkit-net-tests` resolves in the build container,
/// then dies of the same signal so the row still fails the same way.
void printCrash(int sig) {
  static const char kHeader[] = "\n== crash: stack ==\n";
  ssize_t ignored = ::write(STDERR_FILENO, kHeader, sizeof(kHeader) - 1);
  (void)ignored;
  void* frames[64];
  const int count = ::backtrace(frames, 64);
  ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}
}  // namespace
#endif

int main(int argc, char** argv) {
#if defined(__linux__) && !defined(__ANDROID__)
  ::signal(SIGSEGV, printCrash);
  ::signal(SIGABRT, printCrash);
  ::signal(SIGBUS, printCrash);
#endif
  if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
    for (const auto& entry : cases()) std::printf("%s\n", entry.first.c_str());
    return 0;
  }
  if (argc < 3) {
    std::fprintf(stderr, "usage: screenkit-runtime-tests <case> <fixtures-dir>\n");
    std::fprintf(stderr, "       screenkit-runtime-tests --list\n");
    return 64;
  }

  gFixtures = argv[2];
  return runCase(argv[1]);
}

#if defined(SCREENKIT_NET_TESTS_ONLY) && defined(__ANDROID__)
// ============================================================================
// The same rows, inside an Android app process.
//
// A row needs three things the emulator only has there: a JavaVM whose class
// loader can see `dev.screenkit.net.HttpClient`, the INTERNET permission, and
// the app's own storage for the cookie jar. So the net binary is a library the
// debug APK loads (`dev.screenkit.net.NetTests`) rather than an executable
// pushed to /data/local/tmp, and tools/android/android.sh test drives it.
//
// JNI_OnLoad is where the HttpClient class is resolved, for the reason
// runtime/android/jni/HostMain.cpp spells out: the I/O threads are attached
// native threads and their own FindClass cannot see app classes.
// ============================================================================
#include <android/log.h>
#include <jni.h>
#include <pthread.h>

#include <hermes/hermes.h>
#include <jsi/jsi.h>

// SDL learns that the app has started from the main() its own header wraps. A
// row here is called from a Java activity through JNI, so there is no such
// main: without SDL_SetMainReady the runtime's own SDL_InitSubSystem(
// SDL_INIT_EVENTS) (core/src/hermes/HermesHost.cpp) refuses with "Application
// didn't initialize properly", and every row fails before it starts. This is
// SDL's documented escape hatch for embedding it without SDL_main, and it
// belongs here rather than in the runtime, which on every real host does go
// through SDL_main. SDL_MAIN_HANDLED keeps the header from redefining main --
// this file has its own, for the macOS suite.
#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL_main.h>

namespace {

/// A row says what went wrong on stderr (TestSupport.h's CHECK), and an app's
/// stderr goes nowhere on Android -- `setprop log.redirect-stdio` is refused on
/// a user build, which every emulator image is. So the entry point pipes its own
/// stdout and stderr into logcat, and a failing row is readable.
void* pumpStdio(void* argument) {
  const int fd = *static_cast<int*>(argument);
  std::string line;
  char buffer[512];
  for (;;) {
    const ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got <= 0) return nullptr;
    for (ssize_t i = 0; i < got; ++i) {
      if (buffer[i] == '\n') {
        __android_log_write(ANDROID_LOG_INFO, "ScreenKitNetTests", line.c_str());
        line.clear();
      } else {
        line.push_back(buffer[i]);
      }
    }
  }
}

/// The same warm-up runtime/android/jni/HostMain.cpp runs before its runtime
/// starts, for the same reason: Hermes' Intl on Android is Java through fbjni,
/// which caches each class the first time it is used, and the runtime's JS
/// thread is a native thread whose class lookups reach only the system class
/// loader. The prelude itself calls `toLocaleUpperCase` (dom-shim.js), so
/// without this every row fails loading it with a RangeError.
///
/// This runs on the thread the activity started, which is a Java thread: its
/// lookups use the app's loader, and what fbjni caches here stays cached for the
/// process.
void warmIntl() {
  static constexpr const char* kWarm = R"((function () {
    function touch(f) { try { f(); } catch (e) {} }
    touch(function () { 'a'.toLocaleUpperCase(); 'A'.toLocaleLowerCase(); });
    touch(function () { 'a'.localeCompare('b'); });
    touch(function () { 'a'.normalize('NFD'); });
    touch(function () { Intl.getCanonicalLocales('en-US'); });
    touch(function () {
      var c = new Intl.Collator('en-US'); c.compare('a', 'b'); c.resolvedOptions();
      Intl.Collator.supportedLocalesOf('en-US');
    });
    touch(function () {
      var f = new Intl.DateTimeFormat('en-US'); f.format(0); f.formatToParts(0); f.resolvedOptions();
      Intl.DateTimeFormat.supportedLocalesOf('en-US');
      new Date(0).toLocaleString(); new Date(0).toLocaleDateString(); new Date(0).toLocaleTimeString();
    });
    touch(function () {
      var f = new Intl.NumberFormat('en-US'); f.format(1.5); f.formatToParts(1.5); f.resolvedOptions();
      Intl.NumberFormat.supportedLocalesOf('en-US'); (1.5).toLocaleString();
    });
    return 'warm';
  })())";
  try {
    auto runtime = facebook::hermes::makeHermesRuntime();
    runtime->evaluateJavaScript(std::make_shared<facebook::jsi::StringBuffer>(kWarm),
                                "screenkit://warm-intl.js");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  could not prepare Intl: %s\n", e.what());
  }
}

void stdioToLogcat() {
  static int readFd = -1;
  if (readFd >= 0) return;
  int fds[2];
  if (pipe(fds) != 0) return;
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  readFd = fds[0];
  pthread_t thread;
  if (pthread_create(&thread, nullptr, pumpStdio, &readFd) == 0) pthread_detach(thread);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  // The media rows' player, dev.screenkit.media.VideoPlayer, resolved here for
  // the class-loader reason above (media/MediaPlayerAndroid.h). First, so the
  // withheld-VM row below still has one.
  {
    JNIEnv* mediaEnv = nullptr;
    std::string mediaError;
    if (vm->GetEnv(reinterpret_cast<void**>(&mediaEnv), JNI_VERSION_1_6) == JNI_OK &&
        !screenkit::media::prepareAndroidMedia(vm, mediaEnv, mediaError)) {
      std::fprintf(stderr, "no media player: %s\n", mediaError.c_str());
    }
  }
  // The `net-no-javavm` row needs a process where the client never got a VM,
  // and on a device JNI_OnLoad always runs -- so the harness asks for it here,
  // through the variable NetTests sets before it loads this library. Nothing in
  // the client knows about it; this is simply the one caller declining to call.
  const char* withhold = std::getenv("SCREENKIT_NET_TESTS_NO_BACKEND");
  if (withhold != nullptr && *withhold != '\0') {
    stdioToLogcat();
    std::fprintf(stderr, "net tests: the JavaVM is withheld from the network client\n");
    return JNI_VERSION_1_6;
  }
  screenkit::net::setJavaVm(vm);
  JNIEnv* env = nullptr;
  std::string error;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK &&
      !screenkit::net::prepareAndroidNetwork(env, error)) {
    std::fprintf(stderr, "no network client: %s\n", error.c_str());
  }
  return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL Java_dev_screenkit_net_NetTests_nativeRun(
    JNIEnv* env, jclass, jstring name, jstring fixtures, jstring netFixture, jstring domShim) {
  const auto utf = [env](jstring value) {
    if (value == nullptr) return std::string();
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return std::string();
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
  };
  // Before anything creates a runtime: see the SDL_MAIN_HANDLED note above.
  SDL_SetMainReady();
  stdioToLogcat();
  warmIntl();
  const std::string netFixtureDir = utf(netFixture);
  const std::string shim = utf(domShim);
  if (!netFixtureDir.empty()) setenv("SCREENKIT_NET_FIXTURE_DIR", netFixtureDir.c_str(), 1);
  if (!shim.empty()) setenv("SCREENKIT_DOM_SHIM_HBC", shim.c_str(), 1);
  gFixtures = utf(fixtures);
  return runCase(utf(name));
}
#endif

