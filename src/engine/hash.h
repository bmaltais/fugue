#pragma once

// Content hashes (spec 1.5): SHA-256, written "sha256:<64 hex digits>".
// Canonical JSON for hashing (RFC 8785) is engine/canonical.h.

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <functional>

namespace fugue {

// Told how far a file has been read (`done` of `total` bytes) after each chunk, and once before
// the first; returning false stops the reading.
using HashProgress = std::function<bool(qint64 done, qint64 total)>;

// "sha256:" followed by the lowercase hex digest of `bytes`.
QString sha256(QByteArrayView bytes);

// The same for a file's contents, read in chunks so a large file is never held
// whole. Returns an empty string when the file cannot be read.
QString sha256File(const QString &path);
// The same, telling `progress` as it reads; empty when it is stopped.
QString sha256File(const QString &path, const HashProgress &progress);

// True when `text` has the form "sha256:<64 lowercase hex digits>".
bool isSha256(const QString &text);

} // namespace fugue
