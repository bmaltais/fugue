// Video and audio handles (spec 2.4): file-backed, probed with ffprobe, video
// frames streamed through ffmpeg with pending trim and crop applied on the way.
//
// Seams: MediaHandle::probe for video and audio, VideoFrames, and
// MediaHandle::plainFile for a video with pending edits. Fixtures are made
// here with ffmpeg's lavfi sources, so no media is committed. Memory is the
// process's peak resident size (VmHWM), reset before each measured step.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/media.h"
#include "engine/video_frames.h"

using namespace fugue;
using vips::VImage;

namespace {

bool ffmpeg(const QStringList &args) {
    QProcess process;
    process.start(QStringLiteral("ffmpeg"), QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args);
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning().noquote() << "ffmpeg failed:" << process.readAllStandardError();
        return false;
    }
    return true;
}

qint64 procStatus(const char *field) {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &line : status.readAll().split('\n')) {
        if (line.startsWith(field))
            return line.mid(qstrlen(field)).trimmed().split(' ').first().toLongLong() * 1024;
    }
    return -1;
}

void resetPeak() {
    QFile refs(QStringLiteral("/proc/self/clear_refs"));
    QVERIFY(refs.open(QIODevice::WriteOnly));
    QVERIFY(refs.write("5") == 1);
}

std::vector<double> pixel(VImage image, int x, int y) {
    return image.getpoint(x, y);
}

bool near(const std::vector<double> &actual, const std::vector<double> &expected, double tolerance) {
    if (actual.size() != expected.size())
        return false;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (std::abs(actual[i] - expected[i]) > tolerance)
            return false;
    }
    return true;
}

} // namespace

class TestMediaVideo : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_clip;    // 1280x720, 25 fps, 10 s, with a tone: 691 MiB as RGB frames
    QString m_split;   // 64x32, 10 fps, 1 s, left half red and right half blue, lossless
    QString m_rotated; // m_split, flagged to display turned 90 degrees
    QString m_tone;    // 1 s of 440 Hz, 48 kHz stereo
    QString m_still;   // a PNG

    QString path(const char *name) const { return m_dir.filePath(QString::fromLatin1(name)); }

private slots:
    void initTestCase() {
        QVERIFY2(!QStandardPaths::findExecutable("ffmpeg").isEmpty(), "ffmpeg is needed to make the fixtures");
        m_clip = path("clip.mp4");
        m_split = path("split.mkv");
        m_rotated = path("rotated.mp4");
        m_tone = path("tone.wav");
        m_still = path("still.png");
        QVERIFY(ffmpeg({"-f", "lavfi", "-i", "testsrc=size=1280x720:rate=25:duration=10", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=48000:duration=10", "-c:v", "mpeg4", "-q:v", "8", "-c:a",
                        "aac", "-shortest", m_clip}));
        QVERIFY(ffmpeg({"-f", "lavfi", "-i", "color=c=red:size=32x32:rate=10:duration=1", "-f", "lavfi", "-i",
                        "color=c=blue:size=32x32:rate=10:duration=1", "-filter_complex", "[0][1]hstack",
                        "-pix_fmt", "yuv444p", "-c:v", "ffv1", m_split}));
        QVERIFY(ffmpeg({"-display_rotation", "90", "-i", m_split, "-c", "copy", m_rotated}));
        QVERIFY(ffmpeg({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1", "-ac", "2", m_tone}));
        QVERIFY(ffmpeg({"-f", "lavfi", "-i", "color=c=green:size=8x8", "-frames:v", "1", m_still}));
    }

    void probingAVideoReadsItsStreams() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> clip = MediaHandle::probe(MediaType::Video, m_clip, hashes);
        QVERIFY2(clip.ok(), qPrintable(clip.error().message));
        QCOMPARE(clip->type(), MediaType::Video);
        QCOMPARE(clip->path(), m_clip);
        QCOMPARE(clip->hash(), sha256File(m_clip));
        QCOMPARE(clip->format(), QStringLiteral("mp4"));
        QCOMPARE(clip->width(), 1280);
        QCOMPARE(clip->height(), 720);
        QCOMPARE(clip->fps(), 25.0);
        QCOMPARE(clip->frames(), 250);
        QVERIFY2(std::abs(clip->duration() - 10) < 0.1, qPrintable(QString::number(clip->duration())));
        QVERIFY(clip->hasAudio());
        QVERIFY(clip->edits().isEmpty());

        // Matroska has no frame count in its header; it is counted from packets.
        const Result<MediaHandle> split = MediaHandle::probe(MediaType::Video, m_split, hashes);
        QVERIFY2(split.ok(), qPrintable(split.error().message));
        QCOMPARE(split->format(), QStringLiteral("matroska"));
        QCOMPARE(split->frames(), 10);
        QCOMPARE(split->fps(), 10.0);
        QVERIFY(!split->hasAudio());
    }

    void aTurnedVideoHasTheSizeItIsShownAt() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> rotated = MediaHandle::probe(MediaType::Video, m_rotated, hashes);
        QVERIFY2(rotated.ok(), qPrintable(rotated.error().message));
        QCOMPARE(rotated->width(), 32);
        QCOMPARE(rotated->height(), 64);
        Result<VideoFrames> frames = VideoFrames::open(*rotated);
        QVERIFY2(frames.ok(), qPrintable(frames.error().message));
        const std::optional<VImage> frame = frames->next();
        QVERIFY2(frame.has_value(), qPrintable(frames->error().message));
        QCOMPARE(frame->width(), 32);
        QCOMPARE(frame->height(), 64);
    }

    void probingAudioReadsItsStream() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> tone = MediaHandle::probe(MediaType::Audio, m_tone, hashes);
        QVERIFY2(tone.ok(), qPrintable(tone.error().message));
        QCOMPARE(tone->type(), MediaType::Audio);
        QCOMPARE(tone->format(), QStringLiteral("wav"));
        QCOMPARE(tone->channels(), 2);
        QCOMPARE(tone->sampleRate(), 48000);
        QVERIFY(std::abs(tone->duration() - 1) < 0.01);
        QCOMPARE(tone->hash(), sha256File(m_tone));
    }

    void probingTheWrongKindOfFileFails() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> toneAsVideo = MediaHandle::probe(MediaType::Video, m_tone, hashes);
        QVERIFY(!toneAsVideo.ok());
        QCOMPARE(toneAsVideo.error().code, QStringLiteral("media_unreadable"));
        const Result<MediaHandle> stillAsAudio = MediaHandle::probe(MediaType::Audio, m_still, hashes);
        QVERIFY(!stillAsAudio.ok());
        QCOMPARE(stillAsAudio.error().code, QStringLiteral("media_unreadable"));
        const Result<MediaHandle> missing = MediaHandle::probe(MediaType::Video, path("none.mp4"), hashes);
        QCOMPARE(missing.error().code, QStringLiteral("file_not_found"));
    }

    void framesStreamWithoutLoadingTheVideo() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> clip = MediaHandle::probe(MediaType::Video, m_clip, hashes);
        QVERIFY(clip.ok());
        const qint64 decoded = qint64(1280) * 720 * 3 * 250;

        resetPeak();
        qint64 before = procStatus("VmRSS:");
        qint64 count = 0;
        {
            Result<VideoFrames> frames = VideoFrames::open(*clip);
            QVERIFY2(frames.ok(), qPrintable(frames.error().message));
            QCOMPARE(frames->width(), 1280);
            QCOMPARE(frames->height(), 720);
            while (const std::optional<VImage> frame = frames->next()) {
                QCOMPARE(frame->width(), 1280);
                QCOMPARE(frame->bands(), 3);
                QCOMPARE(frame->format(), VIPS_FORMAT_UCHAR);
                ++count;
            }
            QVERIFY2(!frames->failed(), qPrintable(frames->error().message));
        }
        QCOMPARE(count, 250);
        const qint64 peak = procStatus("VmHWM:") - before;
        QVERIFY2(peak < decoded / 16, qPrintable(QStringLiteral("streaming peaked %1 MiB above start").arg(peak >> 20)));

        // Stopping early ends ffmpeg at once rather than decoding the rest.
        QElapsedTimer timer;
        timer.start();
        {
            Result<VideoFrames> frames = VideoFrames::open(*clip);
            QVERIFY(frames.ok());
            for (int i = 0; i < 3; ++i)
                QVERIFY(frames->next().has_value());
        }
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
    }

    void framesHonourPendingTrimAndCrop() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> clip = MediaHandle::probe(MediaType::Video, m_clip, hashes);
        QVERIFY(clip.ok());
        const Result<MediaHandle> trimmed = clip->trimmed(0.5, 1.5);
        QVERIFY(trimmed.ok());
        Result<VideoFrames> frames = VideoFrames::open(*trimmed);
        QVERIFY2(frames.ok(), qPrintable(frames.error().message));
        int count = 0;
        while (frames->next())
            ++count;
        QVERIFY(!frames->failed());
        QCOMPARE(count, 25);

        const Result<MediaHandle> split = MediaHandle::probe(MediaType::Video, m_split, hashes);
        QVERIFY(split.ok());
        const Result<MediaHandle> right = split->cropped(32, 0, 32, 32);
        QVERIFY(right.ok());
        Result<VideoFrames> blue = VideoFrames::open(*right);
        QVERIFY2(blue.ok(), qPrintable(blue.error().message));
        QCOMPARE(blue->width(), 32);
        const std::optional<VImage> frame = blue->next();
        QVERIFY(frame.has_value());
        QCOMPARE(frame->width(), 32);
        QCOMPARE(frame->height(), 32);
        // Away from the left edge, where converting colours blurs red into blue.
        QVERIFY(near(pixel(*frame, 8, 8), {0, 0, 255}, 8));
        QVERIFY(near(pixel(*frame, 31, 31), {0, 0, 255}, 8));
    }

    void savingKeepsEditsPendingAndAPlainFileAppliesThem() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> clip = MediaHandle::probe(MediaType::Video, m_clip, hashes);
        QVERIFY(clip.ok());
        const Result<MediaHandle> edited = clip->trimmed(1, 2);
        QVERIFY(edited.ok());
        const Result<MediaHandle> cut = edited->cropped(0, 0, 640, 360);
        QVERIFY(cut.ok());
        const QString node = path("nodes/cut");

        const Result<MediaHandle> saved = cut->saved(node, "video", hashes);
        QVERIFY(saved.ok());
        QCOMPARE(saved->path(), m_clip);
        QVERIFY(!saved->edits().isEmpty());
        QVERIFY(!QDir(node).exists());

        const Result<MediaHandle> plain = cut->plainFile(node, "video", hashes);
        QVERIFY2(plain.ok(), qPrintable(plain.error().message));
        QCOMPARE(plain->path(), node + "/video.mkv");
        QVERIFY(plain->edits().isEmpty());
        QCOMPARE(plain->width(), 640);
        QCOMPARE(plain->height(), 360);
        QCOMPARE(plain->frames(), 25);
        QVERIFY2(std::abs(plain->duration() - 1) < 0.05, qPrintable(QString::number(plain->duration())));
        QVERIFY(plain->hasAudio());
        QCOMPARE(plain->hash(), sha256File(plain->path()));
        QCOMPARE(QDir(node).entryList(QDir::Files | QDir::Hidden), QStringList{"video.mkv"});

        const Result<MediaHandle> unedited = clip->plainFile(node, "other", hashes);
        QVERIFY(unedited.ok());
        QCOMPARE(unedited->path(), m_clip);
    }

    void onlyVideosHaveFrames() {
        FileHashes hashes(path("hashes.json"));
        const Result<MediaHandle> tone = MediaHandle::probe(MediaType::Audio, m_tone, hashes);
        QVERIFY(tone.ok());
        QVERIFY(!VideoFrames::open(*tone).ok());
    }
};

QTEST_GUILESS_MAIN(TestMediaVideo)
#include "test_media_video.moc"
