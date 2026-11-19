#pragma once

// Reading a workflow from whatever the user hands over (spec 11.4): the kind is detected from
// the content, never the extension. A `.fugue` document, a PNG or WebP with an embedded
// workflow (engine/embed.h), or JSON text from the clipboard.

#include "engine/document.h"
#include "engine/result.h"

#include <QByteArray>
#include <QString>

namespace fugue {

enum class SourceKind { Document, Image };

struct ImportedWorkflow {
    Document document;
    SourceKind kind = SourceKind::Document;
};

// What the first bytes say: an image (PNG or WebP signature) or a document (anything else).
SourceKind detectKind(const QByteArray &head);

// `file_not_found`, `permission_denied`, `media_unreadable`, `invalid_json`,
// `invalid_document` and the document errors of Document::load.
Result<ImportedWorkflow> readWorkflowFile(const QString &path);

// Pasted text: must be a workflow document. Surrounding whitespace is ignored.
Result<Document> readWorkflowText(const QString &text);

} // namespace fugue
