#pragma once

// The model picker of the App view (#386): for a resource type (and the capabilities of the node that takes it), the models a
// person can choose: the ones their providers offer, the files in the model folders (the Recommended one of a model marked,
// #370) and the catalog's models not on disk yet, which a Download fetches. No window and no document: a type in, entries out,
// so any form can draw them. The canvas's picker (WidgetEdit::models) is the same list for a node's input and stays tied to the
// canvas; this is the small version of it a form needs (no "fits this diffusion model" judging, which needs the node's
// neighbours).
//
// Entries (`entries`): {title, note, status ("ready", "loading", "unavailable"), handle (what a pick stores), path?, folder?, size?,
// family?, recommended?, recommendedWhy?}; a catalog model is {catalog: true, id, title, detail, size, note}. A file on disk whose
// hash is not remembered has a handle with no hash: pick() reads the file on a pool thread and says `hashed(type, handle)` when
// it can name it; a download says it the same way once the file has arrived.
//
// Seams for tests: setLibrary (the model folders), setSource (the providers), setCatalog.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QVariantList>

#include <functional>
#include <atomic>
#include <memory>
#include <optional>

#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/model_recommend.h"

namespace fugue {

class FileHashes;
class ModelDownloads;
class Registry;

class AppModels : public QObject {
    Q_OBJECT

public:
    using Source = std::function<void(const QString &type, const QStringList &capabilities, std::function<void(QList<ListedModel>)>)>;

    explicit AppModels(const Registry *registry, QObject *parent = nullptr);
    ~AppModels() override;

    // The window's one download line; none: Download says it is not available here.
    void setDownloads(ModelDownloads *downloads);
    void setLibrary(const ModelLibrary &library) { m_library = library; }
    void setSource(Source source) { m_source = std::move(source); }
    void setCatalog(ModelCatalog catalog) { m_catalog = std::move(catalog); }
    void setHashes(std::shared_ptr<FileHashes> hashes) { m_hashes = std::move(hashes); }

    // Lists what is on disk and in the catalog now and asks the providers and the header reader for the rest; `changed` says
    // when they answer. Asking again is a refresh.
    void request(const QString &type, const QStringList &capabilities);
    QVariantList entries(const QString &type, const QStringList &capabilities) const;
    bool loading(const QString &type, const QStringList &capabilities) const;
    // Why the list is empty, in words that say what to do.
    QString emptyText(const QString &type, const QStringList &capabilities) const;

    // The file at `path` chosen for `type`: its handle through `hashed` with the caller's `ticket` (at once when its hash
    // is remembered).
    void pick(int ticket, const QString &type, const QString &path);
    // What the question before a download shows of catalog model `id` (license, size, source, folder); empty for none of this type.
    QVariantMap catalogInfo(const QString &type, const QString &id) const;
    // Queues the catalog model `id` for download; its handle comes through `hashed` with `ticket` when it has arrived.
    // Returns why not, or empty.
    QString download(int ticket, const QString &type, const QString &id);

signals:
    void changed();
    // The answer to the pick or download given `ticket`; an empty handle: it failed (the file could not be read, the
    // download did not arrive), so the field stops waiting.
    void hashed(int ticket, const QVariantMap &handle);

private:
    struct Entry {
        QList<ListedModel> disk;
        QList<ListedModel> offered;
        QList<CatalogModel> missing;
        QHash<QString, ModelMark> marks;
        bool loading = false;
    };
    struct Download {
        int ticket = 0;
        QString type, path, name, hash;
    };
    static QString keyOf(const QString &type, const QStringList &capabilities);
    ModelLibrary library() const;
    FileHashes &hashes();
    void downloadsFinished();

    const Registry *m_registry;
    std::optional<ModelLibrary> m_library; // none: the model folders as Settings has them
    Source m_source;
    std::optional<ModelCatalog> m_catalog; // none: read each time, so a file dropped in the catalog folder shows
    ModelDownloads *m_downloads = nullptr;
    std::shared_ptr<FileHashes> m_hashes;
    QHash<QString, Entry> m_entries;
    QList<Download> m_downloading;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic_bool> m_stop = std::make_shared<std::atomic_bool>(false);
    int m_asked = 0;
};

} // namespace fugue
