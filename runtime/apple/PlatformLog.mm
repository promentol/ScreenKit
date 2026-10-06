// Copyright (c) ScreenKit contributors. MIT.
#include "PlatformLog.h"

#import <Foundation/Foundation.h>
#import <os/log.h>

#include <cstdio>
#include <string>

#include <screenkit/Log.h>

namespace screenkit {
namespace {

os_log_t logHandle(const std::string& tag) {
  // One handle per tag, cached: os_log_create is not free and the tag set is
  // tiny (one per runtime instance).
  // os_log_t is an Objective-C object type, so under ARC it goes into the
  // dictionary directly -- no __bridge cast, which would not even compile.
  static NSMutableDictionary<NSString*, os_log_t>* cache = [NSMutableDictionary new];

  // stringWithUTF8String: returns nil for anything that is not valid UTF-8, and
  // cache[nil] raises NSInvalidArgumentException. A category name is not worth
  // an exception, so fall back to a literal.
  NSString* key = [NSString stringWithUTF8String:tag.c_str()];
  const char* category = tag.c_str();
  if (key == nil) {
    key = @"screenkit";
    category = "screenkit";
  }

  @synchronized(cache) {
    os_log_t existing = cache[key];
    if (existing != nil) return existing;
    os_log_t handle = os_log_create("dev.screenkit", category);
    cache[key] = handle;
    return handle;
  }
}

os_log_type_t osType(LogLevel level) {
  switch (level) {
    case LogLevel::Log:
      return OS_LOG_TYPE_DEFAULT;
    case LogLevel::Warn:
      // Not OS_LOG_TYPE_ERROR: a console.warn from a bundle is not a runtime
      // error, and typing it as one makes every log query noisy.
      return OS_LOG_TYPE_DEFAULT;
    case LogLevel::Error:
      // OS_LOG_TYPE_FAULT means "a process-level failure", which console.error
      // is not.
      return OS_LOG_TYPE_ERROR;
  }
  return OS_LOG_TYPE_DEFAULT;
}

}  // namespace

void installPlatformLogSink() {
  setLogSink([](LogLevel level, const std::string& tag, const std::string& message) {
    // The sink runs on the JS thread, which has no autorelease pool of its own,
    // so the NSString temporaries would accumulate for the life of the runtime.
    @autoreleasepool {
      // %{public}s: without it os_log redacts the string as <private>, and a
      // console.log nobody can read is not a console.log.
      os_log_with_type(logHandle(tag), osType(level), "[%{public}s] %{public}s",
                       levelName(level), message.c_str());
      std::fprintf(stderr, "[%s] %s: %s\n", tag.c_str(), levelName(level), message.c_str());
      std::fflush(stderr);
    }
  });
}

}  // namespace screenkit
