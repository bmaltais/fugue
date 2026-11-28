#include "engine/readiness.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>
#include "engine/version.h"

#include <algorithm>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/lazy_reach.h"
#include "engine/model_catalog.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"

namespace fugue {

namespace {

QString plural(int count, const QString &one, const QString &many) {
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? one : many);
}

qint64 freeAt(const QString &folder) {
    QString at = QDir::cleanPath(folder);
    while (!at.isEmpty() && !QFileInfo(at).isDir()) {
        const QString parent = QFileInfo(at).absolutePath();
        if (parent == at)
            break;
        at = parent;
    }
    const QStorageInfo info(at);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

Error problem(const QString &code, const QString &message, const QString &hint = {}) {
    Error error = makeError(code, message, hint);
    error.severity = Error::Severity::Warning;
    return error;
}

QString sourceText(const PackSource &source) {
    if (source.location.isEmpty())
        return {};
    return source.ref.isEmpty() ? source.location : QStringLiteral("%1 at %2").arg(source.location, source.ref);
}

QString packName(const PackSummary &pack) { return pack.name.isEmpty() ? pack.id : pack.name; }

} // namespace

ReadinessRequest requestFor(const Document &document, const Registry &registry, const QList<CarriedPack> &carried,
                            const QString &baseFolder) {
    ReadinessRequest request;
    request.models = modelsToHave(document, registry); // not a model only a Switch's lazy side holds (#557)
    request.optionalModels = optionalModels(document, registry); // offered apart (#594)
    const QStringList needed = packsNeeded(document, registry);
    const QJsonObject recorded = document.requirements.value(QStringLiteral("packs")).toObject();
    for (const PackUse &use : TrustStore::packsUsed(document, registry)) {
        WantedPack want;
        want.id = use.id;
        want.version = use.version;
        want.needsInstall = needed.contains(use.id);
        const QJsonObject entry = recorded.value(use.id).toObject();
        want.name = entry.value(QStringLiteral("name")).toString();
        const auto own = std::find_if(carried.cbegin(), carried.cend(), [&](const CarriedPack &c) { return c.id == use.id; });
        if (own != carried.cend()) {
            want.hasSource = true;
            want.source = PackSource{PackSource::Kind::Path, own->folder, {}, {}};
            if (!own->name.isEmpty())
                want.name = own->name;
        } else if (const QString source = entry.value(QStringLiteral("source")).toString(); !source.isEmpty()) {
            // A folder (relative to the workflow's own, not the working directory), or a real git address; else no source.
            const QString folder = QFileInfo(source).isRelative() && !baseFolder.isEmpty() ? QDir(baseFolder).absoluteFilePath(source) : source;
            if (QFileInfo(folder).isDir()) {
                want.hasSource = true;
                want.source = PackSource{PackSource::Kind::Path, QDir::cleanPath(folder), {}, {}};
            } else if (looksLikeGitUrl(source)) {
                want.hasSource = true;
                want.source = PackSource{PackSource::Kind::Git, source, entry.value(QStringLiteral("ref")).toString(), {}};
            }
        }
        request.packs.append(want);
    }
    return request;
}

void ReadinessPlan::recount() {
    environmentBytes = 0;
    unknownEnvironments = 0;
    for (const ReadinessPack &row : std::as_const(packs)) {
        if (!row.hasEnvironment())
            continue;
        if (row.pack.setupSize >= 0)
            environmentBytes += row.pack.setupSize;
        else
            ++unknownEnvironments;
    }
}

int ReadinessPlan::installCount() const {
    return int(std::count_if(packs.cbegin(), packs.cend(), [](const ReadinessPack &p) { return p.installs(); }));
}
int ReadinessPlan::trustOnlyCount() const {
    return int(std::count_if(packs.cbegin(), packs.cend(), [](const ReadinessPack &p) { return p.action == ReadinessPack::Action::Trust; }));
}
int ReadinessPlan::environmentCount() const {
    return int(std::count_if(packs.cbegin(), packs.cend(), [](const ReadinessPack &p) { return p.hasEnvironment(); }));
}

int ReadinessPlan::deferredCount() const {
    return int(std::count_if(models.cbegin(), models.cend(), [](const ReadinessModel &m) { return m.deferred; }));
}

QString ReadinessPlan::headline() const {
    QStringList parts;
    if (const int n = installCount())
        parts << QStringLiteral("install %1").arg(plural(n, QStringLiteral("pack"), QStringLiteral("packs")));
    if (const int n = trustOnlyCount())
        parts << QStringLiteral("trust %1").arg(plural(n, QStringLiteral("pack"), QStringLiteral("packs")));
    if (const int n = environmentCount())
        parts << QStringLiteral("set up %1").arg(plural(n, QStringLiteral("environment"), QStringLiteral("environments")));
    if (!models.isEmpty()) {
        QString text = QStringLiteral("download %1").arg(plural(int(models.size()), QStringLiteral("model"), QStringLiteral("models")));
        if (downloadBytes > 0)
            text += QStringLiteral(" (%1%2)").arg(unknownSizes ? QStringLiteral("at least ") : QString(), sizeText(downloadBytes));
        parts << text;
    }
    if (parts.isEmpty())
        return QStringLiteral("Nothing to get ready.");
    const QString last = parts.takeLast();
    return QStringLiteral("Fugue will %1.").arg(parts.isEmpty() ? last : parts.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last);
}

QStringList ReadinessPlan::lines() const {
    QStringList out{headline(), QString()};
    for (const ReadinessPack &row : packs) {
        const PackSummary &p = row.pack;
        out << QStringLiteral("  %1 %2 %3 (%4)").arg(row.installs() ? QStringLiteral("Install and trust pack:")
                                                      : row.action == ReadinessPack::Action::Setup ? QStringLiteral("Set up pack:")
                                                                                                   : QStringLiteral("Trust pack:"),
                                                  packName(p), p.version, p.id);
        if (const QString from = sourceText(p.source); row.installs() && !from.isEmpty())
            out << QStringLiteral("    From:      %1").arg(from);
        if (row.pending) {
            out << QStringLiteral("    Details after download: what it runs as, what it declares and its setup are shown by the pack itself once fetched (https only, 200 MB at most);");
            out << QStringLiteral("    a pack that declares network, file or GPU access, reads secrets or has a setup asks once more before it is installed.");
            continue;
        }
        out << QStringLiteral("    Runs as:   %1").arg(p.runtimeKinds.isEmpty() ? QStringLiteral("(no nodes)") : p.runtimeKinds.join(QStringLiteral(", ")));
        out << QStringLiteral("    Declares:  %1").arg(p.permissions.isEmpty() ? QStringLiteral("nothing (network, files, GPU)") : p.permissions.join(QStringLiteral(", ")));
        if (!p.secrets.isEmpty())
            out << QStringLiteral("    Reads:     the secrets %1 (you set their values; they are never in a workflow)").arg(p.secrets.join(QStringLiteral(", ")));
        if (row.hasEnvironment()) {
            out << QStringLiteral("    Setup:     %1%2").arg(displayCommand(p.setupCommand),
                                                         p.setupSize >= 0 ? QStringLiteral(" (downloads %1)").arg(sizeText(p.setupSize)) : QString());
            if (!p.setupDescription.isEmpty())
                out << QStringLiteral("               %1").arg(p.setupDescription);
        }
    }
    if (!packs.isEmpty())
        out << QStringLiteral("  Fugue enforces no sandbox: a trusted pack runs programs as you.");
    for (const ReadinessModel &m : models) {
        out << QStringLiteral("  Download model: %1 (%2) from %3%4").arg(m.name, m.size >= 0 ? sizeText(m.size) : QStringLiteral("size not recorded"), m.host,
                                                                          m.license.isEmpty() ? QString() : QStringLiteral(", licence %1").arg(m.license));
        if (m.deferred)
            out << QStringLiteral("    Into:      its %1 folder under %2, once the pack is in (type from %3)")
                       .arg(m.resource, destination, m.typeFrom.isEmpty() ? QStringLiteral("a pack in this list") : m.typeFrom);
        else
            out << QStringLiteral("    Into:      %1").arg(m.folder);
    }
    if (!models.isEmpty()) {
        out << QStringLiteral("  Models go into %1%2").arg(destination, freeBytes >= 0 ? QStringLiteral(" (%1 free)").arg(sizeText(freeBytes)) : QString());
        if (lowSpace())
            out << QStringLiteral("  There is not enough free space there for the download; change Settings > Folders > Download models into.");
    }
    for (const ReadinessModel &m : optionalModels)
        out << QStringLiteral("  Optional model, only if a Switch's other branch is chosen: %1 (%2) from %3")
                   .arg(m.name, m.size >= 0 ? sizeText(m.size) : QStringLiteral("size not recorded"), m.host);
    for (const HostedModel &m : hosted)
        out << QStringLiteral("  Model on %1: %2 (nothing to download here)").arg(m.host, m.name);
    for (const Error &p : problems)
        out << QStringLiteral("  Cannot do: %1").arg(p.message);
    return out;
}

QJsonObject ReadinessPlan::toJson() const {
    QJsonArray packRows, modelRows;
    for (const ReadinessPack &row : packs) {
        const PackSummary &p = row.pack;
        QJsonObject o{{"action", row.installs() ? "install" : row.action == ReadinessPack::Action::Setup ? "setup" : "trust"}, {"pending", row.pending}, {"id", p.id}, {"version", p.version}, {"name", p.name},
                      {"runtime", QJsonArray::fromStringList(p.runtimeKinds)}, {"permissions", QJsonArray::fromStringList(p.permissions)},
                      {"secrets", QJsonArray::fromStringList(p.secrets)}};
        if (const QString from = sourceText(p.source); !from.isEmpty())
            o.insert("source", from);
        if (row.hasEnvironment())
            o.insert("setup", QJsonObject{{"command", QJsonArray::fromStringList(p.setupCommand)}, {"description", p.setupDescription},
                                          {"size", p.setupSize >= 0 ? QJsonValue(double(p.setupSize)) : QJsonValue()}});
        packRows.append(o);
    }
    for (const ReadinessModel &m : models)
        modelRows.append(QJsonObject{{"name", m.name}, {"resource", m.resource}, {"license", m.license}, {"host", m.host},
                                     {"size", m.size >= 0 ? QJsonValue(double(m.size)) : QJsonValue()}, {"folder", m.folder}, {"path", m.target.path},
                                     {"type_from", m.typeFrom}, {"deferred", m.deferred}});
    QJsonObject json{{"headline", headline()}, {"packs", packRows}, {"models", modelRows}, {"destination", destination},
                     {"download_bytes", double(downloadBytes)}, {"unknown_sizes", unknownSizes},
                     {"environment_bytes", double(environmentBytes)}, {"free_bytes", double(freeBytes)},
                     {"low_space", lowSpace()}, {"problems", fugue::toJson(problems)}};
    if (!hosted.isEmpty()) { // on a paired host's model list (#735)
        QJsonArray hostedRows;
        for (const HostedModel &m : hosted)
            hostedRows.append(QJsonObject{{"name", m.name}, {"host", m.host}, {"hash", m.handle.value("hash")}});
        json.insert("hosted", hostedRows);
    }
    if (!optionalModels.isEmpty()) { // only a Switch's other branch needs them (#594)
        QJsonArray optionalRows;
        for (const ReadinessModel &m : optionalModels)
            optionalRows.append(QJsonObject{{"name", m.name}, {"resource", m.resource}, {"license", m.license}, {"host", m.host},
                                            {"size", m.size >= 0 ? QJsonValue(double(m.size)) : QJsonValue()}, {"folder", m.folder}});
        json.insert("optional_models", optionalRows);
    }
    return json;
}

namespace {

// The workflow's own record of `model` (with its source and size), found again by name and type.
QJsonObject entryOf(const QJsonArray &entries, const MissingModel &model) {
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("name")) == model.handle.value(QStringLiteral("name"))
            && entry.value(QStringLiteral("resource")) == model.handle.value(QStringLiteral("resource"))
            && entry.value(QStringLiteral("hash")) == model.handle.value(QStringLiteral("hash")))
            return entry;
    }
    return model.handle;
}

// The ids of the port types a pack folder declares in its pack.json.
QStringList declaredTypeIds(const QString &folder) {
    QFile file(folder + QStringLiteral("/pack.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList ids;
    for (const QJsonValue &type : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("types")).toArray())
        ids << type.toObject().value(QStringLiteral("id")).toString();
    return ids;
}

// When the model's type is not known here but a pack this plan installs gives it (or may: a git pack is not read before the
// yes), the name of that pack ("a pack in this list" when it cannot be told); else nothing, and the model is planned now.
std::optional<QString> typeFromPack(const ReadinessPlan &plan, const QString &resource, const ModelLibrary &library) {
    const std::optional<PortType> type = PortType::parse(resource);
    if (type && !library.downloadFolder(*type).isEmpty())
        return std::nullopt;
    const QString id = type ? type->id : resource;
    QStringList pending;
    for (const ReadinessPack &row : plan.packs) {
        if (!row.installs())
            continue;
        const QString name = packName(row.pack);
        if (row.pending)
            pending << name;
        else if (row.prepared && declaredTypeIds(row.prepared->folder).contains(id))
            return name;
    }
    if (pending.isEmpty())
        return std::nullopt;
    return pending.size() == 1 ? pending.first() : QString();
}

} // namespace

ReadinessPlan buildReadinessPlan(const ReadinessRequest &request, const ReadinessEnv &env) {
    ReadinessPlan plan;
    PackPrompts prompts = env.prompts;
    for (const WantedPack &want : request.packs) {
        const bool installedHere = env.registry && !env.registry->packVersion(want.id).isEmpty();
        if (want.needsInstall) {
            if (installedHere) {
                plan.problems << problem(QStringLiteral("pack_update"),
                                         QStringLiteral("The pack %1 is installed at another version than this needs.").arg(want.id),
                                         QStringLiteral("Update it in Settings > Packs or with `fugue pack update %1`; an update asks on its own.").arg(want.id));
                continue;
            }
            if (!want.hasSource) {
                plan.problems << problem(QStringLiteral("pack_source_unknown"),
                                         QStringLiteral("The pack %1 is not installed and nothing says where to get it.").arg(want.id),
                                         QStringLiteral("Install it with `fugue pack add <folder|git-url>`."));
                continue;
            }
            if (want.source.kind == PackSource::Kind::Git) {
                // Nothing is fetched before the yes: the row shows what the workflow records.
                if (!looksLikeGitUrl(want.source.location) || want.source.ref.isEmpty()) {
                    plan.problems << problem(QStringLiteral("pack_source_unknown"),
                                             QStringLiteral("The pack %1 comes from %2, which is not a git address pinned to a tag or a commit.").arg(want.id, want.source.location),
                                             QStringLiteral("Install it yourself with `fugue pack add <folder|git-url> --ref <tag|commit>`."));
                    continue;
                }
                ReadinessPack row;
                row.pending = true;
                row.pack.id = want.id;
                row.pack.version = want.version;
                row.pack.name = want.name;
                row.pack.source = want.source;
                plan.packs.append(row);
                continue;
            }
            const Result<std::shared_ptr<PreparedPack>> prepared = preparePack(want.source, env.packsDir, prompts);
            if (!prepared) {
                Error e = prepared.error();
                e.severity = Error::Severity::Warning;
                plan.problems << e;
                continue;
            }
            // It must be the pack the workflow needs: its id, and the major its nodes were made with.
            const PackSummary &shown = (*prepared)->summary;
            const std::optional<Version> wantedVersion = Version::parse(want.version);
            if (shown.id != want.id || (wantedVersion && Version::parse(shown.version) && Version::parse(shown.version)->major != wantedVersion->major)) {
                plan.problems << problem(QStringLiteral("pack_invalid"),
                                         QStringLiteral("%1 holds the pack %2 %3, not the pack %4 %5 this needs.").arg(want.source.location, shown.id, shown.version, want.id, want.version));
                continue;
            }
            ReadinessPack row;
            row.pack = shown;
            row.prepared = *prepared;
            plan.packs.append(row);
            continue;
        }
        // Installed here: its trust or its setup may be missing, for a pack in the packs folder. A pack with a setup command
        // and no marker (it never ran here, or it was installed before the marker existed) is offered the setup.
        if (!installedHere || !env.trust || TrustStore::isBuiltIn(want.id))
            continue;
        const bool trusted = env.trust->isTrusted(want.id, want.version);
        for (const QFileInfo &entry : QDir(env.packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.fileName().startsWith(QLatin1Char('.')) || parsePackRef(entry.fileName()).id != want.id)
                continue;
            const Result<PackSummary> summary = summarizePack(entry.filePath());
            if (!summary || summary->version != want.version)
                continue;
            const bool setupNeeded = !summary->setupCommand.isEmpty() && !packSetupDone(entry.filePath(), summary->setupCommand);
            if (trusted && !setupNeeded)
                break;
            ReadinessPack row;
            row.action = trusted ? ReadinessPack::Action::Setup : ReadinessPack::Action::Trust;
            row.pack = *summary;
            row.folderName = entry.fileName();
            row.setupNeeded = setupNeeded;
            plan.packs.append(row);
            break;
        }
    }
    plan.recount();

    // Models: the missing ones with a source, all into one folder (a root of one kind takes them only when all are that kind).
    ModelLibrary library = env.library;
    FileHashes fallback(FileHashes::defaultPath());
    FileHashes &hashes = env.hashes ? *env.hashes : fallback;
    QList<PortType> types;
    for (const MissingModel &model : library.missing(request.models, hashes))
        if (const std::optional<PortType> type = PortType::parse(model.handle.value(QStringLiteral("resource")).toString());
            type && !model.source.isEmpty())
            types << *type;
    library.fitDownloadRoot(types);
    plan.destination = library.downloadRoot();
    plan.destinations = library.downloadRoots(types);
    // One row per missing model, from `entries`. An optional model (#594) that cannot be got (no source, no place) is left out
    // quietly: nothing is lost by it, so there is no problem to report; one the person chose is a row like the others.
    const auto addRows = [&](const QJsonArray &entries, bool optional) {
        for (const MissingModel &model : library.missing(entries, hashes)) {
            const QString name = model.handle.value(QStringLiteral("name")).toString();
            const bool chosen = !optional || request.optionalChosen.contains(model.handle.value(QStringLiteral("hash")).toString());
            // A paired host that lists the file has it: nothing to download or to put here (#735).
            if (env.hostHaving) {
                if (const QString host = env.hostHaving(model.handle); !host.isEmpty()) {
                    plan.hosted.append({name, host, model.handle});
                    continue;
                }
            }
            if (model.source.isEmpty()) {
                if (optional)
                    continue;
                plan.problems << problem(QStringLiteral("model_missing"), QStringLiteral("The model %1 has no source to download from.").arg(name),
                                         QStringLiteral("Put it in %1.").arg(model.folder));
                plan.unobtainable << model.handle;
                continue;
            }
            ReadinessModel row;
            row.name = name;
            row.resource = model.handle.value(QStringLiteral("resource")).toString();
            row.size = model.size;
            row.entry = entryOf(entries, model);
            row.optional = optional;
            // Its type is declared by a pack this plan installs: nothing to plan until that pack is in (#418).
            if (const std::optional<QString> from = typeFromPack(plan, row.resource, library)) {
                row.deferred = true;
                row.typeFrom = *from;
                row.host = QUrl(model.source).host();
            } else {
                const Result<DownloadTarget> target = planDownload(library, model);
                if (!target) {
                    if (optional)
                        continue;
                    for (Error e : target.errors()) {
                        e.severity = Error::Severity::Warning;
                        plan.problems << e;
                    }
                    plan.unobtainable << model.handle;
                    continue;
                }
                row.host = target->source.host();
                row.target = *target;
                row.folder = QFileInfo(target->path).absolutePath();
            }
            if (env.catalog) {
                QString hash = model.handle.value(QStringLiteral("hash")).toString();
                if (hash.startsWith(QLatin1String("sha256:")))
                    hash = hash.mid(7);
                for (const CatalogModel &entry : env.catalog->all())
                    if (!hash.isEmpty() && entry.sha256 == hash) {
                        row.license = entry.license;
                        row.licenseUrl = entry.licenseUrl;
                        break;
                    }
            }
            if (!chosen) {
                plan.optionalModels.append(row);
                continue;
            }
            if (row.size >= 0)
                plan.downloadBytes += row.size;
            else
                ++plan.unknownSizes;
            plan.models.append(row);
        }
    };
    addRows(request.models, false);
    addRows(request.optionalModels, true);
    plan.registry = env.registry;
    plan.library = std::make_shared<ModelLibrary>(library);
    if (!plan.models.isEmpty() || !plan.optionalModels.isEmpty())
        plan.freeBytes = env.freeSpace ? env.freeSpace(plan.destination) : freeAt(plan.destination);
    return plan;
}

namespace {

// How often a download's progress reaches ReadinessRun::onProgress at most.
constexpr qint64 kProgressMs = 100;

// Tells the run's observer, and polls for a stop.
struct Reporter {
    ReadinessRun &run;
    void row(int index, RowState state, const QString &detail = {}) const {
        if (run.onRow)
            run.onRow(index, state, detail);
    }
    bool stopped() const { return run.cancelled && run.cancelled(); }
};

PackPrompts approvals(const PackSummary &shown, ReadinessRun &run, bool &refused) {
    // The one yes was given for exactly this pack: any question whose pack differs is answered No.
    const auto answer = [&shown, &refused](const PackSummary &now) {
        if (samePack(shown, now))
            return Answer::Yes;
        refused = true;
        return Answer::No;
    };
    PackPrompts prompts;
    prompts.install = prompts.trust = prompts.runSetup = answer;
    prompts.log = run.log;
    prompts.cancelled = run.cancelled;
    return prompts;
}

QString rowName(const ReadinessPlan &plan, int index) {
    if (index < plan.packs.size()) {
        const ReadinessPack &row = plan.packs.at(index);
        return QStringLiteral("%1 %2").arg(row.installs() ? QStringLiteral("install pack")
                                           : row.action == ReadinessPack::Action::Setup ? QStringLiteral("set up pack")
                                                                                        : QStringLiteral("trust pack"),
                                           row.pack.id);
    }
    return QStringLiteral("download %1").arg(plan.models.at(index - plan.packs.size()).name);
}

} // namespace

ReadinessOutcome runReadinessPlan(const ReadinessPlan &plan, ReadinessRun &run) {
    ReadinessOutcome outcome;
    const Reporter report{run};
    const int total = plan.rowCount();
    QList<RowState> states(total, RowState::Waiting);
    const auto finish = [&](int index, RowState state, const QString &detail = {}) {
        states[index] = state;
        report.row(index, state, detail);
    };
    const auto stopAt = [&](int index, const Errors &errors, bool cancelled) {
        outcome.failedRow = index;
        outcome.cancelled = cancelled;
        if (cancelled && states[index] == RowState::Running) {
            if (index >= plan.packs.size()) {
                outcome.stoppedDownload = true;
            } else {
                const ReadinessPack &pack = plan.packs.at(index);
                outcome.stoppedInstall = pack.installs();
                outcome.stoppedSetup = pack.hasEnvironment();
            }
        }
        outcome.errors += errors;
        const QString detail = errors.isEmpty() ? QString() : errors.first().message;
        finish(index, cancelled ? RowState::Skipped : RowState::Failed, detail);
    };

    QStringList installedFolders; // the packs this run put in place, to read the types they declare
    TrustStore fallbackTrust;
    TrustStore &trust = run.trust ? *run.trust : fallbackTrust;
    for (int i = 0; i < plan.packs.size() && outcome.failedRow < 0; ++i) {
        if (report.stopped()) {
            stopAt(i, {}, true);
            break;
        }
        const ReadinessPack &row = plan.packs.at(i);
        states[i] = RowState::Running;
        report.row(i, RowState::Running);
        bool refused = false;
        PackSummary shown = row.pack; // what the yes covers for this row
        std::shared_ptr<PreparedPack> prepared = row.prepared;
        QString folderName = row.folderName;
        PackPrompts prompts = approvals(shown, run, refused);
        if (row.installs() && row.pending) {
            // A git pack: only now fetched, over https only, and no bigger than a pack may be.
            if (QUrl(row.pack.source.location).scheme() != QLatin1String("https")) {
                stopAt(i, Errors{makeError(QStringLiteral("pack_invalid"),
                                           QStringLiteral("The pack %1 comes from %2: a workflow or package may only name an https address.").arg(row.pack.id, row.pack.source.location),
                                           QStringLiteral("Install it yourself with `fugue pack add`, which asks first."))}, false);
                break;
            }
            PackPrompts fetching = prompts;
            fetching.maxCloneBytes = 200000000;
            fetching.fetch = run.fetch;
            const Result<std::shared_ptr<PreparedPack>> got = preparePack(row.pack.source, run.packsDir, fetching);
            if (!got) {
                stopAt(i, got.errors(), report.stopped());
                break;
            }
            prepared = *got;
            shown = prepared->summary;
            const std::optional<Version> wantedVersion = Version::parse(row.pack.version);
            const std::optional<Version> gotVersion = Version::parse(shown.version);
            if (shown.id != row.pack.id || (wantedVersion && gotVersion && gotVersion->major != wantedVersion->major)) {
                stopAt(i, Errors{makeError(QStringLiteral("pack_invalid"),
                                           QStringLiteral("%1 holds the pack %2 %3, not the pack %4 %5 the workflow names, so it was not installed.")
                                               .arg(row.pack.source.location, shown.id, shown.version, row.pack.id, row.pack.version))}, false);
                break;
            }
            // Nothing about its permissions could be shown before the download: more than nothing is asked, for this row alone.
            QStringList more = shown.permissions;
            if (!shown.setupCommand.isEmpty()) // its setup was not shown either: it is asked too, never run on the list's yes
                more << QStringLiteral("its setup (%1)").arg(displayCommand(shown.setupCommand));
            if (!shown.secrets.isEmpty())
                more << QStringLiteral("secrets (%1)").arg(shown.secrets.join(QStringLiteral(", ")));
            if (!more.isEmpty() && !(run.allowMore && run.allowMore(shown, more))) {
                finish(i, RowState::Skipped, QStringLiteral("skipped: it declares %1").arg(more.join(QStringLiteral(", "))));
                continue;
            }
            prompts = approvals(shown, run, refused);
        }
        if (row.installs()) {
            const Result<PackAdded> added = installPrepared(prepared, run.packsDir, trust, prompts);
            if (!added) {
                Errors errors = added.errors();
                if (refused)
                    errors.prepend(makeError(QStringLiteral("pack_invalid"),
                                             QStringLiteral("The pack %1 is not what the list showed (its version, setup command or permissions changed), so it was not installed.").arg(row.pack.id),
                                             QStringLiteral("Get ready again to see the pack as it is now.")));
                const bool cancelled = report.stopped() && !errors.isEmpty() && errors.first().code != QLatin1String("setup_failed");
                stopAt(i, errors, cancelled);
                break;
            }
            outcome.packsChanged = true;
            folderName = QFileInfo(added->pack.folder).fileName();
            installedFolders << run.packsDir + QLatin1Char('/') + folderName;
        }
        // An installed pack whose setup never ran: the same yes runs it (the install path already did, 6.6).
        if (!row.installs() && row.setupNeeded) {
            const Result<PackSummary> done = setUpPack(folderName, run.packsDir, prompts);
            if (!done) {
                Errors errors = done.errors();
                if (refused)
                    errors.prepend(makeError(QStringLiteral("pack_invalid"),
                                             QStringLiteral("The pack %1 is not what the list showed, so its setup was not run.").arg(row.pack.id)));
                stopAt(i, errors, report.stopped() && !errors.isEmpty() && errors.first().code != QLatin1String("setup_failed"));
                break;
            }
            outcome.packsChanged = true;
        }
        if (row.action == ReadinessPack::Action::Setup) {
            ++outcome.done;
            finish(i, RowState::Done);
            continue;
        }
        // The same yes trusts it (6.7): trust is per id and version, so a new version asks again.
        const Result<PackSummary> trusted = trustPack(folderName, run.packsDir, trust, prompts);
        if (!trusted) {
            Errors errors = trusted.errors();
            if (refused)
                errors.prepend(makeError(QStringLiteral("pack_invalid"),
                                         QStringLiteral("The pack %1 is not what the list showed, so it was not trusted.").arg(row.pack.id)));
            if (report.stopped() && !refused) {
                // A stop at the trust step: the install, with its setup, is finished and stays, untrusted. Running it again asks for the trust.
                stopAt(i, Errors{makeError(QStringLiteral("cancelled"), QStringLiteral("Stopped: the pack %1 is installed but not trusted.").arg(row.pack.id),
                                           QStringLiteral("Get ready again to trust it."))}, true);
                outcome.packsChanged = true;
                outcome.stoppedInstall = outcome.stoppedSetup = false; // the install finished and stays
                break;
            }
            if (row.installs()) {
                // Installed but not trusted is a half state: undo the install, and say so.
                PackPrompts removing;
                removing.remove = [](const PackSummary &) { return Answer::Yes; };
                const Result<PackSummary> removed = removePack(folderName, run.packsDir, trust, removing);
                errors.prepend(makeError(QStringLiteral("pack_invalid"),
                                         removed ? QStringLiteral("The pack %1 could not be trusted, so its install was undone.").arg(row.pack.id)
                                                 : QStringLiteral("The pack %1 was installed but could not be trusted, and removing it failed: it is in %2 and not trusted.").arg(row.pack.id, run.packsDir)));
            }
            stopAt(i, errors, false);
            outcome.packsChanged = true;
            break;
        }
        outcome.packsChanged = true;
        ++outcome.done;
        finish(i, RowState::Done);
    }

    if (outcome.failedRow < 0 && !plan.models.isEmpty() && !report.stopped()) {
        QNetworkAccessManager network;
        FileHashes fallbackHashes(FileHashes::defaultPath());
        FileHashes &hashes = run.hashes ? *run.hashes : fallbackHashes;
        const int base = int(plan.packs.size());
        QList<ReadinessModel> models = plan.models;
        // Models whose type a pack of this plan gives: planned now that the packs are in, with the types they declare (#418).
        Registry installed;
        std::optional<ModelLibrary> after;
        for (int m = 0; m < models.size() && outcome.failedRow < 0; ++m) {
            ReadinessModel &model = models[m];
            if (!model.deferred)
                continue;
            if (!after) {
                if (!plan.registry || !plan.library) {
                    stopAt(base + m, Errors{makeError(QStringLiteral("pack_invalid"), QStringLiteral("The model %1 could not be planned after its pack was installed.").arg(model.name))}, false);
                    break;
                }
                installed = *plan.registry;
                for (const QString &folder : std::as_const(installedFolders))
                    loadPack(installed, folder);
                after = plan.library->withRegistry(&installed);
            }
            const QString packText = model.typeFrom.isEmpty() ? QStringLiteral("a pack in this list") : model.typeFrom;
            const std::optional<PortType> type = PortType::parse(model.resource);
            if (!type || after->downloadFolder(*type).isEmpty()) {
                stopAt(base + m, Errors{makeError(QStringLiteral("pack_invalid"),
                                                  QStringLiteral("%1 was installed, but it does not declare the model type %2 that %3 needs, so the model cannot be downloaded.")
                                                      .arg(packText, model.resource, model.name),
                                                  QStringLiteral("The workflow and the pack do not match: get a version of the pack that declares %1.").arg(model.resource))}, false);
                break;
            }
            FileHashes lookup(FileHashes::defaultPath());
            FileHashes &known = run.hashes ? *run.hashes : lookup;
            const QList<MissingModel> still = after->missing(QJsonArray{model.entry}, known);
            if (still.isEmpty()) { // it is here already, under the type the pack gave it
                model.deferred = false;
                model.target = {};
                continue;
            }
            const Result<DownloadTarget> target = planDownload(*after, still.first());
            if (!target) {
                stopAt(base + m, target.errors(), false);
                break;
            }
            model.target = *target;
            model.folder = QFileInfo(target->path).absolutePath();
            model.deferred = false;
        }
        // One file after another, each its own queue: the row is the loop's, never a position in a de-duplicated line.
        for (int m = 0; m < plan.models.size() && outcome.failedRow < 0; ++m) {
            const ReadinessModel &model = models.at(m);
            const int row = base + m;
            if (report.stopped()) {
                stopAt(row, {}, true);
                break;
            }
            if (model.target.path.isEmpty()) { // found here once its pack was in
                ++outcome.done;
                finish(row, RowState::Done);
                continue;
            }
            // Free space is checked before each file, not only when the list was shown.
            const qint64 free = run.freeSpace ? run.freeSpace(model.folder) : freeAt(model.folder);
            if (model.size >= 0 && free >= 0 && model.size > free) {
                stopAt(row, Errors{makeError(QStringLiteral("download_failed"),
                                             QStringLiteral("There is not enough free space for %1 in %2 (%3 needed, %4 free).")
                                                 .arg(model.name, model.folder, sizeText(model.size), sizeText(free)),
                                             QStringLiteral("Choose another folder for the models and get ready again."))}, false);
                break;
            }
            DownloadQueue queue(&network, &hashes);
            queue.setRetries(run.retries, run.retryDelayMs);
            QEventLoop loop;
            Errors failed;
            bool arrived = false;
            QObject::connect(&queue, &DownloadQueue::started, &loop, [&](int) { states[row] = RowState::Running; report.row(row, RowState::Running); });
            // The queue says so for every chunk the network hands over (thousands a second on a fast line): the
            // observer hears it at most every kProgressMs, and once at the end.
            QElapsedTimer told;
            QObject::connect(&queue, &DownloadQueue::progress, &loop, [&](int, qint64 got, qint64 all) {
                if (!run.onProgress || (told.isValid() && told.elapsed() < kProgressMs && got != all))
                    return;
                told.start();
                run.onProgress(row, got, all);
            });
            QObject::connect(&queue, &DownloadQueue::fileFinished, &loop, [&](int, const Errors &errors) {
                failed = errors;
                arrived = errors.isEmpty();
            });
            QObject::connect(&queue, &DownloadQueue::finished, &loop, &QEventLoop::quit);
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (report.stopped())
                    queue.cancel();
            });
            poll.start(100);
            queue.add({model.target});
            if (queue.busy())
                loop.exec();
            if (arrived) {
                ++outcome.done;
                finish(row, RowState::Done);
            } else {
                stopAt(row, failed, report.stopped());
            }
        }
    }

    for (int i = 0; i < total; ++i)
        if (states[i] == RowState::Waiting) {
            states[i] = RowState::Skipped;
            report.row(i, RowState::Skipped);
        }
    for (int i = 0; i < total; ++i)
        if (states[i] != RowState::Done)
            outcome.retry << rowName(plan, i);
    outcome.ok = outcome.failedRow < 0 && outcome.done == total;
    return outcome;
}

} // namespace fugue
