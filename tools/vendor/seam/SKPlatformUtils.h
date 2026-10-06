#pragma once
//
// Seam file -- ScreenKit's, not expo's. Replaces upstream EXPlatformUtils.h.
//
// Upstream routes its one logging macro two ways: __android_log_print on
// Android, and EXiOSLog on Apple -- which is declared here but implemented in
// EXPlatformUtils.mm, a file outside packages/expo-gl/common/ and therefore
// outside this import. Vendoring the header unchanged would compile and then
// fail to link.
//
// So the macro lands on screenkit::log instead (runtime/core/include/screenkit/
// Log.h), which is the runtime's one log sink on every platform and is what the
// Apple shell already forwards to os_log. Upstream's EXiOSGetOperatingSystemVersion
// is dropped: nothing in common/ calls it.

#include <screenkit/Log.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace screenkit {
namespace gl {

/// printf-shaped, because the vendored call sites are
/// `SKGLSysLog("Failed to setup SKGLContext [%s]", err.what())`. Formats into a
/// std::string and hands that to the runtime's sink.
__attribute__((format(printf, 1, 2))) inline void sysLog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list probe;
  va_copy(probe, args);
  const int needed = std::vsnprintf(nullptr, 0, fmt, probe);
  va_end(probe);

  std::string message;
  if (needed > 0) {
    message.resize(static_cast<std::size_t>(needed));
    // Writing the terminator at message[size()] is well-defined since C++11.
    std::vsnprintf(&message[0], static_cast<std::size_t>(needed) + 1, fmt, args);
  }
  va_end(args);

  screenkit::log(screenkit::LogLevel::Error, "gl", message);
}

}  // namespace gl
}  // namespace screenkit

// Upstream gates this on EXGL_DEBUG, defaulting to on. There is no off switch
// here: these are five call sites on failure paths, and a GL setup failure that
// logs nothing is the worst version of this.
#define SKGLSysLog(fmt, ...) ::screenkit::gl::sysLog(fmt, ##__VA_ARGS__)
