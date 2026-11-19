#pragma once

// Recovery copies (spec 11.3): the content of a workflow with unsaved changes, kept in the
// state folder so a crash loses at most 30 seconds. A copy is only ever written under the
// recovery folder, never to the workflow's own file. Each tab owns one copy, named by the
// tab's id: `<id>.fugue` with `<id>.json` beside it saying where the workflow came from.

#include "engine/document.h"
#include "engine/result.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace fugue {

struct RecoveryEntry {
    QString id;
    QString originalPath; // the workflow's own file; empty for one never saved
    QString title;
    QDateTime modified;
};

class RecoveryStore {
public:
    // ~/.local/state/fugue/recovery
    static QString defaultDir();

    explicit RecoveryStore(QString dir = defaultDir());

    const QString &dir() const { return m_dir; }

    // Writes (or replaces) the copy of `id`. Empty when written.
    Errors write(const QString &id, const Document &document, const QString &originalPath, const QString &title);
    // Removes the copy of `id`; fine when there is none.
    void remove(const QString &id);
    // Moves the copy of `id` (one that cannot be read) into the `unreadable` folder inside the
    // recovery folder, where it is no longer offered but stays for the person to look at.
    // That folder's path, or empty when the copy could not be moved (it then stays in place).
    QString setAside(const QString &id);
    // Every copy, newest first. Also cleans partial files a dead writer left.
    QList<RecoveryEntry> list() const;
    Result<Document> read(const QString &id) const;

private:
    QString copyPath(const QString &id) const;
    QString metaPath(const QString &id) const;
    static bool validId(const QString &id);

    QString m_dir;
};

} // namespace fugue
