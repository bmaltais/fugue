#pragma once

// Fugue's on-disk JSON form (spec 4.6): keys sorted, two-space indent, one key
// per line, a trailing newline, UTF-8, integers without fraction or exponent.
// Writing the same value twice gives identical bytes.
//
// JSON values are Qt's (QJsonValue): Qt 6 keeps 64-bit integers exact and
// QJsonObject keeps its keys sorted, in UTF-16 code unit order: the order of
// canonical JSON (tested in test_canonical). The canonical form used for
// hashing (RFC 8785) is engine/canonical.h.

#include <QByteArray>
#include <QJsonValue>

namespace fugue {

QByteArray writeJson(const QJsonValue &value);

} // namespace fugue
