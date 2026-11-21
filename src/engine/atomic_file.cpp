#include "atomic_file.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <unistd.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace fugue {

namespace {

// Unique among the partial files of this process; the process id sets it apart from others'.
QString uniquePart() {
    static std::atomic<quint64> next{0};
    return QStringLiteral("fugue-%1-%2").arg(::getpid()).arg(next.fetch_add(1));
}

// The 13.3 code for a failed write, from its errno: a missing folder is file_not_found, a
// refusal is permission_denied, and anything else (a full disk) is node_failed, the code
// for a failure no other code fits. The message names the cause.

Error writeFailed(const QString &target, int error) {
    return makeError(writeFailureCode(error),
                     QStringLiteral("Cannot write %1: %2.").arg(target, QString::fromLocal8Bit(std::strerror(error))));
}

// `.<name>.fugue-<pid>-<n>.part`, with `name` shortened so the whole stays within a file name's
// limit (NAME_MAX, 255 bytes): the name is only there for a person reading the folder.
QString partialName(QString name) {
    const QString unique = uniquePart();
    const auto fits = [&] { return QFile::encodeName(QStringLiteral(".%1.%2.part").arg(name, unique)).size() <= 255; };
    while (!name.isEmpty() && !fits())
        name.chop(name.size() >= 2 && name.at(name.size() - 1).isLowSurrogate() ? 2 : 1);
    return QStringLiteral(".%1.%2.part").arg(name, unique);
}

// Puts what was written to `path` (a file or a folder) on the disk, so a rename after it
// never leaves an empty file behind a power loss. errno on failure.
bool syncToDisk(const QString &path, bool folder) {
    const int fd = ::open(QFile::encodeName(path).constData(), (folder ? O_RDONLY | O_DIRECTORY : O_WRONLY) | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = ::fsync(fd) == 0;
    const int error = errno;
    ::close(fd);
    errno = error;
    return synced;
}

// The file a link at `path` stands for (a dotfile manager's), followed to its end, so it is
// that file that is replaced and the link stays. `path` itself when it is no link.
QString linkedFile(const QString &path) {
    QFileInfo info(path);
    for (int hops = 0; hops < 40 && info.isSymLink(); ++hops)
        info = QFileInfo(info.symLinkTarget());
    return info.absoluteFilePath();
}

} // namespace

QString writeFailureCode(int error) {
    if (error == ENOENT || error == ENOTDIR)
        return QStringLiteral("file_not_found");
    if (error == EACCES || error == EPERM || error == EROFS)
        return QStringLiteral("permission_denied");
    return QStringLiteral("node_failed");
}

Errors makeFolder(const QString &dir) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(QFile::encodeName(dir).toStdString()), error);
    if (!error)
        return {};
    return {makeError(writeFailureCode(error.value()),
                      QStringLiteral("Cannot create the folder %1: %2.").arg(dir, QString::fromStdString(error.message())))};
}

PartialFile::PartialFile(const QString &dir, const QString &name) : m_path(QDir(dir).filePath(partialName(name))) {}

void PartialFile::removeStale(const QString &dir) {
    static const QRegularExpression ours(QStringLiteral("^\\..*\\.fugue-(\\d+)-\\d+\\.part$"));
    const QDateTime before = QDateTime::currentDateTimeUtc().addSecs(-600);
    const QFileInfoList found = QDir(dir).entryInfoList({QStringLiteral(".*.fugue-*.part")}, QDir::Files | QDir::Hidden);
    for (const QFileInfo &file : found) {
        const QRegularExpressionMatch match = ours.match(file.fileName());
        if (!match.hasMatch() || file.lastModified() > before)
            continue;
        bool number = false;
        const qint64 pid = match.captured(1).toLongLong(&number);
        if (!number || pid <= 0 || pid > std::numeric_limits<pid_t>::max())
            continue;
        if (::kill(pid_t(pid), 0) == 0 || errno != ESRCH)
            continue; // its writer is alive (or not ours to ask)
        QFile::remove(file.filePath());
    }
}

PartialFile::~PartialFile() {
    if (!m_committed)
        QFile::remove(m_path);
}

Errors PartialFile::write(const QByteArray &bytes, const QString &what) const {
    QFile out(m_path);
    errno = 0;
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush())
        return {writeFailed(what, errno != 0 ? errno : EIO)};
    return {};
}

Result<bool> PartialFile::commit(const QString &target, Overwrite overwrite) {
    const QByteArray from = QFile::encodeName(m_path);
    const QByteArray to = QFile::encodeName(target);
    if (overwrite == Overwrite::Replace) {
        if (std::rename(from.constData(), to.constData()) != 0)
            return writeFailed(target, errno);
        m_committed = true;
        return true;
    }
    // Claimed in the same step as the move, so a writer that found the name free a moment
    // before cannot take it too.
    if (::renameat2(AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0) {
        m_committed = true;
        return true;
    }
    if (errno == EEXIST)
        return false;
    if (errno != EINVAL && errno != ENOSYS)
        return writeFailed(target, errno);
    // A filesystem without RENAME_NOREPLACE: a hard link is made only when the name is free.
    if (::link(from.constData(), to.constData()) != 0)
        return errno == EEXIST ? Result<bool>(false) : Result<bool>(writeFailed(target, errno));
    QFile::remove(m_path);
    m_committed = true;
    return true;
}

Errors replaceFile(const QString &path, const QByteArray &bytes, const QString &what, FileMode mode) {
    const QFileInfo info(linkedFile(path));
    if (Errors errors = makeFolder(info.absolutePath()); !errors.isEmpty())
        return errors;
    PartialFile partial(info.absolutePath(), info.fileName());
    if (mode == FileMode::OwnerOnly) {
        // Made 0600 before anything is in it, so the content is never readable by others.
        const QByteArray name = QFile::encodeName(partial.path());
        const int fd = ::open(name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            return {writeFailed(what, errno)};
        ::close(fd);
    }
    if (Errors errors = partial.write(bytes, what); !errors.isEmpty())
        return errors;
    if (!syncToDisk(partial.path(), false))
        return {writeFailed(what, errno)};
    const Result<bool> committed = partial.commit(info.absoluteFilePath(), Overwrite::Replace);
    if (!committed)
        return committed.errors();
    syncToDisk(info.absolutePath(), true); // the new name; the content is safe either way
    return {};
}

Errors createFile(const QString &path, const QByteArray &bytes, const QString &what) {
    const QFileInfo info(path);
    const auto taken = [&] {
        return Errors{makeError(QStringLiteral("permission_denied"),
                                QStringLiteral("%1 is there already; Fugue does not replace it.").arg(path),
                                QStringLiteral("Pick another name."))};
    };
    if (info.exists() || info.isSymLink())
        return taken();
    if (Errors errors = makeFolder(info.absolutePath()); !errors.isEmpty())
        return errors;
    PartialFile partial(info.absolutePath(), info.fileName());
    if (Errors errors = partial.write(bytes, what); !errors.isEmpty())
        return errors;
    const Result<bool> committed = partial.commit(info.absoluteFilePath(), Overwrite::Never);
    if (!committed)
        return committed.errors();
    return *committed ? Errors() : taken();
}

} // namespace fugue
