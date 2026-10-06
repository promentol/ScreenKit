// Copyright (c) ScreenKit contributors. MIT.
#include "FontLibrary.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace screenkit::text {
namespace {

// A face is opened once per distinct style it is drawn in, because SDL_ttf
// flushes its glyph cache whenever a font's size or style changes -- and a
// canvas that measures at one size and draws at another would otherwise
// re-rasterise every glyph on every call.
constexpr std::size_t kInstancesPerFace = 8;

// Larger than any texture a TV GPU takes; a run this big is a bug, not text.
constexpr int kMaxMaskSide = 8192;

struct InstanceKey {
  int size64;  // size in 1/64 px: scale factors make 26 and 26.0000001 the same size
  bool bold;
  bool italic;
  bool kerning;
  int outline;
  FontStyle::Join join;
  int miterLimit64;

  bool operator==(const InstanceKey& other) const {
    return size64 == other.size64 && bold == other.bold && italic == other.italic &&
           kerning == other.kerning && outline == other.outline &&
           (outline == 0 || (join == other.join && miterLimit64 == other.miterLimit64));
  }
};

InstanceKey keyFor(const FontStyle& style) {
  return {static_cast<int>(std::lround(style.size * 64.0f)), style.bold, style.italic, style.kerning,
          style.outline, style.join, static_cast<int>(std::lround(style.miterLimit * 64.0f))};
}

std::string sdlError(const char* what) {
  const char* why = SDL_GetError();
  return std::string(what) + (why && *why ? std::string(": ") + why : std::string());
}

}  // namespace

struct FontLibrary::Face {
  struct Instance {
    InstanceKey key;
    TTF_Font* font;
    std::uint64_t lastUse;
  };

  // Every instance reads from these bytes, so they outlive all of them: the
  // destructor closes the instances before the vector goes.
  std::vector<std::uint8_t> bytes;
  FaceInfo info;
  std::vector<Instance> instances;

  ~Face() {
    for (auto& instance : instances) TTF_CloseFont(instance.font);
  }

  TTF_Font* open(float size, std::string& error) const {
    SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (!stream) {
      error = sdlError("SDL_IOFromConstMem failed");
      return nullptr;
    }
    TTF_Font* font = TTF_OpenFontIO(stream, true, size);
    if (!font) error = sdlError("not a font SDL_ttf can read");
    return font;
  }
};

FontLibrary::FontLibrary() : initialized_(TTF_Init()) {}

FontLibrary::~FontLibrary() {
  clear();
  if (initialized_) TTF_Quit();
}

void FontLibrary::clear() { faces_.clear(); }

int FontLibrary::addFace(std::vector<std::uint8_t> bytes, std::string& error) {
  if (!initialized_) {
    error = sdlError("TTF_Init failed");
    return -1;
  }
  if (bytes.empty()) {
    error = "empty font data";
    return -1;
  }
  auto face = std::make_unique<Face>();
  face->bytes = std::move(bytes);
  TTF_Font* probe = face->open(16, error);
  if (!probe) return -1;
  if (!TTF_FontIsScalable(probe)) {
    TTF_CloseFont(probe);
    error = "bitmap-only fonts are not supported";
    return -1;
  }
  const char* family = TTF_GetFontFamilyName(probe);
  const char* style = TTF_GetFontStyleName(probe);
  face->info.family = family ? family : "";
  face->info.style = style ? style : "";
  const int weight = TTF_GetFontWeight(probe);
  face->info.weight = weight > 0 ? std::clamp(weight, 1, 1000) : 400;
  face->info.italic = (TTF_GetFontStyle(probe) & TTF_STYLE_ITALIC) != 0;
  TTF_CloseFont(probe);
  faces_.push_back(std::move(face));
  return static_cast<int>(faces_.size() - 1);
}

const FaceInfo* FontLibrary::face(int id) const {
  if (id < 0 || static_cast<std::size_t>(id) >= faces_.size() || !faces_[id]) return nullptr;
  return &faces_[id]->info;
}

TTF_Font* FontLibrary::instance(int id, const FontStyle& style, std::string& error) {
  if (!face(id)) {
    error = "unknown font face " + std::to_string(id);
    return nullptr;
  }
  if (!(style.size > 0) || !std::isfinite(style.size) || style.size > kMaxMaskSide || style.outline < 0 ||
      style.outline > kMaxMaskSide || !(style.miterLimit >= 0) || !std::isfinite(style.miterLimit)) {
    error = "font size out of range";
    return nullptr;
  }
  Face& face = *faces_[id];
  const InstanceKey key = keyFor(style);
  for (auto& instance : face.instances) {
    if (instance.key == key) {
      instance.lastUse = ++clock_;
      return instance.font;
    }
  }

  TTF_Font* font = face.open(style.size, error);
  if (!font) return nullptr;
  // Light hinting with subpixel positioning: glyphs keep their designed
  // proportions and a run's width scales with its size, which is how a browser
  // on a Mac draws a canvas. Full hinting snaps every advance to whole pixels.
  TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
  TTF_SetFontKerning(font, style.kerning);
  TTF_SetFontStyle(font, (style.bold ? TTF_STYLE_BOLD : 0) | (style.italic ? TTF_STYLE_ITALIC : 0));
  if (style.outline > 0) {
    // FreeType's stroker takes its join and miter limit from the font's
    // properties when the outline is set (SDL_ttf.c, TTF_SetFontOutline). The
    // limit is 16.16 fixed point; the constants are FT_Stroker_LineJoin's.
    const SDL_PropertiesID props = TTF_GetFontProperties(font);
    const Sint64 join = style.join == FontStyle::Join::Round ? 0 : style.join == FontStyle::Join::Bevel ? 1 : 3;
    SDL_SetNumberProperty(props, TTF_PROP_FONT_OUTLINE_LINE_JOIN_NUMBER, join);
    SDL_SetNumberProperty(props, TTF_PROP_FONT_OUTLINE_MITER_LIMIT_NUMBER,
                          static_cast<Sint64>(std::lround(std::max(1.0f, style.miterLimit) * 65536.0f)));
    if (!TTF_SetFontOutline(font, style.outline)) {
      error = sdlError("TTF_SetFontOutline failed");
      TTF_CloseFont(font);
      return nullptr;
    }
  }

  if (face.instances.size() >= kInstancesPerFace) {
    auto oldest = std::min_element(face.instances.begin(), face.instances.end(),
                                   [](const Face::Instance& a, const Face::Instance& b) {
                                     return a.lastUse < b.lastUse;
                                   });
    TTF_CloseFont(oldest->font);
    face.instances.erase(oldest);
  }
  face.instances.push_back({key, font, ++clock_});
  return font;
}

bool FontLibrary::metrics(int id, const FontStyle& style, FontMetrics& out, std::string& error) {
  TTF_Font* font = instance(id, style, error);
  if (!font) return false;
  out.ascent = TTF_GetFontAscent(font);
  out.descent = -TTF_GetFontDescent(font);
  out.lineSkip = TTF_GetFontLineSkip(font);
  return true;
}

namespace {

/// Where the pen origin sits inside the surface SDL_ttf renders a run into, and
/// how far ink reaches above and below the baseline.
///
/// SDL_ttf sizes that surface to the run's ink and shifts the pen right by any
/// ink left of the origin and down by any ink above the ascent, but does not
/// return either shift. Both follow from glyph metrics the same way it derives
/// them (SDL_ttf.c, TTF_Size_Internal): only the first glyph can reach left of
/// the origin, and a glyph reaches above the ascent by its top bearing minus the
/// ascent. Metrics are per code point, so shaping that moves a glyph vertically
/// (a stacked mark) is not seen -- a sub-pixel matter for the scripts a canvas
/// on a TV draws.
struct RunInk {
  int firstLeft = 0;
  int top = 0;
  int bottom = 0;
};

RunInk runInk(TTF_Font* font, const std::string& utf8) {
  RunInk ink;
  const int outline = TTF_GetFontOutline(font);
  const char* cursor = utf8.data();
  std::size_t left = utf8.size();
  bool first = true;
  while (left > 0) {
    const Uint32 ch = SDL_StepUTF8(&cursor, &left);
    int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
    if (!TTF_GetGlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance)) continue;
    maxy -= 2 * outline;  // TTF_GetGlyphMetrics folds the outline into the top
    if (first) ink.firstLeft = minx;
    first = false;
    if (maxx > minx || maxy > miny) {
      ink.top = std::max(ink.top, maxy);
      ink.bottom = std::max(ink.bottom, -miny);
    }
  }
  return ink;
}

}  // namespace

bool FontLibrary::measure(int id, const FontStyle& style, const std::string& utf8, TextExtent& out,
                          std::string& error) {
  out = TextExtent{};
  TTF_Font* font = instance(id, style, error);
  if (!font) return false;
  if (utf8.empty()) return true;
  int w = 0, h = 0;
  if (!TTF_GetStringSize(font, utf8.data(), utf8.size(), &w, &h)) {
    error = sdlError("TTF_GetStringSize failed");
    return false;
  }
  const RunInk ink = runInk(font, utf8);
  const int outline = TTF_GetFontOutline(font);
  // The surface width less the leftward shift is the larger of the advance and
  // the ink's right edge -- for an upright run, the advance.
  const int reach = std::max(0, w - 2 * outline - std::max(0, -ink.firstLeft));
  out.width = reach;
  out.left = -ink.firstLeft;
  out.right = reach;
  out.ascent = ink.top;
  out.descent = ink.bottom;
  return true;
}

bool FontLibrary::render(int id, const FontStyle& style, const std::string& utf8, TextMask& out,
                         std::string& error) {
  out = TextMask{};
  TTF_Font* font = instance(id, style, error);
  if (!font) return false;
  if (utf8.empty()) return true;

  int w = 0, h = 0;
  if (!TTF_GetStringSize(font, utf8.data(), utf8.size(), &w, &h)) {
    error = sdlError("TTF_GetStringSize failed");
    return false;
  }
  if (w <= 0 || h <= 0) return true;
  if (w > kMaxMaskSide || h > kMaxMaskSide) {
    error = "text run is " + std::to_string(w) + "x" + std::to_string(h) + " pixels, over the " +
            std::to_string(kMaxMaskSide) + " limit";
    return false;
  }

  SDL_Surface* surface = TTF_RenderText_Blended(font, utf8.data(), utf8.size(), SDL_Color{255, 255, 255, 255});
  if (!surface) {
    error = sdlError("TTF_RenderText_Blended failed");
    return false;
  }
  if (surface->format != SDL_PIXELFORMAT_ARGB8888) {
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surface);
    surface = converted;
    if (!surface) {
      error = sdlError("SDL_ConvertSurface failed");
      return false;
    }
  }

  const RunInk ink = runInk(font, utf8);
  const int outline = TTF_GetFontOutline(font);
  // TTF_GetFontAscent reports an outlined font's ascent grown by the stroke on
  // both sides; the baseline sits at the face's own ascent.
  const int ascent = TTF_GetFontAscent(font) - 2 * outline;
  out.width = surface->w;
  out.height = surface->h;
  out.originX = std::max(0, -ink.firstLeft) + outline;
  out.baseline = std::max(0, ink.top - ascent) + outline + ascent;
  out.alpha.resize(static_cast<std::size_t>(out.width) * out.height);
  // Blended text in white: every pixel is 0xAARRGGBB with the coverage in AA.
  for (int y = 0; y < out.height; ++y) {
    const auto* row = static_cast<const Uint32*>(surface->pixels) + y * (surface->pitch / 4);
    std::uint8_t* dst = out.alpha.data() + static_cast<std::size_t>(y) * out.width;
    for (int x = 0; x < out.width; ++x) dst[x] = static_cast<std::uint8_t>(row[x] >> 24);
  }
  SDL_DestroySurface(surface);
  return true;
}

}  // namespace screenkit::text
