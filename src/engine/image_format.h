#pragma once

// What an image file is by its bytes, and the limits on an image Fugue takes in (spec 14.1):
// a paste, a drop, a template's or a shared package's sample (#91). In the engine, so the
// window and the CLI judge a file the same way.

#include <QByteArrayView>
#include <QString>

#include <optional>

namespace fugue {

struct ImageFormat {
    QString extension; // "png", "jpg", "webp", "gif", "bmp", "tif", "avif"
    QString mime;      // "image/png", ...
};

// The format the bytes themselves say they are (magic numbers), not what a name or MIME type
// claims. Nothing for anything else.
std::optional<ImageFormat> sniffImage(QByteArrayView bytes);

// What an input file is by its bytes: an image (sniffImage), or a paint canvas (an OpenRaster file
// Fugue writes, engine/paint_file.h: extension "ora"), which a `paint` input holds (#181). Only the
// places that carry input files (a package, a template, the inputs folder) take the second kind;
// a paste or a drop of a picture is sniffImage's alone. Needs the first 128 bytes.
std::optional<ImageFormat> sniffInputFile(QByteArrayView bytes);

// Largest file kept (a camera original is well under it), and the most pixels an image may
// have (a decompression bomb is refused before it is stored).
inline constexpr qint64 maxImageBytes = 256 * 1024 * 1024;
inline constexpr qint64 maxImagePixels = 256 * 1000 * 1000;

} // namespace fugue
