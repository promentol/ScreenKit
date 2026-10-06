// Copyright (c) ScreenKit contributors. MIT.
#include <screenkit/Log.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace screenkit {
namespace {

/// One installed sink, and how many threads are inside it right now.
struct InstalledSink {
  explicit InstalledSink(LogSink sink) : sink(std::move(sink)) {}
  const LogSink sink;
  int calls = 0;  // guarded by Sinks::mutex
};

struct Sinks {
  std::mutex mutex;
  std::condition_variable callsDone;
  std::shared_ptr<InstalledSink> current;
};

Sinks& sinks() {
  static Sinks s;
  return s;
}

#ifdef __ANDROID__
// Everything the runtime logs goes to logcat under one tag, `adb logcat -s
// ScreenKit`, with the runtime's own tag inside the line as on the other
// platforms. An app's stderr goes nowhere on Android.
constexpr const char* kLogcatTag = "ScreenKit";

// logcat cuts a line at about 4 KB, and a JS stack trace can be longer.
constexpr std::size_t kLogcatChunk = 3000;

void writeDefault(LogLevel level, const std::string& tag, const std::string& message) {
  const int priority = level == LogLevel::Error  ? ANDROID_LOG_ERROR
                       : level == LogLevel::Warn ? ANDROID_LOG_WARN
                                                 : ANDROID_LOG_INFO;
  const std::string prefix = "[" + tag + "] " + levelName(level) + ": ";
  if (message.size() <= kLogcatChunk) {
    __android_log_write(priority, kLogcatTag, (prefix + message).c_str());
    return;
  }
  // Longer messages go out in pieces, each cut on a UTF-8 character boundary.
  for (std::size_t at = 0; at < message.size();) {
    std::size_t end = std::min(message.size(), at + kLogcatChunk);
    while (end < message.size() && end > at + 1 && (static_cast<unsigned char>(message[end]) & 0xC0) == 0x80) --end;
    __android_log_write(priority, kLogcatTag, (prefix + message.substr(at, end - at)).c_str());
    at = end;
  }
}
#else
void writeDefault(LogLevel level, const std::string& tag, const std::string& message) {
  std::fprintf(stderr, "[%s] %s: %s\n", tag.c_str(), levelName(level), message.c_str());
  std::fflush(stderr);
}
#endif

}  // namespace

const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Log:
      return "log";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }
  return "log";
}

void setLogSink(LogSink sink) {
  Sinks& s = sinks();
  std::unique_lock<std::mutex> lock(s.mutex);
  std::shared_ptr<InstalledSink> previous = std::move(s.current);
  if (sink) s.current = std::make_shared<InstalledSink>(std::move(sink));
  // A sink usually captures something its owner frees right after replacing it
  // (a test's capture buffer, say), so a log call still running the old sink on
  // the JS thread must finish before this returns. Only calls into the old sink
  // are waited for, so a thread logging flat out cannot starve the swap.
  if (previous) s.callsDone.wait(lock, [&] { return previous->calls == 0; });
}

void log(LogLevel level, const std::string& tag, const std::string& message) {
  Sinks& s = sinks();
  std::shared_ptr<InstalledSink> installed;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    installed = s.current;
    if (installed) ++installed->calls;
  }
  if (!installed) {
    writeDefault(level, tag, message);
    return;
  }
  // Called outside the lock: a sink may log itself, or take its own locks.
  installed->sink(level, tag, message);
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (--installed->calls == 0) s.callsDone.notify_all();
  }
}

}  // namespace screenkit
