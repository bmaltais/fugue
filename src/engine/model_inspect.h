#pragma once

// What the engine says a model file is (#703, spec 2.5, 8.10): `fugue-helper --inspect FILE...` reads a file's header
// with stable-diffusion.cpp's own detection and answers one JSON line per file. Fugue's own tables (model_family,
// model_shape) name 12 of the ~64 versions the engine knows; this asks the engine, so a file of any other family still
// gets a name, and a file the engine will not load is known before a run.
//
// The answers are cached by path, size and modification time in <cacheDir>/model-inspect.json, with the stamp of the helper that
// gave them (a new helper may know more: its answers replace the old). The cache is Fugue's, one for every project; a
// project does not inherit or keep its own. With no local helper nothing is asked and nothing changes.
//
// inspect() blocks (a process of ~0.2 s, more for many files) and so belongs on a worker thread, as the pickers' header
// reads are; cached() never does. Thread-safe.

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <optional>

namespace fugue {

struct EngineModel {
    bool recognised = false;
    QString version; // the engine's name for it ("Qwen Image", "SDXL Inpaint"); empty when not recognised
    QString id; // the engine's family id ("sdxl_inpaint", "flux2_klein")
    bool isDit = false;
    bool video = false;
    bool inpaint = false;
    int latentChannels = 0;
    QString vaeKind;

    QJsonObject toJson() const;
    static EngineModel fromJson(const QJsonObject &object);
};

// The resource kinds the engine's detection is about: a diffusion model or a checkpoint ("fugue/diffusion-model",
// "fugue/model" or the bare kind). A text encoder or VAE is read by the engine as a part, and its detection of one is not meaningful
// (a CLIP-L file comes back as a version).
bool engineDetectsKind(const QString &resourceType);

class ModelInspector {
public:
    // `helper` the fugue-helper to run (empty: none, nothing is asked), `cacheFile` where answers are kept (empty: in memory only).
    ModelInspector(QString helper, QString cacheFile, int timeoutMs = 60000);

    // The process-wide inspector: the local helper (helperProgram(), looked up on each call, so FUGUE_HELPER and a
    // helper installed since are followed) and the cache in Fugue's cache folder.
    static ModelInspector &shared();

    bool available() const { return !m_helper.isEmpty(); }
    // Another helper or cache file: what was kept in memory is dropped.
    void configure(const QString &helper, const QString &cacheFile);

    // The answer kept for `path` while its size and modification time are as they were.
    std::optional<EngineModel> cached(const QString &path) const;
    // The answers for `paths`: the kept ones, and the others asked of the helper in one process per 40 files. A file the helper
    // could not read, or a run that failed or timed out, has no entry (and is not asked again until the helper or the file changes,
    // for this process).
    QHash<QString, EngineModel> inspect(const QStringList &paths);

private:
    struct Entry {
        qint64 size = 0;
        qint64 mtime = 0;
        EngineModel model;
    };
    static QString stamp(const QString &program);
    void load();
    void save();
    bool stampOf(const QString &path, qint64 &size, qint64 &mtime) const;
    QHash<QString, EngineModel> ask(const QStringList &paths);

    QString m_helper;
    QString m_cacheFile;
    int m_timeoutMs;
    QString m_helperStamp;
    bool m_loaded = false;
    QHash<QString, Entry> m_entries;
    QHash<QString, QPair<qint64, qint64>> m_failed; // asked in vain, by path: its stamp
    mutable QMutex m_mutex;
};

} // namespace fugue
