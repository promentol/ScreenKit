// Copyright (c) ScreenKit contributors. MIT.
//
// The fonts one runtime has loaded, drawn through SDL3_ttf: FreeType rasterises
// the glyphs, HarfBuzz shapes the run. This is the native half of the 2D
// canvas's text -- measuring a string and turning it into a coverage mask. Where
// that mask lands, in what colour, under which transform and clip, is the DOM
// shim's (runtime/js/dom-shim.js), exactly as for every other 2D draw.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TTF_Font;

namespace screenkit::text {

/// How a face is drawn. Bold and italic ask SDL_ttf to synthesise the style,
/// which it skips for a face that already has it.
struct FontStyle {
  float size = 10;   // pixels per em
  bool bold = false;
  bool italic = false;
  bool kerning = true;
  int outline = 0;   // stroke half-width, in pixels
  // How the stroke's corners are drawn when `outline` > 0, as a 2D canvas names them.
  enum class Join { Round, Bevel, Miter } join = Join::Miter;
  float miterLimit = 10;
};

/// What a font file says about itself.
struct FaceInfo {
  std::string family;
  std::string style;
  int weight = 400;  // CSS weight, 100-900
  bool italic = false;
};

/// Pixel metrics of a face at a size. Descent is a distance below the baseline,
/// so it is positive for every ordinary font.
struct FontMetrics {
  int ascent = 0;
  int descent = 0;
  int lineSkip = 0;
};

/// A shaped run's advance and its ink, relative to the pen origin on the
/// baseline. `left` is how far ink reaches left of the origin (negative when it
/// starts to the right of it); `ascent`/`descent` are how far it reaches above
/// and below the baseline.
struct TextExtent {
  double width = 0;
  double left = 0;
  double right = 0;
  double ascent = 0;
  double descent = 0;
};

/// A run drawn as 8-bit coverage. The pen origin is column `originX` on row
/// `baseline`. An empty mask (width 0) is a run with no ink, such as spaces.
struct TextMask {
  int width = 0;
  int height = 0;
  int originX = 0;
  int baseline = 0;
  std::vector<std::uint8_t> alpha;
};

class FontLibrary {
 public:
  FontLibrary();
  ~FontLibrary();
  FontLibrary(const FontLibrary&) = delete;
  FontLibrary& operator=(const FontLibrary&) = delete;

  /// A font file in memory (TrueType, OpenType, WOFF). Returns the face id, or
  /// -1 with `error` saying why.
  int addFace(std::vector<std::uint8_t> bytes, std::string& error);

  /// Null for an unknown id.
  const FaceInfo* face(int id) const;

  bool metrics(int face, const FontStyle& style, FontMetrics& out, std::string& error);
  bool measure(int face, const FontStyle& style, const std::string& utf8, TextExtent& out,
               std::string& error);
  bool render(int face, const FontStyle& style, const std::string& utf8, TextMask& out,
              std::string& error);

  /// Close every font. The library stays usable; its faces are gone.
  void clear();

 private:
  struct Face;
  TTF_Font* instance(int face, const FontStyle& style, std::string& error);

  std::vector<std::unique_ptr<Face>> faces_;
  std::uint64_t clock_ = 0;
  bool initialized_ = false;
};

}  // namespace screenkit::text
