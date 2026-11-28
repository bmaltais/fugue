#include "apps/app_models.h"
#include "engine/host_models.h"
#include "engine/providers.h"

#include <QFileInfo>
#include <QPointer>

#include "engine/file_hashes.h"
#include "engine/model_download.h"
#include "engine/model_family.h"
#include "engine/model_inspect.h"
#include "engine/model_speed.h"
#include "engine/port_type.h"
#include "engine/registry.h"
#include "workflow/model_downloads.h"

namespace fugue {

AppModels::AppModels(const Registry *registry, QObject *parent) : QObject(parent), m_registry(registry) {}

AppModels::~AppModels() = default;

QString AppModels::keyOf(const QString &type, const QStringList &capabilities) {
    return type + QLatin1Char('\n') + capabilities.join(QLatin1Char(','));
}

ModelLibrary AppModels::library() const { return m_library ? *m_library : ModelLibrary::current(m_registry); }

void AppModels::setDownloads(ModelDownloads *downloads) {
    m_downloads = downloads;
    if (downloads)
        connect(downloads, &ModelDownloads::finished, this, [this] { downloadsFinished(); });
}

void AppModels::request(const QString &type, const QStringList &capabilities) {
    const std::optional<PortType> portType = PortType::parse(type);
    if (!portType)
        return;
    const QString key = keyOf(type, capabilities);
    Entry &entry = m_entries[key];
    const ModelLibrary lib = library();
    entry.disk = listDiskModels(lib, *portType, hashes());
    entry.missing = (m_catalog ? *m_catalog : ModelCatalog::load()).missing(type, lib, *portType, hashes());
    entry.marks.clear();
    entry.loading = true;
    {
        // A paired helper's kept models (no network), and a background refresh of an old list that lists them again when it answers.
        QList<ProviderInstance> helpers;
        if (const Result<QList<ProviderInstance>> loaded = loadProviderInstances(providersPath()); loaded.ok())
            for (const ProviderInstance &instance : loaded.value())
                if (instance.enabled && isPairedHelper(instance))
                    helpers << instance;
        entry.remote = listHostModels(helpers, HostModelCache::shared(), lib, *portType, entry.disk);
        if (!helpers.isEmpty()) {
            QPointer<AppModels> self(this);
            HostModelCache::shared().refreshStale(helpers, HostModelCache::kTtlSeconds, [self, key, helpers, lib, portType](const HostModelList &) {
                if (!self || !self->m_entries.contains(key))
                    return;
                Entry &again = self->m_entries[key];
                again.remote = listHostModels(helpers, HostModelCache::shared(), lib, *portType, again.disk);
                emit self->changed();
            });
        }
    }
    emit changed();

    // The Recommended mark reads file headers: off the window's thread, applied when it is done unless asked again.
    const int asked = ++m_asked;
    QList<MarkedFile> files;
    for (const ListedModel &model : std::as_const(entry.disk))
        files.append({model.path, model.size});
    if (!files.isEmpty()) {
        const std::shared_ptr<std::atomic_bool> stopped = m_work.stopFlag();
        m_work.read(this, [key, type, files, asked, this, stopped]() -> std::function<void()> {
            const ModelSpeeds speeds;
            const QHash<QString, ModelMark> found = markModels(type, files, RecommendContext{localComputeDevice(), 0, &speeds},
                                                               ModelShapes::shared(), &ModelInspector::shared(), [stopped] { return stopped->load(); });
            return [this, key, found, asked] {
                if (m_asked != asked || !m_entries.contains(key))
                    return;
                m_entries[key].marks = found;
                emit changed();
            };
        });
    }
    if (!m_source)
        m_source = listOfferedModels;
    QPointer<AppModels> self(this);
    m_source(type, capabilities, [self, key](QList<ListedModel> models) {
        if (!self || !self->m_entries.contains(key))
            return;
        Entry &entry = self->m_entries[key];
        entry.offered = std::move(models);
        entry.loading = false;
        emit self->changed();
    });
}

bool AppModels::loading(const QString &type, const QStringList &capabilities) const {
    return m_entries.value(keyOf(type, capabilities)).loading;
}

QVariantList AppModels::entries(const QString &type, const QStringList &capabilities) const {
    const Entry entry = m_entries.value(keyOf(type, capabilities));
    QVariantList list;
    for (const ListedModel &model : entry.offered + entry.disk + entry.remote)
        list.append(modelRow(model, type, entry.marks));
    for (const CatalogModel &model : std::as_const(entry.missing))
        list.append(catalogRow(model));
    return list;
}

QString AppModels::emptyText(const QString &type, const QStringList &capabilities) const {
    const std::optional<PortType> portType = PortType::parse(type);
    return modelsEmptyText(capabilities, portType ? &*portType : nullptr, library());
}

void AppModels::pick(int ticket, const QString &type, const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile()) {
        emit hashed(ticket, {});
        return;
    }
    // The name is what finds the file again: the one the library lists it under.
    QString name = file.fileName();
    if (const std::optional<PortType> portType = PortType::parse(type))
        for (const ListedModel &model : listDiskModels(library(), *portType, hashes()))
            if (QFileInfo(model.path).canonicalFilePath() == file.canonicalFilePath())
                name = model.name;
    if (const QString known = hashes().known(path); !known.isEmpty()) {
        emit hashed(ticket, diskHandle(type, name, known).toVariantMap());
        return;
    }
    // A large model takes seconds to minutes to read: on the pool, the field says it is being checked meanwhile.
    m_work.hash(this, path, {}, [this, ticket, type, name](const QString &hash) {
        // an unreadable file answers with no handle, so the field stops waiting
        emit hashed(ticket, hash.isEmpty() ? QVariantMap() : diskHandle(type, name, hash).toVariantMap());
    });
}

QVariantMap AppModels::catalogInfo(const QString &type, const QString &id) const {
    const ModelCatalog catalog = m_catalog ? *m_catalog : ModelCatalog::load();
    const CatalogModel *model = catalog.find(id);
    const std::optional<PortType> portType = PortType::parse(type);
    if (!model || !portType || model->resource != type)
        return {};
    return catalogDetails(*model, library(), *portType);
}

QString AppModels::download(int ticket, const QString &type, const QString &id) {
    const std::optional<PortType> portType = PortType::parse(type);
    const ModelCatalog catalog = m_catalog ? *m_catalog : ModelCatalog::load();
    const CatalogModel *model = catalog.find(id);
    if (!portType || !model || model->resource != type)
        return tr("This model is not in the catalog for this input.");
    if (!m_downloads)
        return tr("Downloads are not available here.");
    const Result<DownloadTarget> planned = planCatalogDownload(library(), *model, *portType);
    if (!planned)
        return planned.error().message;
    m_downloading.append(Download{ticket, type, planned->path, model->file, QStringLiteral("sha256:") + model->sha256});
    m_downloads->add({planned.value()}, QStringLiteral("pick"));
    return {};
}

void AppModels::downloadsFinished() {
    // What came: in place with its hash remembered (a download keeps nothing else); it is the picked model. What did not
    // come answers with no handle, so its field stops waiting.
    const QList<Download> done = std::exchange(m_downloading, {});
    for (const Download &download : done) {
        const bool came = QFileInfo::exists(download.path) && hashes().known(download.path) == download.hash;
        emit hashed(download.ticket, came ? diskHandle(download.type, download.name, download.hash).toVariantMap() : QVariantMap());
    }
}

} // namespace fugue
