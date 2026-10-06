// Copyright (c) ScreenKit contributors. MIT.
#include "FrameCapture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GLES3/gl3.h>
#include <zlib.h>

#include <screenkit/Log.h>

#include "GlSurface.h"

namespace screenkit {
namespace gfx {
namespace {

constexpr const char* kTag = "screenkit.gl";

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

void appendChunk(std::vector<std::uint8_t>& out, const char type[4], const std::vector<std::uint8_t>& data) {
  appendU32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uLong crc = crc32(0L, out.data() + start, static_cast<uInt>(out.size() - start));
  appendU32(out, static_cast<std::uint32_t>(crc));
}

}  // namespace

std::unique_ptr<FrameCapture> FrameCapture::fromEnvironment() {
  const char* path = std::getenv("SCREENKIT_CAPTURE");
  if (path == nullptr || *path == '\0') return nullptr;
  double delayMs = 2000;
  if (const char* delay = std::getenv("SCREENKIT_CAPTURE_DELAY_MS")) delayMs = std::atof(delay);
  return std::make_unique<FrameCapture>(path, delayMs);
}

void FrameCapture::beforePresent(GlSurface& surface, double nowMs) {
  if (done_) return;
  if (firstFrameMs_ < 0) firstFrameMs_ = nowMs;
  if (nowMs - firstFrameMs_ < delayMs_) return;
  done_ = true;

  const int width = surface.width();
  const int height = surface.height();
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  // GL errors stay latched until read, so one the app's frame left behind would
  // be blamed on the read below. Clear them first.
  while (glGetError() != GL_NO_ERROR) {
  }
  glBindFramebuffer(GL_FRAMEBUFFER, surface.defaultFramebuffer());
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  if (glGetError() != GL_NO_ERROR) {
    log(LogLevel::Error, kTag, "frame capture: glReadPixels failed");
    return;
  }
  // GL's first row is the bottom one; a PNG's is the top.
  std::vector<std::uint8_t> rows(pixels.size());
  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    std::memcpy(rows.data() + static_cast<std::size_t>(y) * stride,
                pixels.data() + static_cast<std::size_t>(height - 1 - y) * stride, stride);
  }
  for (std::size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;

  std::string error;
  if (writePng(path_, width, height, rows, error)) {
    log(LogLevel::Log, kTag, "frame capture: wrote " + std::to_string(width) + "x" + std::to_string(height) +
                                 " to " + path_);
  } else {
    log(LogLevel::Error, kTag, "frame capture: " + error);
  }
}

bool writePng(const std::string& path, int width, int height, const std::vector<std::uint8_t>& rgba,
              std::string& error) {
  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  std::vector<std::uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    const auto row = rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * stride);
    raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(stride));
  }
  uLongf bound = compressBound(static_cast<uLong>(raw.size()));
  std::vector<std::uint8_t> compressed(bound);
  if (compress2(compressed.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
    error = "zlib could not compress the frame";
    return false;
  }
  compressed.resize(bound);

  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<std::uint8_t> header;
  appendU32(header, static_cast<std::uint32_t>(width));
  appendU32(header, static_cast<std::uint32_t>(height));
  header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace
  appendChunk(png, "IHDR", header);
  appendChunk(png, "IDAT", compressed);
  appendChunk(png, "IEND", {});

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    error = "could not open " + path;
    return false;
  }
  const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  std::fclose(file);
  if (!ok) error = "could not write " + path;
  return ok;
}

}  // namespace gfx
}  // namespace screenkit
