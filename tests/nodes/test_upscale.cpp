// Upscale Video, Load Upscale Model and Upscale with Model (spec 14.1, 8.1, #134). Video fixtures
// are made with ffmpeg's lavfi sources, so none is committed. No model is run here: the helper's
// job is driven through the kernel's request and outputs.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"

#include <vips/vips8>

using namespace fugue;

namespace {

bool ffmpegRuns(const QStringList &args) {
    QProcess process;
    process.start(QStringLiteral("ffmpeg"), QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args);
    return process.waitForFinished(60000) && process.exitCode() == 0;
}

} // namespace

class TestUpscale : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    FileHashes m_hashes{m_dir.filePath("hashes.json")};

    NodeContext context(const QString &id, Values inputs) {
        for (const InputSpec &spec : m_registry.nodeType(id)->inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext c;
        c.node = QStringLiteral("t");
        c.type = m_registry.nodeType(id);
        c.inputs = inputs;
        c.workDir = m_dir.filePath("work");
        QDir().mkpath(c.workDir);
        c.progress = [this](double v, const QString &, const QString &) { m_progress << v; };
        c.cancelled = [this] { return m_cancel; };
        c.hashes = &m_hashes;
        c.registry = &m_registry;
        return c;
    }

    QList<double> m_progress;
    bool m_cancel = false;

    MediaHandle clip(int w, int h, double seconds, bool audio) {
        const QString path = m_dir.filePath(QString("in-%1x%2-%3.mkv").arg(w).arg(h).arg(audio));
        QStringList args{"-f", "lavfi", "-i", QString("testsrc=size=%1x%2:rate=12:duration=%3").arg(w).arg(h).arg(seconds)};
        if (audio)
            args += {"-f", "lavfi", "-i", QString("sine=frequency=440:sample_rate=22050:duration=%1").arg(seconds)};
        args += {"-c:v", "libx264", "-pix_fmt", "yuv420p"};
        if (audio)
            args += {"-c:a", "aac"};
        args << path;
        if (!ffmpegRuns(args))
            qFatal("cannot make %s", qPrintable(path));
        const Result<MediaHandle> probed = MediaHandle::probe(MediaType::Video, path, m_hashes);
        if (!probed)
            qFatal("cannot probe %s", qPrintable(path));
        return probed.value();
    }

private slots:
    void initTestCase() {
        initVips();
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }
    void init() {
        m_progress.clear();
        m_cancel = false;
    }

    void upscaleVideoScalesEveryFrameAndKeepsRateLengthAndSound() {
        const MediaHandle in = clip(64, 48, 1.0, true);
        const Result<Values> r = m_kernels.node("fugue/upscale-video")->run(context("fugue/upscale-video", {{"video", Value(in)}}));
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        const MediaHandle out = r.value().value("video").media();
        QCOMPARE(out.width(), 128);
        QCOMPARE(out.height(), 96);
        QCOMPARE(out.fps(), in.fps());
        QVERIFY(qAbs(out.duration() - in.duration()) < 0.1);
        QVERIFY(out.hasAudio());
        QVERIFY(!m_progress.isEmpty());
        QVERIFY(m_progress.last() <= 1.0);
    }
    void upscaleVideoKeepsEvenSides() {
        const MediaHandle in = clip(50, 30, 0.5, false);
        const Result<Values> r = m_kernels.node("fugue/upscale-video")->run(
            context("fugue/upscale-video", {{"video", Value(in)}, {"scale", 1.5}, {"method", "bicubic"}}));
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        const MediaHandle out = r.value().value("video").media();
        QCOMPARE(out.width(), 74); // 75 rounded down to even
        QCOMPARE(out.height(), 44); // 45
        QVERIFY(!out.hasAudio());
    }
    void upscaleVideoStopsWhenCancelled() {
        const MediaHandle in = clip(320, 240, 2.0, false);
        m_cancel = true;
        const Result<Values> r = m_kernels.node("fugue/upscale-video")->run(
            context("fugue/upscale-video", {{"video", Value(in)}, {"scale", 4}}));
        QVERIFY(!r.ok());
        QCOMPARE(r.errors().value(0).code, QString("cancelled"));
    }
    // ---- Upscale with Model: what is sent to the helper, and what comes back ----

    QJsonObject modelFile(const QString &name) {
        const QByteArray content = name.toUtf8() + " weights";
        const QString path = dataDir() + "/models/upscale_models/" + name;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
            qFatal("cannot write %s", qPrintable(path));
        return QJsonObject{{"resource", "fugue/upscale-model"},
                           {"hash", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())},
                           {"name", name}};
    }
    MediaHandle picture(int w, int h, int bands) {
        vips::VImage image = vips::VImage::black(w, h, vips::VImage::option()->set("bands", bands)) + 100;
        const Result<MediaHandle> handle = MediaHandle::fromImage(image.cast(VIPS_FORMAT_UCHAR));
        return handle.value();
    }
    Selection selection(bool shares = true) {
        Selection chosen;
        chosen.instance = QStringLiteral("local");
        chosen.engine = QStringLiteral("sdcpp");
        chosen.model.status = ModelStatus::Ready;
        chosen.sharesFilesystem = shares;
        return chosen;
    }

    void upscaleWithModelAsksForTheCapabilityWithAnyReadyModel() {
        const Requirement requirement = m_kernels.provider("fugue/upscale-with-model")->requirement(*m_registry.nodeType("fugue/upscale-with-model"), {});
        QCOMPARE(requirement.capability, QString("image.upscale"));
        QVERIFY(requirement.model.isEmpty());
    }
    void upscaleWithModelSendsTheModelFileTheTileAndAnRgbPng() {
        const QJsonObject model = modelFile("RealESRGAN_x4plus.safetensors");
        NodeContext c = context("fugue/upscale-with-model", {{"image", Value(picture(8, 6, 4))}, {"model", Value(model)}, {"tile", 256.0}});
        const Result<JobRequest> request = m_kernels.provider("fugue/upscale-with-model")->request(c, selection());
        QVERIFY2(request.ok(), qPrintable(request.errors().value(0).message));
        QCOMPARE(request->capability, QString("image.upscale"));
        QCOMPARE(request->params.value("context").toObject().value("model_path").toString(),
                 dataDir() + "/models/upscale_models/RealESRGAN_x4plus.safetensors");
        QCOMPARE(request->params.value("tile_size").toInt(), 256);
        const vips::VImage sent = vips::VImage::new_from_file(QFile::encodeName(request->inputs.value("image").toObject().value("path").toString()).constData());
        QCOMPARE(sent.bands(), 3); // alpha is kept here, not sent
        QCOMPARE(sent.width(), 8);
    }
    void upscaleWithModelSaysWhenTheModelFileIsMissing() {
        const QJsonObject model{{"resource", "fugue/upscale-model"}, {"hash", "sha256:" + QString(64, 'a')}, {"name", "gone.safetensors"}};
        NodeContext c = context("fugue/upscale-with-model", {{"image", Value(picture(4, 4, 3))}, {"model", Value(model)}});
        const Result<JobRequest> request = m_kernels.provider("fugue/upscale-with-model")->request(c, selection());
        QVERIFY(!request.ok());
        QCOMPARE(request.errors().value(0).code, QString("model_missing"));
        QCOMPARE(request.errors().value(0).input, QString("model"));
    }
    void upscaleWithModelNamesTheFileForAPairedHost() {
        const QJsonObject model = modelFile("host.safetensors");
        NodeContext c = context("fugue/upscale-with-model", {{"image", Value(picture(4, 4, 3))}, {"model", Value(model)}});
        const Result<JobRequest> request = m_kernels.provider("fugue/upscale-with-model")->request(c, selection(false));
        QVERIFY(request.ok());
        const QJsonObject sent = request->params.value("context").toObject().value("model_path").toObject();
        QCOMPARE(sent.value("name").toString(), QString("host.safetensors"));
        QCOMPARE(sent.value("sha256").toString(), model.value("hash").toString());
        QVERIFY(sent.value("size").toDouble() > 0);
    }
    void upscaleWithModelPutsTheScaledAlphaBack() {
        const QString result = m_dir.filePath("helper-out.png");
        (vips::VImage::black(32, 24, vips::VImage::option()->set("bands", 3)) + 90).cast(VIPS_FORMAT_UCHAR).write_to_file(QFile::encodeName(result).constData());
        NodeContext c = context("fugue/upscale-with-model", {{"image", Value(picture(8, 6, 4))}});
        JobResult job;
        job.outputs = QJsonObject{{"image", QJsonObject{{"path", result}}}};
        const Result<Values> out = m_kernels.provider("fugue/upscale-with-model")->outputs(c, selection(), job);
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        QCOMPARE(out.value().value("width").number(), 32.0);
        QCOMPARE(out.value().value("height").number(), 24.0);
        const vips::VImage image = out.value().value("image").media().image().value();
        QCOMPARE(image.bands(), 4);
        QCOMPARE(image.width(), 32);
        QCOMPARE(int(image.extract_band(3).avg() + 0.5), 100);
    }
    void loadUpscaleModelPassesTheHandleOn() {
        const QJsonObject model = modelFile("pass.safetensors");
        const Result<Values> out = m_kernels.node("fugue/load-upscale-model")->run(context("fugue/load-upscale-model", {{"model", Value(model)}}));
        QVERIFY(out.ok());
        QCOMPARE(out.value().value("model").json().toObject(), model);
    }
};

QTEST_GUILESS_MAIN(TestUpscale)
#include "test_upscale.moc"
