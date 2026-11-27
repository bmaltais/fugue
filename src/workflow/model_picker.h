#pragma once

// The glue every model picker shares (#452): the node face's WidgetEdit and the App view's AppModels list models, hash a picked
// file on a worker thread, name what the Recommended mark reads, and queue a catalog download the same way, so it is written
// once here. What differs stays with each: WidgetEdit adds its "fits this diffusion model" judging, its per-input picks with
// progress and its undo steps; AppModels answers by ticket.
//
// Free functions build the maps a picker's QML reads (modelRow, catalogRow, catalogDetails), the words for an empty list and the
// download's plan. ModelWorkers owns the hash store and the two worker threads (one for hashes, one for header reads, so a mark
// never waits behind a multi-GB hash) and runs their answers on the owner's thread.

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

#include "engine/file_hashes.h"
#include "engine/model_catalog.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/model_recommend.h"
#include "engine/port_type.h"
#include "engine/result.h"

class QObject;

namespace fugue {

// The line a picker shows under a diffusion model or checkpoint the engine does not recognise (#703); also what Load Models' check says.
QString notRecognisedNote();
// One listed model as a picker row: {title, name, note, status, provider, host, hostOffline, handle}, and for a file on disk
// also path, folder, size, family and, for the Recommended one of a model, recommended and recommendedWhy (`marks` by path); `family` is the engine's name for a file the family table has none for, and a file
// the engine does not recognise has `engineNote`.
QVariantMap modelRow(const ListedModel &model, const QString &type, const QHash<QString, ModelMark> &marks);
// A catalog model not on disk: {catalog: true, id, title, detail, size, keywords, note}.
QVariantMap catalogRow(const CatalogModel &model);
// What the question before a download shows of `model`: license, source, size and the folder it will be saved in.
QVariantMap catalogDetails(const CatalogModel &model, const ModelLibrary &library, const PortType &type);
// Why a list is empty, in words that say what to do.
QString modelsEmptyText(const QStringList &capabilities, const PortType *type, const ModelLibrary &library);
// Where the catalog `model` would be saved and how to fetch it (the folder fitted to the type); an error says why not.
Result<DownloadTarget> planCatalogDownload(ModelLibrary library, const CatalogModel &model, const PortType &type);

class ModelWorkers {
public:
    ModelWorkers();
    ~ModelWorkers(); // stops the work and waits for it

    void setHashes(std::shared_ptr<FileHashes> hashes) { m_hashes = std::move(hashes); }
    // The remembered hashes, made on first use (on the window's thread).
    FileHashes &hashes();

    // Reads `path` on the hash thread (one file at a time: the disk is the limit), remembers its hash, and tells `owner`'s
    // thread `progress(percent)` as it goes and `done(hash)` at the end ("" when unreadable). Nothing is told after stop().
    void hash(QObject *owner, const QString &path, std::function<void(int)> progress, std::function<void(const QString &)> done);

    // Runs `work` on the header thread; its `done` answer runs on `owner`'s thread unless stopped or `owner` is gone.
    void read(QObject *owner, std::function<std::function<void()>()> work);

    std::shared_ptr<std::atomic_bool> stopFlag() const { return m_stop; }
    QThreadPool &headerPool() { return m_headers; }

private:
    std::shared_ptr<FileHashes> m_hashes;
    std::shared_ptr<std::atomic_bool> m_stop = std::make_shared<std::atomic_bool>(false);
    QThreadPool m_hasher;
    QThreadPool m_headers;
};

} // namespace fugue
