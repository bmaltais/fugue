#pragma once

// A paint canvas as a Photoshop (.psd) file (#227, epic #206): the file Photoshop, Affinity, Photopea, Krita,
// GIMP and Nuke open. Written from Adobe's published "Adobe Photoshop File Formats Specification"; the research
// and the real-program checklist are docs/psd-research.md. It knows no window and no store.
//
// Writing: 8 bits per channel, RGB, version 1 (a PSD, at most 30000 px a side; a larger canvas is refused in
// plain words, though the canvas cap, new_canvas.h, is lower). Every paint and picture layer is a layer record
// the size of the canvas with its own pixels (opacity, blend and layer mask NOT applied: Photoshop applies
// them), its name (Pascal name and the Unicode `luni` block), visibility, opacity, blend key and layer mask
// (channel -2, with its "disabled" flag). The merged image is the flattened canvas, so a program that reads
// no layers shows the same picture. A mask layer (a selection, not a picture) is left out and reported. The
// same document always gives the same bytes: RLE channels, fixed blocks, nothing that varies.
//
// Reading is for hostile files (psd_file.cpp): every length and offset is checked against what is there,
// counts and sizes are capped, RLE expansion is bounded by the size the header promises, and nothing is
// written. Layers, names, visibility, opacity, blend modes and layer masks come across. What does not is
// reported, never silently: the Result's warnings (code psd_dropped, one sentence each) are the list.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::mask {

inline constexpr int psdMaxSide = 30000; // the PSD format's own limit (a PSB allows 300000)

// What a PSD export left out or changed, in plain words (one per item); empty when nothing was.
struct PsdExported {
    QByteArray bytes;
    QStringList notes;
};

// `blend` keys the Photoshop file format knows and the document's composite-op: the four characters.
QString psdBlendKeyOfOp(const QString &compositeOp, bool *exact = nullptr); // "norm" when the op has no key
QString psdOpOfBlendKey(const QString &key, bool *known = nullptr);          // "" (Normal) when the key has no op

// A layer of a PSD with its pixels, as the reader and the writer pass them: a layer (canvas-sized RGBA, and its layer
// mask when it has one) or a group of layers (bottom first). A group has no pixels of its own.
struct PsdLayer {
    LayerInfo info;                    // name, visible, opacity, compositeOp, hasMask, maskEnabled
    MaskPaint pixels = MaskPaint(1, 1, 4);
    std::optional<MaskPaint> mask;     // the layer mask, canvas-sized, one channel (a layer's, or a group's)
    bool group = false;
    bool open = true;                  // a group shown unfolded
    bool lossy = false;                // reading: the group lost something that makes it not worth keeping as a group
    std::vector<PsdLayer> children;
};

// Layers, bottom first, with groups kept as groups, and the merged picture (matted on white is done by the writer).
struct PsdImage {
    int width = 1, height = 1;
    std::vector<PsdLayer> layers;
    MaskPaint merged = MaskPaint(1, 1, 4); // straight RGBA, the canvas flattened
};
Result<PsdExported> paintToPsd(const PsdImage &image);
// Groups are not in a PaintDocument, so a document is written as its layers; a group of a canvas goes through PsdImage
// (engine/canvas_psd.h).
Result<PsdExported> paintToPsd(const PaintDocument &document);

// What a read gives: the layers with groups as the file has them (a group that Fugue cannot keep as a group is already
// flattened into its parent's layers with a warning), or, when no layer could be kept, the merged picture.
struct PsdImported {
    int width = 1, height = 1;
    std::vector<PsdLayer> layers;
    std::optional<MaskPaint> merged;
};
Result<PsdImported> paintFromPsdLayers(const QByteArray &bytes);
// The same with the groups Fugue cannot keep as groups (a mask, an adjustment layer or clipping inside, more than 8 deep, empty)
// flattened into their parent's layers, one warning each; the ones left are real groups.
Result<PsdImported> paintFromPsdSettled(const QByteArray &bytes);

// Reads a .psd. media_unreadable for a file that is not one, or is damaged; input_invalid for one Fugue cannot
// hold (not 8-bit RGB, a PSB, past the canvas size or layer count limits), each saying how to convert it.
// The Result's warnings list what was dropped.
// The same with every group flattened into plain layers (its visibility and opacity folded into each), one warning per group.
Result<PaintDocument> paintFromPsd(const QByteArray &bytes);

bool looksLikePsd(QByteArrayView bytes);

} // namespace fugue::mask
