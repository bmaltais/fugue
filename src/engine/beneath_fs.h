#pragma once

// File operations on folders a sandboxed program can write in (spec 6.7.1, #731), by descriptor only. The program may swap
// any part of such a folder for a link, at any moment, so nothing here takes a path string from inside it to the kernel
// whole: every open goes through openat2 beneath a folder descriptor with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
// RESOLVE_NO_MAGICLINKS, and every create, copy and removal is an *at call on a descriptor opened that way. A link met
// anywhere fails the open (ELOOP or EXDEV); nothing is ever followed out of the folder.
//
// The same openat2 rules from an absolute path (openNoLinks) open what is bound into a sandbox, so that what was checked is
// what bwrap binds (--ro-bind-fd).

#include <QList>
#include <QString>

#include <sys/types.h>

namespace fugue::beneath {

// A file descriptor that closes itself; -1 when there is none (errno says why the open failed).
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd();
    Fd(Fd &&other) noexcept : m_fd(other.release()) {}
    Fd &operator=(Fd &&other) noexcept;
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    explicit operator bool() const { return valid(); }
    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }

private:
    int m_fd = -1;
};

// openat2(dir, relative, flags | O_CLOEXEC, mode) with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS: `relative`
// must stay beneath `dir` and no component of it, the last included, may be a link. An absolute or empty `relative` fails.
Fd openBeneath(int dir, const QString &relative, int flags, mode_t mode = 0);
// openat2(AT_FDCWD, absolute, flags | O_CLOEXEC) with RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS: no component of the path, the
// last included, may be a link. A relative path fails.
Fd openNoLinks(const QString &absolute, int flags);
// The path the kernel has for `fd` (/proc/self/fd/N), empty when it cannot tell (or the file was deleted).
QString realPathOf(int fd);
// Whether openat2 works here (Linux 5.6 or newer); asked once.
bool available();
// Why the last failure of this module happened, in words, for an error message (strerror of errno, ELOOP as "a link").
QString lastProblem();

// For a child between fork and exec (async-signal-safe): every descriptor from 3 up becomes close-on-exec (close_range,
// Linux 5.11; a loop before that), then exactly `fds` are kept across exec. A descriptor another thread of Fugue opened without
// O_CLOEXEC (or dup()ed) never reaches the program.
void inheritOnly(const QList<int> &fds);
// dup with close-on-exec.
int dupCloexec(int fd);

// A new folder in `dir` named by 128 random bits in hex, mode 0700, never one that exists already; the name, or empty.
QString mkdirBeneathRandom(int dir);
// A new folder `name` (no `/`) in `dir`, mode 0700; false when the name exists already or the folder cannot be made.
bool mkdirBeneath(int dir, const QString &name);

// The most a copy in or out takes: a larger file (or one that grows past it while it is copied) fails the copy.
constexpr qint64 kMaxCopyBytes = qint64(4) << 30;

// Opens the regular file `absolute` for reading through openNoLinks (non-blocking, so a FIFO never hangs the caller), or an
// invalid Fd when it is not a regular file or a component is a link.
Fd openRegularFile(const QString &absolute);
// Copies all of the regular file `from` (a descriptor) into a new file `relative` beneath `dir` (O_CREAT | O_EXCL, 0600, the
// parents must exist). A file that is there already is never replaced. Removes a half-written copy.
bool copyInBeneath(int from, int dir, const QString &relative);
// Copies the regular file `relative` beneath `dir` into a new file `name` (no `/`) in the folder `to` (O_CREAT | O_EXCL,
// 0600). Fails, copying nothing, when `relative` leaves `dir`, meets a link, or is not a regular file (a folder, a FIFO, a
// device, a socket).
bool copyOutBeneath(int dir, const QString &relative, int to, const QString &name);
// Removes `relative` beneath `dir` and everything under it. A link is removed as a link, never followed; a folder is opened
// through openBeneath before its entries are removed. True when nothing is left at `relative`.
bool removeTreeBeneath(int dir, const QString &relative);

} // namespace fugue::beneath
