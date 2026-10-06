// Copyright (c) ScreenKit contributors. MIT.
//
// A hand-rolled harness, not a framework. Pulling in GoogleTest would mean
// compiling third-party source, which is exactly what the prebuilts rule
// forbids -- and the suite is small enough not to need one.
#pragma once

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <screenkit/Log.h>

namespace test {

inline int& failures() {
  static int n = 0;
  return n;
}

/// A row that cannot run here -- no window server, say -- rather than one that
/// failed. main() exits 77, which CTest's SKIP_RETURN_CODE reports as skipped,
/// so it is neither a silent pass nor a false failure.
inline std::string& skipped() {
  static std::string reason;
  return reason;
}

inline void skip(const std::string& reason) { skipped() = reason; }

inline void fail(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
  ++failures();
}

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::test::fail(__FILE__, __LINE__, "expected: " #cond);    \
  } while (0)

#define CHECK_EQ(actual, expected)                                            \
  do {                                                                        \
    const auto _a = (actual);                                                 \
    const auto _e = (expected);                                               \
    if (!(_a == _e)) {                                                        \
      ::test::fail(__FILE__, __LINE__,                                        \
                   std::string(#actual) + " == " + #expected + " (got " +     \
                       ::test::show(_a) + ", want " + ::test::show(_e) + ")"); \
    }                                                                         \
  } while (0)

#define CHECK_CONTAINS(haystack, needle)                                             \
  do {                                                                               \
    const std::string _h = (haystack);                                               \
    const std::string _n = (needle);                                                 \
    if (_h.find(_n) == std::string::npos) {                                          \
      ::test::fail(__FILE__, __LINE__, "expected to find \"" + _n + "\" in: " + _h); \
    }                                                                                \
  } while (0)

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return show(std::string(s == nullptr ? "" : s)); }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <typename T>
inline std::string show(T v) {
  return std::to_string(v);
}

/// Captures everything that goes through screenkit::log so a test can assert on
/// what a bundle's console.log actually produced.
class LogCapture {
 public:
  struct Line {
    screenkit::LogLevel level;
    std::string tag;
    std::string message;
  };

  LogCapture() {
    screenkit::setLogSink([this](screenkit::LogLevel level, const std::string& tag,
                                 const std::string& message) {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.push_back({level, tag, message});
    });
  }

  ~LogCapture() { screenkit::setLogSink(nullptr); }

  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  std::vector<Line> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

  std::string joined() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const auto& line : lines_) {
      out += screenkit::levelName(line.level);
      out += ": ";
      out += line.message;
      out += '\n';
    }
    return out;
  }

  bool has(screenkit::LogLevel level, const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& line : lines_) {
      if (line.level == level && line.message.find(needle) != std::string::npos) return true;
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Line> lines_;
};

}  // namespace test
