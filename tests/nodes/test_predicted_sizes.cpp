// The size a picture node says it will make before a run (NodeKernel::outputSizes, #601).
//
// Seam: the registered kernel of each node, called as the scheduler's check calls it (a context with the written
// values and the size of the linked picture) and then as a run does. The guard is that the two agree: a size shown
// before a run must be the size the run makes, whatever the node's rule (Resize's contain, Pad's target, ...).

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <thread>

#include <vips/vips8>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/vips.h"
#include "nodes/kernels.h"

using namespace fugue;

namespace {

MediaHandle pictureOf(int width, int height) {
    const std::vector<float> pixels(size_t(width) * height * 3, 0.5f);
    const vips::VImage image = vips::VImage::new_from_memory_copy(pixels.data(), pixels.size() * sizeof(float), width, height, 3,
                                                                  VIPS_FORMAT_FLOAT);
    return MediaHandle::fromImage(image).value();
}

} // namespace

class TestPredictedSizes : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    // What the check is told: the written values, and the linked picture's size under `image`.
    PictureSizes predict(const QString &id, Values inputs, const QSize &image) const {
        NodeContext context;
        context.node = "n";
        context.inputs = std::move(inputs);
        if (image.isValid())
            context.inputSizes.insert("image", image);
        return m_kernels.node(id)->outputSizes(context);
    }

    // The sizes the run makes, by output name; the picture's inputs are the real thing.
    QMap<QString, QSize> made(const QString &id, Values inputs, int width, int height) const {
        inputs.insert("image", Value(pictureOf(width, height)));
        NodeContext context;
        context.node = "n";
        context.inputs = std::move(inputs);
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        const Result<Values> result = m_kernels.node(id)->run(context);
        QMap<QString, QSize> sizes;
        if (!result.ok())
            return sizes;
        for (auto it = result.value().cbegin(); it != result.value().cend(); ++it)
            if (it.value().isMedia())
                sizes.insert(it.key(), QSize(it.value().media().width(), it.value().media().height()));
        return sizes;
    }

    // The prediction equals the run's size on the `image` output (and on `mask`, when the node has one).
    void agree(const QString &id, const Values &inputs, int width, int height) const {
        const PictureSizes predicted = predict(id, inputs, QSize(width, height));
        const QMap<QString, QSize> actual = made(id, inputs, width, height);
        QVERIFY2(!actual.isEmpty(), qPrintable(id + " did not run"));
        QVERIFY2(predicted.contains("image"), qPrintable(id + " predicted no size"));
        QCOMPARE(predicted.value("image"), actual.value("image"));
        if (actual.contains("mask") && predicted.contains("mask"))
            QCOMPARE(predicted.value("mask"), actual.value("mask"));
    }

private slots:
    void initTestCase() {
        initVips();
        nodes::addPixelKernels(m_kernels);
        nodes::addBasicKernels(m_kernels);
        nodes::addComponentKernels(m_kernels);
    }

    void resizeFollowsItsRuleForEveryFit() {
        for (const QString fit : {"stretch", "contain", "cover"})
            for (const auto &box : {std::pair{200, 100}, std::pair{0, 80}, std::pair{120, 0}, std::pair{30, 300}, std::pair{0, 0}})
                agree("fugue/resize", {{"width", box.first}, {"height", box.second}, {"fit", fit}, {"method", QString("nearest")}}, 64, 48);
    }

    void megapixelsUpscaleAndCropAgreeWithTheRun() {
        agree("fugue/resize-megapixels", {{"megapixels", 0.01}, {"multiple", 16}, {"method", QString("nearest")}}, 64, 48);
        agree("fugue/upscale-image", {{"scale", 2.5}, {"method", QString("nearest")}}, 33, 21);
        agree("fugue/crop", {{"x", 10}, {"y", 5}, {"width", 100}, {"height", 20}}, 64, 48);
        agree("fugue/crop", {{"x", 500}, {"y", 0}, {"width", 4}, {"height", 4}}, 64, 48);
    }

    void padAgreesForSidesAndForATarget() {
        const Values sides{{"left", 3}, {"top", 4}, {"right", 5}, {"bottom", 6}, {"target_width", 0}, {"target_height", 0},
                           {"fill", QString("edge")}, {"feathering", 0}, {"blur", 0}};
        agree("fugue/pad", sides, 20, 10);
        Values target = sides;
        target.insert("left", 0), target.insert("top", 0), target.insert("right", 0), target.insert("bottom", 0);
        target.insert("target_width", 50), target.insert("target_height", 50);
        agree("fugue/pad", target, 20, 10);
        agree("fugue/pad", target, 200, 100); // shrunk to fit, then centered
    }

    void rotateFlipAndTheSameSizeNodesAgree() {
        agree("fugue/sharpen", {{"radius", 1}, {"sigma", 1.0}, {"amount", 1.0}}, 30, 20);
        agree("fugue/brightness-contrast", {{"brightness", 0.1}, {"contrast", 1.2}}, 30, 20);
        agree("fugue/rotate", {{"rotation", QString("90 degrees")}}, 30, 20);
        agree("fugue/rotate", {{"rotation", QString("180 degrees")}}, 30, 20);
        agree("fugue/rotate", {{"rotation", QString("none")}}, 30, 20);
        agree("fugue/flip", {{"direction", QString("vertically")}}, 30, 20);
        agree("fugue/invert", {}, 30, 20);
        agree("fugue/blur", {{"radius", 2}, {"sigma", 1.0}}, 30, 20);
    }

    // A size that is not known is not guessed: a width wired from a node that has not run, or no picture size at all.
    void whatIsNotKnownIsNotPredicted() {
        QVERIFY(predict("fugue/resize", {{"height", 100}, {"fit", QString("stretch")}}, QSize(64, 48)).isEmpty());
        QVERIFY(predict("fugue/resize", {{"width", 100}, {"height", 100}, {"fit", QString("stretch")}}, QSize()).isEmpty());
        QVERIFY(predict("fugue/crop", {{"x", 0}, {"y", 0}, {"width", 4}}, QSize(64, 48)).isEmpty());
        QVERIFY(predict("fugue/pad", {{"left", 1}, {"top", 0}, {"right", 0}, {"bottom", 0}, {"target_width", 10}, {"target_height", 0}},
                        QSize(8, 8)).isEmpty()); // a target of one side only: the node's own error, no size
        QVERIFY(predict("fugue/blur", {}, QSize()).isEmpty());
    }

    // The check refuses what the run refuses, and only that: a Resize of 0 x 0 passes any picture through, a side derived past
    // the limit fails in both.
    void resizeCheckAndRunRefuseTheSameValues() {
        struct Case {
            int width, height, inWidth, inHeight;
            bool refused;
        };
        for (const Case &c : {Case{0, 0, 17000, 2, false}, Case{0, 16000, 1000, 100, true}, Case{100, 0, 200, 100, false},
                              Case{16000, 0, 100, 1000, true}}) {
            const Values inputs{{"width", c.width}, {"height", c.height}, {"fit", QString("stretch")}, {"method", QString("nearest")}};
            NodeContext context;
            context.node = "n";
            context.inputs = inputs;
            context.inputSizes.insert("image", QSize(c.inWidth, c.inHeight));
            const bool flagged = !m_kernels.node("fugue/resize")->check(context).isEmpty();
            const bool failed = made("fugue/resize", inputs, c.inWidth, c.inHeight).isEmpty();
            QVERIFY2(flagged == c.refused && failed == c.refused, qPrintable(QString("%1 x %2 on %3 x %4").arg(c.width).arg(c.height).arg(c.inWidth).arg(c.inHeight)));
        }
    }

    // A FIFO or a device named as the file is not opened: the check returns at once instead of waiting for a writer.
    void aFifoIsNotOpened() {
        QTemporaryDir dir;
        const QByteArray fifo = QFile::encodeName(dir.filePath("pipe.png"));
        QCOMPARE(mkfifo(fifo.constData(), 0600), 0);
        // On a worker with a timed wait: were the guard removed, the open would block for ever, and this must fail, not hang.
        // A blocked open is let go by opening the pipe's other end ourselves, so the worker can be joined.
        std::promise<std::pair<bool, bool>> done;
        std::future<std::pair<bool, bool>> result = done.get_future();
        const QString pipe = dir.filePath("pipe.png");
        std::thread worker([&done, pipe] {
            const bool fifo = MediaHandle::imageHeaderSize(pipe).has_value();
            done.set_value({fifo, MediaHandle::imageHeaderSize("/dev/null").has_value()});
        });
        const bool inTime = result.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (!inTime)
            ::close(::open(fifo.constData(), O_RDWR | O_NONBLOCK));
        worker.join();
        QVERIFY2(inTime, "imageHeaderSize waited on a FIFO");
        QCOMPARE(result.get(), (std::pair<bool, bool>{false, false}));
    }

    // A FIFO is never opened, not even without blocking: a device (a serial port, a camera) would be touched by an open. Watched
    // for IN_OPEN, which a FIFO node gives as a device node does.
    void aSpecialFileIsNotEvenOpened() {
        QTemporaryDir dir;
        const QByteArray fifo = QFile::encodeName(dir.filePath("pipe.png"));
        QCOMPARE(mkfifo(fifo.constData(), 0600), 0);
        const int watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        QVERIFY(watch >= 0);
        QVERIFY(::inotify_add_watch(watch, fifo.constData(), IN_OPEN) >= 0);
        QVERIFY(!MediaHandle::imageHeaderSize(dir.filePath("pipe.png")).has_value());
        char buffer[256];
        const ssize_t events = ::read(watch, buffer, sizeof buffer);
        ::close(watch);
        QVERIFY2(events < 0, "the FIFO was opened");
    }

    // A format libvips loads only from a file (OpenEXR has no source loader) still has its size predicted, and the check leaves no
    // file open behind it.
    void aFileOnlyFormatHasItsSizeAndLeavesNoDescriptor() {
        QTemporaryDir dir;
        const QString file = dir.filePath("t.exr"); // a 4 x 3 OpenEXR file with no pixels in it: the header is all that is read
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QByteArray::fromHex("762f3101020000006368616e6e656c730063686c697374001300000042000200000000000000010000000100000000636f6d7072657373696f6e00636f6d7072657373696f6e0001000000006461746157696e646f7700626f783269001000000000000000000000000300000002000000646973706c617957696e646f7700626f7832690010000000000000000000000003000000020000006c696e654f72646572006c696e654f72646572000100000000706978656c417370656374526174696f00666c6f617400040000000000803f73637265656e57696e646f7743656e746572007632660008000000000000000000000073637265656e57696e646f77576964746800666c6f617400040000000000803f00210100000000000031010000000000004101000000000000000000000800000000000000000000000100000008000000000000000000000002000000080000000000000000000000"));
        out.close();
        const auto openFiles = [] { return QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot).size(); };
        const int before = openFiles();
        QCOMPARE(MediaHandle::imageHeaderSize(file), std::optional<QSize>(QSize(4, 3)));
        QCOMPARE(openFiles(), before);
        // The same through a source loader: nothing is kept either.
        const QString png = dir.filePath("p.png");
        pictureOf(8, 6).image().value().cast(VIPS_FORMAT_UCHAR).pngsave(QFile::encodeName(png).constData());
        QCOMPARE(MediaHandle::imageHeaderSize(png), std::optional<QSize>(QSize(8, 6)));
        QCOMPARE(openFiles(), before);
    }

    // The memo is keyed on what stat says, not on size and millisecond time alone: a file of the same size, put back to the same
    // modification time, is another file (its change time and the time in nanoseconds differ).
    void aSameSizeSameTimeReplacementIsNotTheOldSize() {
        QTemporaryDir dir;
        const QString file = dir.filePath("p.ppm");
        const auto write = [&](const QByteArray &header) {
            QFile out(file);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(header + QByteArray(36, '\x40')); // 4 x 3 and 3 x 4 pixels of one byte: the same size in bytes
        };
        write("P5\n4 3\n255\n");
        struct stat before;
        QCOMPARE(::stat(QFile::encodeName(file).constData(), &before), 0);
        QCOMPARE(MediaHandle::imageHeaderSize(file), std::optional<QSize>(QSize(4, 3)));
        QTest::qWait(20); // past the kernel's coarse clock, so the change time moves
        write("P5\n3 4\n255\n");
        const struct timespec times[2] = {before.st_atim, before.st_mtim};
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(file).constData(), times, 0), 0);
        QCOMPARE(MediaHandle::imageHeaderSize(file), std::optional<QSize>(QSize(3, 4)));
    }

    // An EXIF turn of a quarter swaps the sides: the size is the one the picture is seen at, as the run's handle says.
    void anExifTurnIsApplied() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        for (const int orientation : {1, 5, 6, 7, 8}) {
            const QString file = dir.filePath(QString("o%1.jpg").arg(orientation));
            vips::VImage image = pictureOf(40, 20).image().value().cast(VIPS_FORMAT_UCHAR);
            image = image.copy();
            image.set("orientation", orientation);
            image.jpegsave(QFile::encodeName(file).constData());
            const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Image, file, hashes);
            QVERIFY(probed.ok());
            QCOMPARE(MediaHandle::imageHeaderSize(file), std::optional<QSize>(QSize(probed->width(), probed->height())));
            if (orientation >= 5)
                QCOMPARE(MediaHandle::imageHeaderSize(file), std::optional<QSize>(QSize(20, 40)));
        }
    }

    // A size past 16384 on a side is the node's early error, not a size to show.
    void aSizePastTheLimitIsAnErrorNotASize() {
        NodeContext context;
        context.node = "n";
        context.inputs = {{"scale", 8.0}, {"method", QString("nearest")}};
        context.inputSizes.insert("image", QSize(4000, 100));
        const Errors errors = m_kernels.node("fugue/upscale-image")->check(context);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QString("value_out_of_range"));
        QCOMPARE(errors.first().input, QString("scale"));
        QVERIFY(errors.first().message.contains("32000 x 800"));
        QVERIFY(m_kernels.node("fugue/upscale-image")->outputSizes(context).isEmpty());
        context.inputSizes.insert("image", QSize(400, 100));
        QVERIFY(m_kernels.node("fugue/upscale-image")->check(context).isEmpty());
        QCOMPARE(m_kernels.node("fugue/upscale-image")->outputSizes(context).value("image"), QSize(3200, 800));

        // Resize: one side 0 follows the picture's shape, which can pass the limit on the other.
        context.inputs = {{"width", 0}, {"height", 16000}, {"fit", QString("stretch")}, {"method", QString("nearest")}};
        context.inputSizes.insert("image", QSize(1000, 100));
        QCOMPARE(m_kernels.node("fugue/resize")->check(context).size(), 1);
    }

    // A picture that is only passed on keeps its size past 16384 (a 20000 px file works through Blur and Invert); the node that
    // would make a new picture that big has no size to show and refuses it in its check (#671).
    void aPassedOnPictureKeepsItsSizePastTheLimit() {
        const QSize wide(20000, 10);
        QCOMPARE(predict("fugue/blur", {{"radius", 2}, {"sigma", 1.0}}, wide).value("image"), wide);
        QCOMPARE(predict("fugue/invert", {}, wide).value("image"), wide);
        QCOMPARE(predict("fugue/flip", {{"direction", QString("vertically")}}, wide).value("image"), wide);
        QCOMPARE(predict("fugue/rotate", {{"rotation", QString("90 degrees")}}, wide).value("image"), QSize(10, 20000));
        QCOMPARE(predict("fugue/crop", {{"x", 0}, {"y", 0}, {"width", 18000}, {"height", 5}}, wide).value("image"), QSize(18000, 5));
        QVERIFY(predict("fugue/upscale-image", {{"scale", 2.0}, {"method", QString("nearest")}}, wide).isEmpty());
    }

    void loadImageKnowsItsFilesSizeAndEmptyLatentsAndSolidMasksKnowTheirNumbers() {
        QTemporaryDir dir;
        FileHashes hashes(dir.filePath("hashes.json"));
        const Result<MediaHandle> saved = pictureOf(37, 21).saved(dir.path(), "a", hashes);
        QVERIFY(saved.ok());
        const QString file = saved->path();
        NodeContext context;
        context.node = "n";
        context.inputs = {{"file", file}};
        const PictureSizes loaded = m_kernels.node("fugue/load-image")->outputSizes(context);
        QCOMPARE(loaded.value("image"), QSize(37, 21));
        context.inputs = {{"file", dir.filePath("gone.png")}};
        QVERIFY(m_kernels.node("fugue/load-image")->outputSizes(context).isEmpty());
        // Bytes that are not a picture: no size (and, remembered, not read again until the file changes).
        QFile junk(dir.filePath("junk.png"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("this is not a picture");
        junk.close();
        context.inputs = {{"file", junk.fileName()}};
        QVERIFY(m_kernels.node("fugue/load-image")->outputSizes(context).isEmpty());
        QVERIFY(!MediaHandle::imageHeaderSize(junk.fileName()));
        // The file is replaced by a picture: the memo follows its size and time.
        QVERIFY(QFile::remove(junk.fileName()));
        QVERIFY(QFile::copy(file, junk.fileName()));
        QCOMPARE(MediaHandle::imageHeaderSize(junk.fileName()), std::optional<QSize>(QSize(37, 21)));

        context.inputs = {{"width", 832}, {"height", 1216}};
        QCOMPARE(m_kernels.node("fugue/empty-latent")->outputSizes(context).value("latent"), QSize(832, 1216));
        QCOMPARE(m_kernels.node("fugue/solid-mask")->outputSizes(context).value("mask"), QSize(832, 1216));
        context.inputs = {{"width", 832}};
        QVERIFY(m_kernels.node("fugue/empty-latent")->outputSizes(context).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPredictedSizes)
#include "test_predicted_sizes.moc"
