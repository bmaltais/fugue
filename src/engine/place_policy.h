#pragma once

// The Place policy (spec 8.9, #619): the one rule that says which paths the model-folder search and the first-run sheet may touch.
// A place the person left off is never touched: no stat, no listing, no walk, no statfs. The longest offered place that covers a path
// decides whether it is ticked (an unticked place with a ticked one inside it is blocked except for that one), a network or automount
// is never touched whatever the places say, and Fugue's own models folder is always allowed.
//
// A value type: built once from the places and the mounts, copied to the worker thread, never changed. It does no I/O when it answers
// "may this be touched"; resolve() and freeBytes() read link texts and ask the disk only for what the rule has allowed.

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

#include "engine/model_folder_finder.h"

namespace fugue {

class PlacePolicy {
public:
    // No places, only the mounts of `env`: a network or automount is blocked, everything else is allowed.
    explicit PlacePolicy(const FinderEnv &env = {});
    // `offered` are the places the person was shown; the ones of them not in `ticked` are the ones left off. Empty `offered`: nothing was
    // left off. `ownRoot` (Fugue's own models folder) is never a place to keep out of.
    PlacePolicy(const QList<FinderLocation> &ticked, const QList<FinderLocation> &offered, const FinderEnv &env, const QString &ownRoot = {});
    // The same from the unticked ids of `offered` (the sheet's state).
    static PlacePolicy fromUnticked(const QList<FinderLocation> &offered, const QStringList &unticked, const FinderEnv &env, const QString &ownRoot);
    // Nothing is confirmed yet (the sheet's consent screen, with every place still ticked): only `ownRoot` may be touched.
    static PlacePolicy unconfirmed(const FinderEnv &env, const QString &ownRoot);

    bool isNetwork(const QString &path) const; // in a network or automount (as the mount lists say)
    bool isAutofs(const QString &path) const;  // in an automount: not even listed
    // Whether `path` is in a place left off (as written or as resolved). No I/O.
    bool inLeftOffPlace(const QString &path) const;
    // Whether `path` may be stat'd, listed or walked: not in a network mount and not in a place left off. With `traversal` a folder that
    // only leads to a ticked place (its ancestor) may be passed by name.
    bool mayTouch(const QString &path, bool traversal = false) const;
    // `path` with its links resolved by reading link texts component by component, never stat'ing past a link into a blocked place
    // (empty then, or when the links loop). Each step the rule allowed is shown to the touched probe first.
    QString resolve(const QString &path) const;
    // Whether a folder may be offered as the downloads' folder: not in a place left off, and no link of it leads into one. A network
    // folder passes (it is listed, never looked into). reads link texts of allowed steps only.
    bool mayBeDownloadChoice(const QString &path) const;
    // Free bytes where `path` is, or would be made (the nearest existing parent's disk); -1 (unknown, with no I/O) when `path` may not be
    // touched or a link of it leads where it may not go. `ask` replaces the disk (a test's); it is asked only for an allowed path.
    qint64 freeBytes(const QString &path, const std::function<qint64(const QString &)> &ask = {}) const;

private:
    struct Scope {
        QString path;
        bool ticked;
    };
    void addPlace(const FinderLocation &place, bool ticked);
    bool blocked(const QString &path, bool traversal) const;
    void setOwnRoot(const QString &ownRoot, const FinderEnv &env);
    bool isOwn(const QString &path, bool traversal) const; // Fugue's own folder, as written or as resolved, or (traversal) on the way to it

    QList<Scope> m_scopes; // every place's scopes and whether the place is ticked
    QStringList m_network; // mounts
    QStringList m_autofs;
    QString m_ownRoot;
    QString m_ownResolved; // its links resolved (empty when the same)
    bool m_unconfirmed = false;
    std::function<void(const QString &)> m_touched;
};

// The places offered when `ticked` are the ones searched: those first, then every other place of `table`. What was left off is kept
// out of by its written path alone (reading its links would be touching it). The one list the finder, the sheet and the CLI use.
QList<FinderLocation> offeredPlaces(const QList<FinderLocation> &ticked, const QList<FinderLocation> &table);

} // namespace fugue
