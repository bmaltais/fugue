// Image handles (spec 2.4): a file, a decoded libvips image, or both, converted
// on demand.
//
// Seams: MediaHandle::fromImage, MediaHandle::probe, image(), saved() and
// plainFile(). Expected pixels come from the fixtures the tests draw, and
// expected hashes from sha256File of the written file.
// Memory is checked through the process's peak resident size (VmHWM), reset
// before each measured step, so "not decoded whole" is measured, not assumed.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <vector>

#include "engine/atomic_file.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/media.h"
#include "engine/vips.h"

using namespace fugue;
using vips::VImage;

namespace {

// A 3-band uchar image, black but for a red square at (x, y) and a blue one
// at (2x, 2y), built lazily so even a large one is never in memory whole.
VImage drawn(int width, int height, int x, int y) {
    const VImage black = VImage::black(width, height, VImage::option()->set("bands", 3));
    const VImage red = VImage::black(16, 16).new_from_image(std::vector<double>{255, 0, 0});
    const VImage blue = VImage::black(16, 16).new_from_image(std::vector<double>{0, 0, 255});
    return black.insert(red, x, y)
        .insert(blue, 2 * x, 2 * y)
        .cast(VIPS_FORMAT_UCHAR)
        .copy(VImage::option()->set("interpretation", VIPS_INTERPRETATION_sRGB));
}

std::vector<double> pixel(const VImage &image, int x, int y) {
    return const_cast<VImage &>(image).getpoint(x, y);
}

// Peak resident memory of this process since the last reset, in bytes.
qint64 peakResident() {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &line : status.readAll().split('\n')) {
        if (line.startsWith("VmHWM:"))
            return line.mid(6).trimmed().split(' ').first().toLongLong() * 1024;
    }
    return -1;
}

qint64 resident() {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &line : status.readAll().split('\n')) {
        if (line.startsWith("VmRSS:"))
            return line.mid(6).trimmed().split(' ').first().toLongLong() * 1024;
    }
    return -1;
}

// Makes the next peakResident() start from the current resident size.
void resetPeak() {
    QFile refs(QStringLiteral("/proc/self/clear_refs"));
    QVERIFY(refs.open(QIODevice::WriteOnly));
    QVERIFY(refs.write("5") == 1);
}

} // namespace

class TestMediaImage : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(initVips()); }

    void aDecodedImageNeedsNoFile() {
        const Result<MediaHandle> handle = MediaHandle::fromImage(drawn(64, 48, 4, 4));
        QVERIFY2(handle.ok(), qPrintable(handle.error().message));
        QCOMPARE(handle->type(), MediaType::Image);
        QVERIFY(handle->hasImage());
        QVERIFY(!handle->hasFile());
        QCOMPARE(handle->width(), 64);
        QCOMPARE(handle->height(), 48);
        QCOMPARE(handle->channels(), 3);
        const Result<VImage> image = handle->image();
        QVERIFY(image.ok());
        QCOMPARE(pixel(*image, 5, 5), (std::vector<double>{255, 0, 0}));
    }

    // #414: a file probed through the open folder is the file of that folder, whatever is linked at the
    // folder's path afterwards, and a link at the file's own name is refused.
    void probingThroughTheOpenFolderIgnoresALinkSwappedInForIt() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        QVERIFY(QDir(dir.path()).mkpath("in"));
        QVERIFY(QDir(dir.path()).mkpath("other"));
        const Result<MediaHandle> mine = MediaHandle::fromImage(drawn(64, 48, 4, 4));
        const Result<MediaHandle> theirs = MediaHandle::fromImage(drawn(16, 8, 4, 4));
        QVERIFY(mine.ok() && theirs.ok());
        QVERIFY(mine->saved(dir.filePath("in"), "p", hashes).ok());
        QVERIFY(theirs->saved(dir.filePath("other"), "p", hashes).ok());
        const Result<FolderFd> folder = FolderFd::open(dir.filePath("in"), false);
        QVERIFY(folder.ok());
        QVERIFY(QDir().rename(dir.filePath("in"), dir.filePath("in-moved")));
        QVERIFY(QFile::link(dir.filePath("other"), dir.filePath("in")));

        const Result<MediaHandle> probed = MediaHandle::probeIn(MediaType::Image, *folder, "p.png", hashes);
        QVERIFY2(probed.ok(), qPrintable(probed.error().message));
        QCOMPARE(probed->width(), 64);
        QCOMPARE(probed->hash(), sha256File(dir.filePath("in-moved/p.png")));
        QCOMPARE(probed->path(), dir.filePath("in/p.png"));

        QVERIFY(QFile::link(dir.filePath("other/p.png"), dir.filePath("in-moved/l.png")));
        const Result<MediaHandle> linked = MediaHandle::probeIn(MediaType::Image, *folder, "l.png", hashes);
        QVERIFY(!linked.ok());
        QCOMPARE(linked.error().code, QString("permission_denied"));
        QCOMPARE(MediaHandle::probeIn(MediaType::Image, *folder, "none.png", hashes).error().code, QString("file_not_found"));
    }

    void savingWritesALosslessPngNamedAfterTheOutput() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const Result<MediaHandle> memory = MediaHandle::fromImage(drawn(64, 48, 4, 4));
        QVERIFY(memory.ok());
        const Result<MediaHandle> saved = memory->saved(dir.path(), "image", hashes);
        QVERIFY2(saved.ok(), qPrintable(saved.error().message));
        QCOMPARE(saved->path(), dir.filePath("image.png"));
        QCOMPARE(saved->hash(), sha256File(dir.filePath("image.png")));
        QCOMPARE(saved->format(), QStringLiteral("png"));
        QVERIFY(saved->hasImage()); // both forms now
        QVERIFY(saved->hasFile());
        // Nothing else is left behind, such as a partly written file.
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"image.png"});

        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, saved->path(), hashes);
        QVERIFY(probed.ok());
        const Result<VImage> reread = probed->image();
        QVERIFY(reread.ok());
        QCOMPARE(pixel(*reread, 5, 5), (std::vector<double>{255, 0, 0}));
        QCOMPARE(pixel(*reread, 9, 9), (std::vector<double>{0, 0, 255}));
        QCOMPARE(pixel(*reread, 40, 40), (std::vector<double>{0, 0, 0}));
    }

    void samePixelsGiveTheSameBytes() {
        QTemporaryDir a;
        QTemporaryDir b;
        FileHashes hashes(a.filePath("hashes.json"));
        const Result<MediaHandle> first = MediaHandle::fromImage(drawn(64, 48, 4, 4));
        const Result<MediaHandle> second = MediaHandle::fromImage(drawn(64, 48, 4, 4));
        QVERIFY(first.ok() && second.ok());
        const Result<MediaHandle> one = first->saved(a.path(), "image", hashes);
        const Result<MediaHandle> two = second->saved(b.path(), "image", hashes);
        QVERIFY(one.ok() && two.ok());
        QCOMPARE(one->hash(), two->hash());
    }

    void sixteenBitAndFloatImagesAreSavedAsSixteenBit() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const VImage deep = VImage::black(8, 8).cast(VIPS_FORMAT_USHORT).new_from_image(std::vector<double>{1000});
        const Result<MediaHandle> sixteen = MediaHandle::fromImage(deep);
        QVERIFY(sixteen.ok());
        const Result<MediaHandle> savedDeep = sixteen->saved(dir.path(), "deep", hashes);
        QVERIFY2(savedDeep.ok(), qPrintable(savedDeep.error().message));
        const Result<MediaHandle> reread = MediaHandle::probe(MediaType::Image, savedDeep->path(), hashes);
        QVERIFY(reread.ok());
        QCOMPARE(reread->channels(), 1);
        QCOMPARE(reread->image()->format(), VIPS_FORMAT_USHORT);
        QCOMPARE(pixel(*reread->image(), 3, 3), (std::vector<double>{1000}));

        // Float images hold 0..1, as ComfyUI's do; 0.25 is 16383.75 of 65535.
        const VImage working = VImage::black(8, 8, VImage::option()->set("bands", 3))
                                   .cast(VIPS_FORMAT_FLOAT)
                                   .new_from_image(std::vector<double>{0.25, 1.5, -1});
        const Result<MediaHandle> floating = MediaHandle::fromImage(working);
        QVERIFY(floating.ok());
        const Result<MediaHandle> savedFloat = floating->saved(dir.path(), "float", hashes);
        QVERIFY2(savedFloat.ok(), qPrintable(savedFloat.error().message));
        const Result<MediaHandle> rereadFloat = MediaHandle::probe(MediaType::Image, savedFloat->path(), hashes);
        QVERIFY(rereadFloat.ok());
        QCOMPARE(rereadFloat->image()->format(), VIPS_FORMAT_USHORT);
        QCOMPARE(pixel(*rereadFloat->image(), 0, 0), (std::vector<double>{16384, 65535, 0}));
    }

    void aFileIsNeverEncodedAgain() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const QString jpeg = dir.filePath("photo.jpg");
        drawn(64, 48, 4, 4).jpegsave(jpeg.toUtf8().constData());
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, jpeg, hashes);
        QVERIFY2(probed.ok(), qPrintable(probed.error().message));
        QCOMPARE(probed->format(), QStringLiteral("jpeg"));
        QVERIFY(probed->hasFile());
        QVERIFY(!probed->hasImage());
        const Result<MediaHandle> saved = probed->saved(dir.filePath("node"), "image", hashes);
        QVERIFY(saved.ok());
        QCOMPARE(saved->path(), jpeg);
        const Result<MediaHandle> plain = probed->plainFile(dir.filePath("node"), "image", hashes);
        QVERIFY(plain.ok());
        QCOMPARE(plain->path(), jpeg);
        QVERIFY(!QDir(dir.filePath("node")).exists());
    }

    void probingReadsTheHeaderAndTheHash() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const QString path = dir.filePath("in.png");
        drawn(30, 20, 2, 2).pngsave(path.toUtf8().constData());
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, path, hashes);
        QVERIFY2(probed.ok(), qPrintable(probed.error().message));
        QCOMPARE(probed->path(), path);
        QCOMPARE(probed->hash(), sha256File(path));
        QCOMPARE(probed->width(), 30);
        QCOMPARE(probed->height(), 20);
        QCOMPARE(probed->channels(), 3);
        QCOMPARE(probed->format(), QStringLiteral("png"));
        QCOMPARE(pixel(*probed->image(), 3, 3), (std::vector<double>{255, 0, 0}));
    }

    void probingABadFileFails() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const Result<MediaHandle> missing = MediaHandle::probe(MediaType::Image, dir.filePath("none.png"), hashes);
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code, QStringLiteral("file_not_found"));

        QFile text(dir.filePath("notes.png"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("not an image");
        text.close();
        const Result<MediaHandle> unreadable = MediaHandle::probe(MediaType::Image, text.fileName(), hashes);
        QVERIFY(!unreadable.ok());
        QCOMPARE(unreadable.error().code, QStringLiteral("media_unreadable"));
    }

    void aSerializedImageHandleIsUsable() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const QString path = dir.filePath("in.png");
        drawn(30, 20, 2, 2).pngsave(path.toUtf8().constData());
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, path, hashes);
        QVERIFY(probed.ok());
        const Result<MediaHandle> back = MediaHandle::fromJson(probed->toJson());
        QVERIFY(back.ok());
        QCOMPARE(pixel(*back->image(), 3, 3), (std::vector<double>{255, 0, 0}));
    }

    void largeImagesAreProbedAndReadWithoutHoldingThemWhole() {
        // 8192 x 8192 RGB is 192 MiB decoded.
        const int side = 8192;
        const qint64 decoded = qint64(side) * side * 3;
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const QString path = dir.filePath("large.png");
        drawn(side, side, 3000, 3000).pngsave(path.toUtf8().constData(),
                                              VImage::option()->set("compression", 1));

        resetPeak();
        qint64 before = resident();
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, path, hashes);
        QVERIFY2(probed.ok(), qPrintable(probed.error().message));
        QCOMPARE(probed->width(), side);
        QCOMPARE(probed->height(), side);
        QCOMPARE(probed->hash(), sha256File(path));
        QVERIFY2(peakResident() - before < decoded / 8,
                 qPrintable(QStringLiteral("probe peaked %1 MiB above start").arg((peakResident() - before) >> 20)));

        resetPeak();
        before = resident();
        const Result<VImage> image = probed->image();
        QVERIFY(image.ok());
        QCOMPARE(pixel(*image, 3005, 3005), (std::vector<double>{255, 0, 0}));
        QCOMPARE(pixel(*image, 6005, 6005), (std::vector<double>{0, 0, 255}));
        QVERIFY2(peakResident() - before < decoded / 2,
                 qPrintable(QStringLiteral("reading peaked %1 MiB above start").arg((peakResident() - before) >> 20)));
    }

    void aLargeDecodedImageIsSavedWithoutASecondCopy() {
        const int side = 8192;
        const qint64 decoded = qint64(side) * side * 3;
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const Result<MediaHandle> memory = MediaHandle::fromImage(drawn(side, side, 3000, 3000));
        QVERIFY(memory.ok());

        resetPeak();
        const qint64 before = resident();
        const Result<MediaHandle> saved = memory->saved(dir.path(), "image", hashes);
        QVERIFY2(saved.ok(), qPrintable(saved.error().message));
        QVERIFY2(peakResident() - before < decoded / 4,
                 qPrintable(QStringLiteral("saving peaked %1 MiB above start").arg((peakResident() - before) >> 20)));
        QCOMPARE(saved->hash(), sha256File(saved->path()));
        const Result<MediaHandle> reread = MediaHandle::probe(MediaType::Image, saved->path(), hashes);
        QVERIFY(reread.ok());
        QCOMPARE(reread->width(), side);
        QCOMPARE(pixel(*reread->image(), 6005, 6005), (std::vector<double>{0, 0, 255}));
    }
};

QTEST_GUILESS_MAIN(TestMediaImage)
#include "test_media_image.moc"
