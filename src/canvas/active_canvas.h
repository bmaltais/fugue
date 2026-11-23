#pragma once

// The Active canvas (#246): the canvas a Canvas view last showed under the user's hands. A Canvas Region with no
// canvas chosen reads it (docs/spec.md 14.5), so the picture being worked on is the one the model works on. Each
// Canvas view reports the canvas it shows when the user uses or focuses it (`use`); the window's runs read it through
// shown(), from the run thread too (hence the lock). It holds an id and nothing else: whether that canvas still exists
// is the compile's business (activeCanvasOrNewest), and with none reported the newest canvas of the root is the answer.
// One per window, an App-level object, so the engine has no dependency on any QML view.

#include <QMutex>
#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class ActiveCanvas : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QString id READ id NOTIFY changed)

public:
    explicit ActiveCanvas(QObject *parent = nullptr) : QObject(parent) {}

    // The canvas last reported, "" when none. Safe from any thread.
    QString id() const;
    // Same as id(): the shape RunCompileSetup wants.
    QString shown() const { return id(); }

    // A Canvas view reports the canvas it shows. An empty id (a view with no canvas) is ignored: it is not a choice.
    Q_INVOKABLE void use(const QString &canvasId);

signals:
    void changed();

private:
    mutable QMutex m_lock;
    QString m_id;
};

} // namespace fugue
