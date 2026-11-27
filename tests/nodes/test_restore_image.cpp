// Restore Image (#346, spec 14.1): what the node sends to the helper's image.restore and what it
// does with the picture that comes back. No model is run here: the helper's job is driven through
// the kernel's request and outputs; the fix's arithmetic is tested in test_restore_color.cpp.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>

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
    bool m_remember = true; // the hash of a file written here is known, as after a run or a Get ready

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
    // A safetensors file with these tensor names (and nothing else a reader looks at): what the node reads of a model.
    QJsonObject modelFile(const QString &resource, const QString &folder, const QString &name, const QStringList &tensors) {
        QJsonObject header;
        int offset = 0;
        for (const QString &tensor : tensors) {
            header.insert(tensor, QJsonObject{{"dtype", "F16"}, {"shape", QJsonArray{1}}, {"data_offsets", QJsonArray{offset, offset + 2}}});
            offset += 2;
        }
        const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
        QByteArray content;
        QDataStream(&content, QIODevice::WriteOnly).setByteOrder(QDataStream::LittleEndian);
        for (int i = 0; i < 8; ++i)
            content.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
        content += json + QByteArray(offset, '\0');
        const QString path = dataDir() + "/models/" + folder + "/" + name;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
            qFatal("cannot write %s", qPrintable(path));
        file.close(); // flushed, so the size and time a hash is remembered with are the file's
        const QString hash = "sha256:" + QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        if (m_remember)
            m_hashes.remember(path, hash);
        return QJsonObject{{"resource", resource}, {"hash", hash}, {"name", name}};
    }
    QJsonObject model(const QString &name = "seedvr2_test.safetensors",
                      const QStringList &tensors = {"vid_in.proj.weight", "positive_conditioning", "negative_conditioning"}) {
        return modelFile("fugue/seedvr2-model", "seedvr2", name, tensors);
    }
    QJsonObject vae(const QString &name = "seedvr2_test_vae.safetensors",
                    const QStringList &tensors = {"encoder.conv_out.bias", "decoder.up_blocks.0.upsamplers.0.upscale_conv.weight"}) {
        return modelFile("fugue/seedvr2-vae", "seedvr2_vae", name, tensors);
    }
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
        QCOMPARE(files.value("model_path").toString(), dataDir() + "/models/seedvr2/seedvr2_test.safetensors");
        QCOMPARE(files.value("vae_path").toString(), dataDir() + "/models/seedvr2_vae/seedvr2_test_vae.safetensors");
        QCOMPARE(r->params.value("tile_size").toInt(), 256);
        QCOMPARE(r->params.value("overlap").toInt(), 32);
        QCOMPARE(r->params.value("seed").toInt(), 42);
        const vips::VImage sent = vips::VImage::new_from_file(QFile::encodeName(r->inputs.value("image").toObject().value("path").toString()).constData());
        QCOMPARE(sent.width(), 24); // the default factor is 3
        QCOMPARE(sent.height(), 18);
        QCOMPARE(sent.format(), VIPS_FORMAT_UCHAR);
    }

    void namesBothFilesForAPairedHost() {
        NodeContext c = context(inputs(picture(8, 8, 3)));
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

    QString code(Values in) {
        NodeContext c = context(in);
        const Result<JobRequest> r = request(c);
        return r.ok() ? QString("ok") : r.errors().value(0).code + ":" + r.errors().value(0).input;
    }
    QString message(Values in) {
        NodeContext c = context(in);
        const Result<JobRequest> r = request(c);
        return r.ok() ? QString() : r.errors().value(0).message;
    }

    void refusesAMissingFileOrABadTiling() {
        const MediaHandle image = picture(32, 32, 3);
        Values noVae = inputs(image);
        noVae.remove("vae");
        QCOMPARE(code(noVae), QString("missing_input:vae"));
        Values gone = inputs(image);
        gone.insert("model", Value(QJsonObject{{"resource", "fugue/seedvr2-model"}, {"hash", "sha256:" + QString(64, 'a')}, {"name", "gone.safetensors"}}));
        QCOMPARE(code(gone), QString("model_missing:model"));
        Values wide = inputs(image);
        wide.insert("overlap", Value(300.0)); // more than half of the 512 tile
        QCOMPARE(code(wide), QString("value_out_of_range:overlap"));
        Values tiny = inputs(image);
        tiny.insert("tile", Value(8.0));
        QCOMPARE(code(tiny), QString("value_out_of_range:tile"));
    }

    void aWiredFactorThatIsNotANumberOrIsHugeIsRefusedBeforeAnyConversion() {
        for (double factor : {std::nan(""), 1e300, -3.0, 0.0, 9.0, std::numeric_limits<double>::infinity()}) {
            Values in = inputs(picture(32, 32, 3));
            in.insert("factor", Value(factor));
            QCOMPARE(code(in), QString("value_out_of_range:factor"));
        }
    }

    void aPictureTooSmallAtTheFactorIsRefusedOnTheImageWithTheMinimum() {
        // 5 x 5 at 3 is 15, one short of the 16 the model needs; 6 x 6 is 18.
        Values in = inputs(picture(5, 5, 3));
        QCOMPARE(code(in), QString("value_out_of_range:image"));
        const QString why = message(in);
        QVERIFY2(why.contains("at least 16") && why.contains("at least 6 x 6"), qPrintable(why));
        QCOMPARE(code(inputs(picture(6, 6, 3))), QString("ok"));
        Values wideShort = inputs(picture(600, 4, 3)); // one side is enough to refuse it
        QCOMPARE(code(wideShort), QString("value_out_of_range:image"));
    }

    void aResultBeyondWhatTheCardFinishesIsRefusedWithTheLargestThatFits() {
        // 2000 x 2000 at 3 is 6000 x 6000 (36 MP), past the 16.8 MP cap; factor 2 (4000 x 4000, 16 MP) would fit.
        Values in = inputs(picture(2000, 2000, 3, 10));
        QCOMPARE(code(in), QString("value_out_of_range:factor"));
        const QString why = message(in);
        QVERIFY2(why.contains("36.0 MP") && why.contains("at most 16.8 MP") && why.contains("largest factor for this image is 2"), qPrintable(why));
        QVERIFY2(why.contains("up to 1.9 MP"), qPrintable(why)); // at factor 3: 16.8 / 9
        // A picture too big even at factor 1 is the image's fault.
        QCOMPARE(code([&] { Values big = inputs(picture(4300, 4300, 3, 10)); big.insert("factor", Value(1.0)); return big; }()), QString("value_out_of_range:image"));
    }

    void aWrongModelOrVaeIsModelUnsupportedOnItsInputBeforeAnyJob() {
        const MediaHandle image = picture(32, 32, 3);
        Values klein = inputs(image); // a FLUX.2 Klein DiT in the SeedVR2 folder
        klein.insert("model", Value(model("klein.safetensors", {"double_blocks.0.img_attn.qkv.weight", "txt_in.weight"})));
        QCOMPARE(code(klein), QString("model_unsupported:model"));
        Values gguf = inputs(image); // SeedVR2 layers with no stored embedding (the GGUF, the numz fp8)
        gguf.insert("model", Value(model("numz.safetensors", {"vid_in.proj.weight", "blocks.0.attn.rope.rope.freqs"})));
        QCOMPARE(code(gguf), QString("model_unsupported:model"));
        QVERIFY(message(gguf).contains("stored embeddings"));
        Values flux = inputs(image); // a Flux VAE
        flux.insert("vae", Value(vae("flux.safetensors", {"encoder.conv_in.weight", "decoder.conv_in.weight"})));
        QCOMPARE(code(flux), QString("model_unsupported:vae"));
    }

    // The same model_unsupported before a run (ProviderKernel::check, under validate), from the headers alone; a file that is not
    // here is the scheduler's model_missing, and an input not known yet is not judged.
    void theCheckBeforeARunTellsAWrongModelOrVaeAndAnOverlapThatDoesNotFit() {
        const auto codes = [&](Values in, const QStringList &absent = {}) {
            NodeContext c = context(in);
            for (const QString &name : absent)
                c.inputs.remove(name);
            QStringList found;
            for (const Error &error : m_kernels.provider("fugue/restore-image")->check(c))
                found << error.code + ":" + error.input;
            return found.join(",");
        };
        const MediaHandle image = picture(32, 32, 3);
        QCOMPARE(codes(inputs(image)), QString());
        Values klein = inputs(image);
        klein.insert("model", Value(model("klein.safetensors", {"double_blocks.0.img_attn.qkv.weight", "txt_in.weight"})));
        QCOMPARE(codes(klein), QString("model_unsupported:model"));
        Values flux = inputs(image);
        flux.insert("vae", Value(vae("flux.safetensors", {"encoder.conv_in.weight", "decoder.conv_in.weight"})));
        QCOMPARE(codes(flux), QString("model_unsupported:vae"));
        QCOMPARE(codes(klein, {"vae"}), QString());
        Values gone = inputs(image);
        gone.insert("model", Value(QJsonObject{{"resource", "fugue/seedvr2-model"}, {"hash", "sha256:" + QString(64, 'b')}, {"name", "gone.safetensors"}}));
        QCOMPARE(codes(gone), QString());
        Values wide = inputs(image);
        wide.insert("tile", Value(1024.0));
        wide.insert("overlap", Value(600.0));
        QCOMPARE(codes(wide), QString("value_out_of_range:overlap"));
        // #607: a file of that name whose hash is not known may be another variant than the one a paired host would use: not judged.
        m_remember = false;
        Values unknown = inputs(image);
        unknown.insert("model", Value(model("klein-unknown.safetensors", {"double_blocks.0.img_attn.qkv.weight", "txt_in.weight"})));
        m_remember = true;
        QCOMPARE(codes(unknown), QString());
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
        NodeContext c = context(inputs(picture(8, 8, 3)));
        QVERIFY(request(c).ok());
        const Result<Values> out = m_kernels.provider("fugue/restore-image")->outputs(c, selection(), JobResult{});
        QVERIFY(!out.ok());
        QCOMPARE(out.errors().value(0).code, QString("node_failed"));
    }
};

QTEST_GUILESS_MAIN(TestRestoreImage)
#include "test_restore_image.moc"
