#pragma once

// The window's notice that the faster CUDA helper is not installed (#97, spec 8.10), read by
// Main.qml. The logic is engine/helper_provider.h's CudaHelperNotice.

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/helper_provider.h"

namespace fugue {

class HelperNotice : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(QString command READ command CONSTANT)

public:
    explicit HelperNotice(QObject *parent = nullptr);

    bool shown() const { return m_shown; }
    QString command() const { return CudaHelperNotice::command(); }

    Q_INVOKABLE void copy();
    Q_INVOKABLE void dismiss();

signals:
    void shownChanged();

private:
    bool m_shown;
};

} // namespace fugue
