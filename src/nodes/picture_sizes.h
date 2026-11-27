#pragma once

// What a picture node's size comes to, worked out from sizes and numbers alone (#601): one place for the rule, used by the
// kernel that makes the picture and by the one that tells the canvas, before a run, what size it will be. Pure and cheap: no
// pixel is read. A side is whole pixels; a size past kMaxSide is returned as it is, so the caller can say it is too big.

#include <QSize>
#include <QString>

#include <optional>

#include "engine/node_kernel.h"

namespace fugue::nodes::picturesize {

constexpr int kMaxSide = 16384; // the largest side a node makes, as its manifest's `max`

// `side` scaled by to / from, rounded half to even (Python's round()), and at least 1.
int scaled(int side, int to, int from);

// Resize: a width or height of 0 follows the other (both 0: the picture as it is); `fit` is stretch, contain or cover.
QSize resize(QSize in, int width, int height, const QString &fit);
// Resize to megapixels: each side rounded half to even to a multiple, at least one multiple.
QSize megapixels(QSize in, double megapixels, int multiple);
// Upscale Image: each side times `scale`, rounded half to even, at least 1.
QSize upscale(QSize in, double scale);
// Crop: the origin held inside the picture, the far edge at most its own.
QSize crop(QSize in, int x, int y, int width, int height);
// Pad with four sides, or to a target size when both are above 0 (the picture only ever shrinks to fit it, so the result is the target).
QSize pad(QSize in, int left, int top, int right, int bottom, int targetWidth, int targetHeight);

// True when a side is past kMaxSide.
bool tooBig(QSize size);

// The error a size past kMaxSide is, on `input` (value_out_of_range); the words say what made it.
Error tooBigError(const QString &input, const QString &what, QSize size);

// A number input written (not linked and not yet known: none) as a whole number.
std::optional<int> wholeInput(const NodeContext &context, const char *name);
std::optional<double> numberInput(const NodeContext &context, const char *name);

// A node that makes a picture of the size its `image` input has (blur, sharpen, invert, ...): its one output, "image".
class SameSizeKernel : public NodeKernel {
public:
    PictureSizes outputSizes(const NodeContext &context) const override;
};

} // namespace fugue::nodes::picturesize
