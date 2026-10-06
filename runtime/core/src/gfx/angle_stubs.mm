// Copyright (c) ScreenKit contributors. MIT.
//
// Symbols Godot's prebuilt ANGLE archives reference but do not contain.
//
// Godot links these from its own tree, so its published libEGL/libGLES/libANGLE
// have holes -- and the holes differ per slice, which is why every block here is
// behind a define rather than a platform check. `runtime/cmake/AnglePrebuilt.cmake`
// runs `nm` over the archive that was actually fetched and defines only what is
// genuinely missing: the macOS slice carries angle::GetCurrentSystemTime and
// angle::SetCurrentThreadName, the iOS/tvOS slices do not, and supplying them
// unconditionally is a duplicate-symbol link error on macOS.
//
// The two angle:: entry points are genuine platform utilities and are
// implemented properly. The six astcenc_* entry points belong to the
// third-party ASTC software decoder, which ANGLE uses only to emulate ASTC
// texture formats the GPU lacks; nothing in this milestone binds a texture, so
// they report "not available" and ANGLE disables the feature.
//
// SPIKE SCAFFOLDING. A production build compiles ANGLE from source with
// gn target_platform="tvos" and needs none of this (Architecture.md 10).

#include <cstddef>
#include <cstdint>

#if defined(SCREENKIT_ANGLE_STUB_PLATFORM_UTILS)

#include <mach/mach_time.h>
#include <pthread.h>

namespace angle {

double GetCurrentSystemTime() {
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) mach_timebase_info(&timebase);
  const uint64_t ns = mach_absolute_time() * timebase.numer / timebase.denom;
  return static_cast<double>(ns) * 1e-9;
}

void SetCurrentThreadName(const char* name) {
  pthread_setname_np(name);  // Apple's variant names the calling thread only
}

}  // namespace angle

#endif  // SCREENKIT_ANGLE_STUB_PLATFORM_UTILS

#if defined(SCREENKIT_ANGLE_STUB_ASTCENC)

// The signatures must match exactly or the C++ mangled names will not resolve.
// They are transcribed from astcenc.h as ANGLE consumes it; the enum values are
// irrelevant to a build that never decodes an ASTC block.
enum astcenc_profile { ASTCENC_PRF_LDR_SRGB = 0 };
enum astcenc_error { ASTCENC_SUCCESS = 0, ASTCENC_ERR_NOT_IMPLEMENTED = 1 };
struct astcenc_config;
struct astcenc_context;
struct astcenc_image;
struct astcenc_swizzle;

astcenc_error astcenc_config_init(astcenc_profile, unsigned int, unsigned int, unsigned int, float,
                                  unsigned int, astcenc_config*) {
  return ASTCENC_ERR_NOT_IMPLEMENTED;
}
astcenc_error astcenc_context_alloc(const astcenc_config*, unsigned int, astcenc_context**) {
  return ASTCENC_ERR_NOT_IMPLEMENTED;
}
void astcenc_context_free(astcenc_context*) {}
astcenc_error astcenc_decompress_image(astcenc_context*, const unsigned char*, unsigned long,
                                       astcenc_image*, const astcenc_swizzle*, unsigned int) {
  return ASTCENC_ERR_NOT_IMPLEMENTED;
}
astcenc_error astcenc_decompress_reset(astcenc_context*) {
  return ASTCENC_ERR_NOT_IMPLEMENTED;
}
const char* astcenc_get_error_string(astcenc_error) {
  return "astcenc not linked in this build";
}

#endif  // SCREENKIT_ANGLE_STUB_ASTCENC
