#include "engine/pack_manager.h"

#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/pack_secrets.h"
#include "engine/registry.h"
#include "engine/version.h"

#include <QDateTime>
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

#include <optional>

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


// Safe as a folder name; whether it is a good pack id is the loader's check (summarizePack).
// A folder name may end in @<major> (6.6).
bool validPackId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9.-]*(@(0|[1-9][0-9]{0,8}))?$"));
    return id.size() <= 140 && pattern.match(id).hasMatch() && !id.contains(QStringLiteral(".."));
}

// The major of the version in the pack.json of a folder, or -1.
int majorOfFolder(const QString &folder) {
    const Result<QJsonObject> json = readJsonObject(folder + QStringLiteral("/pack.json"));
    const std::optional<Version> version = json ? Version::parse(json->value("version").toString()) : std::nullopt;
    return version ? version->major : -1;
}

// The folders of `places` holding a pack with this id (named `<id>` or `<id>@<major>`).
QStringList foldersOfPack(const QString &id, const QStringList &places) {
    QStringList found;
    for (const QString &place : places)
        for (const QFileInfo &entry : QDir(place).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (!entry.fileName().startsWith(QLatin1Char('.')) && parsePackRef(entry.fileName()).id == id)
                found << entry.filePath();
    return found;
}

// Text that reaches a terminal must not carry control characters: an escape sequence could
// hide or rewrite the command the person is asked about.
bool hasControl(const QString &text) {
    for (const QChar c : text)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || (c.unicode() >= 0x80 && c.unicode() < 0xa0))
            return true;
    return false;
}

// A git ref we accept as a pin: an existing tag, or a commit id. Not a branch.
bool looksLikeCommit(const QString &ref) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{7,64}$"));
    return hex.match(ref).hasMatch();
}

// Leftovers of an install that was interrupted (Ctrl-C at a prompt): old enough that no other
// running command can still be using them.
void removeStale(const QString &packsDir) {
    for (const QFileInfo &entry : QDir(packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString name = entry.fileName();
        if ((name.startsWith(QLatin1String(".staging-")) || name.startsWith(QLatin1String(".old-")))
            && entry.lastModified().secsTo(QDateTime::currentDateTime()) > 600)
            QDir(entry.filePath()).removeRecursively();
    }
}

struct ProcessResult {
    bool ok = false;
    int exitCode = -1;
    QString output; // stdout and stderr together, last 4 KB
};

// Runs a program (never a shell) and passes its output on line by line.
ProcessResult runProgram(const QString &program, const QStringList &arguments, const QString &workDir,
                         const std::function<void(const QString &)> &log, bool quiet = false,
                         const std::function<bool()> &cancelled = {}) {
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
    // A cancel kills the program: git or a setup may take minutes, and the person can stop it.
    while (process.state() != QProcess::NotRunning && !process.waitForFinished(100)) {
        if (cancelled && cancelled()) {
            process.kill();
            process.waitForFinished(3000);
            break;
        }
    }
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

// The bytes under a folder, links not followed.
qint64 treeBytes(const QString &folder) {
    qint64 total = 0;
    QDirIterator it(folder, QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().isFile() && !it.fileInfo().isSymLink())
            total += it.fileInfo().size();
    }
    return total;
}

// The first symbolic link under a folder (the .git folder aside), or empty.
QString firstSymlink(const QString &folder) {
    QDirIterator it(folder, QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().isSymLink())
            return it.filePath();
    }
    return {};
}

struct CopyBudget {
    qint64 bytes = 0;
    int files = 0;
};

bool copyTree(const QString &from, const QString &to, QString *problem, CopyBudget &budget) {
    if (!QDir().mkpath(to)) {
        *problem = QStringLiteral("cannot create %1").arg(to);
        return false;
    }
    QDirIterator it(from, QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString target = to + QLatin1Char('/') + info.fileName();
        if (info.fileName() == QLatin1String(".git"))
            continue; // history is not part of a pack
        if (++budget.files > 100000 || budget.bytes > (qint64(2) << 30)) {
            *problem = QStringLiteral("the folder is too big to be a pack");
            return false;
        }
        if (info.isSymLink()) {
            // A link could lead out of the pack, to a file the person never saw: a pack holds none.
            *problem = QStringLiteral("it holds a symbolic link (%1)").arg(info.filePath());
            return false;
        } else if (info.isDir()) {
            if (!copyTree(info.filePath(), target, problem, budget))
                return false;
        } else if (info.isFile()) {
            budget.bytes += info.size();
            if (!QFile::copy(info.filePath(), target)) {
                *problem = QStringLiteral("cannot copy %1").arg(info.filePath());
                return false;
            }
        } // anything else (a pipe, a socket, a device) is left out
    }
    return true;
}

Errors writeSource(const QString &folder, const PackSource &source) {
    const char *kind = source.kind == PackSource::Kind::Git ? "git" : source.kind == PackSource::Kind::Bundle ? "bundle" : "path";
    QJsonObject object{{"kind", kind}, {"location", source.location}};
    if (source.kind == PackSource::Kind::Git) {
        object.insert("ref", source.ref);
        object.insert("commit", source.commit);
    }
    return writeJsonFile(folder + QLatin1Char('/') + sourceFile, object);
}

// Makes the program each command node of the pack at `folder` declares (pack.json "nodes", each node.json
// runtime.entry) executable, for a pack that lost its exec bits on the way (a bundle made before #520). Only a
// regular file inside the node's own folder, never a link, and only what the manifest names.
void restoreCommandEntries(const QString &folder) {
    const Result<QJsonObject> pack = readJsonObject(folder + QStringLiteral("/pack.json"));
    if (!pack)
        return;
    const QString root = realPath(folder);
    for (const QJsonValue &node : pack->value("nodes").toArray()) {
        const QString name = node.toString();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.startsWith(QLatin1Char('.')))
            continue;
        const QString nodeFolder = folder + QLatin1Char('/') + name;
        const Result<QJsonObject> manifest = readJsonObject(nodeFolder + QStringLiteral("/node.json"));
        if (!manifest || QFileInfo(nodeFolder).isSymLink())
            continue;
        const QJsonObject runtime = manifest->value("runtime").toObject();
        const QString entry = runtime.value("entry").toString();
        if (runtime.value("kind").toString() != QLatin1String("command") || entry.isEmpty() || QDir::isAbsolutePath(entry))
            continue;
        const QString path = QDir::cleanPath(nodeFolder + QLatin1Char('/') + entry);
        const QFileInfo info(path);
        if (info.isSymLink() || !info.isFile() || !realPath(path).startsWith(root + QLatin1Char('/')))
            continue;
        QFile(path).setPermissions(info.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }
}

// Brings the pack at `source` into a folder named after its id under a staging folder.
// Returns the staged pack folder; the staging folder is the caller's to delete.
struct Staged {
    QString staging;
    QString folder;
    PackSource source;
};

Result<Staged> stage(const PackSource &wanted, const QString &packsDir, const PackPrompts &prompts) {
    const auto &log = prompts.log;
    if (prompts.cancelled && prompts.cancelled())
        return fail(QStringLiteral("cancelled"), QStringLiteral("Cancelled."));
    Staged staged;
    removeStale(packsDir);
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
        const QString from = realPath(wanted.location);
        const QString into = realPath(packsDir);
        if (!QFileInfo(from).isDir())
            return cleanFail(fail(QStringLiteral("file_not_found"), QStringLiteral("%1 is not a folder any more.").arg(wanted.location)));
        if (!QFileInfo(from + QStringLiteral("/pack.json")).isFile())
            return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("%1 has no pack.json.").arg(wanted.location)));
        if (into == from || into.startsWith(from + QLatin1Char('/')))
            return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("%1 contains the packs folder; give the pack's own folder.").arg(wanted.location)));
        QString problem;
        CopyBudget budget;
        if (!copyTree(from, work, &problem, budget))
            return cleanFail(fail(QStringLiteral("permission_denied"), QStringLiteral("Copying the pack failed: %1.").arg(problem)));
        // A pack unpacked from a bundle says so (written by unpackSharePackage), and keeps saying it: its folder is a scratch one.
        if (const PackSource carried = readPackSource(work); carried.kind == PackSource::Kind::Bundle && !carried.location.isEmpty())
            staged.source = carried;
    } else if (prompts.fetch) {
        const Result<QString> fetched = prompts.fetch(wanted, work);
        if (!fetched)
            return cleanFail(fetched.error());
        staged.source.commit = fetched.value();
    } else {
        // Only the schemes of looksLikeGitUrl run (git's other transports, `ext::` and the
        // remote helpers, are off), and `--` ends the options.
        QStringList clone{QStringLiteral("-c"), QStringLiteral("protocol.allow=never")};
        for (const char *scheme : {"https", "ssh", "git", "file"})
            clone << QStringLiteral("-c") << QStringLiteral("protocol.%1.allow=always").arg(QLatin1String(scheme));
        clone << QStringLiteral("clone") << QStringLiteral("--quiet") << QStringLiteral("--no-checkout") << QStringLiteral("--")
              << wanted.location << work;
        const ProcessResult cloned = runProgram(QStringLiteral("git"), clone, {}, log, false, prompts.cancelled);
        if (!cloned.ok)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("git could not clone %1.").arg(wanted.location),
                                  cloned.output.trimmed().isEmpty() ? QStringLiteral("Is git installed, and is the address right?") : cloned.output.trimmed()));
        if (prompts.maxCloneBytes > 0 && treeBytes(work) > prompts.maxCloneBytes)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 is larger than a pack may be (%2 MB).").arg(wanted.location).arg(prompts.maxCloneBytes / 1000000),
                                  QStringLiteral("A pack is a few files; this address holds more.")));
        // A pin is a tag or a commit, never a branch: a branch moves under the user's feet.
        const bool isTag = runProgram(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                                              QStringLiteral("refs/tags/%1^{commit}").arg(wanted.ref)}, work, nullptr, true).ok;
        if (!isTag && !looksLikeCommit(wanted.ref))
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 has no tag \"%2\", and it is not a commit id.").arg(wanted.location, wanted.ref),
                                  QStringLiteral("Pin the pack to a tag or a commit; a branch name is not a pin.")));
        const ProcessResult checkedOut = runProgram(QStringLiteral("git"),
            {QStringLiteral("-c"), QStringLiteral("advice.detachedHead=false"), QStringLiteral("checkout"), QStringLiteral("--quiet"),
             QStringLiteral("--detach"), isTag ? QStringLiteral("refs/tags/%1").arg(wanted.ref) : wanted.ref + QStringLiteral("^{commit}"),
             QStringLiteral("--")}, work, log, false, prompts.cancelled);
        if (!checkedOut.ok)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 has no commit \"%2\".").arg(wanted.location, wanted.ref),
                                  QStringLiteral("Pin the pack to a tag or a commit that exists.")));
        const ProcessResult head = runProgram(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, work, nullptr, true);
        staged.source.commit = head.output.trimmed();
        QDir(work + QStringLiteral("/.git")).removeRecursively();
    }
    if (wanted.kind == PackSource::Kind::Git) {
        if (prompts.maxCloneBytes > 0 && treeBytes(work) > prompts.maxCloneBytes)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 is larger than a pack may be (%2 MB).").arg(wanted.location).arg(prompts.maxCloneBytes / 1000000)));
        if (const QString link = firstSymlink(work); !link.isEmpty())
            return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack holds a symbolic link (%1).").arg(link),
                                  QStringLiteral("A pack holds no links: they could lead out of it.")));
    }
    restoreCommandEntries(work);
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

bool looksLikeGitUrl(const QString &text) {
    static const QRegularExpression scp(QStringLiteral("^[A-Za-z0-9._-]+@[A-Za-z0-9.-]+:[^ ]+$"));
    for (const char *scheme : {"https://", "ssh://", "git://", "file://"})
        if (text.startsWith(QLatin1String(scheme)))
            return true;
    return scp.match(text).hasMatch();
}

bool samePack(const PackSummary &shown, const PackSummary &now) {
    return shown.id == now.id && shown.version == now.version && shown.setupCommand == now.setupCommand
           && shown.permissions == now.permissions && shown.secrets == now.secrets && shown.runtimeKinds == now.runtimeKinds
           && shown.nodes == now.nodes
           && (shown.source.commit.isEmpty() || shown.source.commit == now.source.commit);
}

PreparedPack::~PreparedPack() {
    if (!staging.isEmpty())
        QDir(staging).removeRecursively();
}

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
    const QString kind = read->value("kind").toString();
    source.kind = kind == QLatin1String("git") ? PackSource::Kind::Git
                  : kind == QLatin1String("bundle") ? PackSource::Kind::Bundle : PackSource::Kind::Path;
    source.location = read->value("location").toString();
    source.ref = read->value("ref").toString();
    source.commit = read->value("commit").toString();
    return source;
}

// Which of the pack's secrets already have a value for this id from this source, and which only from another source (6.4.1).
static void fillSecretState(PackSummary &summary) {
    summary.secretsSet.clear();
    summary.secretsOtherSource.clear();
    if (summary.secrets.isEmpty())
        return;
    const PackSecretStore store;
    const QString key = packSecretKey(summary.id, summary.source.location);
    const QStringList mine = store.namesSet(key);
    for (const QString &name : summary.secrets)
        if (mine.contains(name))
            summary.secretsSet << name;
    summary.secretsOtherSource = store.namesSetForOtherSources(summary.id, key);
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
    summary.secrets = packSecretNames(loaded.secrets);
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
    if (const QJsonValue size = json->value("setup").toObject().value("size"); size.isDouble() && size.toDouble() >= 0)
        summary.setupSize = qint64(size.toDouble());
    summary.source = readPackSource(folder);
    fillSecretState(summary);
    QStringList shown{summary.name, summary.version, summary.setupDescription};
    shown += summary.setupCommand;
    for (const QString &text : shown)
        if (hasControl(text))
            return fail(QStringLiteral("pack_invalid"), QStringLiteral("pack.json has control characters in its name, version or setup."),
                        QStringLiteral("They could change what a prompt shows."));
    return summary;
}

namespace {

Error warningOf(Error error, const QString &prefix = {}) {
    error.severity = Error::Severity::Warning;
    if (!prefix.isEmpty())
        error.message = prefix + QStringLiteral(": ") + error.message;
    return error;
}

// Installs a staged pack into packsDir, replacing the installed folder when `replacing`. The
// setup question comes before anything changes; a setup that fails puts everything back.
Result<PackAdded> install(const Staged &staged, PackSummary summary, const QString &packsDir, const QString &target,
                          const PackPrompts &prompts, bool replacing, TrustStore &trust) {
    summary.source = staged.source;
    fillSecretState(summary);
    summary.folder = target;
    PackAdded added;
    added.pack = summary;
    bool runSetup = false;
    if (!summary.setupCommand.isEmpty()) {
        const Result<bool> yes = ask(prompts.runSetup, summary, QStringLiteral("Running a pack's setup"));
        if (!yes)
            return yes.errors();
        runSetup = *yes;
        added.setupSkipped = !*yes;
    }
    const Errors written = writeSource(staged.folder, staged.source);
    if (hasErrors(written))
        return written;
    QString backup;
    if (replacing) {
        backup = packsDir + QStringLiteral("/.old-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
        if (!QDir().rename(target, backup))
            return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot replace %1.").arg(target));
    }
    if (!QDir().rename(staged.folder, target)) {
        if (replacing)
            QDir().rename(backup, target);
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot move the pack into %1.").arg(packsDir));
    }
    if (runSetup) {
        const QStringList &command = summary.setupCommand;
        const ProcessResult done = runProgram(command.first(), command.mid(1), target, prompts.log, false, prompts.cancelled);
        if (!done.ok) {
            QDir(target).removeRecursively();
            if (replacing)
                QDir().rename(backup, target); // the old pack is back as it was
            return fail(QStringLiteral("setup_failed"),
                        QStringLiteral("The setup of %1 failed (%2); the pack was not installed.").arg(summary.id, displayCommand(command)),
                        done.output.trimmed());
        }
        added.setupRan = true;
    }
    if (replacing)
        QDir(backup).removeRecursively();
    // Trust is per version, and a new version, a new commit or a re-install is new code: it asks again.
    // Another major of the pack stays trusted: only this version (and the one replaced) is forgotten.
    const bool beside = foldersOfPack(summary.id, {packsDir}).size() > 1;
    Errors warnings = beside ? trust.forget(summary.id, summary.version) : trust.forget(summary.id);
    if (beside && !summary.oldVersion.isEmpty())
        warnings += trust.forget(summary.id, summary.oldVersion);
    for (Error &warning : warnings)
        warning = warningOf(warning);
    return Result<PackAdded>(added, warnings);
}

// Whether a pack with this id and major is installed in the packs folder or a search path.
bool inSearchPaths(const QString &id, int major, const QString &packsDir) {
    QStringList paths = packSearchPaths();
    paths.prepend(packsDir);
    for (const QString &folder : foldersOfPack(id, paths))
        if (majorOfFolder(folder) == major)
            return true;
    return false;
}

// Where a new pack goes (6.6): `<id>`, the way packs always lived, and `<id>@<major>` when the
// folder of the id is taken by another major.
QString targetFolder(const QString &id, int major, const QString &packsDir) {
    const QString plain = packsDir + QLatin1Char('/') + id;
    return QFileInfo::exists(plain) ? packsDir + QLatin1Char('/') + packFolderName(id, major) : plain;
}

// A pack this project inherits from the Default project is read-only here (1.4.2): the message names where to
// change it.
Error inheritedRefusal(const QString &folderName, const QString &inheritedFolder, const QString &what) {
    return fail(QStringLiteral("usage"),
                QStringLiteral("%1 is a pack this project inherits from the Default project (%2); it cannot be %3 here.")
                    .arg(folderName, inheritedFolder, what),
                QStringLiteral("Change it in the Default project (start fugue without --project), or install your own copy here with "
                               "`fugue pack add`, which hides the inherited one."));
}

} // namespace

Result<std::shared_ptr<PreparedPack>> preparePack(const PackSource &source, const QString &packsDir, const PackPrompts &prompts) {
    if (hasControl(source.location) || hasControl(source.ref))
        return fail(QStringLiteral("usage"), QStringLiteral("The address or the ref has control characters."));
    if (source.kind == PackSource::Kind::Git) {
        if (!looksLikeGitUrl(source.location))
            return fail(QStringLiteral("usage"), QStringLiteral("%1 is not a git address.").arg(source.location),
                        QStringLiteral("Use https://, ssh://, git:// or file://."));
        if (source.ref.isEmpty() || source.ref.startsWith('-'))
            return fail(QStringLiteral("usage"), QStringLiteral("A git pack must be pinned to a tag or a commit."), QStringLiteral("Add a ref."));
    }
    PackSource wanted = source;
    if (wanted.kind == PackSource::Kind::Path)
        wanted.location = realPath(wanted.location);
    Result<Staged> staged = stage(wanted, packsDir, prompts);
    if (!staged)
        return staged.errors();
    auto prepared = std::make_shared<PreparedPack>();
    prepared->staging = staged->staging;
    prepared->folder = staged->folder;
    prepared->source = staged->source;
    Result<PackSummary> summary = summarizePack(staged->folder);
    if (!summary)
        return summary.errors();
    summary->source = staged->source;
    const int major = Version::parse(summary->version)->major; // summarizePack loaded it
    if (inSearchPaths(summary->id, major, packsDir))
        return Error(fail(QStringLiteral("pack_exists"),
                          QStringLiteral("The pack %1 is already installed at major version %2.").arg(summary->id).arg(major),
                          QStringLiteral("`fugue pack update %1` fetches it again; `fugue pack remove %1` removes it. "
                                         "Another major version installs beside it.").arg(summary->id)));
    summary->folder = targetFolder(summary->id, major, packsDir);
    prepared->summary = *summary;
    return prepared;
}

Result<PackAdded> installPrepared(const std::shared_ptr<PreparedPack> &prepared, const QString &packsDir, TrustStore &trust,
                                  const PackPrompts &prompts) {
    const Result<bool> yes = ask(prompts.install, prepared->summary, QStringLiteral("Installing a pack"));
    if (!yes)
        return yes.errors();
    if (!*yes)
        return Error(fail(QStringLiteral("cancelled"), QStringLiteral("The pack was not installed.")));
    const Staged staged{prepared->staging, prepared->folder, prepared->source};
    return install(staged, prepared->summary, packsDir, prepared->summary.folder, prompts, false, trust);
}

Result<PackAdded> addPack(const QString &location, const QString &ref, const QString &packsDir, TrustStore &trust,
                          const PackPrompts &prompts) {
    if (hasControl(location) || hasControl(ref))
        return fail(QStringLiteral("usage"), QStringLiteral("The address or the ref has control characters."));
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

    const Result<std::shared_ptr<PreparedPack>> prepared = preparePack(source, packsDir, prompts);
    if (!prepared)
        return prepared.errors();
    return installPrepared(*prepared, packsDir, trust, prompts);
}

Result<QList<PackAdded>> updatePacks(const QString &id, const QString &ref, const QString &packsDir, TrustStore &trust,
                                     const PackPrompts &prompts) {
    if (!ref.isEmpty() && id.isEmpty())
        return fail(QStringLiteral("usage"), QStringLiteral("--ref needs the id of one pack."));
    QStringList ids;
    if (!id.isEmpty()) {
        if (!validPackId(id) || !QFileInfo(packsDir + QLatin1Char('/') + id).isDir()) {
            if (const QString inherited = validPackId(id) ? inheritedPackFolder(id) : QString(); !inherited.isEmpty())
                return inheritedRefusal(id, inherited, QStringLiteral("updated"));
            return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir));
        }
        ids << id;
    } else {
        for (const QFileInfo &entry : QDir(packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (!entry.fileName().startsWith('.'))
                ids << entry.fileName();
    }
    QList<PackAdded> changed;
    Errors warnings;
    // One pack's trouble stops an update of one pack, but not the others in an update of all.
    const auto problem = [&](const QString &name, const Errors &errors) -> std::optional<Errors> {
        if (id.isEmpty()) {
            for (const Error &error : errors)
                warnings << warningOf(error, name);
            return std::nullopt;
        }
        return errors;
    };
    for (const QString &name : ids) {
        if (prompts.cancelled && prompts.cancelled())
            return Result<QList<PackAdded>>(changed, warnings + Errors{warningOf(fail(QStringLiteral("cancelled"), QStringLiteral("Cancelled before the other packs.")))});
        const QString folder = packsDir + QLatin1Char('/') + name;
        if (QFileInfo(folder).isSymLink()) {
            if (const auto stop = problem(name, {fail(QStringLiteral("usage"), QStringLiteral("%1 is a link, not an installed pack folder.").arg(folder))}))
                return *stop;
            continue;
        }
        PackSource source = readPackSource(folder);
        if (source.location.isEmpty()) {
            if (!id.isEmpty())
                return fail(QStringLiteral("usage"), QStringLiteral("%1 was not installed by `fugue pack add`, so there is nothing to fetch it from.").arg(name));
            continue;
        }
        if (source.kind == PackSource::Kind::Bundle) {
            if (!id.isEmpty())
                return fail(QStringLiteral("usage"), QStringLiteral("%1 came in the bundle %2, which is no source to update it from.").arg(name, source.location),
                            QStringLiteral("Open a newer bundle to replace it, or install it from its folder or git address with `fugue pack add`."));
            continue;
        }
        if (!ref.isEmpty()) {
            if (source.kind != PackSource::Kind::Git)
                return fail(QStringLiteral("usage"), QStringLiteral("--ref is only for packs installed from git."));
            source.ref = ref;
        }
        // The source file is data on disk: it gets the checks of `add` again.
        const bool sane = !hasControl(source.location) && !hasControl(source.ref)
                          && (source.kind == PackSource::Kind::Path ? source.location.startsWith('/')
                                                                    : looksLikeGitUrl(source.location) && !source.ref.isEmpty() && !source.ref.startsWith('-'));
        if (!sane) {
            if (const auto stop = problem(name, {fail(QStringLiteral("pack_invalid"), QStringLiteral("The recorded source of %1 is not usable.").arg(name))}))
                return *stop;
            continue;
        }
        const Result<PackSummary> current = summarizePack(folder);
        Result<Staged> staged = stage(source, packsDir, prompts);
        if (!staged) {
            if (const auto stop = problem(name, staged.errors()))
                return *stop;
            continue;
        }
        const auto cleanup = [&] { QDir(staged->staging).removeRecursively(); };
        Result<PackSummary> next = summarizePack(staged->folder);
        Errors why;
        if (!next)
            why = next.errors();
        else if (next->id != parsePackRef(name).id)
            why = {fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack now says its id is %1, not %2.").arg(next->id, name))};
        else if (const PackRef ref = parsePackRef(name); ref.major >= 0 && Version::parse(next->version)->major != ref.major)
            why = {fail(QStringLiteral("pack_invalid"),
                        QStringLiteral("%1 is now at version %2, another major version than the folder holds.").arg(name, next->version),
                        QStringLiteral("`fugue pack add` installs that major version beside this one."))};
        if (!why.isEmpty()) {
            cleanup();
            if (const auto stop = problem(name, why))
                return *stop;
            continue;
        }
        next->source = staged->source;
        const PackSource old = readPackSource(folder);
        if (source.kind == PackSource::Kind::Git && current && current->version == next->version
            && old.commit == staged->source.commit && old.ref == staged->source.ref) {
            cleanup();
            continue; // the same commit under the same pin
        }
        next->oldVersion = current ? current->version : QString();
        next->updating = true;
        const Result<bool> yes = ask(prompts.install, *next, QStringLiteral("Updating a pack"));
        if (!yes) {
            cleanup();
            return yes.errors();
        }
        if (!*yes) {
            cleanup();
            continue;
        }
        Result<PackAdded> added = install(*staged, *next, packsDir, folder, prompts, true, trust);
        cleanup();
        if (!added) {
            if (const auto stop = problem(name, added.errors()))
                return *stop;
            continue;
        }
        for (const Error &warning : added.errors())
            warnings << warning;
        changed << *added;
    }
    return Result<QList<PackAdded>>(changed, warnings);
}

Result<PackSummary> removePack(const QString &id, const QString &packsDir, TrustStore &trust, const PackPrompts &prompts) {
    const QString folder = packsDir + QLatin1Char('/') + id;
    if (!validPackId(id) || !QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink()) {
        if (const QString inherited = validPackId(id) ? inheritedPackFolder(id) : QString(); !inherited.isEmpty())
            return inheritedRefusal(id, inherited, QStringLiteral("removed"));
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir),
                    QStringLiteral("Only packs in that folder are removed; packs found through settings are the user's own folders."));
    }
    PackSummary summary;
    const Result<PackSummary> read = summarizePack(folder);
    if (read) {
        summary = *read;
    } else {
        summary.id = parsePackRef(id).id; // a broken pack can still be removed
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
    // Another major of the pack stays trusted.
    const QString packId = parsePackRef(id).id;
    const bool beside = !foldersOfPack(packId, {packsDir}).isEmpty();
    const Errors forgotten = beside && !summary.version.isEmpty() ? trust.forget(packId, summary.version) : trust.forget(packId);
    if (hasErrors(forgotten))
        return forgotten;
    if (!beside)
        PackSecretStore().forgetPack(packId); // its keys go with it; best effort, the pack itself is gone
    return summary;
}

Result<PackSummary> trustPack(const QString &id, const QString &packsDir, TrustStore &trust, const PackPrompts &prompts) {
    if (TrustStore::isBuiltIn(parsePackRef(id).id))
        return fail(QStringLiteral("usage"), QStringLiteral("%1 is part of Fugue and is always trusted.").arg(id));
    // A pack in the packs folder, or in a folder of the settings: trust is for any pack found.
    QString folder;
    QStringList places = packSearchPaths();
    places.prepend(packsDir);
    for (const QString &place : places)
        if (validPackId(id) && QFileInfo(place + QLatin1Char('/') + id).isDir()) {
            folder = place + QLatin1Char('/') + id;
            break;
        }
    QString inheritedFolder;
    if (folder.isEmpty() && validPackId(id))
        inheritedFolder = folder = inheritedPackFolder(id);
    if (folder.isEmpty())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir));
    const Result<PackSummary> summary = summarizePack(folder);
    if (!summary)
        return summary.errors();
    if (trust.isTrusted(summary->id, summary->version))
        return *summary;
    if (!inheritedFolder.isEmpty())
        return inheritedRefusal(id, inheritedFolder, QStringLiteral("trusted (the Default project has not trusted this version)"));
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
