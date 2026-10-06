// Copyright (c) ScreenKit contributors. MIT.
//
// The fonts the operating system has installed, as font files FontLibrary can
// load. A canvas asks for "20px Arial" or "bold 16px sans-serif" without ever
// loading a font, and a browser answers from the system; so does this.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace screenkit::text {

struct SystemFace {
  std::string name;  // PostScript name: what systemFontData takes
  int weight = 400;  // CSS weight, 100-900
  bool italic = false;
};

/// Every face of an installed family, matched case-insensitively. Empty when the
/// system has no such family, or no font service at all.
std::vector<SystemFace> systemFaces(const std::string& family);

/// One face as a complete font file, or empty when there is no such face.
std::vector<std::uint8_t> systemFontData(const std::string& postscriptName);

/// The installed family a CSS generic family (`sans-serif`, `serif`,
/// `monospace`, `cursive`, `fantasy`, `system-ui`) stands for, or "" when the
/// system has none for it.
std::string systemGenericFamily(const std::string& generic);

}  // namespace screenkit::text
