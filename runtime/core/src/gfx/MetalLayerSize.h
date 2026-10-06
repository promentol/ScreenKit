// Copyright (c) ScreenKit contributors. MIT.
#pragma once

namespace screenkit {
namespace gfx {

/// The pixel size of a CALayer: its bounds times its contentsScale. For the
/// CAMetalLayer under a window surface, this is the size ANGLE gives the next
/// drawable it takes. False only for a null layer.
///
/// Reads layer properties from the calling thread, as ANGLE itself does from the
/// GL thread when it checks for a resize.
bool metalLayerPixelSize(void* layer, int& width, int& height);

}  // namespace gfx
}  // namespace screenkit
