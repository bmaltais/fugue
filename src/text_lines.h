#pragma once

// Line spacing for a TextEdit (#715): QML's TextEdit has no `lineHeight`, so the text window sets its document's blocks to a
// proportional line height, as Text's `lineHeight` does for the node card. Call it after the text is set; a block made by typing
// Enter takes its format from the block it was made in, so the spacing holds while typing.

#include <QObject>
#include <QQuickTextDocument>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class TextLines : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit TextLines(QObject *parent = nullptr) : QObject(parent) {}

    // Every block of the document at `ratio` times the font's line height (1.5: one and a half). The document's own undo history
    // is not kept: the text was just set, so it has none worth keeping, and a format change must not be an undo step of its own.
    Q_INVOKABLE static void setLineHeight(QQuickTextDocument *document, qreal ratio);
    // ... or `pixels` high exactly, as a Text's `lineHeightMode: Text.FixedHeight` does (the inline editor matches the resting well's lines).
    Q_INVOKABLE static void setFixedLineHeight(QQuickTextDocument *document, qreal pixels);
    // Every line break in [from, to) of the document becomes a space, as a part of the edit just before it (one undo step with a
    // paste): the range is what was just inserted, so the breaks a stored value already had are not touched.
    Q_INVOKABLE static void replaceBreaks(QQuickTextDocument *document, int from, int to);
};

} // namespace fugue
