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
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "engine/result.h"

namespace fugue {

QByteArray writeJson(const QJsonValue &value);

// Writes `value` to a file in that form, replacing the file atomically: a reader sees
// the old content or the new, never half of it. The folder is made if needed. Empty
// when done; else one permission_denied error that names the file and the reason.
Errors writeJsonFile(const QString &path, const QJsonValue &value);

// Reads a file that holds one JSON object; a leading byte order mark is skipped.
// Errors, each naming the file: file_not_found (no such file, or a folder),
// permission_denied (it cannot be read), invalid_json (not JSON, with the line and
// column, or JSON that is not an object). A caller for which a missing file is normal
// looks at the code; a corrupt file is never mistaken for a missing one.
Result<QJsonObject> readJsonObject(const QString &path);

} // namespace fugue
