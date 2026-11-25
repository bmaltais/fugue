#include "workflow/model_picker.h"

#include <QFileInfo>
#include <QPointer>
#include <QUrl>

#include "engine/model_family.h"
#include "engine/model_speed.h"
#include "workflow/model_downloads.h"

namespace fugue {

QVariantMap modelRow(const ListedModel &model, const QString &type, const QHash<QString, ModelMark> &marks) {
    QVariantMap row{{"name", model.name},
                    {"title", model.title.isEmpty() ? model.name : model.title},
                    {"handle", model.handle.toVariantMap()},
                    {"status", model.status},
                    {"provider", model.provider},
                    {"host", model.host},
                    {"hostOffline", model.hostOffline()},
                    {"cloud", model.cloud},
                    {"note", model.note}};
    if (model.onDisk()) {
        row.insert("path", model.path);
        row.insert("folder", model.folder);
        row.insert("size", model.size);
        row.insert("family", modelFamily(familyKindOf(type), QFileInfo(model.path).fileName()));
        if (const auto mark = marks.constFind(model.path); mark != marks.constEnd() && mark->recommended) {
            row.insert("recommended", true);
            row.insert("recommendedWhy", mark->reason);
        }
    }
    return row;
}

QVariantMap catalogRow(const CatalogModel &model) {
    return QVariantMap{{"catalog", true},
                       {"id", model.id},
                       {"title", model.title},
                       {"detail", model.summary()},
                       {"size", model.size},
                       {"keywords", model.keywords()},
                       {"note", model.description}};
}

QVariantMap catalogDetails(const CatalogModel &model, const ModelLibrary &library, const PortType &type) {
    return QVariantMap{{"id", model.id},
                       {"title", model.title},
                       {"description", model.description},
                       {"scale", model.scale},
                       {"architecture", model.architecture},
                       {"license", model.license},
                       {"licenseLine", licenseLine(model)},
                       {"licenseUrl", model.licenseUrl},
                       {"commercial", model.commercial},
                       {"attribution", model.attribution},
                       {"author", model.author},
                       {"source", model.source},
                       {"url", model.url},
                       {"host", QUrl(model.url).host()},
                       {"size", model.size},
                       {"sizeText", ModelDownloads::sizeText(model.size)},
                       {"folder", library.downloadFolder(type)},
                       {"file", model.file}};
}

QString modelsEmptyText(const QStringList &capabilities, const PortType *type, const ModelLibrary &library) {
    const bool image = std::any_of(capabilities.begin(), capabilities.end(),
                                   [](const QString &capability) { return capability.startsWith(QLatin1String("image.")); });
    if (image)
        return QObject::tr("No image model available: start one in omarchy-local-ai or add a provider");
    if (capabilities.contains(QStringLiteral("text.generate")))
        return QObject::tr("No text model available: start one in omarchy-local-ai or add a provider");
    // A model file of this kind goes in its folder in Fugue's models folder (8.9).
    const QString folder = type ? library.folders(*type).value(0) : QString();
    if (folder.isEmpty())
        return QObject::tr("No model available: start one in omarchy-local-ai or add a provider");
    return QObject::tr("No model found: put one in %1, or add its folder to Extra model folders in Settings").arg(folder);
}

Result<DownloadTarget> planCatalogDownload(ModelLibrary library, const CatalogModel &model, const PortType &type) {
    library.fitDownloadRoot({type});
    MissingModel wanted;
    wanted.handle = model.handle();
    wanted.source = model.url;
    wanted.size = model.size;
    wanted.folder = library.downloadFolder(type);
    return planDownload(library, wanted);
}

ModelWorkers::ModelWorkers() {
    m_hasher.setMaxThreadCount(1); // one file after the other: the disk is the limit
    m_headers.setMaxThreadCount(1);
}

ModelWorkers::~ModelWorkers() {
    m_stop->store(true);
    m_hasher.waitForDone();
    m_headers.waitForDone();
}

FileHashes &ModelWorkers::hashes() {
    if (!m_hashes)
        m_hashes = std::make_shared<FileHashes>(FileHashes::defaultPath());
    return *m_hashes;
}

void ModelWorkers::hash(QObject *owner, const QString &path, std::function<void(int)> progress,
                        std::function<void(const QString &)> done) {
    QPointer<QObject> self(owner);
    hashes(); // made here, on the window's thread
    const std::shared_ptr<FileHashes> store = m_hashes;
    const std::shared_ptr<std::atomic_bool> stop = m_stop;
    m_hasher.start([self, store, stop, path, progress = std::move(progress), done = std::move(done)] {
        int shown = -1; // percent last told, so the window is told at most a hundred times
        const QString hash = store->hash(path, [&](qint64 read, qint64 total) {
            const int percent = total > 0 ? int(read * 100 / total) : 100;
            if (percent != shown && progress) {
                shown = percent;
                QMetaObject::invokeMethod(self, [self, progress, percent] {
                    if (self)
                        progress(percent);
                });
            }
            return !stop->load();
        });
        if (stop->load())
            return;
        if (!hash.isEmpty())
            store->save();
        QMetaObject::invokeMethod(self, [self, done, hash] {
            if (self)
                done(hash);
        });
    });
}

void ModelWorkers::read(QObject *owner, std::function<std::function<void()>()> work) {
    QPointer<QObject> self(owner);
    const std::shared_ptr<std::atomic_bool> stop = m_stop;
    m_headers.start([self, stop, work = std::move(work)] {
        if (stop->load())
            return;
        std::function<void()> answer = work();
        QMetaObject::invokeMethod(self, [self, answer = std::move(answer)] {
            if (self && answer)
                answer();
        });
    });
}

} // namespace fugue
