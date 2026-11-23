#pragma once

// Exporting a canvas as pictures (#247 Export as…, #248 Export layers as files; epic #206): the one exporter the
// Canvas view's two dialogs and `fugue canvas export` share, so the file a dialog writes is the file the command
// writes. Photoshop's Export As, Save for Web and Layers to Files are the references (docs/canvas-menus-research.md).
// It works on a PaintDocument and knows no window and no store; the caller loads the revision.
//
// A picture is the flattened canvas (PaintDocument::flatten, the picture the history window shows) in one format,
// at a size, over a background, or the layered .ora or .psd (psd_file.h). Layers to files is one picture per layer: the layer's own
// pixels (its opacity and blend are not applied, the layer mask is unless asked), named from the layer names.
// Nothing here writes over a file: the writers refuse when a name is taken, before they write the first one.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::canvas {

// How one picture is made. A size is a `scale`, or a `width` and/or `height` in pixels (one of them keeps the
// canvas's proportions); `scale` is used only when both are 0.
struct PictureExport {
    QString format = QStringLiteral("png"); // "png", "jpg", "webp", "ora" or "psd" (layers kept)
    int quality = 92;                       // 1 to 100, for "jpg" and for a lossy "webp"
    bool lossless = true;                   // "webp": every pixel and the alpha kept; false follows `quality`
    double scale = 1;
    int width = 0;
    int height = 0;
    // "" is the format's own: "transparent" for png and webp, "white" for jpg (it has no alpha). Else "transparent",
    // "white", "black" or "#rrggbb"; jpg cannot be transparent.
    QString background;
};

struct ExportedPicture {
    QByteArray bytes;
    int width = 0;
    int height = 0;
    QString format;     // "png", "jpg", "webp", "ora" or "psd"
    QStringList notes;  // what a layered format could not keep (a mask layer left out of a psd), plain words; empty for none
    QString background; // what the picture lies over: "transparent", "white", "black" or "#rrggbb"; "" for ora
};

// "png", "jpg" (also "jpeg") or "webp", or "ora" or "psd" when `allowLayered`; else "" .
QString normalFormat(const QString &format, bool allowLayered);
// The picture's size for these options, or input_invalid when it is past the canvas size limit (new_canvas.h).
Result<QSize> exportSize(int canvasWidth, int canvasHeight, const PictureExport &options);
// The canvas as one picture. usage for an unknown format or a quality out of 1..100; input_invalid for a size past
// the limit, a jpg asked to be transparent, a webp too large (16383 px a side) or a psd past 30000 px a side.
Result<ExportedPicture> exportPicture(const mask::PaintDocument &document, const PictureExport &options);

// One picture per layer.
struct LayersExport {
    QString format = QStringLiteral("png"); // "png" or "webp"
    int quality = 92;
    bool lossless = true;
    bool visibleOnly = false; // false: hidden layers too
    bool applyMasks = true;   // each layer's own layer mask (when it has one on) is multiplied into its alpha
    QString prefix;           // first in each file name: "<prefix>-01-Sky.png"
};

struct LayerFile {
    int position = 0;   // 0 is the bottom, as `fugue canvas layer list`
    QString name;       // the layer's
    QString fileName;   // "01-Sky.png": its place from the bottom (1 is the bottom), then its name, safe for any file system
    QByteArray bytes;
    int width = 0;
    int height = 0;
};

struct LayerFiles {
    std::vector<LayerFile> files; // bottom first
    QStringList skipped;          // names of the layers left out: a mask layer is a selection, not a picture
};

// usage for a format other than png and webp or a quality out of range; input_invalid when no layer is left to write.
Result<LayerFiles> exportLayers(const mask::PaintDocument &document, const LayersExport &options);

// A name made safe for a file: no separators or characters a file system refuses, no leading dot or space, at most
// 80 characters; "Layer" when nothing is left.
QString safeFileName(const QString &name);

// Writes the files into `folder` (made when missing), none over a file already there. The names are all checked
// before the first is written: permission_denied naming the files in the way, or a path that is not a folder.
Errors writeLayerFiles(const QString &folder, const LayerFiles &files);

} // namespace fugue::canvas
