// Copyright (c) ScreenKit contributors. MIT.
#include "BytecodeLoader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

// The Hermes half -- bytecode validation through IHermesRootAPI -- is compiled
// only when Hermes is the engine; SpiderMonkey has its own
// (spidermonkey/SpiderMonkeyEngine.cpp). Mapping a file is the same for both.
#if !SCREENKIT_ENGINE_SPIDERMONKEY
#include <hermes/hermes.h>
#endif

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

// The HBC header opens with an 8-byte magic followed by a uint32 version.
// Reading the version ourselves is what lets the mismatch error name both sides
// instead of just saying "not bytecode".
constexpr std::uint8_t kMagic[8] = {0xc6, 0x1f, 0xbc, 0x03, 0xc1, 0x03, 0x19, 0x1f};
constexpr std::size_t kVersionOffset = sizeof(kMagic);
constexpr std::size_t kMinHeader = kVersionOffset + sizeof(std::uint32_t);

#if !SCREENKIT_ENGINE_SPIDERMONKEY
facebook::hermes::IHermesRootAPI* rootAPI() {
  // Static lifetime, per hermes.h.
  static auto* api =
      jsi::castInterface<facebook::hermes::IHermesRootAPI>(facebook::hermes::makeHermesRootAPI());
  return api;
}

#endif

std::uint32_t readLE32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

/// A `jsi::Buffer` over an mmap'd file. Hermes runs bytecode straight out of the
/// mapping -- no read, no copy, and the pages stay evictable.
class MappedFileBuffer final : public jsi::Buffer {
 public:
  MappedFileBuffer(void* data, std::size_t size) : data_(data), size_(size) {}

  ~MappedFileBuffer() override {
    if (data_ != nullptr && data_ != MAP_FAILED) {
      ::munmap(data_, size_);
    }
  }

  MappedFileBuffer(const MappedFileBuffer&) = delete;
  MappedFileBuffer& operator=(const MappedFileBuffer&) = delete;

  std::size_t size() const override { return size_; }
  const std::uint8_t* data() const override { return static_cast<const std::uint8_t*>(data_); }

 private:
  void* data_;
  std::size_t size_;
};

LoadResult fail(std::string error) {
  LoadResult r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

std::string errnoText() { return std::strerror(errno); }

}  // namespace

bool looksLikeHermesBytecode(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) return false;
  const std::size_t n = size < sizeof(kMagic) ? size : sizeof(kMagic);
  return std::memcmp(data, kMagic, n) == 0;
}

#if !SCREENKIT_ENGINE_SPIDERMONKEY
std::uint32_t supportedBytecodeVersion() { return rootAPI()->getBytecodeVersion(); }
#endif

LoadResult mapBundleFile(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return fail("cannot open bundle \"" + path + "\": " + errnoText());
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const std::string why = errnoText();
    ::close(fd);
    return fail("cannot stat bundle \"" + path + "\": " + why);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return fail("bundle \"" + path + "\" is not a regular file");
  }
  if (st.st_size == 0) {
    ::close(fd);
    return fail("bundle \"" + path + "\" is empty");
  }

  const auto size = static_cast<std::size_t>(st.st_size);
  void* addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  const std::string mapError = addr == MAP_FAILED ? errnoText() : std::string();
  ::close(fd);  // the mapping keeps its own reference
  if (addr == MAP_FAILED) {
    return fail("cannot mmap bundle \"" + path + "\": " + mapError);
  }

  LoadResult r;
  r.ok = true;
  r.bundle.buffer = std::make_shared<MappedFileBuffer>(addr, size);
  r.bundle.url = path;
  return r;
}

#if !SCREENKIT_ENGINE_SPIDERMONKEY
LoadResult validateBundle(Bundle bundle) {
  if (!bundle.buffer) return fail("no bundle buffer");

  const std::uint8_t* data = bundle.buffer->data();
  const std::size_t size = bundle.buffer->size();

  if (!looksLikeHermesBytecode(data, size)) {
    // Source text. Nothing to validate here -- a syntax error surfaces from
    // prepareJavaScript as a catchable JSError.
    bundle.bytecode = false;
    LoadResult r;
    r.ok = true;
    r.bundle = std::move(bundle);
    return r;
  }

  if (size < kMinHeader) {
    return fail("truncated Hermes bytecode in \"" + bundle.url + "\": " + std::to_string(size) +
                " bytes, the header alone needs " + std::to_string(kMinHeader));
  }

  auto* api = rootAPI();
  const std::uint32_t found = readLE32(data + kVersionOffset);
  const std::uint32_t expected = api->getBytecodeVersion();
  if (found != expected) {
    return fail("Hermes bytecode version mismatch in \"" + bundle.url + "\": bundle is version " +
                std::to_string(found) + ", this runtime accepts version " +
                std::to_string(expected) +
                " -- recompile with the hermesc pinned in tools/prebuilts/manifest.json");
  }

  // Sanity check first, because it is the one that explains itself:
  // isHermesBytecode answers yes/no, hermesBytecodeSanityCheck says why not.
  // A truncated file fails both, and the useful message only comes from here.
  std::string why;
  if (!api->hermesBytecodeSanityCheck(data, size, &why)) {
    return fail("corrupt Hermes bytecode in \"" + bundle.url + "\": " +
                (why.empty() ? "sanity check failed" : why));
  }

  if (!api->isHermesBytecode(data, size)) {
    return fail("\"" + bundle.url +
                "\" passed the bytecode sanity check but the engine still refuses it as "
                "bytecode");
  }

  bundle.bytecode = true;
  LoadResult r;
  r.ok = true;
  r.bundle = std::move(bundle);
  return r;
}

#endif  // !SCREENKIT_ENGINE_SPIDERMONKEY

}  // namespace screenkit
