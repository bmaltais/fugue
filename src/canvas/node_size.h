#pragma once

// What a node's content needs, measured: the pieces of auto-size (docs/canvas-look.md 4.1,
// spec 12.12). CanvasModel::layOut asks these when the content changes; nothing here runs per
// frame. Sizes are in world units (px at zoom 1), fonts as the canvas draws them.

#include <QString>

namespace fugue::node_size {

// The width a `line` input's well needs to show `label` and the whole `value` side by side:
// the node's 3u margins, the well's 2u padding and 2u between the two.
qreal lineWidgetWidth(const QString &label, const QString &value, int textSize);

// How many lines `text` takes wrapped to `width` (a multiline value, at the label size).
int wrappedLines(const QString &text, qreal width, int textSize);

// The height of a multiline well that shows `lines` lines: 16 px each and 1.5u above and below.
qreal multilineHeight(int lines, int textSize);

} // namespace fugue::node_size
