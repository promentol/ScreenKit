// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <functional>
#include <string>

namespace screenkit {

enum class LogLevel { Log, Warn, Error };

/// "log" / "warn" / "error" -- stable, used as the os_log category suffix and
/// as the stderr prefix, so tests can match on it.
const char* levelName(LogLevel level);

/// Where `console` output ends up. The default sink writes to stderr -- to
/// logcat, tag `ScreenKit`, on Android; the Apple platform layer replaces it with
/// one that also reaches os_log, which is what "the platform log" means on macOS
/// and tvOS.
///
/// A sink is called from whichever thread logged -- in practice the JS thread --
/// so it must be thread-safe. `setLogSink` is safe from any thread, and returns
/// only once no call is still inside the sink it replaced, so whatever that sink
/// captured can be freed straight after. It must not be called from inside a
/// sink, which would wait on itself.
using LogSink =
    std::function<void(LogLevel level, const std::string& tag, const std::string& message)>;

void setLogSink(LogSink sink);
void log(LogLevel level, const std::string& tag, const std::string& message);

}  // namespace screenkit
