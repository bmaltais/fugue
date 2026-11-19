#include "providers_file.h"

#include <QFileInfo>
#include <QJsonArray>

#include "engine/atomic_file.h"
#include "engine/json.h"

namespace fugue {

QJsonArray providerInstancesJson(const QList<ProviderInstance> &instances) {
    QJsonArray list;
    for (const ProviderInstance &instance : instances) {
        QJsonObject entry = instance.settings;
        entry.insert("name", instance.name);
        entry.insert("type", instance.type);
        if (!instance.address.isEmpty())
            entry.insert("address", instance.address);
        if (!instance.key.isEmpty())
            entry.insert("key", instance.key);
        if (!instance.enabled)
            entry.insert("enabled", false);
        list.append(entry);
    }
    return list;
}

Errors saveProviderInstances(const QString &path, const QList<ProviderInstance> &instances) {
    QJsonObject document;
    if (QFileInfo::exists(path)) {
        const Result<QJsonObject> read = readJsonObject(path);
        if (!read)
            return read.errors();
        document = read.value();
    }
    document.insert("providers", providerInstancesJson(instances));
    if (const auto parsed = parseProviderInstances(document); !parsed)
        return parsed.errors();
    return replaceFile(path, writeJson(document), path, FileMode::OwnerOnly);
}

} // namespace fugue
