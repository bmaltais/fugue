#pragma once

// engine/compare_detail.h for QML (#371): whether a Compare Images node's face draws the files instead of the
// 512 px copies. No state, so any QML may call it (CompareNodeView.qml).

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/compare_detail.h"

namespace fugue {

class CompareDetail : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit CompareDetail(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE bool useFile(double shownScale, double fileWidth, bool atFit, bool wasFile) const {
        return compare::useFile(shownScale, compare::copyWidthOf(fileWidth), fileWidth, atFit, wasFile);
    }
};

} // namespace fugue
