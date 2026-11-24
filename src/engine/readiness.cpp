#include "engine/readiness.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#include "engine/document.h"
#include "engine/file_hashes.h"
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

// What the person read is what runs: the same pack, version, command and permissions.
bool sameCommitment(const PackSummary &shown, const PackSummary &now) {
    return shown.id == now.id && shown.version == now.version && shown.setupCommand == now.setupCommand
           && shown.permissions == now.permissions && shown.runtimeKinds == now.runtimeKinds && shown.nodes == now.nodes
           && (shown.source.commit.isEmpty() || shown.source.commit == now.source.commit);
}

QString sourceText(const PackSource &source) {
    if (source.location.isEmpty())
        return {};
    return source.ref.isEmpty() ? source.location : QStringLiteral("%1 at %2").arg(source.location, source.ref);
}

QString packName(const PackSummary &pack) { return pack.name.isEmpty() ? pack.id : pack.name; }

} // namespace

ReadinessRequest requestFor(const Document &document, const Registry &registry, const QList<CarriedPack> &carried) {
    ReadinessRequest request;
    request.models = document.models();
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
            want.hasSource = true;
            const bool folder = QFileInfo(source).isDir();
            want.source = PackSource{folder ? PackSource::Kind::Path : PackSource::Kind::Git, source,
                                     folder ? QString() : entry.value(QStringLiteral("ref")).toString(), {}};
        }
        request.packs.append(want);
    }
    return request;
}

int ReadinessPlan::installCount() const {
    return int(std::count_if(packs.cbegin(), packs.cend(), [](const ReadinessPack &p) { return p.installs(); }));
}
int ReadinessPlan::trustOnlyCount() const { return int(packs.size()) - installCount(); }
int ReadinessPlan::environmentCount() const {
    return int(std::count_if(packs.cbegin(), packs.cend(), [](const ReadinessPack &p) { return p.hasEnvironment(); }));
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
        out << QStringLiteral("  %1 %2 %3 (%4)").arg(row.installs() ? QStringLiteral("Install and trust pack:") : QStringLiteral("Trust pack:"),
                                                  packName(p), p.version, p.id);
        if (const QString from = sourceText(p.source); row.installs() && !from.isEmpty())
            out << QStringLiteral("    From:      %1").arg(from);
        out << QStringLiteral("    Runs as:   %1").arg(p.runtimeKinds.isEmpty() ? QStringLiteral("(no nodes)") : p.runtimeKinds.join(QStringLiteral(", ")));
        out << QStringLiteral("    Declares:  %1").arg(p.permissions.isEmpty() ? QStringLiteral("nothing (network, files, GPU)") : p.permissions.join(QStringLiteral(", ")));
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
        out << QStringLiteral("    Into:      %1").arg(m.folder);
    }
    if (!models.isEmpty()) {
        out << QStringLiteral("  Models go into %1%2").arg(destination, freeBytes >= 0 ? QStringLiteral(" (%1 free)").arg(sizeText(freeBytes)) : QString());
        if (lowSpace())
            out << QStringLiteral("  There is not enough free space there for the download; change Settings > Folders > Download models into.");
    }
    for (const Error &p : problems)
        out << QStringLiteral("  Cannot do: %1").arg(p.message);
    return out;
}

QJsonObject ReadinessPlan::toJson() const {
    QJsonArray packRows, modelRows;
    for (const ReadinessPack &row : packs) {
        const PackSummary &p = row.pack;
        QJsonObject o{{"action", row.installs() ? "install" : "trust"}, {"id", p.id}, {"version", p.version}, {"name", p.name},
                      {"runtime", QJsonArray::fromStringList(p.runtimeKinds)}, {"permissions", QJsonArray::fromStringList(p.permissions)}};
        if (const QString from = sourceText(p.source); !from.isEmpty())
            o.insert("source", from);
        if (row.hasEnvironment())
            o.insert("setup", QJsonObject{{"command", QJsonArray::fromStringList(p.setupCommand)}, {"description", p.setupDescription},
                                          {"size", p.setupSize >= 0 ? QJsonValue(double(p.setupSize)) : QJsonValue()}});
        packRows.append(o);
    }
    for (const ReadinessModel &m : models)
        modelRows.append(QJsonObject{{"name", m.name}, {"resource", m.resource}, {"license", m.license}, {"host", m.host},
                                     {"size", m.size >= 0 ? QJsonValue(double(m.size)) : QJsonValue()}, {"folder", m.folder}, {"path", m.target.path}});
    return QJsonObject{{"headline", headline()}, {"packs", packRows}, {"models", modelRows}, {"destination", destination},
                       {"download_bytes", double(downloadBytes)}, {"unknown_sizes", unknownSizes},
                       {"environment_bytes", double(environmentBytes)}, {"free_bytes", double(freeBytes)},
                       {"low_space", lowSpace()}, {"problems", fugue::toJson(problems)}};
}

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
            const Result<PackSummary> summary = previewPack(want.source, env.packsDir, prompts);
            if (!summary) {
                Error e = summary.error();
                e.severity = Error::Severity::Warning;
                plan.problems << e;
                continue;
            }
            ReadinessPack row;
            row.pack = *summary;
            plan.packs.append(row);
            continue;
        }
        // Installed here: only its trust may be missing, for a pack in the packs folder.
        if (!installedHere || !env.trust || TrustStore::isBuiltIn(want.id) || env.trust->isTrusted(want.id, want.version))
            continue;
        for (const QFileInfo &entry : QDir(env.packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.fileName().startsWith(QLatin1Char('.')) || parsePackRef(entry.fileName()).id != want.id)
                continue;
            const Result<PackSummary> summary = summarizePack(entry.filePath());
            if (!summary || summary->version != want.version)
                continue;
            ReadinessPack row;
            row.action = ReadinessPack::Action::Trust;
            row.pack = *summary;
            row.folderName = entry.fileName();
            plan.packs.append(row);
            break;
        }
    }
    for (const ReadinessPack &row : std::as_const(plan.packs)) {
        if (!row.hasEnvironment())
            continue;
        if (row.pack.setupSize >= 0)
            plan.environmentBytes += row.pack.setupSize;
        else
            ++plan.unknownEnvironments;
    }

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
    for (const MissingModel &model : library.missing(request.models, hashes)) {
        const QString name = model.handle.value(QStringLiteral("name")).toString();
        if (model.source.isEmpty()) {
            plan.problems << problem(QStringLiteral("model_missing"), QStringLiteral("The model %1 has no source to download from.").arg(name),
                                     QStringLiteral("Put it in %1.").arg(model.folder));
            continue;
        }
        const Result<DownloadTarget> target = planDownload(library, model);
        if (!target) {
            for (Error e : target.errors()) {
                e.severity = Error::Severity::Warning;
                plan.problems << e;
            }
            continue;
        }
        ReadinessModel row;
        row.name = name;
        row.resource = model.handle.value(QStringLiteral("resource")).toString();
        row.host = target->source.host();
        row.size = model.size;
        row.target = *target;
        row.folder = QFileInfo(target->path).absolutePath();
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
        if (row.size >= 0)
            plan.downloadBytes += row.size;
        else
            ++plan.unknownSizes;
        plan.models.append(row);
    }
    if (!plan.models.isEmpty())
        plan.freeBytes = env.freeSpace ? env.freeSpace(plan.destination) : freeAt(plan.destination);
    return plan;
}

namespace {

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
        if (sameCommitment(shown, now))
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
        return QStringLiteral("%1 %2").arg(row.installs() ? QStringLiteral("install pack") : QStringLiteral("trust pack"), row.pack.id);
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
        outcome.errors += errors;
        const QString detail = errors.isEmpty() ? QString() : errors.first().message;
        finish(index, cancelled ? RowState::Skipped : RowState::Failed, detail);
    };

    TrustStore fallbackTrust;
    TrustStore &trust = run.trust ? *run.trust : fallbackTrust;
    for (int i = 0; i < plan.packs.size() && outcome.failedRow < 0; ++i) {
        if (report.stopped()) {
            stopAt(i, {}, true);
            break;
        }
        const ReadinessPack &row = plan.packs.at(i);
        report.row(i, RowState::Running);
        bool refused = false;
        const PackPrompts prompts = approvals(row.pack, run, refused);
        QString folderName = row.folderName;
        if (row.installs()) {
            const Result<PackAdded> added = addPack(row.pack.source.location, row.pack.source.ref, run.packsDir, trust, prompts);
            if (!added) {
                Errors errors = added.errors();
                if (refused)
                    errors.prepend(makeError(QStringLiteral("pack_changed"),
                                             QStringLiteral("The pack %1 is not what the list showed (its version, setup command or permissions changed), so it was not installed.").arg(row.pack.id),
                                             QStringLiteral("Get ready again to see the pack as it is now.")));
                const bool cancelled = report.stopped() && !errors.isEmpty() && errors.first().code != QLatin1String("setup_failed");
                stopAt(i, errors, cancelled);
                break;
            }
            outcome.packsChanged = true;
            folderName = QFileInfo(added->pack.folder).fileName();
        }
        // The same yes trusts it (6.7): trust is per id and version, so a new version asks again.
        const Result<PackSummary> trusted = trustPack(folderName, run.packsDir, trust, prompts);
        if (!trusted) {
            Errors errors = trusted.errors();
            if (refused)
                errors.prepend(makeError(QStringLiteral("pack_changed"),
                                         QStringLiteral("The pack %1 is not what the list showed, so it was not trusted.").arg(row.pack.id)));
            stopAt(i, errors, false);
            outcome.packsChanged = outcome.packsChanged || row.installs();
            break;
        }
        outcome.packsChanged = true;
        ++outcome.done;
        finish(i, RowState::Done);
    }

    if (outcome.failedRow < 0 && !plan.models.isEmpty()) {
        QNetworkAccessManager network;
        FileHashes fallbackHashes(FileHashes::defaultPath());
        FileHashes &hashes = run.hashes ? *run.hashes : fallbackHashes;
        DownloadQueue queue(&network, &hashes);
        queue.setRetries(run.retries, run.retryDelayMs);
        QEventLoop loop;
        const int base = int(plan.packs.size());
        QList<DownloadTarget> targets;
        for (const ReadinessModel &m : plan.models)
            targets << m.target;
        bool stoppedByPerson = false;
        QObject::connect(&queue, &DownloadQueue::started, &loop, [&](int index) { report.row(base + index, RowState::Running); });
        QObject::connect(&queue, &DownloadQueue::progress, &loop, [&](int index, qint64 got, qint64 all) {
            if (run.onProgress)
                run.onProgress(base + index, got, all);
        });
        QObject::connect(&queue, &DownloadQueue::fileFinished, &loop, [&](int index, const Errors &failed) {
            if (failed.isEmpty()) {
                ++outcome.done;
                finish(base + index, RowState::Done);
                return;
            }
            if (outcome.failedRow < 0) {
                outcome.failedRow = base + index;
                outcome.errors += failed;
                stoppedByPerson = report.stopped();
                outcome.cancelled = stoppedByPerson;
                finish(base + index, stoppedByPerson ? RowState::Skipped : RowState::Failed, failed.first().message);
                queue.cancel(); // all or nothing per item, and stop: the rest is not started
            }
        });
        QObject::connect(&queue, &DownloadQueue::finished, &loop, &QEventLoop::quit);
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (report.stopped())
                queue.cancel();
        });
        poll.start(100);
        queue.add(targets);
        if (queue.busy())
            loop.exec();
        // A stop between files: every file done or failed is told; the rest was dropped.
        if (outcome.failedRow < 0 && report.stopped()) {
            outcome.cancelled = true;
            outcome.failedRow = base + int(std::count(states.cbegin() + base, states.cend(), RowState::Done));
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
