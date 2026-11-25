#pragma once

// Text to a coverage mask with no GUI and no system font (spec 14.2, Draw Text #549): stb_truetype
// reads a TrueType file, so the run worker and the CLI (a QCoreApplication, where QFontDatabase and
// QPainter text are unavailable) draw the same pixels as the window does. Plain functions, reused by
// every node that writes a label.

#include <QByteArray>
#include <QString>

#include "engine/result.h"
#include "nodes/resample.h"

namespace fugue::nodes {

enum class TextAlign { Left, Center, Right };

// The font Fugue bundles (Noto Sans, Latin subset, SIL OFL 1.1: share/fonts/LICENSE.notosans).
QByteArray bundledFont();

// `text` as a one-channel coverage image (0..1), as tight as the lines are: the width of the longest
// line, `lineCount * line height` tall. `pixelSize` is the font's em height in pixels (ascent plus
// descent); `font` is the bytes of a .ttf (an .otf with TrueType outlines too), empty for the
// bundled one. A line breaks at a newline; with `maxWidth` > 0 it also breaks between words when it
// would pass that width (a word longer than the width breaks between characters). `align` places each
// line in the block. Empty or all-blank text gives a 0 x 0 image. `invalid_font` when the bytes are not a font;
// `value_out_of_range` when the result would pass 16384 pixels a side. A glyph the font lacks shows as its notdef box.
Result<Pixels> rasterizeText(const QString &text, const QByteArray &font, float pixelSize, int maxWidth, TextAlign align);

} // namespace fugue::nodes
