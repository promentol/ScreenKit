// Copyright (c) ScreenKit contributors. MIT.
#include "ImageDecode.h"

#include <climits>

// The implementation is already compiled into the vendored GL archive
// (SKGLImageUtils.cpp defines STB_IMAGE_IMPLEMENTATION). Including the header
// alone declares the functions; defining it again here would duplicate them.
#include "stb_image.h"

namespace jsi = facebook::jsi;

namespace screenkit {

DecodedPixels::~DecodedPixels() { stbi_image_free(pixels_); }

bool decodeImage(const std::uint8_t* bytes, std::size_t length, DecodedImage& out, std::string& error) {
  if (bytes == nullptr || length == 0 || length > static_cast<std::size_t>(INT_MAX)) {
    error = "not a decodable image: no bytes";
    return false;
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  const auto undecodable = [&error] {
    const char* reason = stbi_failure_reason();
    error = std::string("not a decodable image") + (reason ? std::string(" (") + reason + ")" : "");
  };
  // The header first: a hostile or huge image is refused before any pixel memory
  // is allocated for it.
  if (!stbi_info_from_memory(bytes, static_cast<int>(length), &w, &h, &comp)) {
    undecodable();
    return false;
  }
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
      static_cast<long long>(w) * static_cast<long long>(h) > 8192LL * 8192LL) {
    error = "image too large to decode: " + std::to_string(w) + "x" + std::to_string(h) +
            " (at most 16384 on a side and 8192x8192 pixels)";
    return false;
  }
  stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(length), &w, &h, &comp, STBI_rgb_alpha);
  if (pixels == nullptr) {
    undecodable();
    return false;
  }
  out.width = w;
  out.height = h;
  out.pixels = std::make_shared<DecodedPixels>(pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  return true;
}

jsi::Object imageObject(jsi::Runtime& runtime, const DecodedImage& image) {
  jsi::Object object(runtime);
  object.setProperty(runtime, "width", image.width);
  object.setProperty(runtime, "height", image.height);
  object.setProperty(runtime, "data", jsi::ArrayBuffer(runtime, image.pixels));
  return object;
}

}  // namespace screenkit
