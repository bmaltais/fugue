#include "engine/queue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/pidfd.h>
#include <unistd.h>

#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

// The time on CLOCK_BOOTTIME, in milliseconds: one clock for every process, not moved by
// changes to the wall clock, and counting time suspended. It starts again at each boot.
qint64 bootMs() {
    timespec now{};
    ::clock_gettime(CLOCK_BOOTTIME, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
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
    if (const Result<QJsonObject> json = readJsonObject(dir + '/' + file)) {
        entry.run = json->value("run").toString();
        entry.pid = qint64(json->value("pid").toDouble());
        entry.workflow = json->value("workflow").toString();
        entry.queuedBootMs = qint64(json->value("queued_boottime_ms").toDouble());
        entry.inProcess = json->value("owner").toString() == QLatin1String("window");
        entry.processStart = qint64(json->value("process_start").toDouble());
    }
    if (entry.run.isEmpty()) // unreadable: the name's last part still says which run
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

QString Queue::defaultDir() { return stateDir() + QStringLiteral("/queue"); }
QString Queue::defaultLockPath() { return stateDir() + QStringLiteral("/engine.lock"); }

// The on-disk form of 4.6, replaced atomically so a reader never sees half an entry.
bool Queue::write(const QString &file, const QJsonObject &content) const {
    return QDir().mkpath(m_dir) && writeJsonFile(m_dir + '/' + file, content).isEmpty();
}

bool Queue::enqueue(const QString &run, const QString &workflow, bool first) const {
    const Guard guard(m_dir);
    return write(fileName(first ? 3 : 5, QDateTime::currentMSecsSinceEpoch(), run),
                 QJsonObject{{"run", run}, {"pid", 0}, {"queued_boottime_ms", double(bootMs())}, {"workflow", workflow}});
}

bool Queue::enqueueForThisProcess(const QString &run, const QString &workflow, bool first) const {
    const Guard guard(m_dir);
    const qint64 pid = qint64(::getpid());
    return write(fileName(first ? 3 : 5, QDateTime::currentMSecsSinceEpoch(), run),
                 QJsonObject{{"run", run}, {"pid", double(pid)}, {"owner", "window"},
                             {"process_start", double(processStartTime(pid))},
                             {"queued_boottime_ms", double(bootMs())}, {"workflow", workflow}});
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
    // Only the pid changes; what else the entry holds (the time it was queued) stays.
    const Result<QJsonObject> read = readJsonObject(m_dir + '/' + entry.file);
    QJsonObject content = read ? read.value() : QJsonObject{{"run", run}, {"workflow", entry.workflow}};
    content.insert("pid", double(pid));
    return write(entry.file, content);
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

bool Queue::moveToFront(const QString &run) const {
    const Guard guard(m_dir);
    QList<QueueEntry> all = entries();
    const auto self = std::find_if(all.begin(), all.end(), [&](const QueueEntry &e) { return e.run == run; });
    if (self == all.end() || self->active)
        return false;
    // The earliest time among the other entries of class 3: one before it sorts first. With none
    // there, class 3 alone puts the entry ahead of the class 5 ones.
    qint64 earliest = self->file.mid(2, 14).toLongLong();
    bool others = false;
    for (const QueueEntry &entry : all) {
        if (&entry == &*self || entry.file.startsWith(QLatin1String("0-")) || !entry.file.startsWith(QLatin1String("3-")))
            continue;
        const qint64 at = entry.file.mid(2, 14).toLongLong();
        earliest = others ? std::min(earliest, at) : at;
        others = true;
    }
    if (!others)
        return self->file.startsWith(QLatin1String("3-"))
               || QFile::rename(m_dir + '/' + self->file, m_dir + '/' + fileName(3, earliest, run));
    // Already ahead of the rest of class 3: nothing to rename.
    if (self->file.startsWith(QLatin1String("3-")) && self->file.mid(2, 14).toLongLong() < earliest)
        return true;
    return QFile::rename(m_dir + '/' + self->file, m_dir + '/' + fileName(3, std::max<qint64>(0, earliest - 1), run));
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
    const qint64 self = qint64(::getpid());
    const qint64 started = processStartTime(self);
    const auto mine = [self, started](const QueueEntry &entry) {
        return entry.inProcess && entry.pid == self && entry.processStart == started;
    };
    for (const QueueEntry &entry : entries()) {
        if (entry.active || mine(entry))
            continue;
        bool dead = false;
        if (entry.inProcess) {
            dead = entry.pid <= 0 || processStartTime(entry.pid) != entry.processStart;
        } else if (entry.pid > 0) {
            dead = !isRunWorker(entry.pid, entry.run);
        } else if (entry.queuedBootMs > 0) {
            // A worker records its pid as soon as it runs; an entry that never got one
            // is from a worker that did not start. Its age is on the boot clock, so a jump
            // of the wall clock cannot discard a worker that is still starting; a time
            // later than now was taken before the machine restarted.
            const qint64 now = bootMs();
            dead = now < entry.queuedBootMs || now - entry.queuedBootMs > 30000;
        } else {
            // An entry from before the boot time was recorded: the file's time.
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
            if (entry.active && !mine(entry) && remove(entry))
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

qint64 processStartTime(qint64 pid) {
    if (pid <= 0)
        return 0;
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so the fields are
    // counted from the last ')'. starttime is field 22, the 20th after it.
    const QByteArray stat = file.readAll();
    const QList<QByteArray> fields = stat.mid(stat.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 19 ? fields.at(19).toLongLong() : 0;
}

bool signalRunWorker(qint64 pid, const QString &run, int signal) {
    if (pid <= 0)
        return false;
    // While the pidfd is open the id cannot pass to another process, so the command line
    // read next is this process's. If it died meanwhile, the send fails (ESRCH).
    const int process = ::pidfd_open(pid_t(pid), 0);
    if (process < 0)
        return false; // no such process
    const bool sent = isRunWorker(pid, run) && ::pidfd_send_signal(process, signal, nullptr, 0) == 0;
    ::close(process);
    return sent;
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
