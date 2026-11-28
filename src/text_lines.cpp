#include "text_lines.h"

#include <QQuickTextDocument>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

namespace fugue {

namespace {

void apply(QQuickTextDocument *document, qreal height, QTextBlockFormat::LineHeightTypes type) {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text)
        return;
    QTextBlockFormat format;
    format.setLineHeight(height, type);
    const bool undo = text->isUndoRedoEnabled();
    text->setUndoRedoEnabled(false);
    QTextCursor cursor(text);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    text->setUndoRedoEnabled(undo);
}

} // namespace

void TextLines::setLineHeight(QQuickTextDocument *document, qreal ratio) {
    apply(document, ratio * 100, QTextBlockFormat::ProportionalHeight);
}

void TextLines::setFixedLineHeight(QQuickTextDocument *document, qreal pixels) {
    apply(document, pixels, QTextBlockFormat::FixedHeight);
}

} // namespace fugue
