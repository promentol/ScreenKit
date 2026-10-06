// Copyright (c) ScreenKit contributors. MIT.
#include "Package.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <jsi/jsi.h>

#include <screenkit/Log.h>
#include <screenkit/Runtime.h>

#include "../engine/Engine.h"

namespace screenkit {
namespace bundle {
namespace {

constexpr double kPackageFormat = 1;

// runtime/VERSION, compiled in by CMake -- the same file `screenkit bundle`
// writes into every manifest, so the two cannot drift.
constexpr std::uint32_t kRuntimeVersion = SCREENKIT_RUNTIME_VERSION;

// A manifest lists a hash per file, so it grows with the app -- but not to this.
// The cap keeps a stray multi-gigabyte file named manifest.json from being read
// into memory and handed to JSON.parse.
constexpr std::streamsize kMaxManifestBytes = 16 << 20;

/// The fields the gate reads, copied off the JS thread as plain values.
struct PackageManifest {
  double format = 0;
  double runtimeVersion = 0;
  double hermesBytecodeVersion = 0;
  std::string entry;
  // engines.spidermonkey: the packed source, and the stencil precompiled from it
  // (optional). What a SpiderMonkey host runs instead of `entry`.
  std::string spidermonkeySource;
  std::string spidermonkeyStencil;
};

bool isDirectory(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isWholeNumber(double value) {
  return std::isfinite(value) && value >= 0 && value <= 4294967295.0 && std::floor(value) == value;
}

std::string showNumber(double value) {
  if (isWholeNumber(value)) return std::to_string(static_cast<std::uint64_t>(value));
  std::ostringstream out;
  out << value;
  return out.str();
}

/// Parse manifest.json with the runtime's own JSON.parse and read the gate's
/// fields out. Runs on the JS thread -- no third-party JSON parser in the host,
/// and nothing from the package is evaluated: the text is handed to JSON.parse
/// as a string, never as source.
bool parseManifest(const std::shared_ptr<Runtime>& runtime, const std::string& text,
                   const std::string& file, PackageManifest& manifest, std::string& error) {
  struct Cell {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string error;
    PackageManifest manifest;
  };
  auto cell = std::make_shared<Cell>();

  runtime->executor()->invokeAsync([cell, text, file](facebook::jsi::Runtime& js) {
    namespace jsi = facebook::jsi;
    PackageManifest parsed;
    std::string why;
    try {
      jsi::Function parse =
          js.global().getPropertyAsObject(js, "JSON").getPropertyAsFunction(js, "parse");
      jsi::Value value = parse.call(js, jsi::String::createFromUtf8(js, text));
      if (!value.isObject() || value.getObject(js).isArray(js)) {
        why = "package manifest \"" + file + "\" is not a JSON object";
      } else {
        jsi::Object object = value.getObject(js);
        const auto number = [&](const char* key, double& out) {
          const jsi::Value field = object.getProperty(js, key);
          if (!field.isNumber()) {
            why = "package manifest \"" + file + "\" has no numeric \"" + key + "\"";
            return false;
          }
          out = field.getNumber();
          return true;
        };
        if (number("format", parsed.format) && number("runtimeVersion", parsed.runtimeVersion) &&
            number("hermesBytecodeVersion", parsed.hermesBytecodeVersion)) {
          const jsi::Value entry = object.getProperty(js, "entry");
          if (entry.isString()) {
            parsed.entry = entry.getString(js).utf8(js);
          } else {
            why = "package manifest \"" + file + "\" has no string \"entry\"";
          }
          const jsi::Value engines = object.getProperty(js, "engines");
          if (engines.isObject()) {
            const jsi::Value sm = engines.getObject(js).getProperty(js, "spidermonkey");
            if (sm.isObject()) {
              const jsi::Object spidermonkey = sm.getObject(js);
              const jsi::Value source = spidermonkey.getProperty(js, "source");
              const jsi::Value stencil = spidermonkey.getProperty(js, "stencil");
              if (source.isString()) parsed.spidermonkeySource = source.getString(js).utf8(js);
              if (stencil.isString()) parsed.spidermonkeyStencil = stencil.getString(js).utf8(js);
            }
          }
        }
      }
    } catch (const jsi::JSError& e) {
      why = "package manifest \"" + file + "\" is not valid JSON: " + e.getMessage();
    } catch (const std::exception& e) {
      why = "package manifest \"" + file + "\" could not be read: " + e.what();
    }
    {
      std::lock_guard<std::mutex> lock(cell->mutex);
      cell->done = true;
      cell->ok = why.empty();
      cell->error = std::move(why);
      cell->manifest = std::move(parsed);
    }
    cell->cv.notify_all();
  });

  std::unique_lock<std::mutex> lock(cell->mutex);
  if (!cell->cv.wait_for(lock, std::chrono::seconds(20), [&] { return cell->done; })) {
    error = "the JS thread did not answer while reading \"" + file + "\" within 20s";
    return false;
  }
  if (!cell->ok) {
    error = cell->error;
    return false;
  }
  manifest = std::move(cell->manifest);
  return true;
}

/// Open a package and gate it. Every refusal names the file it is about and, for
/// a version, both sides -- worded like BytecodeLoader's own mismatch error.
bool openPackage(const std::shared_ptr<Runtime>& runtime, const std::string& path,
                 const std::string& tag, LaunchTarget& target, std::string& error) {
  std::string dir = path;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  const std::string file = dir + "/manifest.json";

  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in) {
    error = "cannot open package manifest \"" + file + "\": " + std::strerror(errno) +
            " -- a .skpkg is the directory `screenkit bundle` writes";
    return false;
  }
  const std::streamsize size = in.tellg();
  if (size < 0 || size > kMaxManifestBytes) {
    error = "package manifest \"" + file + "\" is " +
            (size < 0 ? std::string("unreadable") : std::to_string(size) + " bytes, over the " +
                                                        std::to_string(kMaxManifestBytes) +
                                                        "-byte limit");
    return false;
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  in.seekg(0, std::ios::beg);
  if (size > 0 && !in.read(text.data(), size)) {
    error = "cannot read package manifest \"" + file + "\"";
    return false;
  }

  PackageManifest manifest;
  if (!parseManifest(runtime, text, file, manifest, error)) return false;

  if (manifest.format != kPackageFormat) {
    error = "package manifest \"" + file + "\" is format " + showNumber(manifest.format) +
            ", this runtime reads format " + showNumber(kPackageFormat);
    return false;
  }

  // The Hermes gate is Hermes': a SpiderMonkey host runs the package's source
  // (or its stencil) and never looks at its bytecode.
  const bool hermes = std::strcmp(engine::name(), "hermes") == 0;
  const std::uint32_t expected = Runtime::hermesBytecodeVersion();
  if (hermes && (!isWholeNumber(manifest.hermesBytecodeVersion) ||
                 static_cast<std::uint32_t>(manifest.hermesBytecodeVersion) != expected)) {
    error = "Hermes bytecode version mismatch in \"" + file + "\": package is version " +
            showNumber(manifest.hermesBytecodeVersion) + ", this runtime accepts version " +
            std::to_string(expected) +
            " -- rebuild the package with `screenkit bundle`, which compiles with the hermesc "
            "pinned in tools/prebuilts/manifest.json";
    return false;
  }

  if (!isWholeNumber(manifest.runtimeVersion) || manifest.runtimeVersion < 1 ||
      manifest.runtimeVersion > kRuntimeVersion) {
    error = "runtime version mismatch in \"" + file + "\": package needs runtimeVersion " +
            showNumber(manifest.runtimeVersion) + ", this runtime supports runtimeVersion " +
            std::to_string(kRuntimeVersion) +
            (manifest.runtimeVersion > kRuntimeVersion ? " -- run it on a newer ScreenKit runtime"
                                                       : "");
    return false;
  }

  // What this engine runs: `entry` for Hermes; for SpiderMonkey the stencil when
  // it was compiled for exactly this engine build, else the source.
  std::string entryName = manifest.entry;
  if (!hermes) {
    if (manifest.spidermonkeySource.empty()) {
      error = "package manifest \"" + file + "\" has no engines.spidermonkey.source, and this runtime is " +
              engine::description() + " -- rebuild the package with `screenkit bundle`, which writes it";
      return false;
    }
    entryName = manifest.spidermonkeySource;
    if (!manifest.spidermonkeyStencil.empty() &&
        engine::canLoadPrecompiled(dir + "/" + manifest.spidermonkeyStencil)) {
      entryName = manifest.spidermonkeyStencil;
    }
  }

  // The entry is a path inside the package. Canonicalising both sides is what
  // rules out `..` and symlinks leading out of it -- the same check HostIO makes
  // for every asset read.
  char rootBuf[PATH_MAX];
  char entryBuf[PATH_MAX];
  const std::string entryPath = dir + "/" + entryName;
  if (entryName.empty() || entryName[0] == '/' ||
      ::realpath(dir.c_str(), rootBuf) == nullptr) {
    error = "package entry \"" + entryName + "\" named by \"" + file +
            "\" is not a path inside the package";
    return false;
  }
  if (::realpath(entryPath.c_str(), entryBuf) == nullptr) {
    error = "package entry \"" + entryName + "\" named by \"" + file +
            "\" does not exist: " + entryPath;
    return false;
  }
  const std::string root = std::string(rootBuf) + "/";
  const std::string resolved = entryBuf;
  struct stat st {};
  if (resolved.compare(0, root.size(), root) != 0) {
    error = "package entry \"" + entryName + "\" named by \"" + file +
            "\" resolves outside the package, to " + resolved;
    return false;
  }
  if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    error = "package entry \"" + entryName + "\" named by \"" + file +
            "\" is not a regular file";
    return false;
  }

  log(LogLevel::Log, tag,
      "package " + dir + ": runtimeVersion " + showNumber(manifest.runtimeVersion) +
          " (runtime " + std::to_string(kRuntimeVersion) + "), hermes bytecode version " +
          showNumber(manifest.hermesBytecodeVersion) + ", " + engine::name() + " entry " + entryName);

  // Run the canonical path that passed the check, not the one the manifest
  // spelled, so a symlink re-pointed after the check cannot lead out of the
  // package.
  target.bundle = resolved;
  target.assetRoot = dir;
  target.package = true;
  return true;
}

}  // namespace

bool resolveLaunch(const std::shared_ptr<Runtime>& runtime, const std::string& path,
                   const std::string& tag, LaunchTarget& target, std::string& error) {
  if (isDirectory(path)) return openPackage(runtime, path, tag, target, error);

  // A `.skpkg` that is not a directory -- a zipped package, a stray file -- is
  // not handed on as a bundle: it would be compiled as source and fail with a
  // parse error that says nothing about packages.
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
  static const std::string kPackageSuffix = ".skpkg";
  if (trimmed.size() >= kPackageSuffix.size() &&
      trimmed.compare(trimmed.size() - kPackageSuffix.size(), kPackageSuffix.size(),
                      kPackageSuffix) == 0) {
    error = "\"" + path + "\" is not a package: a .skpkg is the directory `screenkit bundle` writes" +
            (::access(path.c_str(), F_OK) == 0 ? std::string(", and this is a file")
                                               : std::string(", and nothing is there"));
    return false;
  }

  // A path with no directory component resolves to the working directory,
  // which is where the bundle was found.
  const auto slash = path.find_last_of('/');
  std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
  if (dir.empty()) dir = "/";
  target.bundle = path;
  target.assetRoot = dir;
  target.package = false;
  return true;
}

}  // namespace bundle
}  // namespace screenkit
