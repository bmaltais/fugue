// Use what you have (#605, engine/model_use.h): the file of the same weights that stands in for a model a workflow names that is not on
// disk, and the document with it swapped in.
//
// Seam: findOnDisk and withUses over a folder of small safetensors files with the header figures of Z-Image Turbo (a header alone
// tells the family's shape and whether the engine can load the quantization), and a Registry that declares the diffusion model type.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>

#include "engine/document.h"
#include "engine/model_library.h"
#include "engine/model_recommend.h"
#include "engine/model_use.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

const QString kType = QStringLiteral("fugue/diffusion-model");
const QString kWanted = QStringLiteral("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

// A Z-Image Turbo header (Lumina 2's text projection and 16-channel patch embedding); `quant` adds a ComfyUI quantization tensor whose
// payload names the format, as the engine's own reader reads them.
void writeZImage(const QString &path, const QByteArray &quantFormat = {}) {
    QJsonObject header;
    const auto tensor = [&](const QString &name, const QJsonArray &shape, const char *dtype = "F16", int from = 0, int to = 0) {
        header.insert(name, QJsonObject{{"dtype", dtype}, {"shape", shape}, {"data_offsets", QJsonArray{from, to}}});
    };
    tensor(QStringLiteral("cap_embedder.1.weight"), QJsonArray{3840, 2560});
    tensor(QStringLiteral("x_embedder.weight"), QJsonArray{3840, 64});
    QByteArray data;
    if (!quantFormat.isEmpty()) {
        const QByteArray payload = QByteArray("{\"format\": \"") + quantFormat + "\"}";
        tensor(QStringLiteral("layers.0.attention.q_proj.comfy_quant"), QJsonArray{payload.size()}, "U8", 0, int(payload.size()));
        data = payload;
    }
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out(8, '\0');
    const quint64 size = quint64(json.size());
    std::memcpy(out.data(), &size, 8);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(out + json + data);
}

QJsonObject wanted(const QString &name) { return {{"resource", kType}, {"hash", kWanted}, {"name", name}}; }

} // namespace

class TestModelUse : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    Registry m_registry;
    ModelShapes m_shapes;

    QString root(const QString &name) const { return m_dir.filePath(name); }
    ModelLibrary library(const QString &name) {
        return ModelLibrary({root(name)}, &m_registry);
    }
    static RecommendContext context() { return {ComputeDevice{"cuda", "NVIDIA GeForce RTX 3090", 8.6, 24576}, 0, nullptr}; }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        DeclaredPortType declared;
        declared.type = *PortType::parse(kType);
        declared.kind = PortKind::Resource;
        declared.name = QStringLiteral("A diffusion model");
        declared.extensions = {QStringLiteral(".safetensors"), QStringLiteral(".gguf")};
        declared.folders = {QStringLiteral("diffusion_models")};
        QVERIFY(m_registry.add(declared).isEmpty());
    }

    // The owner's case: the template wants the INT8 file; the bf16 and the fp8 one are here. The Recommended one (the best quality that
    // fits) stands in, and it says so.
    void aMissingInt8IsReplacedByTheRecommendedOtherQuantization() {
        const QString folder = root("a") + "/diffusion_models/";
        writeZImage(folder + "z_image_turbo_bf16.safetensors");
        writeZImage(folder + "z_image_turbo_fp8_e4m3fn.safetensors");
        const std::optional<OnDiskMatch> match =
            findOnDisk(wanted("z_image_turbo_int8_convrot.safetensors"), library("a"), context(), m_shapes);
        QVERIFY(match);
        QCOMPARE(match->name, QStringLiteral("z_image_turbo_bf16.safetensors"));
        QCOMPARE(match->tier, QStringLiteral("bf16"));
        QVERIFY(match->recommended);
        QVERIFY(!match->otherSource); // another quantization, not another source
        const OnDiskUse use{wanted("z_image_turbo_int8_convrot.safetensors"), *match};
        QCOMPARE(use.offerText(), QStringLiteral("Use what you have: z_image_turbo_bf16.safetensors (same model, bf16)"));
        QVERIFY(use.cliHint("models.diffusion_model").contains("--use-on-disk"));
        QVERIFY(use.cliHint("models.diffusion_model").contains(match->path));
    }

    // The file of the same name in another folder, another publisher's bytes: the same model at the same quantization, said so.
    void theSameNameFromAnotherSourceIsOfferedAndSaidSo() {
        writeZImage(root("b") + "/diffusion_models/z_image_turbo_bf16.safetensors");
        const std::optional<OnDiskMatch> match =
            findOnDisk(wanted("zimage/z_image_turbo_bf16.safetensors"), library("b"), context(), m_shapes);
        QVERIFY(match);
        QVERIFY(match->otherSource);
        const OnDiskUse use{wanted("x"), *match};
        QVERIFY(use.offerText().contains("from another source"));
    }

    // Another model of the family (the base, a different stem) is never offered, nor a file of another kind.
    void anotherModelIsNeverOffered() {
        const QString folder = root("c") + "/diffusion_models/";
        writeZImage(folder + "z_image_bf16.safetensors"); // Z-Image, not Turbo: another stem
        QVERIFY(!findOnDisk(wanted("z_image_turbo_int8_convrot.safetensors"), library("c"), context(), m_shapes));
        QVERIFY(!findOnDisk(wanted("notamodel.safetensors"), library("c"), context(), m_shapes)); // no family: nothing to compare
    }

    // A file the engine cannot load (an NVFP4 quantization) is never offered, alone or beside one it can.
    void aFileTheEngineCannotLoadIsNeverOffered() {
        const QString folder = root("d") + "/diffusion_models/";
        writeZImage(folder + "z_image_turbo_nvfp4.safetensors", "nvfp4");
        QVERIFY(!findOnDisk(wanted("z_image_turbo_int8_convrot.safetensors"), library("d"), context(), m_shapes));
        writeZImage(folder + "z_image_turbo_bf16.safetensors");
        const std::optional<OnDiskMatch> match =
            findOnDisk(wanted("z_image_turbo_int8_convrot.safetensors"), library("d"), context(), m_shapes);
        QVERIFY(match);
        QCOMPARE(match->name, QStringLiteral("z_image_turbo_bf16.safetensors"));
    }

    // A file whose header does not read is not known to be the same weights.
    void aFileWithNoReadableHeaderIsNeverOffered() {
        QDir().mkpath(root("e") + "/diffusion_models");
        QFile junk(root("e") + "/diffusion_models/z_image_turbo_bf16.safetensors");
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not a model");
        junk.close();
        QVERIFY(!findOnDisk(wanted("z_image_turbo_int8_convrot.safetensors"), library("e"), context(), m_shapes));
    }

    // Applying swaps the handle wherever it is, in a document's nodes and groups; the document it came from is unchanged, and the
    // document then names the file (what the plan and the run read).
    void aDocumentWithTheFileInPlaceOfTheMissingModel() {
        const QByteArray text = R"({"fugue": 1, "graph": {"nodes": {
            "models": {"type": "fugue/load-models", "version": "1.0.0",
                       "inputs": {"diffusion_model": {"resource": "fugue/diffusion-model", "hash": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "name": "z_image_turbo_int8_convrot.safetensors"}}}}}})";
        const Result<Document> loaded = Document::load(text);
        QVERIFY(loaded);
        const QString replacement = QStringLiteral("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        OnDiskUse use{wanted("z_image_turbo_int8_convrot.safetensors"), {}};
        use.match.name = QStringLiteral("z_image_turbo_bf16.safetensors");
        use.match.resource = kType;
        const Uses uses{{kWanted, useHandle(use, replacement)}};
        const Document swapped = withUses(*loaded, uses);
        const QJsonArray models = swapped.models();
        QCOMPARE(models.size(), 1);
        QCOMPARE(models.first().toObject().value("hash").toString(), replacement);
        QCOMPARE(models.first().toObject().value("name").toString(), QStringLiteral("z_image_turbo_bf16.safetensors"));
        QCOMPARE(loaded->models().first().toObject().value("hash").toString(), kWanted); // the original keeps its own
        QCOMPARE(withUses(*loaded, {}).models(), loaded->models());
    }

    void quantizationReadsAsPeopleSayIt() {
        QCOMPARE(quantLabel("z_image_turbo_bf16.safetensors"), QStringLiteral("bf16"));
        QCOMPARE(quantLabel("z-image-turbo-Q8_0.gguf"), QStringLiteral("Q8_0"));
        QCOMPARE(quantLabel("z_image_turbo_int8_convrot.safetensors"), QStringLiteral("INT8"));
        QCOMPARE(quantLabel("flux-2-klein-9b.safetensors"), qualityTierName(QualityTier::Full));
    }
};

QTEST_APPLESS_MAIN(TestModelUse)
#include "test_model_use.moc"
