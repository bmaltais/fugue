#pragma once

// Which picture a Compare Images node's face draws (#371): the 512 px copy of a result, or the file itself.
// The copy is cheap and enough while the node is small; once one copy pixel covers more than one screen
// pixel the copy shows blocks that are not real pixels, so the face asks for the file. Plain numbers, no Qt
// (src/canvas/compare_detail_qml.h gives it to QML; CompareView.qml draws the file over the copy once loaded).

#include <algorithm>

namespace fugue::compare {

// The width of the copies the run thread makes (run_service.cpp, kThumbnailWidth): a file narrower than this is its own copy.
inline constexpr double kCopyWidth = 512;

inline double copyWidthOf(double fileWidth) { return std::min(fileWidth, kCopyWidth); }

// Screen pixels one copy pixel covers: the shown scale is screen pixels per FILE pixel.
inline double copyPixelScale(double shownScale, double copyWidth, double fileWidth) {
    return copyWidth > 0 ? shownScale * fileWidth / copyWidth : 0;
}

// True when the face should draw the file. `wasFile` is what it drew a moment ago: the file is taken above 1.1
// screen pixels per copy pixel and let go below 0.9, so a zoom resting at the threshold does not flip. This holds at
// the node's own fit too: a large face stretches a 512 px copy as much as a zoom does (the owner saw a soft SeedVR2
// compare, 2026-11-24). A file no wider than its copy is the copy.
inline bool useFile(double shownScale, double copyWidth, double fileWidth, bool wasFile) {
    if (fileWidth <= copyWidth || copyWidth <= 0)
        return false;
    const double covers = copyPixelScale(shownScale, copyWidth, fileWidth);
    return wasFile ? covers >= 0.9 : covers > 1.1;
}

} // namespace fugue::compare
