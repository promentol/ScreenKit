// Loads a Linux Hermes prebuilt the way the runtime does -- libhermesvm.so and the packaged
// headers, nothing else -- and runs two programs through it:
//
//   smoke <smoke.hbc> <smoke.js> <expected bytecode version>
//
//   1. smoke.hbc, compiled from smoke.js by the pinned hermesc from tools/prebuilts on another
//      machine: the bytecode-version parity the manifest promises;
//   2. smoke.js as source, through the compiler inside the library.
//
// Both must produce the line smoke.js expects. Exit 0 only when every check holds.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <hermes/hermes.h>
#include <jsi/jsi.h>

namespace jsi = facebook::jsi;

namespace {

class Bytes final : public jsi::Buffer {
 public:
  explicit Bytes(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  std::size_t size() const override { return bytes_.size(); }
  const std::uint8_t* data() const override { return bytes_.data(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: smoke <smoke.hbc> <smoke.js> <expected bytecode version> [hot.hbc]\n");
    return 2;
  }
  auto* api = jsi::castInterface<facebook::hermes::IHermesRootAPI>(facebook::hermes::makeHermesRootAPI());
  const auto expected = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
  if (api->getBytecodeVersion() != expected) {
    std::fprintf(stderr, "bytecode version %u, expected %u\n", api->getBytecodeVersion(), expected);
    return 1;
  }

  auto bytecode = readFile(argv[1]);
  std::string why;
  if (!api->isHermesBytecode(bytecode.data(), bytecode.size()) ||
      !api->hermesBytecodeSanityCheck(bytecode.data(), bytecode.size(), &why)) {
    std::fprintf(stderr, "%s is not bytecode this library runs: %s\n", argv[1], why.c_str());
    return 1;
  }

  // The configuration the ScreenKit host runs with (runtime/core/src/hermes/HermesHost.cpp): a
  // native microtask queue -- without it Hermes defines no WeakRef -- and ES6 block scoping.
  auto config = ::hermes::vm::RuntimeConfig::Builder().withMicrotaskQueue(true).withES6BlockScoping(true).build();
  auto runtime = facebook::hermes::makeHermesRuntime(config);
  std::string fromBytecode, fromSource;
  try {
    fromBytecode = runtime->evaluateJavaScript(std::make_shared<Bytes>(std::move(bytecode)), argv[1])
                       .getString(*runtime)
                       .utf8(*runtime);
    auto source = readFile(argv[2]);
    fromSource = runtime->evaluateJavaScript(
                             std::make_shared<jsi::StringBuffer>(std::string(source.begin(), source.end())), argv[2])
                     .getString(*runtime)
                     .utf8(*runtime);
  } catch (const jsi::JSIException& e) {
    std::fprintf(stderr, "threw: %s\n", e.what());
    return 1;
  }

  std::printf("bytecode: %s\nsource:   %s\n", fromBytecode.c_str(), fromSource.c_str());
#if SCREENKIT_SMOKE_JIT
  // Only when the caller passed the workload: the recipe runs this program a
  // second time, with the library's own hermesc output and no hot file, to check
  // the two compilers agree.
  if (argc >= 5) {
  // A JIT build must actually JIT. The measurement mirrors what runs on a device:
  // bytecode from `hermesc -O` (argv[4]), evaluated through the same JSI API the
  // runtime uses, once with the JIT off and once with it on. Two shapes matter --
  // Hermes compiles a function when it is *called* (no on-stack replacement), and
  // its baseline JIT speeds up object and method code, not arithmetic loops, which
  // it can even slow down.
  {
    auto time = [&](bool enableJit) {
      auto builder = ::hermes::vm::RuntimeConfig::Builder().withMicrotaskQueue(true).withES6BlockScoping(true);
      if (enableJit) builder.withEnableJIT(true).withForceJIT(true);
      auto rt = facebook::hermes::makeHermesRuntime(builder.build());
      auto hot = readFile(argv[4]);
      const auto start = std::chrono::steady_clock::now();
      const double value =
          rt->evaluateJavaScript(std::make_shared<Bytes>(std::move(hot)), argv[4]).getNumber();
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      return std::pair<double, double>{value, ms};
    };
    const auto interpreted = time(false);
    const auto jitted = time(true);
    std::printf("jit: interpreter %.0f ms, jit %.0f ms (%.2fx), result %.0f\n", interpreted.second, jitted.second,
                interpreted.second / jitted.second, jitted.first);
    if (interpreted.first != jitted.first) {
      std::fprintf(stderr, "the JIT computed %.0f where the interpreter computed %.0f\n", jitted.first,
                   interpreted.first);
      return 1;
    }
    // The shell measures 3.8x on this workload; anything near 1x means the JIT is
    // compiled in but not running.
    if (jitted.second > interpreted.second * 0.8) {
      std::fprintf(stderr, "the JIT gained less than 20%% -- it is not engaging\n");
      return 1;
    }
  }
  }
#endif
  const std::string want = "2,4,6|symbol|function|function|function|18446744073709551616|héllo|"
                           "{\"a\":[1,{\"b\":null}]}|1970-01-01T00:00:00.000Z|6|abC|1/1/1970|ok";
  if (fromBytecode != want || fromSource != want) {
    std::fprintf(stderr, "expected: %s\n", want.c_str());
    return 1;
  }
  return 0;
}
