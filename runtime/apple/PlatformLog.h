// Copyright (c) ScreenKit contributors. MIT.
#pragma once

namespace screenkit {

/// Route screenkit::log through os_log, which is what "the platform log" means
/// on macOS and tvOS: `log stream --predicate 'subsystem == "dev.screenkit"'`,
/// or Console.app. The sink also writes to stderr so `simctl launch --console`
/// and a plain terminal run show the same lines without a second tool.
void installPlatformLogSink();

}  // namespace screenkit
