// Copyright (c) ScreenKit contributors. MIT.
#include "MetalLayerSize.h"

#import <QuartzCore/QuartzCore.h>

namespace screenkit {
namespace gfx {

bool metalLayerPixelSize(void* layer, int& width, int& height) {
  if (layer == nullptr) return false;
  CALayer* caLayer = (__bridge CALayer*)layer;
  const CGSize bounds = caLayer.bounds.size;
  const CGFloat scale = caLayer.contentsScale;
  // ANGLE's WindowSurfaceMtl::calcExpectedDrawableSize, and the same truncation
  // its getSize() applies when it hands the size to EGL.
  width = static_cast<int>(bounds.width * scale);
  height = static_cast<int>(bounds.height * scale);
  return true;
}

}  // namespace gfx
}  // namespace screenkit
