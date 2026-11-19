#include "engine/queue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "engine/json.h"

namespace fugue {

namespace {

QString stateRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/fugue");
}

QString fileName(int queueClass, qint64 millis, const QString &run) {
    return QStringLiteral("%1-%2-%3").arg(queueClass).arg(millis, 14, 10, QLatin1Char('0')).arg(run);
}

// Only whole entries: not the lock file, and not the temporary file an entry is written
// through before it is renamed into place.
bool isEntryName(const QString &file) {
    static const QRegularExpression form(QStringLiteral("^[035]-[0-9]{14}-[a-z0-9-]+$"));
    return form.match(file).hasMatch();
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

// The folder's own lock, held for one change. It blocks: every holder is quick.
class Guard {
public:
    explicit Guard(const QString &dir) {
        QDir().mkpath(dir);
        m_fd = ::open(QFile::encodeName(dir + QStringLiteral("/.lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd >= 0)
            while (::flock(m_fd, LOCK_EX) != 0 && errno == EINTR) {
            }
    }
    ~Guard() {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

private:
    int m_fd = -1;
};

} // namespace

QString Queue::defaultDir() { return stateRoot() + QStringLiteral("/queue"); }
QString Queue::defaultLockPath() { return stateRoot() + QStringLiteral("/engine.lock"); }

// The on-disk form of 4.6, replaced atomically so a reader never sees half an entry.
bool Queue::write(const QString &file, const QJsonObject &content) const {
    if (!QDir().mkpath(m_dir))
        return false;
    QSaveFile out(m_dir + '/' + file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    const QByteArray bytes = writeJson(content);
    if (out.write(bytes) != bytes.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

bool Queue::enqueue(const QString &run, const QString &workflow, bool first) const {
    const Guard guard(m_dir);
    return write(fileName(first ? 3 : 5, QDateTime::currentMSecsSinceEpoch(), run),
                 QJsonObject{{"run", run}, {"pid", 0}, {"workflow", workflow}});
}

QList<QueueEntry> Queue::entries() const {
    QList<QueueEntry> list;
    const QStringList files = QDir(m_dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &file : files)
        if (isEntryName(file))
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
    const Guard guard(m_dir);
    QueueEntry entry;
    // Only an entry that is there is rewritten: one that was cancelled meanwhile must stay
    // gone. The guard keeps a removal from slipping in between the check and the write.
    if (!find(run, &entry))
        return false;
    return write(entry.file, QJsonObject{{"run", run}, {"pid", double(pid)}, {"workflow", entry.workflow}});
}

bool Queue::activate(const QString &run) const {
    const Guard guard(m_dir);
    QueueEntry entry;
    if (!find(run, &entry))
        return false;
    if (entry.active)
        return true;
    // 0-<time>-<run>: the time part stays, only the class changes.
    return QFile::rename(m_dir + '/' + entry.file, m_dir + '/' + QStringLiteral("0") + entry.file.mid(1));
}

bool Queue::remove(const QString &run) const {
    const Guard guard(m_dir);
    QueueEntry entry;
    return find(run, &entry) && QFile::remove(m_dir + '/' + entry.file);
}

bool Queue::remove(const QueueEntry &seen) const {
    const Guard guard(m_dir);
    return isEntryName(seen.file) && QFile::remove(m_dir + '/' + seen.file);
}

QStringList Queue::discardDead(Lock &engineLock) const {
    QStringList gone;
    for (const QueueEntry &entry : entries()) {
        if (entry.active)
            continue;
        bool dead = false;
        if (entry.pid > 0) {
            dead = !isRunWorker(entry.pid, entry.run);
        } else {
            // A worker records its pid as soon as it runs; an entry that never got one
            // is from a worker that did not start.
            dead = QFileInfo(m_dir + '/' + entry.file).lastModified().secsTo(QDateTime::currentDateTime()) > 30;
        }
        if (dead && remove(entry))
            gone << entry.run;
    }
    // A worker takes the engine lock before it activates its entry and holds it until it
    // exits, so whoever can take the lock knows that every active entry is left over.
    const bool held = engineLock.held();
    if (engineLock.tryAcquire()) {
        for (const QueueEntry &entry : entries())
            if (entry.active && remove(entry))
                gone << entry.run;
        if (!held)
            engineLock.release();
    }
    return gone;
}

bool isRunWorker(qint64 pid, const QString &run) {
    if (pid <= 0 || run.isEmpty())
        return false;
    QFile file(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return false; // no such process
    const QList<QByteArray> arguments = file.readAll().split('\0');
    const qsizetype at = arguments.indexOf("__worker");
    return at >= 0 && at + 1 < arguments.size() && arguments.at(at + 1) == run.toUtf8();
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

void Lock::release() {
    if (m_held)
        ::flock(m_fd, LOCK_UN);
    m_held = false;
}

} // namespace fugue
