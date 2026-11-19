#pragma once

// Fugue's on-disk JSON form (spec 4.6): keys sorted, two-space indent, one key
// per line, a trailing newline, UTF-8, integers without fraction or exponent.
// Writing the same value twice gives identical bytes.
//
// JSON values are Qt's (QJsonValue): Qt 6 keeps 64-bit integers exact and
// QJsonObject keeps its keys sorted. The canonical form used for hashing
// (RFC 8785) belongs to the document model, not here.

#include <QByteArray>
#include <QJsonValue>

namespace fugue {

QByteArray writeJson(const QJsonValue &value);

} // namespace fugue
