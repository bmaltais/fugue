#include "video_frames.h"

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

#include <QByteArrayList>
#include <QFile>
#include <QTemporaryFile>

#include "engine/ffmpeg.h"
#include "engine/vips.h"

extern char **environ;

namespace fugue {

namespace {

Error failure(const QString &message) {
    return makeError(QStringLiteral("media_unreadable"), message);
}

} // namespace

Result<VideoFrames> VideoFrames::open(const MediaHandle &video) {
    if (video.type() != MediaType::Video)
        return makeError(QStringLiteral("type_mismatch"),
                         QStringLiteral("Only a video has frames, not %1.").arg(mediaTypeName(video.type())));
    if (!video.hasFile() || video.effectiveWidth() <= 0 || video.effectiveHeight() <= 0)
        return makeError(QStringLiteral("internal"), QStringLiteral("This video handle has no file or no size."));
    if (!initVips())
        return makeError(QStringLiteral("internal"), QStringLiteral("libvips could not be started."));
    const Result<QString> program = ffmpeg::program(QStringLiteral("ffmpeg"));
    if (!program)
        return program.error();

    QStringList args{"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"};
    args << ffmpeg::trimArgs(video.edits()) << "-i" << video.path() << "-map" << "0:v:0";
    const QString crop = ffmpeg::cropFilter(video.edits());
    if (!crop.isEmpty())
        args << "-vf" << crop;
    // passthrough: every decoded frame once, none duplicated or dropped to
    // fit a constant rate.
    args << "-fps_mode" << "passthrough" << "-f" << "rawvideo" << "-pix_fmt" << "rgb24" << "pipe:1";

    // ffmpeg's log goes to a file, so a chatty ffmpeg can never block on a
    // full stderr pipe that nobody reads.
    QTemporaryFile log;
    log.setAutoRemove(false);
    if (!log.open())
        return makeError(QStringLiteral("internal"), QStringLiteral("Cannot create a temporary file."));
    const QByteArray logPath = QFile::encodeName(log.fileName());
    log.close();

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        QFile::remove(log.fileName());
        return makeError(QStringLiteral("internal"), QStringLiteral("Cannot create a pipe for ffmpeg."));
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, logPath.constData(), O_WRONLY | O_TRUNC, 0);

    QByteArrayList encoded;
    for (const QString &arg : args)
        encoded << arg.toUtf8();
    std::vector<char *> argv;
    for (QByteArray &arg : encoded)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const QByteArray path = QFile::encodeName(*program);
    const int spawned = posix_spawn(&pid, path.constData(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (spawned != 0) {
        ::close(fds[0]);
        QFile::remove(log.fileName());
        return makeError(QStringLiteral("internal"), QStringLiteral("ffmpeg could not be started."));
    }

    VideoFrames frames;
    frames.m_width = video.effectiveWidth();
    frames.m_height = video.effectiveHeight();
    frames.m_pid = pid;
    frames.m_fd = fds[0];
    frames.m_stderrPath = log.fileName();
    return Result<VideoFrames>(std::move(frames));
}

VideoFrames::VideoFrames(VideoFrames &&other) noexcept
    : m_width(other.m_width), m_height(other.m_height), m_pid(other.m_pid), m_fd(other.m_fd),
      m_stderrPath(std::move(other.m_stderrPath)), m_error(std::move(other.m_error)) {
    other.m_pid = -1;
    other.m_fd = -1;
    other.m_stderrPath.clear();
}

VideoFrames &VideoFrames::operator=(VideoFrames &&other) noexcept {
    if (this != &other) {
        stop();
        m_width = other.m_width;
        m_height = other.m_height;
        m_pid = other.m_pid;
        m_fd = other.m_fd;
        m_stderrPath = std::move(other.m_stderrPath);
        m_error = std::move(other.m_error);
        other.m_pid = -1;
        other.m_fd = -1;
        other.m_stderrPath.clear();
    }
    return *this;
}

VideoFrames::~VideoFrames() {
    stop();
}

std::optional<vips::VImage> VideoFrames::next() {
    if (m_fd < 0)
        return std::nullopt;
    const qint64 size = qint64(m_width) * m_height * 3;
    QByteArray frame(size, Qt::Uninitialized);
    qint64 got = 0;
    while (got < size) {
        const ssize_t n = ::read(m_fd, frame.data() + got, size_t(size - got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            m_error = failure(QStringLiteral("Reading frames from ffmpeg failed."));
            stop();
            return std::nullopt;
        }
        if (n == 0)
            break;
        got += n;
    }
    if (got < size) {
        finish();
        if (got > 0 && !failed())
            m_error = failure(QStringLiteral("The video ended in the middle of a frame."));
        return std::nullopt;
    }
    try {
        return vips::VImage::new_from_memory_copy(frame.constData(), size_t(size), m_width, m_height, 3,
                                                  VIPS_FORMAT_UCHAR)
            .copy(vips::VImage::option()->set("interpretation", VIPS_INTERPRETATION_sRGB));
    } catch (const vips::VError &error) {
        m_error = vipsError(error);
        stop();
        return std::nullopt;
    }
}

// ffmpeg closed its output: reap it and learn whether it succeeded.
void VideoFrames::finish() {
    ::close(m_fd);
    m_fd = -1;
    int status = 0;
    while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
    }
    m_pid = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        QFile log(m_stderrPath);
        const QString detail = log.open(QIODevice::ReadOnly) ? ffmpeg::tail(log.readAll()) : QString();
        m_error = failure(detail.isEmpty() ? QStringLiteral("ffmpeg could not decode the video.") : detail);
    }
    QFile::remove(m_stderrPath);
    m_stderrPath.clear();
}

// Ends ffmpeg early, without waiting for it to decode the rest.
void VideoFrames::stop() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_pid > 0) {
        ::kill(m_pid, SIGKILL);
        while (::waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {
        }
        m_pid = -1;
    }
    if (!m_stderrPath.isEmpty()) {
        QFile::remove(m_stderrPath);
        m_stderrPath.clear();
    }
}

} // namespace fugue
