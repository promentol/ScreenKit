// Copyright (c) ScreenKit contributors. MIT.
//
// console output, read back out of the unified log.
//
// PlatformLog.mm writes every line twice: to os_log, which is "the platform log"
// (Console.app, `log stream`), and to stderr, so a terminal and `simctl launch
// --console` show the same lines. host-runs-hello can only see the stderr copy.
// So a format string that lost `%{public}` kept the whole suite green while
// every line in Console.app read `[<private>] <private>`.
//
// This binary installs the real sink, logs through a real runtime, and reads its
// own entries back with OSLogStore. The current-process scope needs no
// entitlement and no `log show` subprocess, and it sees only this process, so
// the rows are not timing-sensitive against other logging.
//
// Its own binary rather than a row in screenkit-runtime-tests: that suite swaps
// the log sink for a capture in almost every row, and the platform sink is the
// subject here.

#import <Foundation/Foundation.h>
#import <OSLog/OSLog.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <screenkit/Runtime.h>

#include "../apple/PlatformLog.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "  FAIL %s\n", what.c_str());
  ++failures;
}

struct Found {
  std::string message;
  std::string category;
  OSLogEntryLogLevel level = OSLogEntryLogLevelUndefined;
  bool present = false;
};

/// This process's `dev.screenkit` entries since `since`, looked up by exact
/// composed message. Polls briefly: an entry reaches the store asynchronously.
NSArray<OSLogEntryLog*>* readEntries(NSDate* since, NSUInteger want) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  NSMutableArray<OSLogEntryLog*>* entries = [NSMutableArray array];
  while (std::chrono::steady_clock::now() < deadline) {
    [entries removeAllObjects];
    NSError* error = nil;
    OSLogStore* store = [OSLogStore storeWithScope:OSLogStoreCurrentProcessIdentifier error:&error];
    if (store == nil) {
      fail(std::string("OSLogStore unavailable: ") + error.localizedDescription.UTF8String);
      return entries;
    }
    OSLogPosition* position = [store positionWithDate:since];
    NSPredicate* predicate = [NSPredicate predicateWithFormat:@"subsystem == %@", @"dev.screenkit"];
    OSLogEnumerator* enumerator = [store entriesEnumeratorWithOptions:0
                                                             position:position
                                                            predicate:predicate
                                                                error:&error];
    if (enumerator == nil) {
      fail(std::string("OSLogStore enumerate failed: ") + error.localizedDescription.UTF8String);
      return entries;
    }
    for (OSLogEntry* entry in enumerator) {
      if ([entry isKindOfClass:[OSLogEntryLog class]]) [entries addObject:(OSLogEntryLog*)entry];
    }
    if (entries.count >= want) return entries;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return entries;
}

Found find(NSArray<OSLogEntryLog*>* entries, const std::string& message) {
  Found found;
  for (OSLogEntryLog* entry in entries) {
    if (message == entry.composedMessage.UTF8String) {
      found.message = entry.composedMessage.UTF8String;
      found.category = entry.category.UTF8String;
      found.level = entry.level;
      found.present = true;
    }
  }
  return found;
}

const char* levelLabel(OSLogEntryLogLevel level) {
  switch (level) {
    case OSLogEntryLogLevelUndefined: return "undefined";
    case OSLogEntryLogLevelDebug: return "debug";
    case OSLogEntryLogLevelInfo: return "info";
    case OSLogEntryLogLevelNotice: return "notice";
    case OSLogEntryLogLevelError: return "error";
    case OSLogEntryLogLevelFault: return "fault";
  }
  return "?";
}

void consoleReachesOsLog() {
  @autoreleasepool {
    // A per-run nonce, so an entry can only match the line this run wrote.
    const std::string nonce = [NSUUID UUID].UUIDString.UTF8String;
    // os_log timestamps are wall-clock and can sit a hair before `since`
    // otherwise.
    NSDate* since = [NSDate dateWithTimeIntervalSinceNow:-1];

    screenkit::installPlatformLogSink();

    screenkit::RuntimeConfig config;
    config.name = "oslog-test";
    auto runtime = screenkit::Runtime::create(config);
    if (!runtime) {
      fail("runtime failed to start");
      return;
    }
    const std::string source = "console.log('log " + nonce + "');" +
                               "console.warn('warn " + nonce + "');" +
                               "console.error('error " + nonce + "');";
    const screenkit::EvalResult result = runtime->evaluateSource(source, "oslog.js");
    if (!result.ok) fail("evaluate failed: " + result.error);
    runtime.reset();

    NSArray<OSLogEntryLog*>* entries = readEntries(since, 3);

    struct Row {
      std::string message;
      OSLogEntryLogLevel level;
    };
    // The exact text Console.app shows. `<private>` in place of either field is
    // the regression this exists for; a missing entry is the other.
    const Row rows[] = {
        {"[log] log " + nonce, OSLogEntryLogLevelNotice},
        {"[warn] warn " + nonce, OSLogEntryLogLevelNotice},
        {"[error] error " + nonce, OSLogEntryLogLevelError},
    };
    for (const Row& row : rows) {
      const Found found = find(entries, row.message);
      if (!found.present) {
        std::string seen;
        for (OSLogEntryLog* entry in entries) {
          seen += std::string("\n      ") + entry.composedMessage.UTF8String;
        }
        fail("not in the unified log: \"" + row.message + "\"; this process logged:" +
             (seen.empty() ? std::string(" nothing") : seen));
        continue;
      }
      if (found.category != "oslog-test") {
        fail("\"" + row.message + "\" has category \"" + found.category +
             "\", want the runtime's name \"oslog-test\"");
      }
      if (found.level != row.level) {
        fail("\"" + row.message + "\" logged at " + levelLabel(found.level) + ", want " +
             levelLabel(row.level));
      }
    }
  }
}

}  // namespace

int main() {
  consoleReachesOsLog();
  if (failures == 0) {
    std::fprintf(stderr, "console-os-log: ok\n");
    return 0;
  }
  std::fprintf(stderr, "console-os-log: %d failure(s)\n", failures);
  return 1;
}
