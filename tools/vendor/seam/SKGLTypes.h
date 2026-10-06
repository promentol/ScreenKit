#pragma once
//
// Seam file -- ScreenKit's, not expo's. Replaces upstream EXGLNativeApi.h.
//
// Upstream's EXGLNativeApi.h carries two unrelated things: the two id typedefs
// the whole tree needs, and the C entry points expo's ObjC/Java layer calls to
// drive expo's own context manager (EXGLContextCreate, EXGLContextFlush,
// EXGLContextSetDefaultFramebuffer, ...). We already own the drawable in
// runtime/core/src/gfx/GlSurface, so only the typedefs are kept and the C API
// is left behind -- Architecture.md 9: native owns context management, the
// WebGL object model lives above it.
//
// The Apple branch of upstream's GL include is gone for the same reason it is
// gone from pch.h: <OpenGLES/ES3/gl.h> is Apple's deprecated EAGL GLES, not
// ANGLE's.

#include <GLES3/gl3.h>

namespace screenkit {
namespace gl {

/// Identifies a GL context. No context has the id 0, so 0 reads as null.
using SKGLContextId = unsigned int;

/// Identifies a virtual GL object: a handle JS holds immediately, which maps to
/// a real GL name once the batch that creates it has run on the GL thread. No
/// object has the id 0.
using SKGLObjectId = unsigned int;

}  // namespace gl
}  // namespace screenkit
