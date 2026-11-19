#pragma once

// The canonical JSON form used for hashing (spec 1.5): RFC 8785, the JSON
// Canonicalization Scheme. Keys are sorted by UTF-16 code units, there is no
// whitespace, numbers are written as ECMAScript writes a double, and strings
// escape only '"', '\' and control characters. The result is UTF-8.
//
// This is not the on-disk form (engine/json.h); it is what gets hashed, for
// example the `source` of a compiled graph (4.8) or a resource handle's
// identity (2.5).

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <optional>

namespace fugue {

// The canonical bytes of `value`, or nothing when it has no canonical form:
// a string or key holding a lone UTF-16 surrogate, or a number that is not
// finite. Every number is treated as a double, as RFC 8785 requires, so an
// integer beyond 2^53 loses precision here (spec 2.1 forbids such integers).
std::optional<QByteArray> canonicalJson(const QJsonValue &value);

// sha256() of the canonical bytes ("sha256:<hex>"), or an empty string when
// `value` has no canonical form.
QString canonicalHash(const QJsonValue &value);

} // namespace fugue
