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

#include <vips/vips8>

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

// A picture of one colour, in the test's folder, probed as the scheduler hands it over.
MediaHandle flat(FileHashes &hashes, const QString &path, int width, int height, double gray) {
    initVips();
    (vips::VImage::black(width, height, vips::VImage::option()->set("bands", 3)) + gray).cast(VIPS_FORMAT_UCHAR).write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, hashes);
    if (!image)
        qFatal("cannot probe %s", qPrintable(path));
    return image.value();
}

// What the helper does for a job: writes the file the capability makes into the node's folder.
// A decode makes a picture of the latent's size in `redrawn` (gray, 0 to 255).
JobResult finished(const JobRequest &job, const QString &workDir, double redrawn) {
    const QString capability = job.capability;
    if (capability == QLatin1String("latent.decode")) {
        const QString path = workDir + "/image.png";
        initVips();
        (vips::VImage::black(job.params.value("width").toInt(), job.params.value("height").toInt(),
                             vips::VImage::option()->set("bands", 3)) + redrawn).cast(VIPS_FORMAT_UCHAR).write_to_file(path.toUtf8().constData());
        return JobResult{QJsonObject{{"image", QJsonObject{{"path", path}}}}, {}};
    }
    const QString path = workDir + "/latent.bin";
    writeBytes(path, QJsonDocument(job.params).toJson(QJsonDocument::Compact));
    return JobResult{QJsonObject{{"latent", QJsonObject{{"path", path}}}}, {}};
}

} // namespace

class TestDetailRegions : public QObject {
    Q_OBJECT

private slots:
    // --- the arithmetic ---

    void boxesAreFourNumbersALine() {
        std::string problem;
        const auto boxes = parseBoxes("10 20 110.5 220\n\n  5 5 40 40  \n", &problem);
        QVERIFY(boxes);
        QCOMPARE(boxes->size(), size_t(2));
        QCOMPARE(boxes->at(0).x2, 110.5);
        QVERIFY(!parseBoxes("10 20 30\n", &problem));
        QVERIFY(QString::fromStdString(problem).contains("line 1"));
        QVERIFY(!parseBoxes("1 2 3 4 5\n", &problem));
        QVERIFY(parseBoxes("", &problem)->empty());
    }

    void aCropIsPaddedAndCutAtTheEdgeAndSmallOnesAreSkipped() {
        QCOMPARE(*detailCrop({100, 100, 200, 180}, 32, 640, 480), (CropBox{68, 68, 164, 144}));
        QCOMPARE(*detailCrop({10, 10, 60, 60}, 32, 640, 480), (CropBox{0, 0, 92, 92})); // cut, not moved
        QCOMPARE(*detailCrop({600, 400, 700, 500}, 32, 640, 480), (CropBox{568, 368, 72, 112}));
        QVERIFY(!detailCrop({100, 100, 110, 140}, 2, 640, 480)); // 14 wide with its padding
        QVERIFY(detailCrop({100, 100, 112, 140}, 2, 640, 480)); // exactly 16
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

    void theFeatherRampsOnAllFourEdgesAndNeverPastHalfTheCrop() {
        const Pixels w = featherWeights(100, 60, 11);
        QCOMPARE(w.at(50, 30, 0), 1.0f);
        QCOMPARE(w.at(0, 30, 0), 0.0f);
        QCOMPARE(w.at(99, 30, 0), 0.0f);
        QCOMPARE(w.at(50, 0, 0), 0.0f);
        QCOMPARE(w.at(50, 59, 0), 0.0f);
        QCOMPARE(w.at(5, 30, 0), 0.5f); // 5 of 10 steps in
        QCOMPARE(w.at(11, 30, 0), 1.0f);
        const Pixels corner = featherWeights(100, 60, 11);
        QCOMPARE(corner.at(5, 5, 0), 0.25f); // the two ramps multiply
        // f = min(feather, h / 2, w / 2): 24 on a 30 x 100 crop is 15, on a 100 x 100 one 24.
        QCOMPARE(featherWeights(100, 30, 24).at(7, 15, 0), 0.5f);
        QCOMPARE(featherWeights(100, 30, 24).at(15, 15, 0), 1.0f);
        const Pixels none = featherWeights(20, 20, 0);
        QCOMPARE(none.at(0, 0, 0), 1.0f);

        Pixels canvas(10, 10, 4, 0.0f);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                canvas.at(x, y, 3) = 0.7f;
        pasteFeathered(canvas, {2, 2, 6, 6}, Pixels(6, 6, 3, 1.0f), 2);
        QCOMPARE(canvas.at(5, 5, 0), 1.0f);  // inside
        QCOMPARE(canvas.at(2, 5, 0), 0.0f);  // the crop's edge weighs 0
        QCOMPARE(canvas.at(0, 0, 0), 0.0f);  // outside
        QCOMPARE(canvas.at(5, 5, 3), 0.7f);  // alpha kept
    }

    // --- the kernel, job by job ---

    void aNodeWithNoBoxAsksForNoJobAndGivesThePictureBack() {
        Rig rig;
        const MediaHandle image = flat(rig.hashes, rig.dir.filePath("in.png"), 128, 128, 90);
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
    }

    void everyBoxIsEncodedSampledAndDecodedAndPastedBeforeTheNextOne() {
        Rig rig;
        const MediaHandle image = flat(rig.hashes, rig.dir.filePath("in.png"), 256, 256, 90);
        // Box 1 is too small to redraw; boxes 0 and 2 overlap, so box 2's crop holds box 0's paste.
        NodeContext context = rig.context(image, "20 20 100 100\n5 5 6 6\n80 80 160 160\n", {{"seed", Value(776)}, {"seed_shift", Value(2)},
                                                                                              {"padding", Value(8)}, {"target_size", Value(256)},
                                                                                              {"feather", Value(4)}, {"steps", Value(4)}, {"denoise", Value(0.3)}});
        QStringList sequence;
        QList<QJsonObject> samples;
        double secondCropGray = -1;
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        double progressEnd = 0;
        while (true) {
            sequence << job->capability;
            if (job->capability == QLatin1String("latent.sample"))
                samples << job->params;
            if (job->capability == QLatin1String("latent.encode") && sequence.count(job->capability) == 2) {
                const Result<MediaHandle> crop = MediaHandle::probe(MediaType::Image, job->inputs.value("image").toObject().value("path").toString(), rig.hashes);
                QVERIFY(crop);
                const Result<Pixels> pixels = pixelsOf(Value(crop.value()), "image");
                QVERIFY(pixels);
                // Box 2 is the 96 x 96 crop 72..168 (80..160 and 8 of padding), scaled to 256: its top left corner
                // lies inside box 0's redraw (red 200), its bottom right outside it (90).
                secondCropGray = pixels->at(10, 10, 0) * 255.0;
                QCOMPARE(pixels->width, 256); // redrawn at the target
            }
            progressEnd = job->progressFrom + job->progressSpan;
            const JobResult result = finished(job.value(), context.workDir, 200);
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), result);
            QVERIFY2(more, qPrintable(more.error().message));
            if (!more->has_value())
                break;
            job = *more.value();
        }
        QCOMPARE(sequence, (QStringList{"latent.encode", "latent.sample", "latent.decode", "latent.encode", "latent.sample", "latent.decode"}));
        QCOMPARE(progressEnd, 1.0);
        // The seeds: seed + shift * 100000 + the box's place in the list (the skipped one counts).
        QCOMPARE(samples.at(0).value("seed").toInteger(), qint64(776 + 200000 + 0));
        QCOMPARE(samples.at(1).value("seed").toInteger(), qint64(776 + 200000 + 2));
        // 4 steps at 0.3 are ComfyUI's cut: 13 steps, the last 4 kept (strength 3.5 / 13).
        const double cut = 3.5 / 13;
        for (const QJsonObject &sample : samples) {
            QCOMPARE(sample.value("sample.steps").toInt(), 13);
            QVERIFY(std::fabs(sample.value("strength").toDouble() - cut) < 1e-6);
            QCOMPARE(sample.value("sample.args").toString(), QStringLiteral("strength_as_noise_level=false"));
            QCOMPARE(sample.value("sample.method").toString(), QStringLiteral("euler"));
            QCOMPARE(sample.value("sample.scheduler").toString(), QStringLiteral("simple"));
        }
        QVERIFY2(std::fabs(secondCropGray - 200.0) < 3.0, qPrintable(QString::number(secondCropGray)));

        const Result<Values> out = rig.kernel->outputs(context, helperSelection(), JobResult());
        QVERIFY(out);
        QCOMPARE(out.warnings().size(), 1); // the small box
        QCOMPARE(out.warnings().first().code, QStringLiteral("box_skipped"));
        const Result<Pixels> done = pixelsOf(out->value("image"), "image");
        QVERIFY(done);
        QCOMPARE(done->width, 256);
        QVERIFY(std::fabs(done->at(60, 60, 0) * 255.0 - 200.0) < 1.5); // inside the first box
        QVERIFY(std::fabs(done->at(200, 30, 0) * 255.0 - 90.0) < 1.5); // outside every box: as it was
        QVERIFY(std::fabs(done->at(150, 150, 0) * 255.0 - 200.0) < 1.5); // inside the second
        // The node's folder keeps nothing of the work.
        QVERIFY(!QFileInfo::exists(context.workDir + "/canvas.png"));
        QVERIFY(!QFileInfo::exists(context.workDir + "/detail.json"));
    }

    void aCancelBetweenBoxesStopsTheNode() {
        Rig rig;
        const MediaHandle image = flat(rig.hashes, rig.dir.filePath("in.png"), 128, 128, 90);
        NodeContext context = rig.context(image, "10 10 60 60\n60 60 120 120\n");
        Result<JobRequest> job = rig.kernel->request(context, helperSelection());
        QVERIFY2(job, qPrintable(job.error().message));
        bool cancelled = false;
        context.cancelled = [&] { return cancelled; };
        for (int i = 0; i < 3; ++i) { // box 0 whole
            const Result<std::optional<JobRequest>> more = rig.kernel->next(context, helperSelection(), finished(job.value(), context.workDir, 200));
            QVERIFY(more && more->has_value());
            job = *more.value();
        }
        cancelled = true;
        const Result<std::optional<JobRequest>> stopped = rig.kernel->next(context, helperSelection(), finished(job.value(), context.workDir, 200));
        QVERIFY(!stopped);
        QCOMPARE(stopped.error().code, QStringLiteral("cancelled"));
    }

    void aBadBoxListIsRefusedBeforeAnyJob() {
        Rig rig;
        const MediaHandle image = flat(rig.hashes, rig.dir.filePath("in.png"), 64, 64, 90);
        const Result<JobRequest> job = rig.kernel->request(rig.context(image, "10 10 20\n"), helperSelection());
        QVERIFY(!job);
        QCOMPARE(job.error().input, QStringLiteral("boxes"));
    }

    // --- the shipped binary, the fake helper ---

    void aRunMakesThreeJobsAPerBoxAndNoneForNoBox() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("inputs"));
        FileHashes hashes(dir.filePath("h.json"));
        const MediaHandle image = flat(hashes, dir.filePath("inputs/in.png"), 256, 256, 90);
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
        env.insert("FUGUE_HELPER", QStringLiteral(FUGUE_FAKE_HELPER));
        env.insert("FAKE_HELPER_LOG", log);
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(dir.path());
        process.start(QStringLiteral(FUGUE_BINARY), args);
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
