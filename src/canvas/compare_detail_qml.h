#pragma once

// engine/compare_detail.h for QML (#371): whether a Compare Images node's face draws the files instead of the
// 512 px copies. No state, so any QML may call it (CompareNodeView.qml).

#include <QObject>
#include <QSizeF>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/compare_detail.h"
#include "engine/compare_scale.h"

namespace fugue {

class CompareDetail : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit CompareDetail(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE bool useFile(double shownScale, double fileWidth, bool wasFile) const {
        return compare::useFile(shownScale, compare::copyWidthOf(fileWidth), fileWidth, wasFile);
    }
    // 100% of A or of B (engine/compare_scale.h, #420).
    Q_INVOKABLE double actualScale(QSizeF a, QSizeF b, bool ofB) const {
        return compare::actualScale(a.width(), a.height(), b.width(), b.height(), ofB);
    }
    Q_INVOKABLE bool bIsLarger(QSizeF a, QSizeF b) const {
        return compare::bIsLarger(a.width(), a.height(), b.width(), b.height());
    }
    Q_INVOKABLE QVariantMap nextActual(QSizeF a, QSizeF b, bool actual, bool ofB) const {
        const compare::Actual next = compare::nextActual(a.width(), a.height(), b.width(), b.height(), actual, ofB);
        return {{"actual", next.actual}, {"actualB", next.ofB}};
    }
};

} // namespace fugue
