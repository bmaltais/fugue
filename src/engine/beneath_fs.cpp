#include "engine/beneath_fs.h"

#include <QFile>
#include <QRandomGenerator>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how {
    std::uint64_t flags;
    std::uint64_t mode;
    std::uint64_t resolve;
};
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#endif

#ifndef SYS_openat2
#define SYS_openat2 437 // the same number on every architecture
#endif

namespace fugue::beneath {

namespace {

thread_local int t_errno = 0;

int remember(int error) {
    t_errno = error;
    return -1;
}

int openat2Call(int dir, const char *path, int flags, mode_t mode, std::uint64_t resolve) {
    open_how how{};
    how.flags = std::uint64_t(flags | O_CLOEXEC);
    how.mode = (flags & (O_CREAT | O_TMPFILE)) ? std::uint64_t(mode) : 0;
    how.resolve = resolve;
    for (;;) {
        const long fd = ::syscall(SYS_openat2, dir, path, &how, sizeof how);
        if (fd >= 0)
            return int(fd);
        if (errno == EAGAIN) // the kernel saw a rename or mount race in the walk and asks to try again
            continue;
        return remember(errno);
    }
}

constexpr std::uint64_t kBeneath = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
constexpr std::uint64_t kNoLinks = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

bool plainName(const QByteArray &name) { return !name.isEmpty() && name != "." && name != ".." && !name.contains('/'); }

// Copies `from` to `to` until the end of `from`.
bool copyBytes(int from, int to) {
    // copy_file_range first (a reflink or an in-kernel copy), then read and write when the kernel or the file systems refuse it.
    for (;;) {
        const ssize_t done = ::copy_file_range(from, nullptr, to, nullptr, 1 << 30, 0);
        if (done == 0)
            return true;
        if (done > 0)
            continue;
        if (errno == EINTR)
            continue;
        if (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP || errno == EBADF)
            break;
        return remember(errno), false;
    }
    char buffer[1 << 16];
    for (;;) {
        const ssize_t got = ::read(from, buffer, sizeof buffer);
        if (got == 0)
            return true;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return remember(errno), false;
        }
        for (ssize_t written = 0; written < got;) {
            const ssize_t put = ::write(to, buffer + written, size_t(got - written));
            if (put < 0) {
                if (errno == EINTR)
                    continue;
                return remember(errno), false;
            }
            written += put;
        }
    }
}

// A regular file, opened non-blocking (a FIFO would wait for a writer), then made blocking again for the copy.
Fd regularOnly(int fd) {
    if (fd < 0)
        return Fd();
    Fd owned(fd);
    struct stat info {};
    if (::fstat(owned.get(), &info) != 0)
        return remember(errno), Fd();
    if (!S_ISREG(info.st_mode))
        return remember(EINVAL), Fd();
    const int flags = ::fcntl(owned.get(), F_GETFL);
    if (flags >= 0)
        ::fcntl(owned.get(), F_SETFL, flags & ~O_NONBLOCK);
    return owned;
}

// The new file `name` (a plain name) in `dir`, copied from `from`; removed again when the copy fails.
bool copyInto(int from, int dir, const QByteArray &relative) {
    const int out = openat2Call(dir, relative.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOCTTY, 0600, kBeneath);
    if (out < 0)
        return false;
    Fd owned(out);
    if (copyBytes(from, owned.get()))
        return true;
    const int saved = t_errno;
    // The half copy: removed through the folder it was made in (its parent, beneath `dir`).
    const qsizetype slash = relative.lastIndexOf('/');
    if (slash < 0) {
        ::unlinkat(dir, relative.constData(), 0);
    } else {
        const int parent = openat2Call(dir, relative.left(slash).constData(), O_RDONLY | O_DIRECTORY, 0, kBeneath);
        if (parent >= 0) {
            ::unlinkat(parent, relative.mid(slash + 1).constData(), 0);
            ::close(parent);
        }
    }
    t_errno = saved;
    return false;
}

constexpr int kMaxDepth = 128; // deeper than any folder a program makes in earnest; a deeper one is left for the next sweep

// Removes the entry `name` of the folder `parent`, a tree when it is a folder, never following a link.
bool removeEntry(int parent, const QByteArray &name, int depth) {
    struct stat info {};
    if (::fstatat(parent, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT || (remember(errno), false);
    if (!S_ISDIR(info.st_mode))
        return ::unlinkat(parent, name.constData(), 0) == 0 || errno == ENOENT || (remember(errno), false);
    if (depth > kMaxDepth)
        return remember(ELOOP), false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int sub = openat2Call(parent, name.constData(), O_RDONLY | O_DIRECTORY, 0, kBeneath);
        if (sub < 0) {
            // Swapped for a link or a file since the stat: it goes as what it is now.
            if (::unlinkat(parent, name.constData(), 0) == 0 || errno == ENOENT)
                return true;
            if (errno != EISDIR)
                return remember(errno), false;
            continue;
        }
        Fd folder(sub);
        std::vector<QByteArray> entries;
        if (DIR *listing = ::fdopendir(::fcntl(folder.get(), F_DUPFD_CLOEXEC, 0))) {
            while (const dirent *entry = ::readdir(listing)) {
                const QByteArray entryName(entry->d_name);
                if (entryName != "." && entryName != "..")
                    entries.push_back(entryName);
            }
            ::closedir(listing);
        }
        for (const QByteArray &entry : entries)
            removeEntry(folder.get(), entry, depth + 1); // what could not go makes the folder's own removal fail below
        if (::unlinkat(parent, name.constData(), AT_REMOVEDIR) == 0 || errno == ENOENT)
            return true;
        if (errno != ENOTEMPTY && errno != EEXIST)
            return remember(errno), false;
        // Something was added meanwhile (the program is still at work): once more.
    }
    return remember(ENOTEMPTY), false;
}

} // namespace

Fd::~Fd() {
    if (m_fd >= 0)
        ::close(m_fd);
}

Fd &Fd::operator=(Fd &&other) noexcept {
    if (this != &other) {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = other.release();
    }
    return *this;
}

Fd openBeneath(int dir, const QString &relative, int flags, mode_t mode) {
    const QByteArray path = QFile::encodeName(relative);
    if (path.isEmpty() || path.startsWith('/'))
        return remember(EINVAL), Fd();
    return Fd(openat2Call(dir, path.constData(), flags, mode, kBeneath));
}

Fd openNoLinks(const QString &absolute, int flags) {
    const QByteArray path = QFile::encodeName(absolute);
    if (!path.startsWith('/'))
        return remember(EINVAL), Fd();
    return Fd(openat2Call(AT_FDCWD, path.constData(), flags, 0, kNoLinks));
}

QString realPathOf(int fd) {
    if (fd < 0)
        return {};
    char target[4096];
    const QByteArray link = "/proc/self/fd/" + QByteArray::number(fd);
    const ssize_t size = ::readlink(link.constData(), target, sizeof target - 1);
    if (size <= 0)
        return {};
    const QByteArray path(target, size);
    if (!path.startsWith('/') || path.endsWith(" (deleted)"))
        return {};
    return QFile::decodeName(path);
}

bool available() {
    static const bool works = [] {
        const int fd = openat2Call(AT_FDCWD, "/", O_PATH | O_DIRECTORY, 0, kNoLinks);
        if (fd < 0)
            return false;
        ::close(fd);
        return true;
    }();
    return works;
}

QString lastProblem() {
    if (t_errno == ELOOP || t_errno == EXDEV)
        return QStringLiteral("a link, or a path that leaves its folder");
    if (t_errno == ENOSYS)
        return QStringLiteral("this kernel has no openat2 (Linux 5.6 or newer is needed)");
    return QString::fromLocal8Bit(std::strerror(t_errno));
}

void inheritOnly(const QList<int> &fds) {
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
    if (::syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) != 0) {
        const long most = ::sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < (most > 0 && most < 65536 ? most : 65536); ++fd)
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    for (const int fd : fds)
        ::fcntl(fd, F_SETFD, 0);
}

int dupCloexec(int fd) { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }

bool mkdirBeneath(int dir, const QString &name) {
    const QByteArray bytes = QFile::encodeName(name);
    if (!plainName(bytes))
        return remember(EINVAL), false;
    return ::mkdirat(dir, bytes.constData(), 0700) == 0 || (remember(errno), false);
}

QString mkdirBeneathRandom(int dir) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        quint64 bits[2];
        QRandomGenerator::system()->fillRange(bits);
        const QString name = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(bits), sizeof bits).toHex());
        if (::mkdirat(dir, name.toLatin1().constData(), 0700) == 0)
            return name;
        if (errno != EEXIST)
            return remember(errno), QString();
    }
    return remember(EEXIST), QString();
}

Fd openRegularFile(const QString &absolute) {
    return regularOnly(openNoLinks(absolute, O_RDONLY | O_NONBLOCK | O_NOCTTY).release());
}

bool copyInBeneath(int from, int dir, const QString &relative) {
    const QByteArray path = QFile::encodeName(relative);
    if (path.isEmpty() || path.startsWith('/'))
        return remember(EINVAL), false;
    return copyInto(from, dir, path);
}

bool copyOutBeneath(int dir, const QString &relative, int to, const QString &name) {
    const QByteArray target = QFile::encodeName(name);
    if (!plainName(target))
        return remember(EINVAL), false;
    const Fd source = regularOnly(openBeneath(dir, relative, O_RDONLY | O_NONBLOCK | O_NOCTTY).release());
    if (!source)
        return false;
    return copyInto(source.get(), to, target);
}

bool removeTreeBeneath(int dir, const QString &relative) {
    const QByteArray path = QFile::encodeName(relative);
    if (path.isEmpty() || path.startsWith('/'))
        return remember(EINVAL), false;
    const qsizetype slash = path.lastIndexOf('/');
    if (slash < 0)
        return plainName(path) ? removeEntry(dir, path, 0) : (remember(EINVAL), false);
    const QByteArray name = path.mid(slash + 1);
    if (!plainName(name))
        return remember(EINVAL), false;
    const int parent = openat2Call(dir, path.left(slash).constData(), O_RDONLY | O_DIRECTORY, 0, kBeneath);
    if (parent < 0)
        return t_errno == ENOENT;
    Fd owned(parent);
    return removeEntry(owned.get(), name, 0);
}

} // namespace fugue::beneath
