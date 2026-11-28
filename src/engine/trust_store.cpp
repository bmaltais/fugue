#include "engine/trust_store.h"

#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/project.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSet>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QSysInfo>

#include <optional>

#include <cerrno>
#include <csignal>

namespace fugue {

QString TrustStore::defaultPath() { return stateDir() + "/trust.json"; }

QString TrustStore::defaultProjectPath() { return baseStateDir() + "/trust.json"; }

QString TrustStore::packOf(const QString &typeId) {
    const int slash = typeId.lastIndexOf('/');
    return slash <= 0 ? QString() : typeId.left(slash);
}

bool TrustStore::isBuiltIn(const QString &packId) {
    return packId == QStringLiteral("fugue") || packId == QStringLiteral("local");
}

TrustStore::TrustStore(QString path) : m_path(std::move(path)) {}

namespace {

QMutex &fileLock() {
    static QMutex mutex;
    return mutex;
}

// trust.json is read, changed and written back. One change at a time: the mutex for this process (the window's worker thread
// and its GUI thread both make changes), `trust.json.lock` for every process (Settings, the window and the CLI change it, and
// the packs folder's lock does not cover it). Like that lock, a QLockFile, so a holder that died releases it.
class ChangeLock {
public:
    // `waitMs`: a change takes milliseconds, so 5 s is another command stuck; 0 for a best-effort change that is skipped when busy.
    explicit ChangeLock(const QString &path, int waitMs = 5000)
        : m_mutex(&fileLock()), m_path(path + QStringLiteral(".lock")), m_file(m_path) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_file.setStaleLockTime(0); // a holder that died on this computer releases it; no age limit
        m_held = m_file.tryLock(waitMs);
    }
    bool held() const { return m_held; }
    // Like the packs folder's lock (PacksLock::busy): the holder, and the file to remove when it is gone or on another computer.
    Errors busy() const {
        if (m_file.error() == QLockFile::PermissionError)
            return {makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot write the lock file %1.").arg(m_path),
                              QStringLiteral("Check that you may write in the folder of trust.json."))};
        if (m_file.error() == QLockFile::UnknownError)
            return {makeError(QStringLiteral("internal"), QStringLiteral("Cannot take the lock file %1.").arg(m_path))};
        qint64 holder = 0;
        QString host, app;
        m_file.getLockInfo(&holder, &host, &app);
        if (!host.isEmpty() && host != QSysInfo::machineHostName())
            return {makeError(QStringLiteral("pack_busy"), QStringLiteral("The trust file is locked from another computer (%1).").arg(host),
                              QStringLiteral("Wait for it to finish. If it is gone, remove %1.").arg(m_path))};
        if (holder > 0 && ::kill(pid_t(holder), 0) != 0 && errno != EPERM)
            return {makeError(QStringLiteral("pack_busy"), QStringLiteral("The trust file is locked by pid %1, which is gone.").arg(holder),
                              QStringLiteral("Remove %1, then try again.").arg(m_path))};
        return {makeError(QStringLiteral("pack_busy"),
                          holder > 0 ? QStringLiteral("Another Fugue command (pid %1) is changing the trust file.").arg(holder)
                                     : QStringLiteral("Another Fugue command is changing the trust file."),
                          QStringLiteral("Wait for it to finish, then try again; if it does not, remove %1.").arg(m_path))};
    }

private:
    QMutexLocker<QMutex> m_mutex;
    QString m_path;
    QLockFile m_file;
    bool m_held = false;
};

QJsonObject readFileObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject readPacks(const QString &path) { return readFileObject(path).value("packs").toObject(); }

// The digest of the secrets a pack declared when it was trusted (packSecretsDigest), per pack id and version; absent
// for a yes given before this was recorded or for a pack that was not installed then.
QJsonObject readSealed(const QString &path) { return readFileObject(path).value("secrets").toObject(); }

// Whether the pack is still the one that was trusted: its declared secrets are what the yes covered (#497).
bool secretsUnchanged(const QJsonObject &sealed, const QString &packId, const QString &version) {
    const QString recorded = sealed.value(packId).toObject().value(version).toString();
    if (recorded.isEmpty())
        return true;
    const QString now = packSecretsDigest(packId, version);
    return now.isEmpty() || now == recorded;
}

QJsonObject readOpen(const QString &path) { return readFileObject(path).value("unsandboxed").toObject(); }

// `open`: the versions run without a sandbox (setUnsandboxed), kept only for a version that is still trusted. Unless given,
// the file's own list stays as it is.
Errors writeTrust(const QString &path, const QJsonObject &packs, QJsonObject sealed, std::optional<QJsonObject> open = std::nullopt) {
    QJsonObject unsandboxed = open ? *open : readOpen(path);
    for (const QString &id : unsandboxed.keys()) {
        QJsonArray kept;
        for (const QJsonValue &version : unsandboxed.value(id).toArray())
            for (const QJsonValue &trusted : packs.value(id).toArray())
                if (trusted == version)
                    kept.append(version);
        if (kept.isEmpty())
            unsandboxed.remove(id);
        else
            unsandboxed.insert(id, kept);
    }
    // Only what a trusted version still has.
    for (const QString &id : sealed.keys()) {
        QJsonObject versions = sealed.value(id).toObject();
        for (const QString &version : versions.keys()) {
            bool kept = false;
            for (const QJsonValue &value : packs.value(id).toArray())
                kept |= value.toString() == version;
            if (!kept)
                versions.remove(version);
        }
        if (versions.isEmpty())
            sealed.remove(id);
        else
            sealed.insert(id, versions);
    }
    QJsonObject file{{"packs", packs}};
    if (!sealed.isEmpty())
        file.insert("secrets", sealed);
    if (!unsandboxed.isEmpty())
        file.insert("unsandboxed", unsandboxed);
    // Only its owner may read or change what they trust (replaceFile makes the folder).
    return replaceFile(path, writeJson(file), QFileInfo(path).fileName(), FileMode::OwnerOnly);
}

} // namespace

bool TrustStore::layered() const {
    return m_path == defaultPath() && defaultProjectPath() != m_path && sessionInherits().contains(QStringLiteral("packs"));
}

bool TrustStore::isTrustedByDefault(const QString &packId, const QString &version) const {
    if (isBuiltIn(packId) || !layered())
        return false;
    for (const QJsonValue &value : readPacks(defaultProjectPath()).value(packId).toArray())
        if (value.toString() == version)
            return isInheritedPack(packId, version) && secretsUnchanged(readSealed(defaultProjectPath()), packId, version);
    return false;
}

bool TrustStore::isListed(const QString &packId, const QString &version) const {
    for (const QJsonValue &value : readPacks(m_path).value(packId).toArray())
        if (value.toString() == version)
            return true;
    return false;
}

QString TrustStore::sealedDigest(const QString &packId, const QString &version) const {
    return readSealed(m_path).value(packId).toObject().value(version).toString();
}

bool TrustStore::isTrusted(const QString &packId, const QString &version) const {
    if (isBuiltIn(packId))
        return true;
    for (const QJsonValue &value : readPacks(m_path).value(packId).toArray())
        if (value.toString() == version && secretsUnchanged(readSealed(m_path), packId, version))
            return true;
    return isTrustedByDefault(packId, version);
}

namespace {

Error defaultsTrust(const QString &packId, const QString &version) {
    return makeError(QStringLiteral("usage"),
                     QStringLiteral("The trust of %1 %2 is the Default project's: this project inherits that pack and honours the Default's "
                                    "decision, read-only.").arg(packId, version),
                     QStringLiteral("Change it in the Default project (start fugue without --project), or install your own copy of the pack "
                                    "here, which hides the inherited one."));
}

} // namespace

Errors TrustStore::trust(const QString &packId, const QString &version, const QString &secretsDigest) {
    if (isBuiltIn(packId) || isTrusted(packId, version))
        return {};
    const ChangeLock lock(m_path);
    if (!lock.held())
        return lock.busy();
    if (isTrusted(packId, version)) // another command's yes, given meanwhile
        return {};
    QJsonObject packs = readPacks(m_path);
    QJsonArray versions = packs.value(packId).toArray();
    bool known = false; // a yes whose pack.json changed since: the version is there, the digest is renewed
    for (const QJsonValue &value : versions)
        known |= value.toString() == version;
    if (!known)
        versions.append(version);
    packs.insert(packId, versions);
    QJsonObject sealed = readSealed(m_path);
    QJsonObject own = sealed.value(packId).toObject();
    if (const QString digest = secretsDigest.isEmpty() ? packSecretsDigest(packId, version) : secretsDigest; !digest.isEmpty())
        own.insert(version, digest);
    else
        own.remove(version);
    sealed.insert(packId, own);
    return writeTrust(m_path, packs, sealed);
}

Errors TrustStore::restore(const QString &packId, const QString &version, const QString &digest) {
    // Only while the version is unlisted: a yes given since it was forgotten (Settings, another command) is newer, and stays as it
    // is, digest included. Checked again under the lock, so no change made between the check and the write is lost.
    if (isBuiltIn(packId) || isListed(packId, version))
        return {};
    const ChangeLock lock(m_path);
    if (!lock.held())
        return lock.busy();
    if (isListed(packId, version))
        return {};
    QJsonObject packs = readPacks(m_path);
    QJsonArray versions = packs.value(packId).toArray();
    versions.append(version);
    packs.insert(packId, versions);
    QJsonObject sealed = readSealed(m_path);
    QJsonObject own = sealed.value(packId).toObject();
    if (digest.isEmpty())
        own.remove(version);
    else
        own.insert(version, digest);
    sealed.insert(packId, own);
    return writeTrust(m_path, packs, sealed);
}

Errors TrustStore::adoptSecretsDigest(const QString &packId, const QString &version, const QString &digest) {
    if (isBuiltIn(packId) || digest.isEmpty())
        return {};
    // Nothing to do (the common case, at every load) takes no lock.
    const auto needed = [&] { return isListed(packId, version) && sealedDigest(packId, version).isEmpty(); };
    if (!needed())
        return {};
    // Best effort, and never a wait: it runs inside every pack load (the window's too). Busy now, it is adopted at the next load.
    const ChangeLock lock(m_path, 0);
    if (!lock.held())
        return {};
    if (!needed())
        return {};
    QJsonObject sealed = readSealed(m_path);
    QJsonObject own = sealed.value(packId).toObject();
    own.insert(version, digest);
    sealed.insert(packId, own);
    return writeTrust(m_path, readPacks(m_path), sealed);
}

Errors TrustStore::forget(const QString &packId, const QString &version, TrustEntry *taken) {
    if (taken)
        *taken = {};
    if (!isListed(packId, version)) // nothing to forget takes no lock, and needs no writable folder
        return {};
    const ChangeLock lock(m_path);
    if (!lock.held())
        return lock.busy();
    QJsonObject packs = readPacks(m_path);
    QJsonArray kept;
    bool found = false;
    for (const QJsonValue &value : packs.value(packId).toArray()) {
        if (value.toString() == version)
            found = true;
        else
            kept.append(value);
    }
    if (!found)
        return {};
    if (kept.isEmpty())
        packs.remove(packId);
    else
        packs.insert(packId, kept);
    const QJsonObject sealed = readSealed(m_path);
    const Errors written = writeTrust(m_path, packs, sealed);
    if (taken && !hasErrors(written))
        *taken = {true, sealed.value(packId).toObject().value(version).toString()};
    return written;
}

bool TrustStore::isUnsandboxed(const QString &packId, const QString &version) const {
    if (isBuiltIn(packId))
        return false;
    for (const QJsonValue &value : readOpen(m_path).value(packId).toArray())
        if (value.toString() == version)
            return isTrusted(packId, version); // the yes stands only for what was trusted: its digest (secrets, source, needs) too
    return false;
}

Errors TrustStore::setUnsandboxed(const QString &packId, const QString &version, bool unsandboxed) {
    if (isBuiltIn(packId))
        return {makeError(QStringLiteral("usage"), QStringLiteral("%1 is part of Fugue; it has no sandbox to turn off.").arg(packId))};
    if (isUnsandboxed(packId, version) == unsandboxed)
        return {};
    if (unsandboxed && !isListed(packId, version))
        return {makeError(QStringLiteral("pack_untrusted"), QStringLiteral("%1 %2 is not trusted, so it cannot be set to run without a sandbox.").arg(packId, version),
                          QStringLiteral("Trust it first (`fugue pack trust %1`).").arg(packId))};
    const ChangeLock lock(m_path);
    if (!lock.held())
        return lock.busy();
    QJsonObject open = readOpen(m_path);
    QJsonArray versions;
    for (const QJsonValue &value : open.value(packId).toArray())
        if (value.toString() != version)
            versions.append(value);
    if (unsandboxed)
        versions.append(version);
    if (versions.isEmpty())
        open.remove(packId);
    else
        open.insert(packId, versions);
    return writeTrust(m_path, readPacks(m_path), readSealed(m_path), open);
}

Errors TrustStore::forget(const QString &packId) {
    if (!readPacks(m_path).contains(packId))
        return {};
    const ChangeLock lock(m_path);
    if (!lock.held())
        return lock.busy();
    QJsonObject packs = readPacks(m_path);
    if (!packs.contains(packId))
        return {};
    packs.remove(packId);
    return writeTrust(m_path, packs, readSealed(m_path));
}

Errors TrustStore::revoke(const QString &packId, const QString &version) {
    if (layered())
        for (const InheritedPack &pack : inheritedPacks())
            if (pack.id == packId && (version.isEmpty() || pack.version == version) && isTrustedByDefault(packId, pack.version))
                return {defaultsTrust(packId, pack.version)};
    return version.isEmpty() ? forget(packId) : forget(packId, version);
}

QList<PackUse> TrustStore::packsUsed(const Document &document, const Registry &registry) {
    QMap<QString, PackUse> used;
    const QJsonObject recorded = document.requirements.value("packs").toObject();
    const auto addNodes = [&](const Nodes &nodes) {
        for (const Node &node : nodes) {
            if (document.groups.contains(node.type))
                continue;
            const QString pack = packOf(node.type);
            if (pack.isEmpty() || isBuiltIn(pack))
                continue;
            // The version asked about is the one of the pack that holds the node's major (6.6).
            const NodeType *type = registry.nodeTypeFor(node.type, node.version.major);
            const QJsonObject entry = recorded.value(pack).toObject();
            QString installed = type ? type->packVersion : QString();
            if (installed.isEmpty())
                installed = registry.packVersion(pack);
            const QString version = installed.isEmpty() ? entry.value("version").toString() : installed;
            PackUse &use = used[pack + QLatin1Char('@') + version];
            if (use.id.isEmpty()) {
                use.id = pack;
                use.version = version;
                use.source = entry.value("source").toString();
            }
            if (!use.types.contains(node.type))
                use.types << node.type;
        }
    };
    addNodes(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        addNodes(group.nodes);
    QList<PackUse> list = used.values();
    for (PackUse &use : list)
        use.types.sort();
    return list;
}

QList<PackUse> TrustStore::untrusted(const Document &document, const Registry &registry) const {
    QList<PackUse> list;
    for (const PackUse &use : packsUsed(document, registry))
        if (!isTrusted(use.id, use.version))
            list << use;
    return list;
}

} // namespace fugue
