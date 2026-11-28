#pragma once

// Getting a template, workflow or package ready with one approval (spec 6.6, 6.7, 6.8, 8.9, 11.10; #379).
//
// A ReadinessPlan is the whole list of what is missing: the packs to install and trust (carried by a
// template or named by the workflow's `requires.packs`), the environment each one's setup command makes,
// and the models to download, with the totals and the free space where the models go. It is built once,
// from a workflow, a template or a package, and the window's "Get ready" sheet and the CLI's `prepare`
// commands both show that same plan.
//
// runReadinessPlan() is the one executor. The person's one yes covers every row: the pack's install, its
// trust, its setup command and the model downloads, with no second question. It still decides nothing
// itself: it is only called after a yes, and a pack that changed since the plan showed it (another
// version, another setup command, other permissions) is refused, so the yes is never stretched to
// something the person did not read. A model whose resource type a pack of the same plan declares is planned after the pack rows,
// with the types the installed packs declare, and downloaded in the same run (#418). Each row is all or nothing (a pack is staged and put in place only
// once it loads and its setup worked, 6.6; a model is written as a `.part` and kept only when its hash
// matches, 8.9). The first failure stops the run: the rows after it are not started, nothing is half
// installed, and the outcome says what to run again (a new plan holds only what is still missing).

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#include "engine/error.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/pack_manager.h"
#include "engine/templates.h"
#include "engine/trust_store.h"

namespace fugue {

class Document;
class FileHashes;
class ModelCatalog;
class Registry;

// A pack a workflow needs, and where it can be had.
struct WantedPack {
    QString id;
    QString version; // the workflow's or the carried pack's, may be empty
    QString name;
    bool needsInstall = false; // its nodes are not installed here (packsNeeded); else only trust may be missing
    bool hasSource = false;
    PackSource source;
};

struct ReadinessRequest {
    QList<WantedPack> packs;
    QJsonArray models; // Document::models()'s form
    // Models only a Switch's lazy side holds (#594): they block nothing, so the plan lists them apart (`optionalModels`), unless the
    // person chose one (its hash is in `optionalChosen`): then it is a row of the plan like the others, marked `optional`.
    QJsonArray optionalModels;
    QSet<QString> optionalChosen;
};

// The packs a document uses (TrustStore::packsUsed), each with where to get it: the carried pack's
// folder, else `requires.packs`' source and ref. A pack with neither has no source.
// A relative folder source is read against `baseFolder` (the workflow's or the template's folder), never the working directory;
// a source that is neither a folder nor a git address gives no source.
ReadinessRequest requestFor(const Document &document, const Registry &registry, const QList<CarriedPack> &carried = {},
                            const QString &baseFolder = {});

struct ReadinessPack {
    // Trust: installed here already, not trusted yet. Setup: installed and trusted, its setup command never ran to its end
    // here (no marker, 6.6): a pack installed before the marker existed is offered it too.
    enum class Action { Install, Trust, Setup };
    Action action = Action::Install;
    PackSummary pack;   // what is shown and approved: for an install as it was previewed
    QString folderName; // Trust, Setup: the installed folder's name, which `trustPack` and `setUpPack` take
    bool setupNeeded = false; // Trust, Setup: the pack has a setup command and no marker; the same yes runs it before it is trusted
    // A git pack: before the yes nothing is fetched, so `pack` holds only what the workflow records (id, version, name, source)
    // and the row says its details come after the download. After the yes it is cloned (https only, size capped), checked
    // against that record, and installed from that clone.
    bool pending = false;
    // A folder pack, copied to a staging folder when the plan was built; what is installed is that copy.
    std::shared_ptr<PreparedPack> prepared;

    bool installs() const { return action == Action::Install; }
    bool hasEnvironment() const { return (installs() || setupNeeded) && !pending && !pack.setupCommand.isEmpty(); }
};

struct ReadinessModel {
    QString name;       // the handle's name
    QString resource;   // "fugue/diffusion-model"
    QString license;    // from the catalog when the file is in it, else empty
    QString licenseUrl;
    QString host;       // the source's host
    qint64 size = -1;   // -1: not recorded
    DownloadTarget target; // the file it becomes
    QString folder;     // where it goes; empty while `deferred`
    QJsonObject entry;  // what the workflow records (Document::models()'s form), to plan it again once its pack is in
    // Its resource type is declared by a pack this plan installs, so it cannot be planned before that pack is in (#418):
    // it is listed like any other model and the same yes covers it. The run plans it after the pack rows.
    bool deferred = false;
    // A paired host that may have the file: it cannot be asked (describe lists no model of a helper that takes its files in the job), so the
    // row stays and says so (#493). Empty: this machine is where it runs.
    QString onHost;
    QString typeFrom;   // deferred: the pack that gives its type (its name), or empty when the list cannot tell
    bool optional = false; // only a Switch's lazy side needs it (#594): in `models` only because the person chose to download it
};

// A model that is not on this machine but is on a paired host that lists it (#735): it needs no download and no Get ready row, and
// is shown as "on <host>".
struct HostedModel {
    QString name;
    QString host;
    QJsonObject handle;
};

struct ReadinessPlan {
    QList<ReadinessPack> packs;
    QList<ReadinessModel> models;
    // Missing here, on a paired host's list: counted as available, never offered as a download.
    QList<HostedModel> hosted;
    // The missing models only a lazy side needs and the person did not choose: non-blocking, so not in `models`, `empty()`, the
    // headline or the sizes. Each has a download target, or is left out (no source, no place to put it).
    QList<ReadinessModel> optionalModels;
    QString destination;   // the model folder the models go into (settings' download_folder, or Fugue's own)
    QStringList destinations; // every model folder they may go in (the Change… choices)
    qint64 downloadBytes = 0;    // the models' sizes that are known
    int unknownSizes = 0;        // models that do not record a size
    qint64 environmentBytes = 0; // the setups' sizes that are known
    int unknownEnvironments = 0;
    qint64 freeBytes = -1;       // free where the models go; -1 when it cannot be told
    // What a run needs to plan the deferred models again once the packs are in: the registry the plan was built with
    // (it must outlive the plan, and nothing may change it during a run on another thread: give the run a copy then) and
    // the model folders as they were chosen.
    const Registry *registry = nullptr;
    std::shared_ptr<ModelLibrary> library;
    // What cannot be made ready from here: a pack with no known source, a model with no source, a
    // pack that did not preview. Warnings, not part of the one yes.
    Errors problems;
    // The handles of the missing models that cannot be got here, with no source or one that cannot be used (each is also a `model_missing` or
    // `download_failed` problem): what a paired host may still have (#491).
    QList<QJsonObject> unobtainable;

    // The environment totals again (the builder calls it).
    void recount();
    int installCount() const;
    int trustOnlyCount() const;
    int environmentCount() const;
    bool empty() const { return packs.isEmpty() && models.isEmpty(); }
    // A row per pack, then per model (the run's row numbers).
    int rowCount() const { return int(packs.size() + models.size()); }
    // Disk the whole needs: the models and the environments, as far as sizes are known.
    int deferredCount() const;
    qint64 diskBytes() const { return downloadBytes + environmentBytes; }
    // The models do not fit where they go (needs a known free space).
    bool lowSpace() const { return freeBytes >= 0 && downloadBytes > freeBytes; }
    // "Fugue will install 1 pack, set up 1 environment and download 1 model (1.2 GB)".
    QString headline() const;
    // The list a terminal shows, one line each.
    QStringList lines() const;
    QJsonObject toJson() const;
};

struct ReadinessEnv {
    explicit ReadinessEnv(ModelLibrary models) : library(std::move(models)) {}
    const Registry *registry = nullptr;
    ModelLibrary library; // the settings in effect (ModelLibrary::current)
    FileHashes *hashes = nullptr;
    TrustStore *trust = nullptr;
    QString packsDir;
    const ModelCatalog *catalog = nullptr; // for a model's license; may be null
    // The paired host whose model list has the file this handle names (by its hash), or empty: the model is then available
    // there (ReadinessPlan::hosted) and is not a download. Null: no host is asked. Reads kept lists only (HostModelCache).
    std::function<QString(const QJsonObject &handle)> hostHaving;
    // Free bytes where a folder is (a folder that does not exist yet counts as its parent); default QStorageInfo.
    std::function<qint64(const QString &)> freeSpace;
    // What a folder pack's copy prints and `cancelled` to stop it. May be null.
    PackPrompts prompts;
};

ReadinessPlan buildReadinessPlan(const ReadinessRequest &request, const ReadinessEnv &env);

enum class RowState { Waiting, Running, Done, Failed, Skipped };

struct ReadinessRun {
    ReadinessRun(QString packsFolder, TrustStore *trustStore, FileHashes *fileHashes)
        : packsDir(std::move(packsFolder)), trust(trustStore), hashes(fileHashes) {}
    QString packsDir;
    TrustStore *trust = nullptr;
    FileHashes *hashes = nullptr;
    // Called as a row changes: its number (packs, then models), its state and a line (the error on Failed).
    std::function<void(int row, RowState state, const QString &detail)> onRow;
    // A model's bytes, as they arrive: at most every 100 ms, and once more when the last byte is in.
    std::function<void(int row, qint64 received, qint64 total)> onProgress;
    // What git and a setup print.
    std::function<void(const QString &)> log;
    // True when the person stopped the work; polled while it runs, from the running thread.
    std::function<bool()> cancelled;
    int retries = 5;
    int retryDelayMs = 2000;
    // Brings a git pack in (tests: nothing reaches a network); null runs git.
    std::function<Result<QString>(const PackSource &, const QString &)> fetch;
    // A pack that was cloned after the yes declares more than the list could show (its permissions): asked once, for that
    // row only. False skips the row. Null refuses.
    std::function<bool(const PackSummary &pack, const QStringList &permissions)> allowMore;
    // Free bytes where a folder is; default QStorageInfo. Checked before each download.
    std::function<qint64(const QString &)> freeSpace;
};

struct ReadinessOutcome {
    bool ok = false;
    bool cancelled = false;
    // What a stop interrupted (only with `cancelled`): a download (its part is kept), an install (undone), a setup (may be half made).
    bool stoppedDownload = false;
    bool stoppedInstall = false;
    bool stoppedSetup = false;
    int failedRow = -1;
    int done = 0;       // rows finished
    bool packsChanged = false; // a pack was installed or trusted: the node types are to be read again
    Errors errors;
    QStringList retry;  // the rows not done, by name: what a second run does
};

// Runs the plan, blocking, with its own event loop for the downloads: call it from a thread that may
// block (the CLI's, or a worker's). Packs first, in order, then the models in order.
ReadinessOutcome runReadinessPlan(const ReadinessPlan &plan, ReadinessRun &run);

} // namespace fugue
