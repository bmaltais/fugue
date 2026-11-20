#include "engine/pack_manager.h"

#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace fugue {

namespace {

const QString sourceFile = QStringLiteral(".fugue-source.json");

Error fail(const QString &code, const QString &message, const QString &hint = {}) {
    return makeError(code, message, hint);
}

Error needsHuman(const QString &what) {
    return fail(QStringLiteral("needs_confirmation"), QStringLiteral("%1 needs a person to agree, and there is no terminal to ask on.").arg(what),
                QStringLiteral("Run the same command in a terminal; no flag answers for the person."));
}

// Asks; null callback, or NoHuman, is needs_confirmation, and No is the caller's decision.
Result<bool> ask(const std::function<Answer(const PackSummary &)> &prompt, const PackSummary &pack, const QString &what) {
    const Answer answer = prompt ? prompt(pack) : Answer::NoHuman;
    if (answer == Answer::NoHuman)
        return needsHuman(what);
    return answer == Answer::Yes;
}

bool looksLikeGitUrl(const QString &text) {
    static const QRegularExpression scp(QStringLiteral("^[A-Za-z0-9._-]+@[A-Za-z0-9.-]+:[^ ]+$"));
    for (const char *scheme : {"https://", "ssh://", "git://", "file://"})
        if (text.startsWith(QLatin1String(scheme)))
            return true;
    return scp.match(text).hasMatch();
}

// Safe as a folder name; whether it is a good pack id is the loader's check (summarizePack).
bool validPackId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9.-]*$"));
    return id.size() <= 128 && pattern.match(id).hasMatch() && !id.contains(QStringLiteral(".."));
}

struct ProcessResult {
    bool ok = false;
    int exitCode = -1;
    QString output; // stdout and stderr together, last 4 KB
};

// Runs a program (never a shell) and passes its output on line by line.
ProcessResult runProgram(const QString &program, const QStringList &arguments, const QString &workDir,
                         const std::function<void(const QString &)> &log, bool quiet = false) {
    ProcessResult result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty())
        process.setWorkingDirectory(workDir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(env);
    QByteArray pending;
    const auto flush = [&](bool all) {
        for (;;) {
            const int newline = pending.indexOf('\n');
            if (newline < 0)
                break;
            const QString line = QString::fromUtf8(pending.left(newline)).trimmed();
            pending.remove(0, newline + 1);
            result.output += line + QLatin1Char('\n');
            if (log && !quiet)
                log(line);
        }
        if (all && !pending.isEmpty()) {
            const QString line = QString::fromUtf8(pending).trimmed();
            result.output += line + QLatin1Char('\n');
            if (log && !quiet)
                log(line);
            pending.clear();
        }
    };
    QObject::connect(&process, &QProcess::readyRead, &process, [&] {
        pending += process.readAll();
        flush(false);
    });
    process.start(program, arguments);
    if (!process.waitForStarted(10000))
        return result;
    process.closeWriteChannel();
    process.waitForFinished(-1);
    pending += process.readAll();
    flush(true);
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.ok = result.exitCode == 0;
    if (result.output.size() > 4096)
        result.output = result.output.right(4096);
    return result;
}

Result<QJsonObject> readPackJson(const QString &folder) {
    const Result<QJsonObject> read = readJsonObject(folder + QStringLiteral("/pack.json"));
    if (!read)
        return Error(fail(QStringLiteral("pack_invalid"), QStringLiteral("%1 has no readable pack.json.").arg(folder),
                          read.error().message));
    return read.value();
}

QStringList setupCommandOf(const QJsonObject &pack) {
    QStringList command;
    for (const QJsonValue &part : pack.value("setup").toObject().value("command").toArray())
        command << part.toString();
    return command;
}

QString realPath(const QString &path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical;
}

// A fresh folder in packsDir for staging; the leading dot keeps the loader away from it.
QString makeStaging(const QString &packsDir) {
    QDir().mkpath(packsDir);
    const QString staging = packsDir + QStringLiteral("/.staging-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    return QDir().mkpath(staging) ? staging : QString();
}

bool copyTree(const QString &from, const QString &to, QString *problem) {
    QDir().mkpath(to);
    QDirIterator it(from, QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString target = to + QLatin1Char('/') + info.fileName();
        if (info.fileName() == QLatin1String(".git"))
            continue; // history is not part of a pack
        if (info.isSymLink()) {
            // A link may point anywhere, so it is copied as the link it is: the loader refuses
            // symbolic node folders, and a node program reaching outside is a trust matter.
            if (!QFile::link(QFile::symLinkTarget(info.filePath()), target)) {
                *problem = QStringLiteral("cannot copy the link %1").arg(info.filePath());
                return false;
            }
        } else if (info.isDir()) {
            if (!copyTree(info.filePath(), target, problem))
                return false;
        } else if (!QFile::copy(info.filePath(), target)) {
            *problem = QStringLiteral("cannot copy %1").arg(info.filePath());
            return false;
        }
    }
    return true;
}

void writeSource(const QString &folder, const PackSource &source) {
    QJsonObject object{{"kind", source.kind == PackSource::Kind::Git ? "git" : "path"}, {"location", source.location}};
    if (source.kind == PackSource::Kind::Git) {
        object.insert("ref", source.ref);
        object.insert("commit", source.commit);
    }
    writeJsonFile(folder + QLatin1Char('/') + sourceFile, object);
}

// Brings the pack at `source` into a folder named after its id under a staging folder.
// Returns the staged pack folder; the staging folder is the caller's to delete.
struct Staged {
    QString staging;
    QString folder;
    PackSource source;
};

Result<Staged> stage(const PackSource &wanted, const QString &packsDir, const std::function<void(const QString &)> &log) {
    Staged staged;
    staged.staging = makeStaging(packsDir);
    if (staged.staging.isEmpty())
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot create a folder in %1.").arg(packsDir));
    const auto cleanFail = [&](const Error &error) -> Result<Staged> {
        QDir(staged.staging).removeRecursively();
        return error;
    };
    staged.source = wanted;
    const QString work = staged.staging + QStringLiteral("/work");
    if (wanted.kind == PackSource::Kind::Path) {
        QString problem;
        if (!copyTree(wanted.location, work, &problem))
            return cleanFail(fail(QStringLiteral("permission_denied"), QStringLiteral("Copying the pack failed: %1.").arg(problem)));
    } else {
        // `--` ends the options; the URL was checked against a list of schemes, and `ext::` is refused by git itself.
        const ProcessResult cloned = runProgram(QStringLiteral("git"),
            {QStringLiteral("-c"), QStringLiteral("protocol.ext.allow=never"), QStringLiteral("clone"), QStringLiteral("--quiet"),
             QStringLiteral("--no-checkout"), QStringLiteral("--"), wanted.location, work}, {}, log);
        if (!cloned.ok)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("git could not clone %1.").arg(wanted.location),
                                  cloned.output.trimmed().isEmpty() ? QStringLiteral("Is git installed, and is the address right?") : cloned.output.trimmed()));
        const ProcessResult checkedOut = runProgram(QStringLiteral("git"),
            {QStringLiteral("-c"), QStringLiteral("advice.detachedHead=false"), QStringLiteral("checkout"), QStringLiteral("--quiet"),
             QStringLiteral("--detach"), wanted.ref + QStringLiteral("^{commit}"), QStringLiteral("--")}, work, log);
        if (!checkedOut.ok)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 has no tag or commit \"%2\".").arg(wanted.location, wanted.ref),
                                  QStringLiteral("Pin the pack to a tag or a commit that exists.")));
        const ProcessResult head = runProgram(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, work, nullptr, true);
        staged.source.commit = head.output.trimmed();
        QDir(work + QStringLiteral("/.git")).removeRecursively();
    }
    // The pack is `work`, to be named after its id.
    const Result<QJsonObject> json = readPackJson(work);
    if (!json)
        return cleanFail(json.error());
    const QString id = json->value("id").toString();
    if (!validPackId(id))
        return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("pack.json has no usable \"id\" (%1).").arg(id)));
    if (id == QLatin1String("fugue") || id == QLatin1String("local"))
        return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack id \"%1\" belongs to Fugue.").arg(id)));
    staged.folder = staged.staging + QLatin1Char('/') + id;
    if (!QDir().rename(work, staged.folder))
        return cleanFail(fail(QStringLiteral("internal"), QStringLiteral("Cannot prepare the pack folder.")));
    return staged;
}

} // namespace

QString displayCommand(const QStringList &command) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    QStringList parts;
    for (const QString &part : command)
        parts << (plain.match(part).hasMatch() ? part : QStringLiteral("'") + QString(part).replace('\'', QStringLiteral("'\\''")) + QStringLiteral("'"));
    return parts.join(QLatin1Char(' '));
}

PackSource readPackSource(const QString &packFolder) {
    PackSource source;
    const Result<QJsonObject> read = readJsonObject(packFolder + QLatin1Char('/') + sourceFile);
    if (!read)
        return source;
    source.kind = read->value("kind").toString() == QLatin1String("git") ? PackSource::Kind::Git : PackSource::Kind::Path;
    source.location = read->value("location").toString();
    source.ref = read->value("ref").toString();
    source.commit = read->value("commit").toString();
    return source;
}

Result<PackSummary> summarizePack(const QString &folder) {
    const Result<QJsonObject> json = readPackJson(folder);
    if (!json)
        return json.error();
    Registry scratch;
    const LoadedPack loaded = loadPack(scratch, folder);
    if (!loaded.enabled) {
        const QString why = loaded.errors.isEmpty() ? QStringLiteral("it did not load") : loaded.errors.first().message;
        return fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack is not valid: %1").arg(why));
    }
    PackSummary summary;
    summary.id = loaded.id;
    summary.version = loaded.version;
    summary.name = loaded.name;
    summary.folder = folder;
    summary.nodes = loaded.nodes;
    QSet<QString> kinds, permissions;
    for (const QString &id : loaded.nodes) {
        const NodeType *type = scratch.nodeType(id);
        if (!type)
            continue;
        kinds.insert(type->runtime.value("kind").toString());
        if (type->needs.contains("gpu"))
            permissions.insert(QStringLiteral("gpu"));
        for (const char *key : {"network", "files"})
            if (type->needs.value(QLatin1String(key)).toBool())
                permissions.insert(QLatin1String(key));
    }
    summary.runtimeKinds = QStringList(kinds.begin(), kinds.end());
    summary.runtimeKinds.sort();
    summary.permissions = QStringList(permissions.begin(), permissions.end());
    summary.permissions.sort();
    summary.setupCommand = setupCommandOf(*json);
    summary.setupDescription = json->value("setup").toObject().value("description").toString();
    summary.source = readPackSource(folder);
    return summary;
}

namespace {

// Runs the pack's setup after the person agreed; folder is the installed pack.
Result<bool> maybeSetup(PackAdded &added, const PackPrompts &prompts) {
    if (added.pack.setupCommand.isEmpty())
        return true;
    const Result<bool> yes = ask(prompts.runSetup, added.pack, QStringLiteral("Running a pack's setup"));
    if (!yes)
        return yes.error();
    if (!*yes) {
        added.setupSkipped = true;
        return true;
    }
    const QStringList &command = added.pack.setupCommand;
    const ProcessResult done = runProgram(command.first(), command.mid(1), added.pack.folder, prompts.log);
    if (!done.ok)
        return fail(QStringLiteral("setup_failed"),
                    QStringLiteral("The setup of %1 failed (%2).").arg(added.pack.id, displayCommand(command)),
                    done.output.trimmed());
    added.setupRan = true;
    return true;
}

// Installs a staged pack into packsDir, replacing `replace` (an installed folder) when given.
Result<PackAdded> install(const Staged &staged, PackSummary summary, const QString &packsDir, const PackPrompts &prompts,
                          bool replacing) {
    const QString target = packsDir + QLatin1Char('/') + summary.id;
    QString backup;
    if (replacing) {
        backup = packsDir + QStringLiteral("/.old-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
        if (!QDir().rename(target, backup))
            return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot replace %1.").arg(target));
    }
    writeSource(staged.folder, staged.source);
    if (!QDir().rename(staged.folder, target)) {
        if (replacing)
            QDir().rename(backup, target);
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot move the pack into %1.").arg(packsDir));
    }
    if (replacing)
        QDir(backup).removeRecursively();
    summary.folder = target;
    summary.source = staged.source;
    PackAdded added;
    added.pack = summary;
    const Result<bool> setup = maybeSetup(added, prompts);
    if (!setup)
        return Errors{setup.error()};
    return added;
}

} // namespace

Result<PackAdded> addPack(const QString &location, const QString &ref, const QString &packsDir, const PackPrompts &prompts) {
    PackSource source;
    if (QFileInfo(location).isDir()) {
        if (!ref.isEmpty())
            return fail(QStringLiteral("usage"), QStringLiteral("--ref is for git addresses; %1 is a folder.").arg(location));
        source.kind = PackSource::Kind::Path;
        source.location = realPath(location);
    } else if (looksLikeGitUrl(location)) {
        if (ref.isEmpty())
            return fail(QStringLiteral("usage"), QStringLiteral("A git pack must be pinned to a tag or a commit."),
                        QStringLiteral("Add --ref <tag|commit>."));
        if (ref.startsWith('-'))
            return fail(QStringLiteral("usage"), QStringLiteral("\"%1\" is not a tag or a commit.").arg(ref));
        source.kind = PackSource::Kind::Git;
        source.location = location;
        source.ref = ref;
    } else {
        return fail(QStringLiteral("file_not_found"), QStringLiteral("%1 is neither a folder nor a git address.").arg(location),
                    QStringLiteral("Use a folder, or https://, ssh://, git:// or file:// with --ref."));
    }

    Result<Staged> staged = stage(source, packsDir, prompts.log);
    if (!staged)
        return staged.errors();
    const auto done = [&](Result<PackAdded> result) {
        QDir(staged->staging).removeRecursively();
        return result;
    };
    Result<PackSummary> summary = summarizePack(staged->folder);
    if (!summary)
        return done(summary.errors());
    summary->source = staged->source;
    if (QFileInfo::exists(packsDir + QLatin1Char('/') + summary->id))
        return done(Error(fail(QStringLiteral("pack_exists"), QStringLiteral("The pack %1 is already installed.").arg(summary->id),
                               QStringLiteral("`fugue pack update %1` fetches it again; `fugue pack remove %1` removes it.").arg(summary->id))));
    const Result<bool> yes = ask(prompts.install, *summary, QStringLiteral("Installing a pack"));
    if (!yes)
        return done(yes.errors());
    if (!*yes)
        return done(Error(fail(QStringLiteral("cancelled"), QStringLiteral("The pack was not installed."))));
    return done(install(*staged, *summary, packsDir, prompts, false));
}

Result<QList<PackAdded>> updatePacks(const QString &id, const QString &ref, const QString &packsDir, const PackPrompts &prompts) {
    QStringList ids;
    if (!id.isEmpty()) {
        if (!QFileInfo(packsDir + QLatin1Char('/') + id).isDir() || !validPackId(id))
            return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir));
        ids << id;
    } else {
        for (const QFileInfo &entry : QDir(packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (!entry.fileName().startsWith('.'))
                ids << entry.fileName();
    }
    QList<PackAdded> changed;
    Errors warnings;
    for (const QString &name : ids) {
        const QString folder = packsDir + QLatin1Char('/') + name;
        PackSource source = readPackSource(folder);
        if (source.location.isEmpty()) {
            if (!id.isEmpty())
                return fail(QStringLiteral("usage"), QStringLiteral("%1 was not installed by `fugue pack add`, so there is nothing to fetch it from.").arg(name));
            continue;
        }
        if (!ref.isEmpty()) {
            if (source.kind != PackSource::Kind::Git || ref.startsWith('-'))
                return fail(QStringLiteral("usage"), QStringLiteral("--ref is only for packs installed from git."));
            source.ref = ref;
        }
        const Result<PackSummary> current = summarizePack(folder);
        Result<Staged> staged = stage(source, packsDir, prompts.log);
        if (!staged) {
            if (id.isEmpty()) {
                Error warning = staged.error();
                warning.severity = Error::Severity::Warning;
                warning.message = QStringLiteral("%1: %2").arg(name, warning.message);
                warnings << warning;
                continue;
            }
            return staged.errors();
        }
        const auto cleanup = [&] { QDir(staged->staging).removeRecursively(); };
        Result<PackSummary> next = summarizePack(staged->folder);
        if (!next) {
            cleanup();
            return next.errors();
        }
        if (next->id != name) {
            cleanup();
            return fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack now says its id is %1, not %2.").arg(next->id, name));
        }
        next->source = staged->source;
        const bool same = current && current->version == next->version && source.commit == staged->source.commit
                          && source.kind == staged->source.kind && source.ref == staged->source.ref
                          && (source.kind == PackSource::Kind::Git || QFileInfo(folder).exists());
        // A folder source has no commit: the same version is "no change" only for git.
        if (same && source.kind == PackSource::Kind::Git) {
            cleanup();
            continue;
        }
        next->oldVersion = current ? current->version : QString();
        const Result<bool> yes = ask(prompts.install, *next, QStringLiteral("Updating a pack"));
        if (!yes) {
            cleanup();
            return yes.errors();
        }
        if (!*yes) {
            cleanup();
            continue;
        }
        Result<PackAdded> added = install(*staged, *next, packsDir, prompts, true);
        cleanup();
        if (!added)
            return added.errors();
        changed << *added;
    }
    return Result<QList<PackAdded>>(changed, warnings);
}

Result<PackSummary> removePack(const QString &id, const QString &packsDir, TrustStore &trust, const PackPrompts &prompts) {
    const QString folder = packsDir + QLatin1Char('/') + id;
    if (!validPackId(id) || !QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir),
                    QStringLiteral("Only packs in that folder are removed; packs found through settings are the user's own folders."));
    PackSummary summary;
    const Result<PackSummary> read = summarizePack(folder);
    if (read) {
        summary = *read;
    } else {
        summary.id = id; // a broken pack can still be removed
        summary.folder = folder;
        summary.name = id;
    }
    const Result<bool> yes = ask(prompts.remove, summary, QStringLiteral("Removing a pack"));
    if (!yes)
        return yes.error();
    if (!*yes)
        return fail(QStringLiteral("cancelled"), QStringLiteral("The pack was not removed."));
    if (!QDir(folder).removeRecursively())
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot remove %1 completely.").arg(folder));
    trust.forget(id);
    return summary;
}

Result<PackSummary> trustPack(const QString &id, const QString &packsDir, TrustStore &trust, const PackPrompts &prompts) {
    if (TrustStore::isBuiltIn(id))
        return fail(QStringLiteral("usage"), QStringLiteral("%1 is part of Fugue and is always trusted.").arg(id));
    const QString folder = packsDir + QLatin1Char('/') + id;
    if (!validPackId(id) || !QFileInfo(folder).isDir())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir));
    const Result<PackSummary> summary = summarizePack(folder);
    if (!summary)
        return summary.errors();
    if (trust.isTrusted(summary->id, summary->version))
        return *summary;
    const Result<bool> yes = ask(prompts.trust, *summary, QStringLiteral("Trusting a pack"));
    if (!yes)
        return yes.error();
    if (!*yes)
        return fail(QStringLiteral("cancelled"), QStringLiteral("The pack was not trusted."));
    const Errors errors = trust.trust(summary->id, summary->version);
    if (hasErrors(errors))
        return errors;
    return *summary;
}

} // namespace fugue
