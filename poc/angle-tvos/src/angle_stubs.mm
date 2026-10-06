// Symbols Godot's prebuilt ANGLE archives reference but do not contain.
//
// Godot links these from its own tree, so its published libEGL/libGLES/libANGLE
// have holes. Two are genuine ANGLE platform utilities and are implemented here
// properly. The six astcenc_* entry points belong to the third-party ASTC
// software decoder, which ANGLE uses only to emulate ASTC texture formats the
// GPU lacks -- a hello-world that binds no textures never reaches them, so they
// report "not available" and ANGLE disables the feature.
//
// SPIKE SCAFFOLDING. A production build compiles ANGLE from source with
// gn target_platform="tvos" and needs none of this.

#include <pthread.h>
#include <mach/mach_time.h>
#include <cstddef>

namespace angle {

double GetCurrentSystemTime() {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    const uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
    return double(ns) * 1e-9;
}

void SetCurrentThreadName(const char *name) {
    pthread_setname_np(name);   // Apple's variant names the calling thread only
}

}  // namespace angle

// --- astcenc: report unavailable ------------------------------------------
// Signatures must match exactly or the C++ mangled names will not resolve.
enum astcenc_profile { ASTCENC_PRF_LDR_SRGB = 0 };
enum astcenc_error { ASTCENC_SUCCESS = 0, ASTCENC_ERR_NOT_IMPLEMENTED = 1 };
struct astcenc_config;
struct astcenc_context;
struct astcenc_image;
struct astcenc_swizzle;

astcenc_error astcenc_config_init(astcenc_profile, unsigned int, unsigned int,
                                  unsigned int, float, unsigned int, astcenc_config *) {
    return ASTCENC_ERR_NOT_IMPLEMENTED;
}
astcenc_error astcenc_context_alloc(const astcenc_config *, unsigned int, astcenc_context **) {
    return ASTCENC_ERR_NOT_IMPLEMENTED;
}
void astcenc_context_free(astcenc_context *) {}
astcenc_error astcenc_decompress_image(astcenc_context *, const unsigned char *, unsigned long,
                                       astcenc_image *, const astcenc_swizzle *, unsigned int) {
    return ASTCENC_ERR_NOT_IMPLEMENTED;
}
astcenc_error astcenc_decompress_reset(astcenc_context *) {
    return ASTCENC_ERR_NOT_IMPLEMENTED;
}
const char *astcenc_get_error_string(astcenc_error) {
    return "astcenc not linked in this build";
}
