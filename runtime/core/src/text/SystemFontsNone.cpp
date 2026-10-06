// Copyright (c) ScreenKit contributors. MIT.
//
// A platform without a font service yet: no system fonts. Text still draws in
// any font an app loads itself (FontFace, document.fonts).
#include "SystemFonts.h"

namespace screenkit::text {

std::vector<SystemFace> systemFaces(const std::string&) { return {}; }

std::vector<std::uint8_t> systemFontData(const std::string&) { return {}; }

std::string systemGenericFamily(const std::string&) { return ""; }

}  // namespace screenkit::text
