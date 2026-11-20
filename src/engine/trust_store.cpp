#include "engine/trust_store.h"

#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/paths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QDir>

namespace fugue {

QString TrustStore::defaultPath() { return stateDir() + "/trust.json"; }

QString TrustStore::packOf(const QString &typeId) {
    const int slash = typeId.lastIndexOf('/');
    return slash <= 0 ? QString() : typeId.left(slash);
}

bool TrustStore::isBuiltIn(const QString &packId) {
    return packId == QStringLiteral("fugue") || packId == QStringLiteral("local");
}

TrustStore::TrustStore(QString path) : m_path(std::move(path)) {}

namespace {

QJsonObject readPacks(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("packs").toObject();
}

} // namespace

bool TrustStore::isTrusted(const QString &packId, const QString &version) const {
    if (isBuiltIn(packId))
        return true;
    for (const QJsonValue &value : readPacks(m_path).value(packId).toArray())
        if (value.toString() == version)
            return true;
    return false;
}

Errors TrustStore::trust(const QString &packId, const QString &version) {
    if (isBuiltIn(packId) || isTrusted(packId, version))
        return {};
    QJsonObject packs = readPacks(m_path);
    QJsonArray versions = packs.value(packId).toArray();
    versions.append(version);
    packs.insert(packId, versions);
    // Only its owner may read or change what they trust (replaceFile makes the folder).
    return replaceFile(m_path, writeJson(QJsonObject{{"packs", packs}}), QFileInfo(m_path).fileName(), FileMode::OwnerOnly);
}

Errors TrustStore::forget(const QString &packId) {
    QJsonObject packs = readPacks(m_path);
    if (!packs.contains(packId))
        return {};
    packs.remove(packId);
    return replaceFile(m_path, writeJson(QJsonObject{{"packs", packs}}), QFileInfo(m_path).fileName(), FileMode::OwnerOnly);
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
            PackUse &use = used[pack];
            if (use.id.isEmpty()) {
                const QJsonObject entry = recorded.value(pack).toObject();
                use.id = pack;
                const QString installed = registry.packVersion(pack);
                use.version = installed.isEmpty() ? entry.value("version").toString() : installed;
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
