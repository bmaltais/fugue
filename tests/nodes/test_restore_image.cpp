// Restore Image (#346, spec 14.1): what the node sends to the helper's image.restore and what it
// does with the picture that comes back. No model is run here: the helper's job is driven through
// the kernel's request and outputs; the fix's arithmetic is tested in test_restore_color.cpp.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonObject>
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

class TestRestoreImage : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    FileHashes m_hashes{m_dir.filePath("hashes.json")};
    int m_runs = 0;

    NodeContext context(Values inputs) {
        for (const InputSpec &spec : m_registry.nodeType("fugue/restore-image")->inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext c;
        c.node = QStringLiteral("t");
        c.type = m_registry.nodeType("fugue/restore-image");
        c.inputs = inputs;
        c.workDir = m_dir.filePath(QString("work%1").arg(++m_runs));
        QDir().mkpath(c.workDir);
        c.hashes = &m_hashes;
        c.registry = &m_registry;
        return c;
    }
    QJsonObject modelFile(const QString &resource, const QString &folder, const QString &name) {
        const QByteArray content = name.toUtf8() + " weights";
        const QString path = dataDir() + "/models/" + folder + "/" + name;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
            qFatal("cannot write %s", qPrintable(path));
        return QJsonObject{{"resource", resource},
                           {"hash", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())},
                           {"name", name}};
    }
    QJsonObject model() { return modelFile("fugue/diffusion-model", "diffusion_models", "seedvr2_test.safetensors"); }
    QJsonObject vae() { return modelFile("fugue/vae", "vae", "seedvr2_test_vae.safetensors"); }
    MediaHandle picture(int w, int h, int bands, double value = 100) {
        vips::VImage image = vips::VImage::black(w, h, vips::VImage::option()->set("bands", bands)) + value;
        return MediaHandle::fromImage(image.cast(VIPS_FORMAT_UCHAR)).value();
    }
    Selection selection(bool shares = true) {
        Selection chosen;
        chosen.instance = QStringLiteral("local");
        chosen.engine = QStringLiteral("sdcpp");
        chosen.model.status = ModelStatus::Ready;
        chosen.sharesFilesystem = shares;
        return chosen;
    }
    Result<JobRequest> request(NodeContext &c, bool shares = true) { return m_kernels.provider("fugue/restore-image")->request(c, selection(shares)); }
    Values inputs(const MediaHandle &image) { return {{"image", Value(image)}, {"model", Value(model())}, {"vae", Value(vae())}}; }

    // A band's mean on the 0..255 scale: the node's output is float pixels in 0..1.
    static double mean255(const vips::VImage &image) { return image.avg() * (image.format() == VIPS_FORMAT_UCHAR ? 1.0 : 255.0); }

    // A helper result: a picture of `value` at the size the request sent.
    JobResult helperResult(int w, int h, int value) {
        const QString path = m_dir.filePath(QString("helper-%1.png").arg(++m_runs));
        (vips::VImage::black(w, h, vips::VImage::option()->set("bands", 3)) + value).cast(VIPS_FORMAT_UCHAR).write_to_file(QFile::encodeName(path).constData());
        JobResult job;
        job.outputs = QJsonObject{{"image", QJsonObject{{"path", path}}}};
        return job;
    }

private slots:
    void initTestCase() {
        initVips();
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    void asksForTheRestoreCapabilityWithAnyReadyModel() {
        const Requirement requirement = m_kernels.provider("fugue/restore-image")->requirement(*m_registry.nodeType("fugue/restore-image"), {});
        QCOMPARE(requirement.capability, QString("image.restore"));
        QVERIFY(requirement.model.isEmpty());
    }

    void sendsBothModelFilesTheTilingTheSeedAndThePictureAtItsFinalSize() {
        NodeContext c = context(inputs(picture(8, 6, 3)));
        c.inputs.insert("seed", Value(42.0));
        c.inputs.insert("tile", Value(256.0));
        c.inputs.insert("overlap", Value(32.0));
        const Result<JobRequest> r = request(c);
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        QCOMPARE(r->capability, QString("image.restore"));
        const QJsonObject files = r->params.value("context").toObject();
        QCOMPARE(files.value("model_path").toString(), dataDir() + "/models/diffusion_models/seedvr2_test.safetensors");
        QCOMPARE(files.value("vae_path").toString(), dataDir() + "/models/vae/seedvr2_test_vae.safetensors");
        QCOMPARE(r->params.value("tile_size").toInt(), 256);
        QCOMPARE(r->params.value("overlap").toInt(), 32);
        QCOMPARE(r->params.value("seed").toInt(), 42);
        const vips::VImage sent = vips::VImage::new_from_file(QFile::encodeName(r->inputs.value("image").toObject().value("path").toString()).constData());
        QCOMPARE(sent.width(), 24); // the default factor is 3
        QCOMPARE(sent.height(), 18);
        QCOMPARE(sent.format(), VIPS_FORMAT_UCHAR);
    }

    void namesBothFilesForAPairedHost() {
        NodeContext c = context(inputs(picture(4, 4, 3)));
        const Result<JobRequest> r = request(c, false);
        QVERIFY(r.ok());
        const QJsonObject files = r->params.value("context").toObject();
        for (const char *key : {"model_path", "vae_path"}) {
            const QJsonObject sent = files.value(key).toObject();
            QVERIFY(!sent.value("name").toString().isEmpty());
            QVERIFY(sent.value("sha256").toString().startsWith("sha256:"));
            QVERIFY(sent.value("size").toDouble() > 0);
        }
    }

    void refusesWhatCannotRun() {
        const MediaHandle image = picture(4, 4, 3);
        const auto code = [&](Values in) {
            NodeContext c = context(in);
            const Result<JobRequest> r = request(c);
            return r.ok() ? QString("ok") : r.errors().value(0).code + ":" + r.errors().value(0).input;
        };
        Values noVae = inputs(image);
        noVae.remove("vae");
        QCOMPARE(code(noVae), QString("missing_input:vae"));
        Values gone = inputs(image);
        gone.insert("model", Value(QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, 'a')}, {"name", "gone.safetensors"}}));
        QCOMPARE(code(gone), QString("model_missing:model"));
        Values wide = inputs(image);
        wide.insert("overlap", Value(300.0)); // more than half of the 512 tile
        QCOMPARE(code(wide), QString("value_out_of_range:overlap"));
        Values huge = inputs(picture(4000, 10, 3));
        huge.insert("factor", Value(8.0));
        QCOMPARE(code(huge), QString("value_out_of_range:factor"));
    }

    void colorFixNoneKeepsTheModelsPicture() {
        NodeContext c = context(inputs(picture(8, 6, 3, 100)));
        c.inputs.insert("color_fix", Value(QString("none")));
        QVERIFY(request(c).ok());
        const Result<Values> out = m_kernels.provider("fugue/restore-image")->outputs(c, selection(), helperResult(24, 18, 60));
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        QCOMPARE(out.value().value("width").number(), 24.0);
        QCOMPARE(out.value().value("height").number(), 18.0);
        QCOMPARE(int(mean255(out.value().value("image").media().image().value()) + 0.5), 60);
    }

    void colorFixLabPullsTheColoursBackToTheInput() {
        // The model returned a picture much darker than the input; both fixes take the input's low
        // frequencies, so a flat input gives back its own level (the fix's arithmetic is in
        // test_restore_color).
        const auto mean = [&](const QString &fix) {
            NodeContext c = context(inputs(picture(8, 6, 3, 100)));
            c.inputs.insert("color_fix", Value(fix));
            if (!request(c).ok())
                qFatal("request failed");
            const Result<Values> out = m_kernels.provider("fugue/restore-image")->outputs(c, selection(), helperResult(24, 18, 60));
            return mean255(out.value().value("image").media().image().value());
        };
        const double none = mean("none"), lab = mean("lab"), wavelet = mean("wavelet");
        QVERIFY2(none < 70 && std::fabs(lab - 100) < 3, qPrintable(QString::number(lab)));
        QVERIFY2(std::fabs(wavelet - 100) < 2, qPrintable(QString::number(wavelet)));
    }

    void anInputAlphaComesBackAtTheNewSize() {
        NodeContext c = context(inputs(picture(8, 6, 4, 100)));
        QVERIFY(request(c).ok());
        const Result<Values> out = m_kernels.provider("fugue/restore-image")->outputs(c, selection(), helperResult(24, 18, 100));
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        const vips::VImage image = out.value().value("image").media().image().value();
        QCOMPARE(image.bands(), 4);
        QCOMPARE(int(mean255(image.extract_band(3)) + 0.5), 100);
    }

    void aHelperWithoutAPictureIsNodeFailed() {
        NodeContext c = context(inputs(picture(4, 4, 3)));
        QVERIFY(request(c).ok());
        const Result<Values> out = m_kernels.provider("fugue/restore-image")->outputs(c, selection(), JobResult{});
        QVERIFY(!out.ok());
        QCOMPARE(out.errors().value(0).code, QString("node_failed"));
    }
};

QTEST_GUILESS_MAIN(TestRestoreImage)
#include "test_restore_image.moc"
