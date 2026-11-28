#include "text_lines.h"

#include <algorithm>

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

void TextLines::replaceBreaks(QQuickTextDocument *document, int from, int to) {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text)
        return;
    QTextCursor cursor(text);
    cursor.joinPreviousEditBlock();
    for (int at = std::min(to, text->characterCount() - 1) - 1; at >= from && at >= 0; --at) {
        const QChar c = text->characterAt(at);
        if (c == QChar::ParagraphSeparator || c == QChar::LineSeparator || c == u'\n' || c == u'\r') {
            cursor.setPosition(at);
            cursor.setPosition(at + 1, QTextCursor::KeepAnchor);
            cursor.insertText(QStringLiteral(" "));
        }
    }
    cursor.endEditBlock();
}

void TextLines::setFixedLineHeight(QQuickTextDocument *document, qreal pixels) {
    apply(document, pixels, QTextBlockFormat::FixedHeight);
}

} // namespace fugue
