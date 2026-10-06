// Copyright (c) ScreenKit contributors. MIT.
//
// System fonts on Android: the font files in /system/fonts, read once.
//
// Android's own font service is Java (Typeface, and since API 29 SystemFonts),
// and /system/fonts is world-readable on every release, so the files are read
// directly, the way SystemFontsLinux.cpp reads /usr/share/fonts: SDL_ttf opens
// each one for its family, weight and slant. The list is built on first use --
// the first canvas text that names a family the app did not load -- and kept
// for the process.
//
// The generic families are the platform's own defaults: Roboto for
// `sans-serif`, Noto Serif for `serif`, Droid Sans Mono for `monospace`
// (/system/etc/fonts.xml). Fire OS ships the same files.
//
// JS never names a file: a face's name is its path in this list, so
// systemFontData reads only files the scan found, never an arbitrary path.
#include "SystemFonts.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>

#include <SDL3_ttf/SDL_ttf.h>

namespace screenkit::text {
namespace {

struct InstalledFace {
  std::string path;
  std::string family;
  int weight = 400;
  bool italic = false;
};

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isFontFile(const std::filesystem::path& path) {
  // .ttc collections (the CJK faces) hold several fonts in one file, which
  // TTF_OpenFont reads only the first of; they are skipped, as on Linux.
  const std::string ext = lower(path.extension().string());
  return ext == ".ttf" || ext == ".otf";
}

// /system/fonts on every Android; /product/fonts holds OEM additions since API 29.
constexpr std::array<const char*, 2> kFontDirectories = {"/system/fonts", "/product/fonts"};

const std::vector<InstalledFace>& installedFaces() {
  static std::vector<InstalledFace> faces;
  static std::once_flag once;
  std::call_once(once, [] {
    if (!TTF_Init()) return;
    for (const char* dir : kFontDirectories) {
      std::error_code ec;
      std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
      for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isFontFile(it->path())) continue;
        TTF_Font* font = TTF_OpenFont(it->path().c_str(), 12);
        if (!font) continue;
        const char* family = TTF_GetFontFamilyName(font);
        // Not scalable: bitmap-only faces such as NotoColorEmoji.
        if (family && *family && TTF_FontIsScalable(font)) {
          InstalledFace face;
          face.path = it->path().string();
          face.family = family;
          const int weight = TTF_GetFontWeight(font);
          face.weight = weight > 0 ? weight : 400;
          face.italic = (TTF_GetFontStyle(font) & TTF_STYLE_ITALIC) != 0;
          faces.push_back(std::move(face));
        }
        TTF_CloseFont(font);
      }
    }
    // A stable order, so the same family resolves to the same file every run.
    std::sort(faces.begin(), faces.end(),
              [](const InstalledFace& a, const InstalledFace& b) { return a.path < b.path; });
    TTF_Quit();
  });
  return faces;
}

bool hasFamily(const std::string& family) {
  const std::string wanted = lower(family);
  for (const auto& face : installedFaces()) {
    if (lower(face.family) == wanted) return true;
  }
  return false;
}

}  // namespace

std::vector<SystemFace> systemFaces(const std::string& family) {
  std::vector<SystemFace> out;
  const std::string wanted = lower(family);
  for (const auto& face : installedFaces()) {
    if (lower(face.family) != wanted) continue;
    SystemFace system;
    system.name = face.path;
    system.weight = face.weight;
    system.italic = face.italic;
    out.push_back(std::move(system));
  }
  return out;
}

std::vector<std::uint8_t> systemFontData(const std::string& name) {
  for (const auto& face : installedFaces()) {
    if (face.path != name) continue;
    std::ifstream in(face.path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
  }
  return {};
}

std::string systemGenericFamily(const std::string& generic) {
  // Android's defaults first (fonts.xml), then what older or OEM images carry
  // instead; anything installed at all when none of them is. Roboto on API 30+
  // is one variable file, so its bold is synthesised from the regular instance.
  static const std::array<const char*, 4> kSans = {"Roboto", "Roboto Static", "Noto Sans", "Droid Sans"};
  static const std::array<const char*, 4> kSerif = {"Noto Serif", "Droid Serif", "Source Serif Pro", "Roboto"};
  static const std::array<const char*, 4> kMono = {"Droid Sans Mono", "Noto Sans Mono", "Cutive Mono", "Roboto"};
  const auto& candidates = generic == "serif" ? kSerif : generic == "monospace" ? kMono : kSans;
  for (const char* candidate : candidates) {
    if (hasFamily(candidate)) return candidate;
  }
  const auto& faces = installedFaces();
  return faces.empty() ? std::string() : faces.front().family;
}

}  // namespace screenkit::text
