#include "cli/queue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fugue::cli {

namespace {

QString stateRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/fugue");
}

QString fileName(int queueClass, qint64 millis, const QString &run) {
    return QStringLiteral("%1-%2-%3").arg(queueClass).arg(millis, 14, 10, QLatin1Char('0')).arg(run);
}

QueueEntry parse(const QString &file, const QString &dir) {
    QueueEntry entry;
    entry.file = file;
    entry.active = file.startsWith(QLatin1String("0-"));
    QFile handle(dir + '/' + file);
    if (handle.open(QIODevice::ReadOnly)) {
        const QJsonObject json = QJsonDocument::fromJson(handle.readAll()).object();
        entry.run = json.value("run").toString();
        entry.pid = qint64(json.value("pid").toDouble());
        entry.workflow = json.value("workflow").toString();
    }
    if (entry.run.isEmpty()) // fall back to the name's last part
        entry.run = file.section('-', 2);
    return entry;
}

} // namespace

QString Queue::defaultDir() { return stateRoot() + QStringLiteral("/queue"); }
QString Queue::defaultLockPath() { return stateRoot() + QStringLiteral("/engine.lock"); }

bool Queue::write(const QString &file, const QJsonObject &content) const {
    if (!QDir().mkpath(m_dir))
        return false;
    QSaveFile out(m_dir + '/' + file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(content).toJson(QJsonDocument::Compact));
    return out.commit();
}

bool Queue::enqueue(const QString &run, const QString &workflow, bool first) const {
    return write(fileName(first ? 3 : 5, QDateTime::currentMSecsSinceEpoch(), run),
                 QJsonObject{{"run", run}, {"pid", 0}, {"workflow", workflow}});
}

QList<QueueEntry> Queue::entries() const {
    QList<QueueEntry> list;
    const QStringList files = QDir(m_dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &file : files)
        if (file.size() > 2 && file.at(1) == QLatin1Char('-'))
            list.append(parse(file, m_dir));
    return list;
}

bool Queue::find(const QString &run, QueueEntry *out) const {
    for (const QueueEntry &entry : entries()) {
        if (entry.run == run) {
            if (out)
                *out = entry;
            return true;
        }
    }
    return false;
}

bool Queue::setPid(const QString &run, qint64 pid) const {
    QueueEntry entry;
    if (!find(run, &entry))
        return false;
    // Rewritten in place, never created: an entry that was cancelled meanwhile must stay gone.
    QFile file(m_dir + '/' + entry.file);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::ExistingOnly))
        return false;
    return file.write(QJsonDocument(QJsonObject{{"run", run}, {"pid", double(pid)}, {"workflow", entry.workflow}})
                          .toJson(QJsonDocument::Compact)) > 0;
}

bool Queue::activate(const QString &run) const {
    QueueEntry entry;
    if (!find(run, &entry))
        return false;
    if (entry.active)
        return true;
    // 0-<time>-<run>: the time part stays, only the class changes.
    return QFile::rename(m_dir + '/' + entry.file, m_dir + '/' + QStringLiteral("0") + entry.file.mid(1));
}

bool Queue::remove(const QString &run) const {
    QueueEntry entry;
    return find(run, &entry) && QFile::remove(m_dir + '/' + entry.file);
}

QStringList Queue::discardStale() const {
    QStringList gone;
    for (const QueueEntry &entry : entries()) {
        bool stale = false;
        if (entry.pid > 0) {
            stale = !processExists(entry.pid);
        } else {
            // The worker's pid is recorded just after it is spawned; an entry that
            // never got one is from a spawn that failed.
            stale = QFileInfo(m_dir + '/' + entry.file).lastModified().secsTo(QDateTime::currentDateTime()) > 30;
        }
        if (stale && QFile::remove(m_dir + '/' + entry.file))
            gone << entry.run;
    }
    return gone;
}

bool processExists(qint64 pid) {
    return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM);
}

Lock::Lock(const QString &path) {
    QDir().mkpath(QFileInfo(path).path());
    m_fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
}

Lock::~Lock() {
    if (m_fd >= 0)
        ::close(m_fd); // releases the lock
}

bool Lock::tryAcquire() {
    if (m_fd < 0)
        return false;
    if (m_held)
        return true;
    m_held = ::flock(m_fd, LOCK_EX | LOCK_NB) == 0;
    return m_held;
}

} // namespace fugue::cli
