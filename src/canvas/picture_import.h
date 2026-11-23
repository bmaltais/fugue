#pragma once

// A picture that comes from outside to be put on a canvas (#236): the clipboard's, or a file dropped on the Canvas view. It is
// untrusted: its size is checked before anything is converted or decoded (a canvas's own limit, engine/canvas_transform.h
// placeableProblem), a file is read only up to a size cap and only when its name says it is a picture, and a failure is one
// sentence in plain words, never a crash. The result is straight RGBA, what a layer holds.

#include <QImage>
#include <QString>

#include "engine/mask_paint.h"

namespace fugue {

// Whether `pixels` holds a picture; otherwise `problem` says why.
struct ImportedPicture {
    mask::MaskPaint pixels{1, 1, 4};
    QString problem;
    bool ok() const { return problem.isEmpty(); }
};

// The largest picture file read (bytes).
inline constexpr qint64 maxPictureFileBytes = qint64(256) << 20;

ImportedPicture pictureFromImage(const QImage &image);
// The clipboard's picture (the application's clipboard; an application with none, or no picture on it, is a problem sentence).
ImportedPicture pictureFromClipboard();
// A PNG, JPEG, WebP, GIF, BMP or TIFF file, a path or a file: URL.
ImportedPicture pictureFromFile(const QString &pathOrUrl);
// Whether the name of a path or URL is one pictureFromFile reads (what a drop accepts before it reads anything).
bool isPictureFile(const QString &pathOrUrl);

} // namespace fugue
