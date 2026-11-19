#include "atomic_file.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <QDir>
#include <QFile>

namespace fugue {

namespace {

// Unique among the partial files of this process; the process id sets it apart from others'.
QString uniquePart() {
    static std::atomic<quint64> next{0};
    return QStringLiteral("%1-%2").arg(::getpid()).arg(next.fetch_add(1));
}

Error moveFailed(const QString &target, int error) {
    return makeError(QStringLiteral("permission_denied"),
                     QStringLiteral("Cannot write %1: %2.").arg(target, QString::fromLocal8Bit(std::strerror(error))));
}

} // namespace

PartialFile::PartialFile(const QString &dir, const QString &name)
    : m_path(QDir(dir).filePath(QStringLiteral(".%1.%2.part").arg(name, uniquePart()))) {}

PartialFile::~PartialFile() {
    if (!m_committed)
        QFile::remove(m_path);
}

Errors PartialFile::write(const QByteArray &bytes, const QString &what) const {
    QFile out(m_path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush())
        return {makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot write %1.").arg(what))};
    return {};
}

Result<bool> PartialFile::commit(const QString &target, Overwrite overwrite) {
    const QByteArray from = QFile::encodeName(m_path);
    const QByteArray to = QFile::encodeName(target);
    if (overwrite == Overwrite::Replace) {
        if (std::rename(from.constData(), to.constData()) != 0)
            return moveFailed(target, errno);
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
        return moveFailed(target, errno);
    // A filesystem without RENAME_NOREPLACE: a hard link is made only when the name is free.
    if (::link(from.constData(), to.constData()) != 0)
        return errno == EEXIST ? Result<bool>(false) : Result<bool>(moveFailed(target, errno));
    QFile::remove(m_path);
    m_committed = true;
    return true;
}

} // namespace fugue
