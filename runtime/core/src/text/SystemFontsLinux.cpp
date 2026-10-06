// Copyright (c) ScreenKit contributors. MIT.
//
// System fonts on Linux: the font files under the usual directories, read once.
//
// No fontconfig. A Batocera image or a minimal Pi install may not carry it, and
// what a canvas needs is small -- a family's faces and their weight and slant --
// which SDL_ttf reads from each file directly. The list is built on first use
// and kept for the process.
//
// JS never names a file: a face's name is an index into this list, so
// systemFontData reads only files the scan found, never an arbitrary path.
#include "SystemFonts.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
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
  const std::string ext = lower(path.extension().string());
  return ext == ".ttf" || ext == ".otf";
}

std::vector<std::filesystem::path> fontDirectories() {
  std::vector<std::filesystem::path> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
  if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) {
    dirs.emplace_back(std::filesystem::path(data) / "fonts");
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    dirs.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
    dirs.emplace_back(std::filesystem::path(home) / ".fonts");
  }
  return dirs;
}

const std::vector<InstalledFace>& installedFaces() {
  static std::vector<InstalledFace> faces;
  static std::once_flag once;
  std::call_once(once, [] {
    if (!TTF_Init()) return;
    for (const auto& dir : fontDirectories()) {
      std::error_code ec;
      std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
      for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isFontFile(it->path())) continue;
        TTF_Font* font = TTF_OpenFont(it->path().c_str(), 12);
        if (!font) continue;
        const char* family = TTF_GetFontFamilyName(font);
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
  // The families common Linux images carry for each generic, most likely first;
  // anything installed at all when none of them is.
  static const std::array<const char*, 5> kSans = {"DejaVu Sans", "Liberation Sans", "Noto Sans", "FreeSans", "Roboto"};
  static const std::array<const char*, 5> kSerif = {"DejaVu Serif", "Liberation Serif", "Noto Serif", "FreeSerif", "DejaVu Sans"};
  static const std::array<const char*, 5> kMono = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "FreeMono", "DejaVu Sans"};
  const auto& candidates = generic == "serif" ? kSerif : generic == "monospace" ? kMono : kSans;
  for (const char* candidate : candidates) {
    if (hasFamily(candidate)) return candidate;
  }
  const auto& faces = installedFaces();
  return faces.empty() ? std::string() : faces.front().family;
}

}  // namespace screenkit::text
