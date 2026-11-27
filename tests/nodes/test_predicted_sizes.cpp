// The size a picture node says it will make before a run (NodeKernel::outputSizes, #601).
//
// Seam: the registered kernel of each node, called as the scheduler's check calls it (a context with the written
// values and the size of the linked picture) and then as a run does. The guard is that the two agree: a size shown
// before a run must be the size the run makes, whatever the node's rule (Resize's contain, Pad's target, ...).

#include <QTemporaryDir>
#include <QtTest>

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

    void loadImageReadsOnlyTheHeaderAndEmptyLatentsAndSolidMasksKnowTheirNumbers() {
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
        context.inputs = {{"file", dir.filePath("a.png") + "x"}};
        QVERIFY(m_kernels.node("fugue/load-image")->outputSizes(context).isEmpty());

        context.inputs = {{"width", 832}, {"height", 1216}};
        QCOMPARE(m_kernels.node("fugue/empty-latent")->outputSizes(context).value("latent"), QSize(832, 1216));
        QCOMPARE(m_kernels.node("fugue/solid-mask")->outputSizes(context).value("mask"), QSize(832, 1216));
        context.inputs = {{"width", 832}};
        QVERIFY(m_kernels.node("fugue/empty-latent")->outputSizes(context).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPredictedSizes)
#include "test_predicted_sizes.moc"
