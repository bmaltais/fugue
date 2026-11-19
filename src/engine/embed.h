#pragma once

// A workflow inside an image file (spec 11.7), so that dropping the file back on Fugue
// reloads its graph.
//
//   - PNG: an `iTXt` chunk with the keyword `fugue`, uncompressed UTF-8, placed right
//     after IHDR. pngWithWorkflow() writes it itself rather than ask libvips, which
//     writes `tEXt` for its text fields.
//   - WebP: the XMP chunk, written by libvips from an XMP packet whose
//     `<fugue:workflow>` element holds the JSON, XML-escaped.
//
// What goes in is the share copy with its `run` section (Save Image makes it); this file
// only makes bytes; writing them is the caller's (atomic_file.h).

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vips/vips8>

#include "engine/result.h"

namespace fugue {

// The PNG with a `fugue` iTXt chunk holding `workflowJson`; one that was there is
// replaced. `media_unreadable` when the bytes are not a PNG.
Result<QByteArray> pngWithWorkflow(const QByteArray &png, const QByteArray &workflowJson);

// `image` as the bytes of a lossless 8-bit WebP. A non-empty `workflowJson` goes in the XMP
// chunk; empty writes none.
Result<QByteArray> webpWithWorkflow(const vips::VImage &image, const QByteArray &workflowJson);

// The workflow embedded in a PNG or WebP file, as the document JSON. `file_not_found`,
// `media_unreadable` (neither format), or `invalid_document` when the file holds no
// workflow or one that is not a JSON object.
Result<QJsonObject> readEmbeddedWorkflow(const QString &path);

} // namespace fugue
