// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

namespace screenkit {

namespace text {
class FontLibrary;
}

/// Install `__screenkit.text`: the fonts under the 2D canvas's text
/// (runtime/js/README.md, "Canvas 2D"). Sizes are pixels per em; `flags` is
/// 1 bold, 2 italic (each synthesised only when the face lacks it), 4 no kerning,
/// and for an outline 8 round joins or 16 bevel joins (miter otherwise).
///
///   addFont(bytes) -> {face, family, style, weight, italic}      throws on bad data
///   systemFaces(family) -> [{name, weight, italic}]               [] when not installed
///   addSystemFont(name) -> {face, family, style, weight, italic} | null
///   genericFamily(generic) -> family | ''                         e.g. 'sans-serif' -> 'Helvetica'
///   metrics(face, size, flags) -> {ascent, descent, lineSkip}
///   measure(face, size, flags, text) -> {width, left, right, ascent, descent}
///   render(face, size, flags, outline, text, miterLimit?)
///       -> {width, height, originX, baseline, data: ArrayBuffer} | null
///
/// `render` returns 8-bit coverage, one byte per pixel, with the pen origin at
/// (originX, baseline); null for a run with no ink. `outline` > 0 draws the
/// stroke of that half-width instead of the fill.
///
/// `__screenkit` must already exist (installHostIO). JS thread only. The library
/// is returned so the host can close its fonts at teardown (shutdownText).
std::shared_ptr<text::FontLibrary> installText(facebook::jsi::Runtime& runtime);

/// Close every font while the runtime still exists. JS thread only; idempotent.
void shutdownText(text::FontLibrary& library);

}  // namespace screenkit
