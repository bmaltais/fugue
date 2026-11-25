#include "apps/app_models.h"

#include <QFileInfo>
#include <QPointer>

#include "engine/file_hashes.h"
#include "engine/model_download.h"
#include "engine/model_family.h"
#include "engine/model_speed.h"
#include "engine/port_type.h"
#include "engine/registry.h"
#include "workflow/model_downloads.h"

namespace fugue {

AppModels::AppModels(const Registry *registry, QObject *parent) : QObject(parent), m_registry(registry) {
    m_pool.setMaxThreadCount(1); // headers and hashes one after the other: the disk is the limit
}

AppModels::~AppModels() {
    m_stop->store(true);
    m_pool.waitForDone();
}

QString AppModels::keyOf(const QString &type, const QStringList &capabilities) {
    return type + QLatin1Char('\n') + capabilities.join(QLatin1Char(','));
}

ModelLibrary AppModels::library() const { return m_library ? *m_library : ModelLibrary::current(m_registry); }

FileHashes &AppModels::hashes() {
    if (!m_hashes)
        m_hashes = std::make_shared<FileHashes>(FileHashes::defaultPath());
    return *m_hashes;
}

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
    emit changed();

    // The Recommended mark reads file headers: off the window's thread, applied when it is done unless asked again.
    const int asked = ++m_asked;
    QList<MarkedFile> files;
    for (const ListedModel &model : std::as_const(entry.disk))
        files.append({model.path, model.size});
    if (!files.isEmpty()) {
        QPointer<AppModels> self(this);
        const std::shared_ptr<std::atomic_bool> stop = m_stop;
        m_pool.start([self, stop, key, type, files, asked] {
            if (stop->load())
                return;
            const ModelSpeeds speeds;
            const QHash<QString, ModelMark> found =
                markModels(type, files, RecommendContext{localComputeDevice(), 0, &speeds}, ModelShapes::shared());
            QMetaObject::invokeMethod(self, [self, key, found, asked] {
                if (!self || self->m_asked != asked || !self->m_entries.contains(key))
                    return;
                self->m_entries[key].marks = found;
                emit self->changed();
            });
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
    for (const ListedModel &model : entry.offered + entry.disk) {
        QVariantMap row{{"title", model.title.isEmpty() ? model.name : model.title},
                        {"name", model.name},
                        {"note", model.note},
                        {"status", model.status},
                        {"provider", model.provider},
                        {"handle", model.handle.toVariantMap()}};
        if (model.onDisk()) {
            row.insert("path", model.path);
            row.insert("folder", model.folder);
            row.insert("size", model.size);
            row.insert("family", modelFamily(familyKindOf(type), QFileInfo(model.path).fileName()));
            if (const auto mark = entry.marks.constFind(model.path); mark != entry.marks.constEnd() && mark->recommended) {
                row.insert("recommended", true);
                row.insert("recommendedWhy", mark->reason);
            }
        }
        list.append(row);
    }
    for (const CatalogModel &model : std::as_const(entry.missing))
        list.append(QVariantMap{{"catalog", true},
                                {"id", model.id},
                                {"title", model.title},
                                {"detail", model.summary()},
                                {"size", model.size},
                                {"note", model.description}});
    return list;
}

QString AppModels::emptyText(const QString &type, const QStringList &capabilities) const {
    const bool image = std::any_of(capabilities.begin(), capabilities.end(),
                                   [](const QString &capability) { return capability.startsWith(QLatin1String("image.")); });
    if (image)
        return tr("No image model available: start one in omarchy-local-ai or add a provider");
    if (capabilities.contains(QStringLiteral("text.generate")))
        return tr("No text model available: start one in omarchy-local-ai or add a provider");
    const std::optional<PortType> portType = PortType::parse(type);
    const QString folder = portType ? library().folders(*portType).value(0) : QString();
    if (folder.isEmpty())
        return tr("No model available: start one in omarchy-local-ai or add a provider");
    return tr("No model found: put one in %1, or add its folder to Extra model folders in Settings").arg(folder);
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
    QPointer<AppModels> self(this);
    const std::shared_ptr<FileHashes> store = m_hashes;
    const std::shared_ptr<std::atomic_bool> stop = m_stop;
    hashes(); // made here, on the window's thread
    const std::shared_ptr<FileHashes> shared = m_hashes;
    m_pool.start([self, shared, stop, ticket, type, path, name] {
        if (stop->load())
            return;
        const QString hash = shared->hash(path);
        if (stop->load())
            return;
        QMetaObject::invokeMethod(self, [self, ticket, type, name, hash] {
            if (self) // an unreadable file answers with no handle, so the field stops waiting
                emit self->hashed(ticket, hash.isEmpty() ? QVariantMap() : diskHandle(type, name, hash).toVariantMap());
        });
    });
}

QVariantMap AppModels::catalogInfo(const QString &type, const QString &id) const {
    const ModelCatalog catalog = m_catalog ? *m_catalog : ModelCatalog::load();
    const CatalogModel *model = catalog.find(id);
    const std::optional<PortType> portType = PortType::parse(type);
    if (!model || !portType || model->resource != type)
        return {};
    return QVariantMap{{"id", model->id},
                       {"title", model->title},
                       {"description", model->description},
                       {"licenseLine", licenseLine(*model)},
                       {"licenseUrl", model->licenseUrl},
                       {"commercial", model->commercial},
                       {"author", model->author},
                       {"host", QUrl(model->url).host()},
                       {"sizeText", ModelDownloads::sizeText(model->size)},
                       {"folder", library().downloadFolder(*portType)}};
}

QString AppModels::download(int ticket, const QString &type, const QString &id) {
    const std::optional<PortType> portType = PortType::parse(type);
    const ModelCatalog catalog = m_catalog ? *m_catalog : ModelCatalog::load();
    const CatalogModel *model = catalog.find(id);
    if (!portType || !model || model->resource != type)
        return tr("This model is not in the catalog for this input.");
    if (!m_downloads)
        return tr("Downloads are not available here.");
    ModelLibrary lib = library();
    lib.fitDownloadRoot({*portType});
    MissingModel wanted;
    wanted.handle = model->handle();
    wanted.source = model->url;
    wanted.size = model->size;
    wanted.folder = lib.downloadFolder(*portType);
    const Result<DownloadTarget> planned = planDownload(lib, wanted);
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
