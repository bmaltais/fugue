#pragma once

// Small helpers the canvas model's files share: the links in an input value (engine/links.h),
// and a layout point.

#include "engine/document.h"
#include "engine/links.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QPointF>

#include <optional>

namespace canvas_detail {

using fugue::linksIn;
using fugue::stripLinks;
using fugue::withoutLinks;

inline std::optional<QPointF> point(const QJsonValue &value) {
    const QJsonArray pair = value.toArray();
    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
        return std::nullopt;
    return QPointF(pair[0].toDouble(), pair[1].toDouble());
}

} // namespace canvas_detail
