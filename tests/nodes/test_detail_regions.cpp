// Detail Regions (spec 14.3, #345): the box arithmetic (nodes/detail_ops.h) on numbers and float
// buffers; the node's kernel driven by hand as the scheduler drives a node of several jobs
// (request, then next after each job, then outputs), with files written as the helper writes
// them; and the shipped `fugue` binary through the fake helper for the jobs a run makes.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include <vips/vips8>

#include "../test_paths.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/media.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/denoise_cut.h"
#include "nodes/detail_ops.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"

using namespace fugue;
using namespace fugue::nodes;

namespace {

const QString kDetail = QStringLiteral("fugue/detail-regions");

QJsonObject modelFile(const QString &type, const QString &file) {
    const QByteArray content = file.toUtf8() + " weights";
    const QString path = dataDir() + "/models/" + file;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(content) != content.size())
        qFatal("cannot write %s", qPrintable(path));
    return QJsonObject{{"resource", type}, {"hash", sha256(content)}, {"name", QFileInfo(file).fileName()}};
}

// Klein's three files, and the model Load Models makes of them.
QJsonObject kleinParts() {
    return QJsonObject{{"diffusion_model", modelFile("fugue/diffusion-model", "diffusion_models/flux-2-klein-9b-Q8_0.gguf")},
                       {"text_encoder", modelFile("fugue/text-encoder", "clip/Qwen3-8B-Q8_0.gguf")},
                       {"vae", modelFile("fugue/vae", "vae/flux2-vae.safetensors")}};
}

Selection helperSelection() {
    Selection chosen;
    chosen.instance = QStringLiteral("Fugue helper");
    chosen.engine = QStringLiteral("sdcpp");
    chosen.model.status = ModelStatus::Ready;
    return chosen;
}

void writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

// The ramp the tests read geometry from: 40 + x/2 + y/4 in every colour band (whole numbers), and
// with `alpha` a fourth band of 255 - x. Probed as the scheduler hands a picture over.
int rampAt(int x, int y) { return 40 + x / 2 + y / 4; }
MediaHandle ramp(FileHashes &hashes, const QString &path, int width, int height, bool alpha = false) {
    initVips();
    const int bands = alpha ? 4 : 3;
    std::vector<unsigned char> bytes(size_t(width) * height * bands);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < bands; ++c)
                bytes[(size_t(y) * width + x) * bands + c] = (unsigned char)(c == 3 ? 255 - x : rampAt(x, y));
    vips::VImage::new_from_memory_copy(bytes.data(), bytes.size(), width, height, bands, VIPS_FORMAT_UCHAR)
        .write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, hashes);
    if (!image)
        qFatal("cannot probe %s", qPrintable(path));
    return image.value();
}

// A TIFF: a format the helper cannot read, so a reference of it is converted into the node's folder.
MediaHandle tiff(FileHashes &hashes, const QString &path) {
    initVips();
    (vips::VImage::black(32, 32, vips::VImage::option()->set("bands", 3)) + 80).cast(VIPS_FORMAT_UCHAR).write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, hashes);
    if (!image)
        qFatal("cannot probe %s", qPrintable(path));
    return image.value();
}

QJsonObject readJson(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// What the helper does for a job, as far as the node can tell: the file each stage makes in the node's
// folder, and what each reads is checked. An encode makes a latent with a tag of its own; a sample
// reads the latent it is given (it must be the encode's file) and makes one that names that tag and its
// seed; a decode reads the latent it is given (it must be the sample's file) and makes a gray picture of
// the latent's size whose value comes from the tag: 100 + 60 x tag, so each box is told apart.
struct FakeHelper {
    int encodes = 0;
    QStringList problems;
    QList<QJsonObject> samples;

    JobResult run(const JobRequest &job, const QString &workDir) {
        const QString capability = job.capability;
        if (capability == QLatin1String("latent.decode")) {
            const QString from = job.inputs.value("latent").toObject().value("path").toString();
            const QJsonObject latent = readJson(from);
            if (latent.value("kind").toString() != QLatin1String("sample") || !from.endsWith("latent-sampled.bin"))
                problems << "decode read " + from;
            const QString path = workDir + "/image.png";
            initVips();
            (vips::VImage::black(job.params.value("width").toInt(), job.params.value("height").toInt(), vips::VImage::option()->set("bands", 3)) +
             (100 + 60 * latent.value("from").toInt())).cast(VIPS_FORMAT_UCHAR).write_to_file(path.toUtf8().constData());
            return JobResult{QJsonObject{{"image", QJsonObject{{"path", path}}}}, {}};
        }
        QJsonObject made{{"kind", capability == QLatin1String("latent.encode") ? "encode" : "sample"}};
        if (capability == QLatin1String("latent.encode")) {
            made.insert("tag", ++encodes);
        } else {
            const QString from = job.inputs.value("init_latent").toObject().value("path").toString();
            const QJsonObject encoded = readJson(from);
            if (encoded.value("kind").toString() != QLatin1String("encode") || !from.endsWith("latent-encoded.bin"))
                problems << "sample read " + from;
            made.insert("from", encoded.value("tag").toInt());
            made.insert("seed", job.params.value("seed"));
            samples << job.params;
        }
        const QString path = workDir + "/latent.bin";
        writeBytes(path, QJsonDocument(made).toJson(QJsonDocument::Compact));
        return JobResult{QJsonObject{{"latent", QJsonObject{{"path", path}}}}, {}};
    }
};

} // namespace

class TestDetailRegions : public QObject {
    Q_OBJECT

private slots:
    // --- the arithmetic ---

    void boxesAreFourFiniteNumbersALineAndOnlyABlankLineIsSkipped() {
        std::string problem;
        const auto boxes = parseBoxes("10 20 110.5 220\n\n  5 5 40 40  \n \t \n", &problem);
        QVERIFY(boxes);
        QCOMPARE(boxes->size(), size_t(2));
        QCOMPARE(boxes->at(0).x2, 110.5);
        QVERIFY(parseBoxes("", &problem)->empty());
        for (const char *bad : {"10 20 30\n", "1 2 3 4 5\n", "face 10 20 30 40\n", "# eyes\n", "x1 y1 x2 y2\n", "inf 0 10 10\n",
                                "1 2 3 nan\n", "10 20 30 40\n#\n"}) {
            problem.clear();
            QVERIFY2(!parseBoxes(bad, &problem), bad);
            QVERIFY2(QString::fromStdString(problem).startsWith("line "), bad);
        }
        QVERIFY(QString::fromStdString(problem).startsWith("line 2")); // the line is named
    }

    void aCropIsPaddedAndCutAtTheEdgeAndSmallOnesAreSkipped() {
        QCOMPARE(*detailCrop({100, 100, 200, 180}, 32, 640, 480), (CropBox{68, 68, 164, 144}));
        QCOMPARE(*detailCrop({10, 10, 60, 60}, 32, 640, 480), (CropBox{0, 0, 92, 92})); // cut, not moved
        QCOMPARE(*detailCrop({600, 400, 700, 500}, 32, 640, 480), (CropBox{568, 368, 72, 112}));
        QVERIFY(!detailCrop({100, 100, 110, 140}, 2, 640, 480)); // 14 wide with its padding
        QVERIFY(detailCrop({100, 100, 112, 140}, 2, 640, 480)); // exactly 16
        // A value far outside any picture is held before it is rounded: no overflow, the box just spans the picture.
        QCOMPARE(*detailCrop({-1e300, 0, 1e300, 100}, 32, 640, 480), (CropBox{0, 0, 640, 132}));
        QVERIFY(!detailCrop({1e300, 1e300, 2e300, 2e300}, 32, 640, 480));
    }

    void aThinCropIsGrownInsideThePictureNotStretched() {
        // 1000 x 16 at a target of 1024 would have a short side of 15.6: it grows to 63 about its middle...
        QCOMPARE(widenThin({0, 500, 1000, 16}, 1024, 2000, 1000), (CropBox{0, 477, 1000, 63}));
        // ... moves in at the picture's edge ...
        QCOMPARE(widenThin({0, 0, 1000, 16}, 1024, 2000, 1000), (CropBox{0, 0, 1000, 63}));
        QCOMPARE(widenThin({0, 990, 1000, 10}, 1024, 2000, 1000), (CropBox{0, 937, 1000, 63}));
        // ... stops at the picture's size, and leaves a crop that is thick enough alone.
        QCOMPARE(widenThin({0, 0, 1000, 16}, 1024, 2000, 40), (CropBox{0, 0, 1000, 40}));
        QCOMPARE(widenThin({0, 0, 300, 200}, 1024, 2000, 1000), (CropBox{0, 0, 300, 200}));
        QCOMPARE(widenThin({100, 0, 16, 1000}, 1024, 2000, 1000), (CropBox{77, 0, 63, 1000})); // tall: the same on the other axis
    }

    void aCropIsRedrawnAtTheTargetInTheShapeOfTheCrop() {
        QCOMPARE(detailSize({0, 0, 300, 300}, 1024, 16), (CropSize{1024, 1024}));
        QCOMPARE(detailSize({0, 0, 300, 200}, 1024, 16), (CropSize{1024, 688})); // 682.7 to a step of 16
        QCOMPARE(detailSize({0, 0, 200, 300}, 1024, 16), (CropSize{688, 1024}));
        QCOMPARE(detailSize({0, 0, 1000, 20}, 1024, 16), (CropSize{1024, 64})); // the short side is at least 64
        QCOMPARE(detailSize({0, 0, 300, 200}, 1000, 8), (CropSize{1000, 664}));
    }

    void theSeedAndTheScaledDenoiseFollowTheReference() {
        QCOMPARE(detailSeed(776, 0, 0), qint64(776));
        QCOMPARE(detailSeed(776, 2, 3), qint64(776 + 200000 + 3));
        const double image = 1000.0 * 1000.0;
        QCOMPARE(scaledDenoise(0.3, DenoiseScale::Fixed, 100.0, image), 0.3);
        // A box over half the picture: ratio 1. smaller_less x1, smaller_more x0.4.
        QCOMPARE(scaledDenoise(0.5, DenoiseScale::SmallerLess, image, image), 0.5);
        QCOMPARE(scaledDenoise(0.5, DenoiseScale::SmallerMore, image, image), 0.2);
        // A box under 1%: ratio 0.02. smaller_less x0.412, smaller_more x0.988.
        QCOMPARE(scaledDenoise(0.5, DenoiseScale::SmallerLess, 100.0, image), 0.206);
        QCOMPARE(scaledDenoise(0.5, DenoiseScale::SmallerMore, 100.0, image), 0.494);
        QCOMPARE(scaledDenoise(0.1, DenoiseScale::SmallerMore, image, image), 0.05); // floor
        QCOMPARE(scaledDenoise(1.0, DenoiseScale::SmallerMore, 100.0, image), 0.988);
    }

    void aBoxCountsOnlyTheAreaInsideThePicture() {
        QCOMPARE(clampedArea({10, 20, 60, 100}, 640, 480), 50.0 * 80.0);
        QCOMPARE(clampedArea({-100, -100, 50, 50}, 640, 480), 50.0 * 50.0);
        QCOMPARE(clampedArea({600, 400, 700, 500}, 640, 480), 40.0 * 80.0);
        QCOMPARE(clampedArea({-1e300, 0, 1e300, 100}, 640, 480), 640.0 * 100.0);
        QCOMPARE(clampedArea({700, 10, 800, 90}, 640, 480), 0.0);
    }

    void theFeatherRampsOnTheEdgesAskedAndNeverPastHalfTheCrop() {
        const Pixels w = featherWeights(100, 60, 11);
        QCOMPARE(w.at(50, 30, 0), 1.0f);
        QCOMPARE(w.at(0, 30, 0), 0.0f);
        QCOMPARE(w.at(99, 30, 0), 0.0f);
        QCOMPARE(w.at(50, 0, 0), 0.0f);
        QCOMPARE(w.at(50, 59, 0), 0.0f);
        QCOMPARE(w.at(5, 30, 0), 0.5f); // 5 of 10 steps in
        QCOMPARE(w.at(11, 30, 0), 1.0f);
        QCOMPARE(w.at(5, 5, 0), 0.25f); // the two ramps multiply
        // f = min(feather, h / 2, w / 2): 24 on a 30 x 100 crop is 15, on a 100 x 100 one 24.
        QCOMPARE(featherWeights(100, 30, 24).at(7, 15, 0), 0.5f);
        QCOMPARE(featherWeights(100, 30, 24).at(15, 15, 0), 1.0f);
        QCOMPARE(featherWeights(20, 20, 0).at(0, 0, 0), 1.0f);
        // An edge that is not ramped keeps its full weight.
        const Pixels noLeft = featherWeights(100, 60, 11, {false, true, true, true});
        QCOMPARE(noLeft.at(0, 30, 0), 1.0f);
        QCOMPARE(noLeft.at(99, 30, 0), 0.0f);
        QCOMPARE(noLeft.at(0, 0, 0), 0.0f); // the top ramp still reaches the corner
    }

    void aPasteStaysOffTheAlphaAndIsNotRampedAtThePicturesBorder() {
        Pixels canvas(10, 10, 4, 0.0f);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                canvas.at(x, y, 3) = 0.7f;
        pasteFeathered(canvas, {2, 2, 6, 6}, Pixels(6, 6, 4, 1.0f), 2); // a 4-channel redraw: its alpha is not used
        QCOMPARE(canvas.at(5, 5, 0), 1.0f);  // inside
        QCOMPARE(canvas.at(2, 5, 0), 0.0f);  // the crop's edge weighs 0
        QCOMPARE(canvas.at(0, 0, 0), 0.0f);  // outside
        QCOMPARE(canvas.at(5, 5, 3), 0.7f);  // alpha kept
        QCOMPARE(canvas.at(2, 5, 3), 0.7f);
        // A crop on the left and top border: those edges are redrawn in full, the others ramp.
        Pixels edge(10, 10, 3, 0.0f);
        pasteFeathered(edge, {0, 0, 6, 6}, Pixels(6, 6, 3, 1.0f), 2);
        QCOMPARE(edge.at(0, 3, 0), 1.0f);
        QCOMPARE(edge.at(3, 0, 0), 1.0f);
        QCOMPARE(edge.at(0, 0, 0), 1.0f);
        QCOMPARE(edge.at(5, 3, 0), 0.0f);
        QCOMPARE(edge.at(3, 5, 0), 0.0f);
    }

    // --- the kernel, job by job ---

    void aNodeWithNoBoxAsksForNoJobAndGivesThePictureBack() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        NodeContext context = rig.context(image, "");
        const Result<JobRequest> first = rig.kernel->request(context, helperSelection());
        QVERIFY(first);
        QVERIFY(first->capability.isEmpty()); // no model call
        const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), JobResult());
        QVERIFY(more);
        QVERIFY(!more->has_value());
        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        QCOMPARE(out->value("image").media().path(), image.path());
        QVERIFY(out.warnings().isEmpty());
        QVERIFY(rig.workFiles().isEmpty()); // it kept nothing
    }

    void boxesThatAreAllTooSmallAreTheSameAndSayWhy() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        NodeContext context = rig.context(image, "5 5 6 6\n", {{"padding", Value(8)}});
        const Result<JobRequest> first = rig.kernel->request(context, helperSelection());
        QVERIFY(first && first->capability.isEmpty());
        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        QCOMPARE(out.warnings().size(), 1);
        QCOMPARE(out.warnings().first().code, QStringLiteral("box_skipped"));
        QVERIFY(rig.workFiles().isEmpty());
    }

    void everyBoxIsEncodedSampledAndDecodedAndPastedWhereItBelongsBeforeTheNextOne() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 256, 256);
        // Box 1 is too small to redraw; boxes 0 and 2 overlap, so box 2's crop holds box 0's paste.
        // The crops are 12..108 and 72..168 (96 square), redrawn at 192, pasted with a 4 pixel feather.
        NodeContext context = rig.context(image, "20 20 100 100\n5 5 6 6\n80 80 160 160\n",
                                          {{"seed", Value(776)}, {"seed_shift", Value(2)}, {"padding", Value(8)}, {"target_size", Value(192)},
                                           {"feather", Value(4)}, {"steps", Value(4)}, {"denoise", Value(0.3)}});
        FakeHelper helper;
        QStringList sequence;
        QList<Pixels> crops;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        double progressEnd = 0;
        while (true) {
            sequence << job->capability;
            if (job->capability == QLatin1String("latent.encode")) {
                const Result<MediaHandle> crop = MediaHandle::probe(MediaType::Image, job->inputs.value("image").toObject().value("path").toString(), rig.hashes);
                QVERIFY(crop);
                const Result<Pixels> pixels = pixelsOf(Value(crop.value()), "image");
                QVERIFY(pixels);
                crops << pixels.value();
            }
            progressEnd = job->progressFrom + job->progressSpan;
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY2(more, qPrintable(more.error().message));
            if (!more->has_value())
                break;
            job = *more.value();
        }
        QCOMPARE(sequence, (QStringList{"latent.encode", "latent.sample", "latent.decode", "latent.encode", "latent.sample", "latent.decode"}));
        QCOMPARE(progressEnd, 1.0);
        QVERIFY2(helper.problems.isEmpty(), qPrintable(helper.problems.join("; "))); // each stage read the file of the stage before it
        // The seeds: seed + shift * 100000 + the box's place in the list (the skipped one counts).
        QCOMPARE(helper.samples.size(), 2);
        QCOMPARE(helper.samples.at(0).value("seed").toInteger(), qint64(776 + 200000 + 0));
        QCOMPARE(helper.samples.at(1).value("seed").toInteger(), qint64(776 + 200000 + 2));
        // 4 steps at 0.3 are ComfyUI's cut: 13 steps, the last 4 kept (strength 3.5 / 13).
        for (const QJsonObject &sample : helper.samples) {
            QCOMPARE(sample.value("sample.steps").toInt(), 13);
            QCOMPARE(sample.value("width").toInt(), 192); // the crop's size, not the picture's
            QCOMPARE(sample.value("height").toInt(), 192);
            QVERIFY(std::fabs(sample.value("strength").toDouble() - 3.5 / 13) < 1e-6);
            QCOMPARE(sample.value("sample.args").toString(), QStringLiteral("strength_as_noise_level=false"));
            QCOMPARE(sample.value("sample.method").toString(), QStringLiteral("euler"));
            QCOMPARE(sample.value("sample.scheduler").toString(), QStringLiteral("simple"));
        }
        // What was cut: box 0's crop is the picture at 12..108, scaled up; box 2's is the picture as box 0 left it.
        QCOMPARE(crops.size(), 2);
        QCOMPARE(crops.at(0).width, 192);
        QVERIFY(std::fabs(crops.at(0).at(0, 0, 0) * 255.0 - rampAt(12, 12)) < 3.0);
        QVERIFY(std::fabs(crops.at(0).at(191, 191, 0) * 255.0 - rampAt(107, 107)) < 3.0);
        QVERIFY(std::fabs(crops.at(1).at(0, 0, 0) * 255.0 - 160.0) < 3.0);               // inside box 0's redraw (tag 1: 160)
        QVERIFY(std::fabs(crops.at(1).at(191, 191, 0) * 255.0 - rampAt(167, 167)) < 3.0); // beyond it: untouched

        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        QCOMPARE(out.warnings().size(), 1); // the small box
        QCOMPARE(out.warnings().first().code, QStringLiteral("box_skipped"));
        const Result<Pixels> done = pixelsOf(out->value("image"), "image");
        QVERIFY(done);
        QCOMPARE(done->width, 256);
        QCOMPARE(done->height, 256);
        const auto at = [&](int x, int y) { return done->at(x, y, 0) * 255.0; };
        const auto near = [&](double value, double expected) { return std::fabs(value - expected) < 1.5; };
        const auto mixed = [](double before, double redrawn, double weight) { return before * (1 - weight) + redrawn * weight; };
        // Outside every box: as it was.
        QVERIFY(near(at(200, 30), rampAt(200, 30)));
        QVERIFY(near(at(11, 30), rampAt(11, 30)));   // just left of box 0's crop
        QVERIFY(near(at(108, 30), rampAt(108, 30))); // just right of it
        QVERIFY(near(at(30, 108), rampAt(30, 108))); // just under it
        // Box 0's crop: its edge weighs 0, then 1/3, 2/3, then all of the redraw (tag 1 gives 160), on every side.
        QVERIFY(near(at(12, 30), rampAt(12, 30)));
        QVERIFY(near(at(13, 30), mixed(rampAt(13, 30), 160, 1.0 / 3)));
        QVERIFY(near(at(14, 30), mixed(rampAt(14, 30), 160, 2.0 / 3)));
        QVERIFY(near(at(15, 30), 160));
        QVERIFY(near(at(30, 12), rampAt(30, 12)));
        QVERIFY(near(at(30, 13), mixed(rampAt(30, 13), 160, 1.0 / 3)));
        QVERIFY(near(at(107, 30), rampAt(107, 30)));
        QVERIFY(near(at(106, 30), mixed(rampAt(106, 30), 160, 1.0 / 3)));
        QVERIFY(near(at(30, 107), rampAt(30, 107)));
        QVERIFY(near(at(50, 50), 160));
        // Box 2's crop (tag 2 gives 220), over box 0's: its own edge ramps from what box 0 left there.
        QVERIFY(near(at(150, 150), 220));
        QVERIFY(near(at(100, 100), 220));
        QVERIFY(near(at(72, 100), 160));
        QVERIFY(near(at(73, 100), mixed(160, 220, 1.0 / 3)));
        QVERIFY(near(at(167, 150), rampAt(167, 150))); // its right edge
        QVERIFY(near(at(168, 150), rampAt(168, 150)));
        // The node's folder keeps nothing of the work.
        QVERIFY(rig.workFiles().isEmpty());
    }

    void aBoxAtThePicturesBorderIsRedrawnRightUpToIt() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        // The crop is 0..68 across and 92..168 down cut at 128: a left and a bottom edge on the border.
        NodeContext context = rig.context(image, "0 100 60 160\n", {{"padding", Value(8)}, {"target_size", Value(128)}, {"feather", Value(4)}});
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        while (true) {
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY2(more, qPrintable(more.error().message));
            if (!more->has_value())
                break;
            job = *more.value();
        }
        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        const Result<Pixels> done = pixelsOf(out->value("image"), "image");
        QVERIFY(done);
        const auto at = [&](int x, int y) { return done->at(x, y, 0) * 255.0; };
        QVERIFY(std::fabs(at(0, 115) - 160) < 1.5);   // on the left border: the whole redraw
        QVERIFY(std::fabs(at(30, 127) - 160) < 1.5);  // on the bottom border
        QVERIFY(std::fabs(at(67, 115) - rampAt(67, 115)) < 1.5); // the crop's right edge ramps
        QVERIFY(std::fabs(at(30, 92) - rampAt(30, 92)) < 1.5);   // so does its top
    }

    void thePicturesAlphaSurvivesTheRealPath() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128, true);
        NodeContext context = rig.context(image, "20 20 80 80\n", {{"target_size", Value(128)}});
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        while (true) {
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY2(more, qPrintable(more.error().message));
            if (!more->has_value())
                break;
            job = *more.value();
        }
        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        const Result<Pixels> done = pixelsOf(out->value("image"), "image");
        QVERIFY(done);
        QCOMPARE(done->channels, 4);
        for (const QPoint &p : {QPoint(0, 0), QPoint(50, 50), QPoint(30, 40), QPoint(120, 120)})
            QVERIFY(std::fabs(done->at(p.x(), p.y(), 3) * 255.0 - (255 - p.x())) < 1.0);
        QVERIFY(std::fabs(done->at(50, 50, 0) * 255.0 - 160) < 1.5); // and the colour was redrawn
    }

    void theReferencesATextEncodeCarriedAreConvertedAndRemoved() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        const MediaHandle reference = tiff(rig.hashes, rig.dir.filePath("ref.tif"));
        const Value conditioning(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"},
                                             {"meta", QJsonObject{{"references", QJsonArray{reference.toJson()}}}}});
        NodeContext context = rig.context(image, "20 20 80 80\n", {{"conditioning", conditioning}});
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        bool seen = false;
        while (true) {
            if (job->capability == QLatin1String("latent.sample"))
                seen = !QDir(context.workDir).entryList({"reference-*.png"}, QDir::Files).isEmpty();
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY2(more, qPrintable(more.error().message));
            if (!more->has_value())
                break;
            job = *more.value();
        }
        QVERIFY(seen); // the sample did write one
        QVERIFY(rig.kernel->outputs(context, helperSelection(), JobResult()));
        QVERIFY(rig.workFiles().isEmpty());
    }

    void aCancelBetweenBoxesStopsTheNodeAndAbandonLeavesNothing() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        NodeContext context = rig.context(image, "10 10 60 60\n60 60 120 120\n");
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        bool cancelled = false;
        context.cancelled = [&] { return cancelled; };
        for (int i = 0; i < 3; ++i) { // box 0 whole
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY(more && more->has_value());
            job = *more.value();
        }
        QVERIFY(!rig.workFiles().isEmpty()); // the second box's crop and the canvas
        cancelled = true;
        const Result<std::optional<JobRequest>> stopped = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
        QVERIFY(!stopped);
        QCOMPARE(stopped.error().code, QStringLiteral("cancelled"));
        rig.kernel->abandon(context); // the scheduler does this after an error or a cancel
        QVERIFY2(rig.workFiles().isEmpty(), qPrintable(rig.workFiles().join(", ")));
    }

    void aFailedJobLeavesNothingAfterAbandon() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        NodeContext context = rig.context(image, "10 10 60 60\n");
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY(job);
        for (int i = 0; i < 2; ++i) { // encode and sample
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), helper.run(job.value(), context.workDir));
            QVERIFY(more && more->has_value());
            job = *more.value();
        }
        const Result<std::optional<JobRequest>> broken = rig.kernel->next(context, helperSelection(), JobResult()); // a decode with no picture
        QVERIFY(!broken);
        QCOMPARE(broken.error().code, QStringLiteral("node_failed"));
        rig.kernel->abandon(context);
        QVERIFY2(rig.workFiles().isEmpty(), qPrintable(rig.workFiles().join(", ")));
    }

    void aStateThatCannotBeKeptFailsTheNodeAtOnce() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 128, 128);
        NodeContext context = rig.context(image, "10 10 60 60\n");
        FakeHelper helper;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY(job);
        const JobResult encoded = helper.run(job.value(), context.workDir);
        QFile::remove(context.workDir + "/detail.json");
        QDir().mkdir(context.workDir + "/detail.json"); // a folder where the file goes: it cannot be written
        const Result<std::optional<JobRequest>> next = rig.kernel->next(context, helperSelection(), encoded);
        QVERIFY(!next);
        QCOMPARE(next.error().code, QStringLiteral("node_failed"));
        QDir().rmdir(context.workDir + "/detail.json");
        rig.kernel->abandon(context);
    }

    void aBadBoxListIsRefusedBeforeAnyJob() {
        Rig rig;
        const MediaHandle image = ramp(rig.hashes, rig.dir.filePath("in.png"), 64, 64);
        for (const char *bad : {"10 10 20\n", "face 10 10 20 20\n"}) {
            const Result<JobRequest> job = rig.kernel->request(rig.context(image, bad), helperSelection());
            QVERIFY(!job);
            QCOMPARE(job.error().input, QStringLiteral("boxes"));
            QCOMPARE(job.error().code, QStringLiteral("input_invalid"));
        }
    }

    // --- the shipped binary, the fake helper ---

    void aRunMakesThreeJobsAPerBoxAndNoneForNoBox() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("inputs"));
        FileHashes hashes(dir.filePath("h.json"));
        const MediaHandle image = ramp(hashes, dir.filePath("inputs/in.png"), 256, 256);
        const QJsonObject parts = kleinParts();
        const auto node = [](const QString &type, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        const auto from = [](const QString &output) { return QJsonObject{{"from", output}}; };
        QJsonObject detail{{"image", from("load.image")}, {"model", from("models.model")}, {"conditioning", from("encode.conditioning")},
                           {"boxes", "20 20 100 100\n120 120 220 220\n"}, {"seed", 3}};
        const QJsonObject nodes{{"load", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.1.0"}, {"inputs", QJsonObject{{"file", image.path()}}}}},
                                {"models", node("fugue/load-models", parts)},
                                {"encode", node("fugue/text-encode", {{"model", from("models.model")}, {"prompt", "a face"}})},
                                {"detail", node(kDetail, detail)}};
        const QString flow = dir.filePath("flow.fugue");
        writeBytes(flow, QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}).toJson(QJsonDocument::Compact));
        const QString log = dir.filePath("helper.log");

        const QList<QJsonObject> two = fugue(dir, log, {"run", flow, "--to", "detail", "--wait", "--json"});
        QCOMPARE(two.value(two.size() - 1).value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(jobs(log), (QStringList{"text.encode", "latent.encode", "latent.sample", "latent.decode", "latent.encode", "latent.sample", "latent.decode"}));

        QFile::remove(log);
        const QList<QJsonObject> none = fugue(dir, log, {"run", flow, "--to", "detail", "--set", "detail.boxes=", "--wait", "--json"});
        QCOMPARE(none.value(none.size() - 1).value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(jobs(log), QStringList{}); // the conditioning came from the cache, and no box asked for the model
    }

private:
    // A kernel with its inputs as the scheduler fills them.
    struct Rig {
        Rig() : hashes(dir.filePath("hashes.json")) {
            addBuiltinNodeTypes(registry);
            addBuiltinKernels(kernels);
            kernel = kernels.provider(kDetail);
            const QJsonObject parts = kleinParts(); // the model Load Models makes of them
            model = Value(QJsonObject{{"resource", "fugue/model"},
                                      {"derived", QJsonObject{{"base", parts.value("diffusion_model")},
                                                               {"text_encoder", parts.value("text_encoder")},
                                                               {"vae", parts.value("vae")}}}});
            QDir().mkpath(dir.filePath("work"));
        }
        NodeContext context(const MediaHandle &image, const QString &boxes, Values more = {}) {
            Values inputs{{"image", Value(image)}, {"model", model}, {"boxes", Value(boxes)},
                          {"conditioning", Value(QJsonObject{{"data", "fugue/conditioning"}, {"path", "/c.bin"}})}};
            for (auto it = more.begin(); it != more.end(); ++it)
                inputs.insert(it.key(), it.value());
            for (const InputSpec &spec : registry.nodeType(kDetail)->inputs)
                if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                    inputs.insert(spec.name, Value(spec.defaultValue));
            NodeContext context;
            context.node = QStringLiteral("n");
            context.run = QStringLiteral("20261123-120000-000-test");
            context.type = registry.nodeType(kDetail);
            context.inputs = inputs;
            context.workDir = dir.filePath("work");
            context.progress = [](double, const QString &, const QString &) {};
            context.cancelled = [] { return false; };
            context.hashes = &hashes;
            context.registry = &registry;
            return context;
        }
        // The files in the node's folder.
        QStringList workFiles() const { return QDir(dir.filePath("work")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot); }

        QTemporaryDir dir;
        FileHashes hashes;
        Registry registry;
        Kernels kernels;
        std::shared_ptr<ProviderKernel> kernel;
        Value model;
    };

    static QList<QJsonObject> fugue(const QTemporaryDir &dir, const QString &log, const QStringList &args) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        env.insert("XDG_CACHE_HOME", dir.filePath("cache"));
        env.insert("XDG_STATE_HOME", dir.filePath("state"));
        env.insert("FUGUE_HELPER", fugueFakeHelper());
        env.insert("FAKE_HELPER_LOG", log);
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(dir.path());
        process.start(fugueBinary(), args);
        if (!process.waitForFinished(60000)) {
            process.kill();
            process.waitForFinished();
            return {};
        }
        const QByteArray out = process.readAllStandardOutput();
        if (process.exitCode() != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), out.constData(), process.readAllStandardError().constData());
        QList<QJsonObject> events;
        for (const QByteArray &line : out.trimmed().split('\n'))
            events.append(QJsonDocument::fromJson(line).object());
        return events;
    }

    static QStringList jobs(const QString &log) {
        QFile file(log);
        QStringList ran;
        if (file.open(QIODevice::ReadOnly))
            for (const QString &line : QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts))
                if (line.startsWith(QLatin1String("job ")))
                    ran << line.mid(4);
        return ran;
    }
};

QTEST_GUILESS_MAIN(TestDetailRegions)
#include "test_detail_regions.moc"
