#pragma once

// The guides and grid of one canvas (#253, epic #216): where a person put guide lines on a canvas, and the grid's spacing.
// It is a small sidecar in the canvas's own folder (`canvases/<id>/guides.json`), NOT a revision: moving a guide is not an
// edit of the picture, so it makes no history step and Undo does not take it back. Plain data and no Qt GUI: the Canvas
// view reads and writes it through CanvasStore::guides and setGuides, and a CLI could do the same.
//
// The file is {"format": 1, "grid": 64, "guides": [{"axis": "x", "at": 120}, ...]}. `axis` "x" is a vertical line at
// pixel column `at`, "y" a horizontal one at pixel row `at`. The file is read as untrusted: guidesFromJson refuses a
// format it does not know, a bad axis or position, a grid outside its range, or more than maxGuides lines. A position
// outside the canvas is kept (the canvas may be resized back); the view draws only the ones inside.

#include <QByteArray>

#include <vector>

#include "engine/result.h"

namespace fugue::canvas {

inline constexpr int guidesFormat = 1;
inline constexpr int maxGuides = 200;
inline constexpr int minGridSpacing = 2;
inline constexpr int maxGridSpacing = 4096;
inline constexpr int defaultGridSpacing = 64;
inline constexpr int maxGuidePosition = 1000000; // far past the largest canvas (8192)

struct Guide {
    bool vertical = true; // true: a vertical line at x = `at`; false: a horizontal line at y = `at`
    int at = 0;           // canvas pixels
    bool operator==(const Guide &o) const { return vertical == o.vertical && at == o.at; }
};

struct GuideSet {
    std::vector<Guide> guides;
    int grid = defaultGridSpacing; // the grid's spacing, canvas pixels
    bool operator==(const GuideSet &o) const { return guides == o.guides && grid == o.grid; }
};

// The set as the file's bytes. Guides are written sorted (vertical first, by position) and without repeats, so the same
// set is always the same file.
QByteArray guidesToJson(const GuideSet &set);
// The set the bytes hold; canvas_corrupt (in plain words) for anything that is not a valid file.
Result<GuideSet> guidesFromJson(const QByteArray &bytes);

} // namespace fugue::canvas
