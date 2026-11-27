#include "place_policy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStorageInfo>

#include <unistd.h>

#include <algorithm>

namespace fugue {

namespace {

constexpr int kMaxLinkHops = 40;

bool under(const QString &path, const QString &folder) {
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

bool inMounts(const QString &path, const QStringList &mounts) {
    return std::any_of(mounts.begin(), mounts.end(), [&path](const QString &mount) { return under(path, mount); });
}

} // namespace

PlacePolicy::PlacePolicy(const FinderEnv &env) : m_network(env.networkMounts), m_autofs(env.autofsMounts), m_touched(env.touched) {}

PlacePolicy::PlacePolicy(const QList<FinderLocation> &ticked, const QList<FinderLocation> &offered, const FinderEnv &env,
                         const QString &ownRoot)
    : PlacePolicy(env) {
    m_ownRoot = ownRoot;
    // Only a place that was offered and left off is a place to keep out of; one that was never listed (it does not exist) is not.
    QSet<QString> tickedIds;
    for (const FinderLocation &place : ticked) {
        tickedIds.insert(place.id);
        addPlace(place, true);
    }
    for (const FinderLocation &place : offered)
        if (!tickedIds.contains(place.id))
            addPlace(place, false);
}

PlacePolicy PlacePolicy::fromUnticked(const QList<FinderLocation> &offered, const QStringList &unticked, const FinderEnv &env,
                                      const QString &ownRoot) {
    QList<FinderLocation> ticked;
    for (const FinderLocation &place : offered)
        if (!unticked.contains(place.id))
            ticked << place;
    return PlacePolicy(ticked, offered, env, ownRoot);
}

PlacePolicy PlacePolicy::unconfirmed(const FinderEnv &env, const QString &ownRoot) {
    PlacePolicy policy(env);
    policy.m_ownRoot = ownRoot;
    policy.m_unconfirmed = true;
    return policy;
}

// A place is kept as written and as resolved: the walk goes by resolved paths, and a home behind a link (/home -> /var/home) or a link
// from elsewhere into it must meet the same scope.
void PlacePolicy::addPlace(const FinderLocation &place, bool ticked) {
    for (const QString &where : {place.path, place.scope, place.resolved, place.resolvedScope})
        if (!where.isEmpty())
            m_scopes.append({where, ticked}); // the place's own resolved folder, never its parent (a whole drive)
}

bool PlacePolicy::isNetwork(const QString &path) const { return inMounts(path, m_network); }

bool PlacePolicy::isAutofs(const QString &path) const { return inMounts(path, m_autofs); }

bool PlacePolicy::inLeftOffPlace(const QString &path) const { return blocked(path, false) && !isNetwork(path); }

bool PlacePolicy::mayTouch(const QString &path, bool traversal) const { return !blocked(path, traversal); }

// The network first; Fugue's own folder is never a place to keep out of; then the longest place that covers the path decides: a ticked
// install folder inside an unticked home is fine, an unticked one inside a ticked home is not.
bool PlacePolicy::blocked(const QString &path, bool traversal) const {
    if (isNetwork(path))
        return true;
    if (!m_ownRoot.isEmpty() && (under(path, m_ownRoot) || (traversal && m_ownRoot.startsWith(path + QLatin1Char('/')))))
        return false; // a folder that only leads to it may be passed by name
    if (m_unconfirmed)
        return true;
    if (traversal)
        for (const Scope &scope : m_scopes)
            if (scope.ticked && scope.path.startsWith(path + QLatin1Char('/')))
                return false;
    int best = -1;
    bool bestTicked = true;
    for (const Scope &scope : m_scopes)
        if (under(path, scope.path) && int(scope.path.size()) > best) {
            best = int(scope.path.size());
            bestTicked = scope.ticked;
        }
    return best >= 0 && !bestTicked;
}

// Every link read by its text, component by component from the root: what a link leads to is never stat'd, and each step is offered to
// the rule (and to the probe, after it passed) before anything touches it. Nothing here follows a link: each readlink is on a path with
// no link in it.
QString PlacePolicy::resolve(const QString &path) const {
    QStringList pending = QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current;
    int hops = 0;
    while (!pending.isEmpty()) {
        const QString part = pending.takeFirst();
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            current = current.left(std::max(0, int(current.lastIndexOf(QLatin1Char('/')))));
            continue;
        }
        const QString next = current + QLatin1Char('/') + part;
        if (blocked(next, true))
            return {};
        if (m_touched)
            m_touched(next);
        char buffer[4096];
        const ssize_t n = ::readlink(QFile::encodeName(next).constData(), buffer, sizeof buffer);
        if (n >= 0) {
            if (++hops > kMaxLinkHops)
                return {};
            const QString target = QFile::decodeName(QByteArray(buffer, int(n)));
            if (target.startsWith(QLatin1Char('/')))
                current.clear();
            pending = target.split(QLatin1Char('/'), Qt::SkipEmptyParts) + pending;
            continue;
        }
        current = next;
    }
    return current.isEmpty() ? QStringLiteral("/") : current;
}

bool PlacePolicy::mayBeDownloadChoice(const QString &path) const {
    if (inLeftOffPlace(path))
        return false;
    return isNetwork(path) || !resolve(path).isEmpty();
}

qint64 PlacePolicy::freeBytes(const QString &path, const std::function<qint64(const QString &)> &ask) const {
    // Never a network or automount (a statfs there can mount it or hang the window), nor a place left off, nor a path that leads into
    // either by a link.
    if (!mayTouch(path))
        return -1;
    const QString safe = resolve(path);
    if (safe.isEmpty())
        return -1;
    if (ask)
        return ask(path);
    QString dir = safe;
    while (!dir.isEmpty() && !QFileInfo(dir).isDir()) {
        const QString parent = QFileInfo(dir).absolutePath();
        // Fugue's own folder may not exist yet: its parents are asked about by name only for it.
        const bool ownParent = !m_ownRoot.isEmpty() && m_ownRoot.startsWith(parent + QLatin1Char('/'));
        if (parent == dir || (!ownParent && !mayTouch(parent)))
            return -1;
        dir = parent;
    }
    const QStorageInfo info(dir);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

QList<FinderLocation> offeredPlaces(const QList<FinderLocation> &ticked, const QList<FinderLocation> &table) {
    QList<FinderLocation> offered = ticked;
    for (const FinderLocation &place : table)
        if (std::none_of(ticked.begin(), ticked.end(), [&place](const FinderLocation &c) { return c.id == place.id; }))
            offered << place;
    return offered;
}

QString finderResolve(const QString &path, const FinderEnv &env) { return PlacePolicy(env).resolve(path); }

} // namespace fugue
