#include "run_folder.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTimeZone>

#include "engine/ids.h"
#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

bool isRunId(const QString &id) {
    static const QRegularExpression form(QStringLiteral("^[a-z0-9-]{1,64}$"));
    return form.match(id).hasMatch();
}

// run.json and compiled.json are read while a run goes; one that is missing or cannot be
// read is an empty object to the reader, who then knows nothing about the run yet.
QJsonObject readObject(const QString &path) {
    const Result<QJsonObject> object = readJsonObject(path);
    return object ? object.value() : QJsonObject();
}

QMutex &eventsMutex() {
    static QMutex mutex;
    return mutex;
}

} // namespace

QString runStatusName(RunStatus status) {
    switch (status) {
    case RunStatus::Queued:
        return QStringLiteral("queued");
    case RunStatus::Running:
        return QStringLiteral("running");
    case RunStatus::Ok:
        return QStringLiteral("ok");
    case RunStatus::Failed:
        return QStringLiteral("failed");
    case RunStatus::Cancelled:
        return QStringLiteral("cancelled");
    case RunStatus::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

RunStatus parseRunStatus(const QString &text) {
    for (const RunStatus status : {RunStatus::Queued, RunStatus::Running, RunStatus::Ok, RunStatus::Failed, RunStatus::Cancelled})
        if (text == runStatusName(status))
            return status;
    return RunStatus::Unknown;
}

bool isTerminal(RunStatus status) {
    return status == RunStatus::Ok || status == RunStatus::Failed || status == RunStatus::Cancelled;
}

QString newRunId() {
    // Within one process each id gets a later millisecond than the last, so
    // ids sort in the order they were made even when made in the same one.
    static std::atomic<qint64> last{0};
    qint64 ms = QDateTime::currentMSecsSinceEpoch();
    qint64 previous = last.load();
    while (!last.compare_exchange_weak(previous, std::max(ms, previous + 1))) {
    }
    ms = std::max(ms, previous + 1);

    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    QString suffix;
    for (int i = 0; i < 4; ++i)
        suffix += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(36)]);
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyyMMdd-HHmmss-zzz-")) +
           suffix;
}

QString RunFolder::defaultRoot() {
    return cacheDir() + QStringLiteral("/runs");
}

Result<RunFolder> RunFolder::create(const QString &root, const QString &id) {
    if (!id.isEmpty() && !isRunId(id))
        return makeError(QStringLiteral("usage"), QStringLiteral("A run id is made of a-z, 0-9 and '-'."));
    if (!QDir().mkpath(root))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create %1.").arg(root));
    // mkdir fails when the folder exists, which makes the id unique across
    // processes; a made-up id that collides is simply made again.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString runId = id.isEmpty() ? newRunId() : id;
        const QString path = QDir(root).filePath(runId);
        if (QDir().mkdir(path)) {
            if (!QDir().mkdir(path + QStringLiteral("/nodes")))
                break;
            return RunFolder(runId, path);
        }
        if (!id.isEmpty())
            return makeError(QStringLiteral("usage"), QStringLiteral("The run %1 already exists.").arg(id));
    }
    return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create a run folder in %1.").arg(root));
}

Result<RunFolder> RunFolder::open(const QString &root, const QString &id) {
    const QString path = QDir(root).filePath(id);
    if (!isRunId(id) || !QDir(path).exists())
        return makeError(QStringLiteral("unknown_run"), QStringLiteral("There is no run %1.").arg(id),
                         QStringLiteral("`fugue runs` lists the runs."));
    return RunFolder(id, path);
}

QString RunFolder::runJsonPath() const {
    return m_path + QStringLiteral("/run.json");
}

QString RunFolder::compiledJsonPath() const {
    return m_path + QStringLiteral("/compiled.json");
}

QString RunFolder::eventsPath() const {
    return m_path + QStringLiteral("/events.jsonl");
}

bool RunFolder::writeState(const QJsonObject &state) const {
    return writeJsonFile(runJsonPath(), state).isEmpty();
}

QJsonObject RunFolder::state() const {
    return readObject(runJsonPath());
}

bool RunFolder::writeCompiled(const QJsonObject &compiled) const {
    return writeJsonFile(compiledJsonPath(), compiled).isEmpty();
}

QJsonObject RunFolder::compiled() const {
    return readObject(compiledJsonPath());
}

Errors RunFolder::writeWorkflow(const QJsonObject &document) const {
    return writeJsonFile(m_path + QStringLiteral("/workflow.json"), document);
}

QJsonObject RunFolder::workflow() const {
    return readObject(m_path + QStringLiteral("/workflow.json"));
}

Errors RunFolder::writeRoots(const FileRoots &roots) const {
    return writeJsonFile(m_path + QStringLiteral("/roots.json"), roots.toJson());
}

std::optional<FileRoots> RunFolder::roots() const {
    const QString file = m_path + QStringLiteral("/roots.json");
    if (!QFileInfo::exists(file))
        return std::nullopt;
    const Result<QJsonObject> json = readJsonObject(file);
    if (!json)
        return FileRoots{}; // unreadable: allow nothing rather than everything
    const Result<FileRoots> roots = FileRoots::fromJson(*json);
    return roots ? roots.value() : FileRoots{};
}

bool RunFolder::queued(const QString &workflow) const {
    QJsonObject state{{"status", runStatusName(RunStatus::Queued)}};
    if (!workflow.isEmpty())
        state.insert("workflow", QJsonObject{{"path", workflow}});
    return writeState(state) && appendEvent(QJsonObject{{"event", "run.queued"}});
}

QJsonObject RunFolder::finish(RunStatus status, const Errors &errors, const QDateTime &now) const {
    // Under a lock on events.jsonl (flock: it excludes other processes and other open
    // files of this one), so of two finishers only the first writes; the second finds
    // its run.done. run.done, not run.json's `ended`, says the run is over: a finisher
    // that died between the two writes must not stop the next one.
    const int lock = ::open(QFile::encodeName(eventsPath()).constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (lock >= 0)
        while (::flock(lock, LOCK_EX) != 0 && errno == EINTR) {
        }
    const auto unlock = qScopeGuard([lock] {
        if (lock >= 0)
            ::close(lock);
    });
    const QList<QJsonObject> written = events();
    for (auto it = written.crbegin(); it != written.crend(); ++it)
        if (it->value("event").toString() == QLatin1String("run.done"))
            return *it;

    QJsonObject ended = state();
    ended.insert("status", runStatusName(status));
    ended.insert("ended", isoTime(now));
    writeState(ended);
    const QJsonObject done{{"event", "run.done"}, {"status", runStatusName(status)}, {"errors", toJson(errors)}};
    appendEvent(done);
    return done;
}

RunStatus RunFolder::status() const { return status(events()); }

RunStatus RunFolder::status(const QList<QJsonObject> &events) const {
    for (auto it = events.crbegin(); it != events.crend(); ++it)
        if (it->value("event").toString() == QLatin1String("run.done"))
            return parseRunStatus(it->value("status").toString());
    const RunStatus written = parseRunStatus(state().value("status").toString());
    if (written != RunStatus::Unknown || events.isEmpty())
        return written;
    return events.last().value("event").toString() == QLatin1String("run.queued") ? RunStatus::Queued : RunStatus::Running;
}

bool RunFolder::appendEvent(const QJsonObject &event) const {
    QJsonObject line = event;
    if (!line.contains("run"))
        line.insert("run", m_id);
    if (!line.contains("time"))
        line.insert("time", isoTime(utcNow()));
    const QByteArray bytes = compactJson(line) + '\n';

    QMutexLocker lock(&eventsMutex());
    QFile file(eventsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered))
        return false;
    return file.write(bytes) == bytes.size();
}

QList<QJsonObject> RunFolder::events() const {
    QList<QJsonObject> events;
    QFile file(eventsPath());
    if (!file.open(QIODevice::ReadOnly))
        return events;
    const QList<QByteArray> lines = file.readAll().split('\n');
    // The last piece is empty after a final newline, or a torn line without one.
    for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
        const QJsonDocument document = QJsonDocument::fromJson(lines.at(i));
        if (document.isObject())
            events.append(document.object());
    }
    return events;
}

Result<QString> RunFolder::nodeDir(const QString &pathId) const {
    if (!isPathId(pathId))
        return makeError(QStringLiteral("internal"), QStringLiteral("\"%1\" is not a path id.").arg(pathId));
    const QString path = m_path + QStringLiteral("/nodes/") + pathId;
    if (!QDir().mkpath(path))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create %1.").arg(path));
    return path;
}

} // namespace fugue
