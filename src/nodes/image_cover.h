#pragma once

// A picture fitted to an exact size for a model (Generate Image's start image, Generate Video's
// first frame): scaled to cover the size, cut from the middle.

#include <QJsonObject>
#include <QSize>
#include <QString>

#include <functional>

#include "engine/media.h"
#include "engine/result.h"

namespace fugue::nodes {

// `image` scaled to cover `size`, cut from the middle and written as an 8-bit RGB PNG to `path`;
// the path. The caller has run initVips() and catches vips::VError.
Result<QString> coverToFile(const MediaHandle &image, QSize size, const QString &path);

// The `width` and `height` in `params` against the model's `min_`/`max_` limits (8.1): one error
// per side outside them, blamed on the input `blame(side)` names (empty: none). `model` is the name
// the message gives.
Errors outsideSizeLimits(const QString &model, const QJsonObject &limits, const QJsonObject &params,
                         const std::function<QString(const QString &side)> &blame);

} // namespace fugue::nodes
