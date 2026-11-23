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

Result<PsdExported> paintToPsd(const PaintDocument &document);

// Reads a .psd. media_unreadable for a file that is not one, or is damaged; input_invalid for one Fugue cannot
// hold (not 8-bit RGB, a PSB, past the canvas size or layer count limits), each saying how to convert it.
// The Result's warnings list what was dropped.
Result<PaintDocument> paintFromPsd(const QByteArray &bytes);

bool looksLikePsd(QByteArrayView bytes);

} // namespace fugue::mask
