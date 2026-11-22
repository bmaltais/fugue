#pragma once

// The canvases of this root as a list to choose from (#211, epic #206, spec 14.5, 6.2 widget `canvas`):
// what the picker of a Canvas Region's `canvas` input shows, and the two ways it makes a canvas (a blank
// one of a size, or one from a picture file). All the work is engine/canvas_store.h; this turns it into
// rows QML binds to and keeps them current.
//
// A row is {id, name, width, height, size ("1024 x 768"), changed (ISO time of the head revision),
// changedText (that time as the person's language writes it), head}. A canvas with no revision yet has
// width and height 0. The store is the session's (`<data>/canvases`, a project's own under FUGUE_HOME)
// unless a test sets its own. `revision` is bumped whenever the list may have changed: a canvas made
// here, by `fugue canvas new` or by a running workflow's Canvas Write (a folder watch, so another
// process's change shows within a moment).

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/canvas_store.h"

namespace fugue {

// The rows of `store`, newest first; one that cannot be read is left out (CanvasStore::list).
QVariantList canvasRows(const canvas::CanvasStore &store);

class CanvasChoices : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through WidgetEdit")
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit CanvasChoices(QObject *parent = nullptr);

    // The folder of the store; empty is the session's. A test sets its own.
    void setStoreRoot(const QString &root);
    QString storeRoot() const { return m_root; }
    int revision() const { return m_revision; }

    // The rows now (read from disk each time; a few small files per canvas), and the store watched.
    QVariantList rows();
    // The row of one canvas, or an empty map when this root has none by that id.
    QVariantMap row(const QString &id);

    // A blank canvas of one empty paint layer. Returns why not, or empty, and the new id in `id`.
    // `width` and `height` are within CanvasStore's limits (the same as `fugue canvas new`).
    QString makeBlank(const QString &name, int width, int height, QString *id);
    // A canvas from a PNG, JPEG, WebP or .ora file (a path or a file: URL). A blank name is the file's.
    QString makeFromFile(const QString &path, const QString &name, QString *id);
    // The size a picture file would give, "" when it cannot be read: for the dialog's line "1920 x 1080".
    QString pictureSize(const QString &path) const;

signals:
    void changed();

private:
    canvas::CanvasStore store() const;
    // Watches the store's folder and each canvas's, so a head moved by another process is seen.
    void watch();
    void touched();

    QString m_root;
    int m_revision = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
};

} // namespace fugue
