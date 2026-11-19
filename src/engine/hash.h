#pragma once

// Content hashes (spec 1.5): SHA-256, written "sha256:<64 hex digits>".
// Canonical JSON for hashing (RFC 8785) lives with the document model.

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace fugue {

// "sha256:" followed by the lowercase hex digest of `bytes`.
QString sha256(QByteArrayView bytes);

// The same for a file's contents, read in chunks so a large file is never held
// whole. Returns an empty string when the file cannot be read.
QString sha256File(const QString &path);

// True when `text` has the form "sha256:<64 lowercase hex digits>".
bool isSha256(const QString &text);

} // namespace fugue
