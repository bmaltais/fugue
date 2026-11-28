#include "engine/model_offers.h"

#include <QFileInfo>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>

#include "engine/file_hashes.h"
#include "engine/host_models.h"
#include "engine/model_family.h"
#include "engine/model_recommend.h"
#include "engine/model_library.h"
#include "engine/providers.h"
#include "engine/remote_consent.h"

namespace fugue {

void listOfferedModels(const QString &type, const QStringList &capabilities,
                       std::function<void(QList<ListedModel>)> done) {
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    if (!instances.ok())
        return done({});
    struct Pending {
        int left = 0;
        QList<QList<ListedModel>> byInstance; // in the user's order
        std::function<void(QList<ListedModel>)> done;
        std::vector<std::shared_ptr<Provider>> providers;
    };
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    QList<std::tuple<std::shared_ptr<Provider>, QString, QString, QString>> asked; // provider, name, host, cloud
    for (const ProviderInstance &instance : instances.value()) {
        if (!instance.enabled)
            continue;
        Result<std::shared_ptr<Provider>> made = createProvider(instance);
        if (!made.ok())
            continue;
        pending->providers.push_back(made.value());
        const bool offMachine = placementOf(instance) == Placement::OffMachine;
        asked.append({made.value(), instance.name, instance.address.isEmpty() || offMachine ? QString() : instance.name,
                      offMachine ? instance.name : QString()});
    }
    pending->left = int(asked.size());
    pending->byInstance.resize(asked.size());
    if (asked.isEmpty())
        return pending->done({});
    for (qsizetype index = 0; index < asked.size(); ++index) {
        const auto &[provider, name, host, cloud] = asked.at(index);
        provider->describe([pending, type, capabilities, index, name = name, host = host, cloud = cloud](const Result<Description> &description) {
            QList<ListedModel> &models = pending->byInstance[index];
            if (description.ok()) {
                for (const CapabilityOffer &offer : description->capabilities) {
                    if (!capabilities.isEmpty() && !capabilities.contains(offer.capability))
                        continue;
                    for (const OfferedModel &model : offer.models) {
                        if (model.handle.isEmpty()) // model files come in the job (provider.h)
                            continue;
                        if (!type.isEmpty() && model.handle.value("resource").toString() != type)
                            continue;
                        const bool listed = std::any_of(models.begin(), models.end(), [&](const ListedModel &m) {
                            return sameModel(m.handle, model.handle);
                        });
                        if (listed)
                            continue;
                        ListedModel entry;
                        entry.name = model.handle.value("name").toString();
                        entry.handle = model.handle;
                        entry.status = modelStatusName(model.status);
                        entry.provider = name;
                        entry.host = host;
                        entry.cloud = cloud;
                        entry.title = model.title;
                        if (model.status != ModelStatus::Ready)
                            entry.note = (model.reason + QLatin1Char(' ') + model.hint).trimmed();
                        models.append(entry);
                    }
                }
            }
            if (--pending->left > 0)
                return;
            QList<ListedModel> all;
            for (const QList<ListedModel> &each : std::as_const(pending->byInstance))
                all += each;
            pending->done(all);
            // Not from inside a provider's own reply.
            QTimer::singleShot(0, [pending] { pending->providers.clear(); });
        });
    }
}

QList<ListedModel> listHostModels(const QList<ProviderInstance> &instances, const HostModelCache &cache, const ModelLibrary &library,
                                  const PortType &type, const QList<ListedModel> &local) {
    QSet<QString> localHashes;
    QSet<QString> localNames; // "name\nsize"
    for (const ListedModel &file : local) {
        if (!file.handle.value("hash").toString().isEmpty())
            localHashes.insert(file.handle.value("hash").toString());
        localNames.insert(file.name + QLatin1Char('\n') + QString::number(file.size));
    }
    QSet<QString> folders; // the type's folder names, as the host's roots have them (ComfyUI's layout)
    for (const QString &folder : library.folders(type))
        folders.insert(QFileInfo(folder).fileName());
    QList<ListedModel> out;
    if (folders.isEmpty())
        return out;
    for (const ProviderInstance &instance : instances) {
        if (!instance.enabled || !isPairedHelper(instance))
            continue;
        const HostModelList list = cache.cached(instance);
        if (!list.supported)
            continue;
        for (const HostModel &model : list.models) {
            if (!folders.contains(model.folder))
                continue;
            if (!model.handleHash().isEmpty() && localHashes.contains(model.handleHash()))
                continue; // the same weights are here: the local file wins
            ListedModel entry;
            entry.name = model.handleName();
            entry.folder = model.folder;
            entry.handle = diskHandle(type.toString(), entry.name, model.handleHash());
            entry.status = QStringLiteral("ready");
            entry.provider = instance.name;
            entry.host = instance.name;
            entry.size = model.size;
            entry.remote = true;
            QString family = modelFamily(familyKindOf(type.toString()), QFileInfo(model.name).fileName());
            if (family.isEmpty() && !model.family.isEmpty())
                family = engineFamilyName(model.family);
            entry.family = family;
            if (model.sha256.isEmpty() && localNames.contains(entry.name + QLatin1Char('\n') + QString::number(model.size)))
                entry.note = QObject::tr("Also here, by its name and size.");
            else if (model.sha256.isEmpty())
                entry.note = QObject::tr("%1 has not hashed this file yet; picking it asks it to.").arg(instance.name);
            out.append(entry);
        }
    }
    return out;
}

QJsonObject diskHandle(const QString &resource, const QString &name, const QString &hash) {
    QJsonObject handle{{"resource", resource}, {"name", name}};
    if (!hash.isEmpty())
        handle.insert("hash", hash);
    return handle;
}

QList<ListedModel> listDiskModels(const ModelLibrary &library, const PortType &type, FileHashes &hashes) {
    QList<ListedModel> out;
    for (const ModelFile &file : library.list(type)) {
        ListedModel entry;
        entry.name = file.name;
        entry.handle = diskHandle(type.toString(), file.name, hashes.known(file.path));
        entry.status = QStringLiteral("ready");
        entry.path = file.path;
        entry.folder = file.folder;
        entry.size = file.size;
        out.append(entry);
    }
    return out;
}

} // namespace fugue
