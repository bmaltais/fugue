#pragma once

// Recovery copies (spec 11.3): the content of a workflow with unsaved changes, kept in the
// state folder so a crash loses at most 30 seconds. A copy is only ever written under the
// recovery folder, never to the workflow's own file. Each tab owns one copy, named by the
// tab's id: `<id>.fugue` with `<id>.json` beside it saying where the workflow came from and
// which store (one per running Fugue) wrote it.
//
// Several Fugues may run at once on one recovery folder. Each store has an owner id and holds
// `owner-<id>.lock` (a QLockFile) from its first write until it is gone, so another store
// never offers its live copies, which would discard or duplicate them. A copy whose owner's
// lock is free (its Fugue ended) or stale (its process died) is offered, and the store that
// lists it holds that lock from then on, so a third start does not offer it again.

#include "engine/document.h"
#include "engine/result.h"

#include <QDateTime>
#include <QList>
#include <QLockFile>
#include <QString>

#include <map>
#include <memory>

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
    ~RecoveryStore();
    RecoveryStore(const RecoveryStore &) = delete;
    RecoveryStore &operator=(const RecoveryStore &) = delete;

    const QString &dir() const { return m_dir; }
    // This store's owner id, written into each note.
    const QString &owner() const { return m_owner; }

    // Writes (or replaces) the copy of `id`. Empty when written.
    Errors write(const QString &id, const Document &document, const QString &originalPath, const QString &title);
    // Removes the copy of `id`; fine when there is none.
    void remove(const QString &id);
    // Moves the copy of `id` (one that cannot be read) into the `unreadable` folder inside the
    // recovery folder, where it is no longer offered but stays for the person to look at.
    // That folder's path, or empty when the copy could not be moved (it then stays in place).
    QString setAside(const QString &id);
    // Every copy no other running store owns, newest first; the owners of those it lists are
    // this store's from now on. Also cleans partial files a dead writer left.
    QList<RecoveryEntry> list();
    Result<Document> read(const QString &id) const;

private:
    QString copyPath(const QString &id) const;
    QString metaPath(const QString &id) const;
    static bool validId(const QString &id);
    QString lockPath(const QString &owner) const;
    // Whether this store holds `owner`'s lock, taking it when it is free or stale.
    bool holds(const QString &owner);

    QString m_dir;
    QString m_owner;
    std::unique_ptr<QLockFile> m_lock; // its own, from the first write
    std::map<QString, std::unique_ptr<QLockFile>> m_taken; // of owners that ended
};

} // namespace fugue
