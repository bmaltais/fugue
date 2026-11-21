#pragma once

// The workflow files opened or saved lately (#56), for the workflows panel: newest first, kept in
// a small file of the state folder ({"files": [...]}), at most `limit` of them. A file that is no
// longer there is not listed (and is dropped from the file the next time it is written), and
// neither is one that is open in a tab: the panel's other list shows those.
//
// It follows a Workflows object by watching its tabs: a tab with a path is a file opened or saved.
// It knows nothing else of workflows; the panel opens an entry through Workflows::openPath.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class Workflows;

class RecentFiles : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // Maps: path, name (the file's name without .fugue), folder (the folder, home as ~).
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)

public:
    RecentFiles(QString path, int limit = 12, QObject *parent = nullptr);

    QVariantList entries() const;

    // Puts a file first. A blank path is ignored.
    void remember(const QString &path);
    // The paths open in tabs now: left out of `entries`.
    void setOpen(const QStringList &paths);
    // Drops a file from the list (the file itself stays).
    Q_INVOKABLE void forget(const QString &path);
    // Remembers the path of every tab of `workflows` now and whenever its tabs change.
    void follow(Workflows *workflows);

signals:
    void entriesChanged();

private:
    void save() const;

    QString m_path;
    int m_limit;
    QStringList m_files; // newest first
    QStringList m_open;
};

} // namespace fugue
