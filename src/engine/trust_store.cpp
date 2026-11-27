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
#include <QMap>
#include <QSet>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>

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

// trust.json is read, changed and written back: one change at a time in this process (the
// window's worker thread and its GUI thread both make changes).
QMutex &fileLock() {
    static QMutex mutex;
    return mutex;
}

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

Errors writeTrust(const QString &path, const QJsonObject &packs, QJsonObject sealed) {
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
    QMutexLocker lock(&fileLock());
    if (isBuiltIn(packId) || isTrusted(packId, version))
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
    QMutexLocker lock(&fileLock());
    if (isBuiltIn(packId))
        return {};
    QJsonObject packs = readPacks(m_path);
    QJsonArray versions = packs.value(packId).toArray();
    bool known = false;
    for (const QJsonValue &value : versions)
        known |= value.toString() == version;
    if (!known)
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
    QMutexLocker lock(&fileLock());
    const QJsonObject packs = readPacks(m_path);
    bool trusted = false;
    for (const QJsonValue &value : packs.value(packId).toArray())
        trusted |= value.toString() == version;
    QJsonObject sealed = readSealed(m_path);
    QJsonObject own = sealed.value(packId).toObject();
    if (!trusted || !own.value(version).toString().isEmpty())
        return {};
    own.insert(version, digest);
    sealed.insert(packId, own);
    return writeTrust(m_path, packs, sealed);
}

Errors TrustStore::forget(const QString &packId, const QString &version) {
    QMutexLocker lock(&fileLock());
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
    return writeTrust(m_path, packs, readSealed(m_path));
}

Errors TrustStore::forget(const QString &packId) {
    QMutexLocker lock(&fileLock());
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
