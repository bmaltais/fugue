#include "run_folder.h"

#include <algorithm>
#include <atomic>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include "engine/json.h"

namespace fugue {

namespace {

bool isRunId(const QString &id) {
    static const QRegularExpression form(QStringLiteral("^[a-z0-9-]{1,64}$"));
    return form.match(id).hasMatch();
}

bool isNodeId(const QString &id) {
    static const QRegularExpression form(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    return form.match(id).hasMatch();
}

bool writeAtomically(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return file.commit();
}

QJsonObject readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

QMutex &eventsMutex() {
    static QMutex mutex;
    return mutex;
}

} // namespace

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
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/fugue/runs");
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
    return writeAtomically(runJsonPath(), writeJson(state));
}

QJsonObject RunFolder::state() const {
    return readObject(runJsonPath());
}

bool RunFolder::writeCompiled(const QJsonObject &compiled) const {
    return writeAtomically(compiledJsonPath(), writeJson(compiled));
}

QJsonObject RunFolder::compiled() const {
    return readObject(compiledJsonPath());
}

bool RunFolder::appendEvent(const QJsonObject &event) const {
    QJsonObject line = event;
    if (!line.contains("run"))
        line.insert("run", m_id);
    if (!line.contains("time"))
        line.insert("time", nowIso());
    const QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';

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
    const QStringList segments = pathId.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!isNodeId(segment))
            return makeError(QStringLiteral("internal"),
                             QStringLiteral("\"%1\" is not a path id.").arg(pathId));
    }
    const QString path = m_path + QStringLiteral("/nodes/") + pathId;
    if (!QDir().mkpath(path))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create %1.").arg(path));
    return path;
}

} // namespace fugue
