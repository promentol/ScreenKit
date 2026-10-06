// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <jsi/jsi.h>

namespace screenkit {

/// RGBA8 pixels decoded by stb_image, owned by it until the last holder lets go.
/// A `jsi::MutableBuffer`, so the pixels become an ArrayBuffer without a copy.
class DecodedPixels final : public facebook::jsi::MutableBuffer {
 public:
  DecodedPixels(std::uint8_t* pixels, std::size_t size) : pixels_(pixels), size_(size) {}
  ~DecodedPixels() override;
  DecodedPixels(const DecodedPixels&) = delete;
  DecodedPixels& operator=(const DecodedPixels&) = delete;
  std::size_t size() const override { return size_; }
  std::uint8_t* data() override { return pixels_; }

 private:
  std::uint8_t* pixels_;
  std::size_t size_;
};

struct DecodedImage {
  int width = 0;
  int height = 0;
  std::shared_ptr<DecodedPixels> pixels;
};

/// Encoded image bytes in memory -> RGBA8. False with `error` saying why: not an
/// image stb_image reads, or larger than 16384 on a side or 8192x8192 pixels --
/// refused from the header, before any pixel memory is allocated. Safe from any
/// thread: stb_image keeps its failure reason thread-local.
bool decodeImage(const std::uint8_t* bytes, std::size_t length, DecodedImage& out, std::string& error);

/// The JS shape texImage2D's image-source path uploads: `{width, height, data}`.
facebook::jsi::Object imageObject(facebook::jsi::Runtime& runtime, const DecodedImage& image);

}  // namespace screenkit
