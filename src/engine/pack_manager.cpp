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
#include <QCoreApplication>
#include <QJsonArray>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSysInfo>
#include <QSet>
#include <QUuid>

#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <algorithm>
#include <optional>

namespace fugue {

std::atomic<qint64> &runningSetupGroup() {
    static std::atomic<qint64> group{0};
    return group;
}

namespace {

const QString sourceFile = QStringLiteral(".fugue-source.json");
const QString setupMarkerFile = QStringLiteral(".fugue-setup.json");

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

// Names of the folders a command makes in the packs folder. Each carries the owner's pid and the time it was made, so a later
// command can tell whether the owner still works (#418), which a modification time cannot: a slow clone writes nothing for
// minutes, and a rename keeps a folder's time. A pid that was handed out again after the owner died is told by its start time.
//   .staging-<pid>-<epoch s>-<random>                scratch space
//   .old-<pid>-<epoch s>-<random>-<folder name>      an installed pack set aside by an update: the name says where it goes back
//   .rescue-<random>-<folder name>                   an old pack that could not be put back; nothing sweeps it
QString randomPart() { return QUuid::createUuid().toString(QUuid::Id128).left(12); }

QString stagingName() {
    return QStringLiteral(".staging-%1-%2-%3").arg(QCoreApplication::applicationPid()).arg(QDateTime::currentSecsSinceEpoch()).arg(randomPart());
}

QString backupName(const QString &folderName) {
    return QStringLiteral(".old-%1-%2-%3-%4").arg(QCoreApplication::applicationPid()).arg(QDateTime::currentSecsSinceEpoch()).arg(randomPart(), folderName);
}

// When process `pid` started (epoch s), or -1 when /proc cannot say.
qint64 processStartEpoch(qint64 pid) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray line = stat.readAll();
    const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' '); // after "pid (comm) ": field 3 on
    if (fields.size() < 20)
        return -1;
    const qint64 ticks = fields.at(19).toLongLong(); // field 22: start time in clock ticks after boot
    QFile uptime(QStringLiteral("/proc/uptime"));
    if (ticks <= 0 || !uptime.open(QIODevice::ReadOnly))
        return -1;
    const double up = uptime.readAll().split(' ').value(0).toDouble();
    const long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? QDateTime::currentSecsSinceEpoch() - qint64(up) + ticks / hz : -1;
}

// Whether the process that made a folder (`pid`, at `madeAt`) is still running: not when the pid belongs to a process that
// started after the folder was made.
bool ownerRunning(qint64 pid, qint64 madeAt) {
    if (pid <= 0 || (::kill(pid_t(pid), 0) != 0 && errno != EPERM)) // EPERM: it exists, and is another user's
        return false;
    const qint64 started = processStartEpoch(pid);
    return started < 0 || started <= madeAt + 5; // 5 s: the two clocks are read apart
}

bool processAlive(qint64 pid) { return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM); }

// Removes a folder tree. A link at the top is refused, never followed: it points where this command may not delete.
bool removeTree(const QString &path) {
    if (QFileInfo(path).isSymLink())
        return false;
    return QDir(path).removeRecursively();
}

// Removes what is at `path`: a link as a link, a folder as a tree.
bool removeEntry(const QString &path) {
    return QFileInfo(path).isSymLink() ? QFile::remove(path) : removeTree(path);
}

// Whether the pack in `folder` is whole: it has no setup, or its setup ran to its end (the marker names the command).
bool packComplete(const QString &folder) {
    const Result<QJsonObject> json = readJsonObject(folder + QStringLiteral("/pack.json"));
    if (!json)
        return false;
    QStringList command;
    for (const QJsonValue &part : json->value("setup").toObject().value("command").toArray())
        command << part.toString();
    return command.isEmpty() || packSetupDone(folder, command);
}

// Renames an old pack that cannot be put back to a `.rescue-` name (nothing sweeps it). Returns where it is.
QString rescueBackup(const QString &packsDir, const QString &folderName, const QString &backup) {
    const QString rescue = packsDir + QLatin1Char('/') + QStringLiteral(".rescue-%1-%2").arg(randomPart(), folderName);
    return QDir().rename(backup, rescue) ? rescue : backup;
}

// Puts the pack set aside at `backup` back at `target`, as it was. What is at `target` (the half-made folder, or a link someone
// made there) is moved aside first, never deleted before the old pack is back, and both moves are checked. Returns an error text,
// empty on success; on a failure the backup is renamed to a `.rescue-` name and the text says where it is.
QString restoreBackup(const QString &packsDir, const QString &target, const QString &backup) {
    const QString name = QFileInfo(target).fileName();
    const auto kept = [&] { return QStringLiteral("The old version could not be put back; it is kept in %1").arg(rescueBackup(packsDir, name, backup)); };
    QString aside;
    if (QFileInfo(target).isSymLink() || QFileInfo::exists(target)) {
        aside = packsDir + QLatin1Char('/') + stagingName();
        if (!QDir().rename(target, aside))
            return kept();
    }
    if (!QDir().rename(backup, target)) {
        if (!aside.isEmpty())
            QDir().rename(aside, target); // as it was
        return kept();
    }
    if (!aside.isEmpty())
        removeEntry(aside); // best effort: a leftover is a staging folder, swept once this command is gone
    return {};
}

// The packs folder's lock (#418): held while a pack is put in place, replaced, removed or set up, so the Get ready sheet,
// Settings > Packs and the CLI, in this process or another, never change one pack at once. Like the hash store's lock
// (FileHashes::save) it is a QLockFile, so a holder that died releases it. There is no age limit: a setup may run for minutes.
class PacksLock {
public:
    explicit PacksLock(const QString &packsDir, int timeoutMs = 2000) : m_path(packsDir + QStringLiteral("/.packs.lock")), m_file(m_path) {
        QDir().mkpath(packsDir);
        m_file.setStaleLockTime(0);
        m_held = m_file.tryLock(timeoutMs);
    }
    bool held() const { return m_held; }
    Error busy() const {
        qint64 holder = 0;
        QString host, app;
        m_file.getLockInfo(&holder, &host, &app);
        if (m_file.error() == QLockFile::PermissionError)
            return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot write the lock file %1.").arg(m_path),
                        QStringLiteral("Check that you may write in the packs folder."));
        if (m_file.error() == QLockFile::UnknownError)
            return fail(QStringLiteral("internal"), QStringLiteral("Cannot take the lock file %1.").arg(m_path));
        if (!host.isEmpty() && host != QSysInfo::machineHostName())
            return fail(QStringLiteral("pack_busy"), QStringLiteral("The packs folder is locked from another computer (%1).").arg(host),
                        QStringLiteral("Wait for it to finish. If it is gone, remove %1.").arg(m_path));
        if (holder > 0 && !processAlive(holder))
            return fail(QStringLiteral("pack_busy"), QStringLiteral("The packs folder is locked by pid %1, which is gone.").arg(holder),
                        QStringLiteral("Remove %1, then try again.").arg(m_path));
        return fail(QStringLiteral("pack_busy"),
                    holder > 0 ? QStringLiteral("Another Fugue command (pid %1) is changing packs.").arg(holder)
                               : QStringLiteral("Another Fugue command is changing packs."),
                    QStringLiteral("Wait for it to finish, then try again."));
    }

private:
    QString m_path;
    QLockFile m_file;
    bool m_held = false;
};

// The `.old-` folders of dead owners for one pack folder name, oldest first: [{path, epoch}].
struct DeadBackup {
    QString path;
    qint64 epoch = 0;
};

QList<DeadBackup> deadBackupsOf(const QString &packsDir, const QString &folderName) {
    static const QRegularExpression pattern(QStringLiteral("^\\.old-([0-9]{1,10})-([0-9]{1,12})-[0-9a-f]{12}-(.+)$"));
    QList<DeadBackup> found;
    for (const QFileInfo &entry : QDir(packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
        const QRegularExpressionMatch match = pattern.match(entry.fileName());
        if (match.hasMatch() && match.captured(3) == folderName
            && !ownerRunning(match.captured(1).toLongLong(), match.captured(2).toLongLong()))
            found.append({entry.filePath(), match.captured(2).toLongLong()});
    }
    std::sort(found.begin(), found.end(), [](const DeadBackup &x, const DeadBackup &y) { return x.epoch < y.epoch; });
    return found;
}

// Leftovers of a command that did not finish (Ctrl-C at a prompt, a crash, a kill): a folder whose owner process is gone. Run
// under the packs lock, so no update is in the middle of its swap. One whose owner still runs is another command's work and is
// never touched; a `.old-` of a live owner is kept whatever its age (its mtime is its pack's, kept across the rename). A
// dead owner's `.old-` is an installed pack an update set aside: the oldest of a folder's is put back unless the new copy is
// whole, the others are deleted only when it is whole, and what cannot be settled is kept as `.rescue-` and said through `log`.
// A link at the target is never followed, nor deleted through. Names from before pids stay as they are, except a `.staging-`
// one that is old. A trust the update forgot stays forgotten (#418).
void removeStale(const QString &packsDir, const std::function<void(const QString &)> &log) {
    static const QRegularExpression stagingPattern(QStringLiteral("^\\.staging-([0-9]{1,10})-([0-9]{1,12})-[0-9a-f]{12}$"));
    static const QRegularExpression oldPattern(QStringLiteral("^\\.old-[0-9]{1,10}-[0-9]{1,12}-[0-9a-f]{12}-(.+)$"));
    const PacksLock lock(packsDir, 0);
    if (!lock.held())
        return; // someone is changing packs now: the next command sweeps
    const auto say = [&](const QString &text) {
        if (log)
            log(text);
    };
    QSet<QString> folders;
    for (const QFileInfo &entry : QDir(packsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
        const QString name = entry.fileName();
        if (name.startsWith(QLatin1String(".staging-"))) {
            const QRegularExpressionMatch match = stagingPattern.match(name);
            const bool stale = match.hasMatch() ? !ownerRunning(match.captured(1).toLongLong(), match.captured(2).toLongLong())
                                                : entry.lastModified().secsTo(QDateTime::currentDateTime()) > 600;
            if (stale)
                removeTree(entry.filePath());
        } else if (const QRegularExpressionMatch match = oldPattern.match(name); match.hasMatch() && validPackId(match.captured(1))) {
            folders.insert(match.captured(1));
        }
    }
    for (const QString &folderName : std::as_const(folders)) {
        const QString target = packsDir + QLatin1Char('/') + folderName;
        const QList<DeadBackup> backups = deadBackupsOf(packsDir, folderName);
        bool restored = false; // the oldest backup is back: later ones are not put over it
        for (const DeadBackup &backup : backups) {
            if (QFileInfo(target).isSymLink()) { // someone's link: never followed
                say(QStringLiteral("%1 is a link, so the old version of the pack was kept in %2").arg(target, rescueBackup(packsDir, folderName, backup.path)));
            } else if (QFileInfo::exists(target) && packComplete(target)) {
                removeTree(backup.path); // the update finished; only its cleanup did not
            } else if (!restored) {
                const QString lost = restoreBackup(packsDir, target, backup.path); // the update did not finish: the old pack comes back
                restored = lost.isEmpty();
                if (!restored)
                    say(lost);
            } else {
                say(QStringLiteral("Another old version of %1 was kept in %2").arg(folderName, rescueBackup(packsDir, folderName, backup.path)));
            }
        }
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
                         const std::function<bool()> &cancelled = {}, bool ownGroup = false) {
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
    // A setup gets its own process group, so a stop ends what it started too (sh -c, uv's children), not only the program. Git is
    // left in ours: ssh asks for a passphrase on the terminal, which a background group may not read.
    if (ownGroup)
        process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start(program, arguments);
    if (!process.waitForStarted(10000))
        return result;
    if (ownGroup)
        runningSetupGroup().store(process.processId()); // a second Ctrl-C ends it from the signal handler
    struct Unpublish {
        bool on;
        ~Unpublish() { if (on) runningSetupGroup().store(0); }
    } unpublish{ownGroup};
    process.closeWriteChannel();
    // A cancel kills the program: git or a setup may take minutes, and the person can stop it.
    while (process.state() != QProcess::NotRunning && !process.waitForFinished(100)) {
        if (cancelled && cancelled()) {
            if (const qint64 pid = process.processId(); ownGroup && pid > 1)
                ::kill(pid_t(-pid), SIGKILL);
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
    const QString staging = packsDir + QLatin1Char('/') + stagingName();
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
    removeStale(packsDir, log);
    staged.staging = makeStaging(packsDir);
    if (staged.staging.isEmpty())
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot create a folder in %1.").arg(packsDir));
    const auto cleanFail = [&](const Error &error) -> Result<Staged> {
        removeTree(staged.staging);
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
        if (!cloned.ok && prompts.cancelled && prompts.cancelled())
            return cleanFail(fail(QStringLiteral("cancelled"), QStringLiteral("Cancelled.")));
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
        if (!checkedOut.ok && prompts.cancelled && prompts.cancelled())
            return cleanFail(fail(QStringLiteral("cancelled"), QStringLiteral("Cancelled.")));
        if (!checkedOut.ok)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 has no commit \"%2\".").arg(wanted.location, wanted.ref),
                                  QStringLiteral("Pin the pack to a tag or a commit that exists.")));
        const ProcessResult head = runProgram(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, work, nullptr, true);
        staged.source.commit = head.output.trimmed();
        removeTree(work + QStringLiteral("/.git"));
    }
    if (wanted.kind == PackSource::Kind::Git) {
        if (prompts.maxCloneBytes > 0 && treeBytes(work) > prompts.maxCloneBytes)
            return cleanFail(fail(QStringLiteral("fetch_failed"), QStringLiteral("%1 is larger than a pack may be (%2 MB).").arg(wanted.location).arg(prompts.maxCloneBytes / 1000000)));
        if (const QString link = firstSymlink(work); !link.isEmpty())
            return cleanFail(fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack holds a symbolic link (%1).").arg(link),
                                  QStringLiteral("A pack holds no links: they could lead out of it.")));
    }
    // The setup marker is written by Fugue after a setup that ran here: one that came in with the files proves nothing.
    QFile::remove(work + QLatin1Char('/') + setupMarkerFile);
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
        removeTree(staging);
}

QString displayCommand(const QStringList &command) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    QStringList parts;
    for (const QString &part : command)
        parts << (plain.match(part).hasMatch() ? part : QStringLiteral("'") + QString(part).replace('\'', QStringLiteral("'\\''")) + QStringLiteral("'"));
    return parts.join(QLatin1Char(' '));
}

bool packSetupDone(const QString &packFolder, const QStringList &command) {
    const Result<QJsonObject> read = readJsonObject(packFolder + QLatin1Char('/') + setupMarkerFile);
    if (!read)
        return false;
    QStringList done;
    for (const QJsonValue &part : read->value("command").toArray())
        done << part.toString();
    return !command.isEmpty() && done == command;
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

// Records that the setup command ran to its end in the pack's folder (best effort: without it Get ready offers the setup again).
void markSetupDone(const QString &packFolder, const QStringList &command) {
    writeJsonFile(packFolder + QLatin1Char('/') + setupMarkerFile,
                  QJsonObject{{"command", QJsonArray::fromStringList(command)}, {"ran", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}

// Installs a staged pack into packsDir, replacing the installed folder when `replacing`. The
// setup question comes before anything changes (declining it on an update keeps the old pack); a setup that fails
// puts everything back.
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
        // An update swaps in a fresh copy, which has no environment (a .venv, downloads): without its setup the
        // pack would stop working and the old environment would be deleted with the old folder. So the old pack
        // stays as it was (#521).
        if (replacing && !*yes)
            return fail(QStringLiteral("setup_declined"),
                        QStringLiteral("%1 was not updated: the new version needs its setup run to work, and you declined it.")
                            .arg(summary.id),
                        QStringLiteral("The installed version and its environment are unchanged. Update again and accept the setup."));
    }
    const Errors written = writeSource(staged.folder, staged.source);
    if (hasErrors(written))
        return written;
    // From here the packs folder changes: one command at a time. What was asked about before the lock may have moved
    // meanwhile, so the folder is looked at again under it.
    const PacksLock lock(packsDir, prompts.lockWaitMs);
    if (!lock.held())
        return lock.busy();
    if (replacing != QFileInfo(target).isDir())
        return fail(replacing ? QStringLiteral("unknown_pack") : QStringLiteral("pack_exists"),
                    replacing ? QStringLiteral("%1 is no longer installed, so it was not updated.").arg(summary.id)
                              : QStringLiteral("The pack %1 was installed meanwhile, so it was not installed again.").arg(summary.id),
                    QStringLiteral("Get ready again to see what is installed now."));
    // A stop between the last answer and the first move installs nothing.
    if (prompts.cancelled && prompts.cancelled())
        return fail(QStringLiteral("cancelled"), QStringLiteral("Cancelled; %1 was not %2.").arg(summary.id, replacing ? QStringLiteral("updated") : QStringLiteral("installed")));
    QString backup;
    const QString folderName = QFileInfo(target).fileName();
    // A pack that was removed after a crash must not come back: the sweep would put its old copy over the new install.
    if (!replacing)
        for (const DeadBackup &dead : deadBackupsOf(packsDir, folderName))
            removeTree(dead.path);
    // Trust is per version, and a new version, a new commit or a re-install is new code: it asks again. The new version's trust
    // is forgotten BEFORE the new code is in place: the same id and version, trusted, must never run other code for the length
    // of a setup. The old version's, and the id's other versions', go only once the update has worked, so a failed one leaves
    // what it did not change. A failed update of the same version puts the trust back, only if the old pack is back as it was.
    const int foldersNow = int(foldersOfPack(summary.id, {packsDir}).size());
    const bool beside = replacing ? foldersNow > 1 : foldersNow > 0;
    const bool wasTrusted = replacing && !summary.oldVersion.isEmpty() && summary.oldVersion == summary.version && summary.oldTrusted;
    const QString trustedDigest = wasTrusted ? packSecretsDigestOfFolder(target) : QString();
    Errors warnings = trust.forget(summary.id, summary.version);
    // The old pack comes back (or says where it is): the text for the error, empty when it is back.
    const auto putBack = [&]() -> QString {
        const QString lost = restoreBackup(packsDir, target, backup);
        if (lost.isEmpty() && wasTrusted && packSecretsDigestOfFolder(target) == trustedDigest)
            trust.trust(summary.id, summary.version, trustedDigest);
        return lost;
    };
    if (replacing) {
        backup = packsDir + QLatin1Char('/') + backupName(folderName);
        if (!QDir().rename(target, backup))
            return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot replace %1.").arg(target));
    }
    if (!QDir().rename(staged.folder, target)) {
        const QString lost = replacing ? putBack() : QString();
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot move the pack into %1.").arg(packsDir),
                    lost.isEmpty() ? QString() : lost);
    }
    if (runSetup) {
        const QStringList &command = summary.setupCommand;
        const ProcessResult done = runProgram(command.first(), command.mid(1), target, prompts.log, false, prompts.cancelled, true);
        if (!done.ok) {
            const bool stopped = prompts.cancelled && prompts.cancelled(); // a stop kills the setup, so it ends like a failure (#418)
            QString lost;
            if (replacing) {
                lost = putBack(); // the old pack is back as it was, or says where it is
            } else {
                const QString aside = packsDir + QLatin1Char('/') + stagingName();
                if (QDir().rename(target, aside))
                    removeTree(aside);
                else
                    removeTree(target);
            }
            const QString outcome = !lost.isEmpty() ? lost : replacing ? QStringLiteral("the old version was put back") : QStringLiteral("the pack was not installed");
            if (stopped)
                return fail(QStringLiteral("cancelled"), QStringLiteral("The setup of %1 was stopped; %2.").arg(summary.id, outcome));
            return fail(QStringLiteral("setup_failed"),
                        QStringLiteral("The setup of %1 failed (%2); %3.").arg(summary.id, displayCommand(command), outcome),
                        done.output.trimmed());
        }
        added.setupRan = true;
        markSetupDone(target, command);
    }
    if (replacing)
        removeTree(backup);
    if (!beside)
        warnings += trust.forget(summary.id);
    else if (!summary.oldVersion.isEmpty())
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
        if (id.isEmpty() && !(prompts.cancelled && prompts.cancelled())) { // a stop ends the whole update, it is no one pack's trouble
            for (const Error &error : errors)
                warnings << warningOf(error, name);
            return std::nullopt;
        }
        Errors reported = errors; // a stop ends the whole update: say what was done before it
        if (prompts.cancelled && prompts.cancelled() && !changed.isEmpty() && !reported.isEmpty())
            reported.first().message += QStringLiteral(" %1 pack(s) were updated before it.").arg(changed.size());
        return reported;
    };
    for (const QString &name : ids) {
        if (prompts.cancelled && prompts.cancelled())
            return fail(QStringLiteral("cancelled"), QStringLiteral("Stopped; %1 pack(s) were updated before it, the others were left as they are.").arg(changed.size()));
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
        // Asked before the new copy is read: reading it records its secrets in the process, and the old one would look changed.
        const bool oldTrusted = current && trust.isTrusted(current->id, current->version);
        Result<Staged> staged = stage(source, packsDir, prompts);
        if (!staged) {
            if (const auto stop = problem(name, staged.errors()))
                return *stop;
            continue;
        }
        const auto cleanup = [&] { removeTree(staged->staging); };
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
        next->oldTrusted = oldTrusted;
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
    const PacksLock lock(packsDir, prompts.lockWaitMs);
    if (!lock.held())
        return lock.busy();
    if (!QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("The pack %1 is gone already.").arg(id));
    if (!removeTree(folder))
        return fail(QStringLiteral("permission_denied"), QStringLiteral("Cannot remove %1 completely.").arg(folder));
    for (const DeadBackup &dead : deadBackupsOf(packsDir, id)) // an old copy a crash left would come back on the next sweep
        removeTree(dead.path);
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
    if (prompts.cancelled && prompts.cancelled())
        return fail(QStringLiteral("cancelled"), QStringLiteral("Stopped; the pack was not trusted."));
    const Errors errors = trust.trust(summary->id, summary->version, packSecretsDigestOfFolder(folder));
    if (hasErrors(errors))
        return errors;
    return *summary;
}

Result<PackSummary> setUpPack(const QString &id, const QString &packsDir, const PackPrompts &prompts) {
    if (!validPackId(parsePackRef(id).id) || !QFileInfo(packsDir + QLatin1Char('/') + id).isDir())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("No pack %1 is installed in %2.").arg(id, packsDir));
    const QString folder = packsDir + QLatin1Char('/') + id;
    const Result<PackSummary> summary = summarizePack(folder);
    if (!summary)
        return summary.errors();
    if (summary->setupCommand.isEmpty())
        return *summary;
    const Result<bool> yes = ask(prompts.runSetup, *summary, QStringLiteral("Running a pack's setup"));
    if (!yes)
        return yes.error();
    if (!*yes)
        return fail(QStringLiteral("setup_declined"), QStringLiteral("The setup of %1 was not run.").arg(summary->id));
    const PacksLock lock(packsDir, prompts.lockWaitMs);
    if (!lock.held())
        return lock.busy();
    // What was asked about is looked at again under the lock: the pack may have been replaced meanwhile.
    if (!QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink())
        return fail(QStringLiteral("unknown_pack"), QStringLiteral("The pack %1 is gone already.").arg(id));
    const Result<PackSummary> now = summarizePack(folder);
    if (!now || !samePack(*summary, *now))
        return fail(QStringLiteral("pack_invalid"), QStringLiteral("The pack %1 changed while it was waiting, so its setup was not run.").arg(id),
                    QStringLiteral("Get ready again to see the pack as it is now."));
    if (prompts.cancelled && prompts.cancelled())
        return fail(QStringLiteral("cancelled"), QStringLiteral("Stopped before the setup of %1.").arg(summary->id));
    const QStringList &command = summary->setupCommand;
    const ProcessResult done = runProgram(command.first(), command.mid(1), folder, prompts.log, false, prompts.cancelled, true);
    if (!done.ok && prompts.cancelled && prompts.cancelled())
        return fail(QStringLiteral("cancelled"), QStringLiteral("The setup of %1 was stopped; its environment may be half made.").arg(summary->id),
                    QStringLiteral("Run the setup again from Get ready."));
    if (!done.ok)
        return fail(QStringLiteral("setup_failed"), QStringLiteral("The setup of %1 failed (%2).").arg(summary->id, displayCommand(command)),
                    done.output.trimmed());
    markSetupDone(folder, command);
    return *summary;
}

} // namespace fugue
