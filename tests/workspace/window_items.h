#pragma once

// The default window's items in the form of data/default-window.txt (#217): "items <count>", then "<name> <count>"
// per item name, sorted. test_area_window compares the default window with the file; test_area_split_window
// compares the window after a split and a close with it.

#include <QFile>
#include <QMap>
#include <QQuickItem>
#include <QString>

namespace window_items {

// Qt's shared ToolTip popup comes and goes with the pointer and the timing of a loaded machine (a hover during a busy
// suite left one in the window, three items): it is not part of the window's structure, so it is not counted.
inline bool transient(QQuickItem *item) { return item->objectName() == QLatin1String("ToolTip"); }

inline int countItems(QQuickItem *from) {
    int count = 1;
    for (auto *child : from->childItems())
        if (!transient(child))
            count += countItems(child);
    return count;
}

inline void collectNames(QQuickItem *from, QMap<QString, int> &names) {
    for (auto *child : from->childItems()) {
        if (transient(child))
            continue;
        if (!child->objectName().isEmpty())
            ++names[child->objectName()];
        collectNames(child, names);
    }
}

inline QString describe(QQuickItem *content) {
    QMap<QString, int> names;
    collectNames(content, names);
    QString text = QStringLiteral("items %1\n").arg(countItems(content));
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        text += QStringLiteral("%1 %2\n").arg(it.key()).arg(it.value());
    return text;
}

// The file, without its comments.
inline QString expected() {
    QFile file(QStringLiteral(FUGUE_SOURCE_DIR "/tests/workspace/data/default-window.txt"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString text;
    for (const QString &line : QString::fromUtf8(file.readAll()).split('\n'))
        if (!line.isEmpty() && !line.startsWith('#'))
            text += line + '\n';
    return text;
}

} // namespace window_items
