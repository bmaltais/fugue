#include "canvas/node_size.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace fugue::node_size {

namespace {

// The label size the widgets draw with (look.h: textSize x 11/12).
QFont labelFont(int textSize, QFont::Weight weight) {
    QFont font = qGuiApp ? QGuiApplication::font() : QFont();
    font.setPixelSize(std::max(1, int(std::lround(textSize * 11 / 12.0))));
    font.setWeight(weight);
    return font;
}

} // namespace

qreal lineWidgetWidth(const QString &label, const QString &value, int textSize) {
    const qreal u = textSize / 3.0;
    const QString first = value.section(QLatin1Char('\n'), 0, 0);
    const qreal labelWidth = QFontMetricsF(labelFont(textSize, QFont::Normal)).horizontalAdvance(label);
    const qreal valueWidth = QFontMetricsF(labelFont(textSize, QFont::Medium)).horizontalAdvance(first);
    return std::ceil(labelWidth + valueWidth + 3 * u * 2 + 2 * u * 2 + 2 * u);
}

int wrappedLines(const QString &text, qreal width, int textSize) {
    if (text.isEmpty())
        return 1;
    QString shown = text;
    shown.replace(QLatin1Char('\n'), QChar::LineSeparator); // QTextLayout breaks lines at U+2028
    QTextLayout layout(shown, labelFont(textSize, QFont::Normal));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    int lines = 0;
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(std::max<qreal>(width, 1));
        ++lines;
    }
    layout.endLayout();
    return std::max(lines, 1);
}

qreal multilineHeight(int lines, int textSize) {
    return lines * 16 + 2 * std::round(1.5 * textSize / 3.0);
}

} // namespace fugue::node_size
