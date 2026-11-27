#include "text_lines.h"

#include <QQuickTextDocument>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

namespace fugue {

void TextLines::setLineHeight(QQuickTextDocument *document, qreal ratio) {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text)
        return;
    QTextBlockFormat format;
    format.setLineHeight(ratio * 100, QTextBlockFormat::ProportionalHeight);
    const bool undo = text->isUndoRedoEnabled();
    text->setUndoRedoEnabled(false);
    QTextCursor cursor(text);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    text->setUndoRedoEnabled(undo);
}

} // namespace fugue
