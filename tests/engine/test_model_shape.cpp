// Model shapes from file headers, and which parts fit which diffusion model (#85).
//
// Seam: readModelShape on small GGUF and safetensors files written here with the tensor names
// and shapes of the owner's models (read from their headers on 2026-11-20), and the fit rules.

#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cstring>

#include "engine/model_shape.h"

using namespace fugue;

namespace {

// A GGUF v3 header: metadata, then tensor infos, no data (the reader stops before it).
class Gguf {
public:
    Gguf &text(const QByteArray &key, const QByteArray &value) {
        key_(key);
        u32(8);
        str(value);
        ++m_fields;
        return *this;
    }
    Gguf &u32Field(const QByteArray &key, quint32 value) {
        key_(key);
        u32(4);
        u32(value);
        ++m_fields;
        return *this;
    }
    // A list of strings, as a tokenizer's vocabulary is: read past, never kept.
    Gguf &strings(const QByteArray &key, int count) {
        key_(key);
        u32(9);
        u32(8);
        u64(quint64(count));
        for (int i = 0; i < count; ++i)
            str("tok" + QByteArray::number(i));
        ++m_fields;
        return *this;
    }
    // A tensor with its shape as safetensors writes it (outermost first); GGUF stores it turned.
    Gguf &tensor(const QByteArray &name, QList<quint64> shape) {
        m_tensors += [&] {
            QByteArray out;
            appendStr(out, name);
            appendU32(out, quint32(shape.size()));
            std::reverse(shape.begin(), shape.end());
            for (quint64 d : shape)
                appendU64(out, d);
            appendU32(out, 8);  // Q8_0
            appendU64(out, 0);  // offset
            return out;
        }();
        ++m_tensorCount;
        return *this;
    }
    void write(const QString &path) const {
        QByteArray out("GGUF");
        appendU32(out, 3);
        appendU64(out, m_tensorCount);
        appendU64(out, m_fields);
        out += m_kv + m_tensors;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(out) != out.size())
            qFatal("cannot write %s", qPrintable(path));
    }

private:
    static void appendU32(QByteArray &out, quint32 v) { out.append(reinterpret_cast<const char *>(&v), 4); }
    static void appendU64(QByteArray &out, quint64 v) { out.append(reinterpret_cast<const char *>(&v), 8); }
    static void appendStr(QByteArray &out, const QByteArray &s) {
        appendU64(out, quint64(s.size()));
        out += s;
    }
    void key_(const QByteArray &key) { appendStr(m_kv, key); }
    void u32(quint32 v) { appendU32(m_kv, v); }
    void u64(quint64 v) { appendU64(m_kv, v); }
    void str(const QByteArray &s) { appendStr(m_kv, s); }

    QByteArray m_kv, m_tensors;
    quint64 m_fields = 0, m_tensorCount = 0;
};

void writeSafetensors(const QString &path, const QJsonObject &shapes) {
    QJsonObject header{{"__metadata__", QJsonObject{{"format", "pt"}}}};
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        header.insert(it.key(), QJsonObject{{"dtype", "F16"}, {"shape", it.value()}, {"data_offsets", QJsonArray{0, 0}}});
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out(8, '\0');
    const quint64 size = quint64(json.size());
    std::memcpy(out.data(), &size, 8);
    out += json;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(out) != out.size())
        qFatal("cannot write %s", qPrintable(path));
}

ModelShape shape(const QString &path) {
    const std::optional<ModelShape> read = readModelShape(path);
    if (!read)
        qFatal("no shape read from %s", qPrintable(path));
    return *read;
}

} // namespace

class TestModelShape : public QObject {
    Q_OBJECT

private slots:
    void init() { QVERIFY(m_dir.isValid()); }

    void theOwnersModelsReadAsTheyAre() {
        Gguf().text("general.architecture", "lumina2")
            .tensor("cap_embedder.0.weight", {2560})
            .tensor("cap_embedder.1.weight", {3840, 2560})
            .tensor("x_embedder.weight", {3840, 64})
            .write(file("z-image-turbo.gguf"));
        Gguf().text("general.architecture", "flux")
            .tensor("img_in.weight", {3072, 128})
            .tensor("txt_in.weight", {3072, 7680})
            .write(file("klein-4b.gguf"));
        Gguf().text("general.architecture", "qwen3")
            .strings("tokenizer.ggml.tokens", 1000)
            .u32Field("qwen3.block_count", 36)
            .u32Field("qwen3.embedding_length", 4096)
            .tensor("token_embd.weight", {151936, 4096})
            .write(file("Qwen3-8B.gguf"));
        writeSafetensors(file("flux2-vae.safetensors"),
                         {{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}},
                          {"encoder.conv_in.weight", QJsonArray{128, 3, 3, 3}}});

        const ModelShape zImage = shape(file("z-image-turbo.gguf"));
        QCOMPARE(zImage.textWidth, std::optional<int>(2560));
        QCOMPARE(zImage.latentChannels, std::optional<int>(16)); // 2x2 patches of 16
        const ModelShape klein = shape(file("klein-4b.gguf"));
        QCOMPARE(klein.textWidth, std::optional<int>(7680));
        QCOMPARE(klein.latentChannels, std::optional<int>(32));
        QCOMPARE(shape(file("Qwen3-8B.gguf")).hiddenSize, std::optional<int>(4096));
        QCOMPARE(shape(file("flux2-vae.safetensors")).latentChannels, std::optional<int>(32));
    }

    void aSafetensorsModelWithAPrefixAndAUnetRead() {
        writeSafetensors(file("klein.safetensors"), {{"model.diffusion_model.txt_in.weight", QJsonArray{3072, 7680}},
                                                     {"model.diffusion_model.img_in.weight", QJsonArray{3072, 128}}});
        QCOMPARE(shape(file("klein.safetensors")).textWidth, std::optional<int>(7680));
        // An SD 1.5 UNet takes its 4 channels through a convolution, not in patches.
        writeSafetensors(file("sd15.safetensors"), {{"model.diffusion_model.input_blocks.0.0.weight", QJsonArray{320, 4, 3, 3}}});
        QCOMPARE(shape(file("sd15.safetensors")).latentChannels, std::optional<int>(4));
        // A text encoder in safetensors: the width of its token embedding.
        writeSafetensors(file("qwen_2.5_vl_7b.safetensors"), {{"model.embed_tokens.weight", QJsonArray{152064, 3584}}});
        QCOMPARE(shape(file("qwen_2.5_vl_7b.safetensors")).hiddenSize, std::optional<int>(3584));
    }

    void aFileThatIsNoModelHasNoShape() {
        QFile text(file("notes.safetensors"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("not a header at all");
        text.close();
        QVERIFY(!readModelShape(file("notes.safetensors")));
        QVERIFY(!readModelShape(file("missing.gguf")));
        QFile cut(file("cut.gguf")); // a GGUF file that ends inside its header
        QVERIFY(cut.open(QIODevice::WriteOnly));
        cut.write(QByteArray("GGUF\x03\0\0\0", 8) + QByteArray(4, '\xff'));
        cut.close();
        QVERIFY(!readModelShape(file("cut.gguf")));
    }

    // The owner's pairs (helper.md): Z-Image and Klein 4B read Qwen3-4B, Klein 9B Qwen3-8B; Z-Image
    // decodes with ae (16 channels), Klein with flux2-vae (32).
    void theFitRulesPairTheOwnersModels() {
        const ModelShape zImage{2560, 16, {}, TextKind::Llm, false}, klein4b{7680, 32, {}, TextKind::Llm, false},
            klein9b{12288, 32, {}, TextKind::Llm, false};
        const ModelShape qwen4b{{}, {}, 2560, TextKind::Llm, {}}, qwen8b{{}, {}, 4096, TextKind::Llm, {}};
        QCOMPARE(textEncoderFits(zImage, qwen4b), Fit::Fits);
        QCOMPARE(textEncoderFits(zImage, qwen8b), Fit::DoesNotFit);
        QCOMPARE(textEncoderFits(klein4b, qwen4b), Fit::Fits);
        QCOMPARE(textEncoderFits(klein4b, qwen8b), Fit::DoesNotFit);
        QCOMPARE(textEncoderFits(klein9b, qwen8b), Fit::Fits);
        QCOMPARE(textEncoderFits(klein9b, qwen4b), Fit::DoesNotFit); // 12288 is 4.8 x 2560
        const ModelShape ae{{}, 16, {}, {}, false}, flux2vae{{}, 32, {}, {}, false};
        QCOMPARE(vaeFits(zImage, ae), Fit::Fits);
        QCOMPARE(vaeFits(zImage, flux2vae), Fit::DoesNotFit);
        QCOMPARE(vaeFits(klein9b, flux2vae), Fit::Fits);
        // What a header does not tell is never "does not fit".
        QCOMPARE(textEncoderFits(ModelShape{}, qwen4b), Fit::Unknown);
        QCOMPARE(vaeFits(zImage, ModelShape{}), Fit::Unknown);
    }

    // Widths alone mix kinds up; the tensor names tell them apart.
    void theKindsComeFromTheTensorNames() {
        writeSafetensors(file("t5xxl.safetensors"), {{"encoder.embed_tokens.weight", QJsonArray{32128, 4096}},
                                                     {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}});
        writeSafetensors(file("qwen_3_8b.safetensors"), {{"model.embed_tokens.weight", QJsonArray{151936, 4096}},
                                                         {"model.layers.0.self_attn.q_proj.weight", QJsonArray{4096, 4096}}});
        writeSafetensors(file("clip_l.safetensors"), {{"text_model.embeddings.token_embedding.weight", QJsonArray{49408, 768}}});
        writeSafetensors(file("flux1-dev.safetensors"), {{"txt_in.weight", QJsonArray{3072, 4096}},
                                                         {"vector_in.in_layer.weight", QJsonArray{3072, 768}},
                                                         {"img_in.weight", QJsonArray{3072, 64}}});
        Gguf().text("general.architecture", "qwen_image")
            .tensor("img_in.weight", {3072, 64})
            .tensor("txt_in.weight", {3072, 3584})
            .tensor("time_text_embed.timestep_embedder.linear_1.weight", {3072, 256})
            .write(file("qwen-image-edit.gguf"));
        writeSafetensors(file("qwen_image_vae.safetensors"), {{"decoder.conv1.weight", QJsonArray{384, 16, 3, 3, 3}}});
        writeSafetensors(file("ae.safetensors"), {{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}});

        const ModelShape t5 = shape(file("t5xxl.safetensors")), qwen8b = shape(file("qwen_3_8b.safetensors"));
        QCOMPARE(t5.textKind, std::optional<TextKind>(TextKind::T5));
        QCOMPARE(qwen8b.textKind, std::optional<TextKind>(TextKind::Llm));
        QCOMPARE(shape(file("clip_l.safetensors")).textKind, std::optional<TextKind>(TextKind::Clip));
        const ModelShape klein9b{12288, 32, {}, TextKind::Llm, false};
        QCOMPARE(textEncoderFits(klein9b, qwen8b), Fit::Fits);
        QCOMPARE(textEncoderFits(klein9b, t5), Fit::DoesNotFit); // 4096 wide too, but T5
        const ModelShape flux1 = shape(file("flux1-dev.safetensors"));
        QCOMPARE(textEncoderFits(flux1, t5), Fit::Fits);
        QCOMPARE(textEncoderFits(flux1, qwen8b), Fit::DoesNotFit);

        const ModelShape qwenImage = shape(file("qwen-image-edit.gguf"));
        const ModelShape videoVae = shape(file("qwen_image_vae.safetensors")), ae = shape(file("ae.safetensors"));
        QCOMPARE(videoVae.latentChannels, std::optional<int>(16));
        QCOMPARE(vaeFits(qwenImage, videoVae), Fit::Fits);
        QCOMPARE(vaeFits(qwenImage, ae), Fit::DoesNotFit); // 16 channels too, but not a video VAE
        QCOMPARE(vaeFits(flux1, ae), Fit::Fits);
        QCOMPARE(vaeFits(flux1, videoVae), Fit::DoesNotFit);
    }

    // A list of lists is no model header: refused, not followed (it could recurse without end).
    void aGgufWithAListOfListsHasNoShape() {
        QByteArray out("GGUF");
        const auto u32 = [&](quint32 v) { out.append(reinterpret_cast<const char *>(&v), 4); };
        const auto u64 = [&](quint64 v) { out.append(reinterpret_cast<const char *>(&v), 8); };
        u32(3);
        u64(0); // tensors
        u64(1); // fields
        u64(3);
        out += "key";
        for (int depth = 0; depth < 1000; ++depth) { // an array of an array of ...
            u32(9);
            u32(9);
            u64(1);
        }
        QFile file(this->file("nested.gguf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(out);
        file.close();
        QVERIFY(!readModelShape(this->file("nested.gguf")));
    }

    // Load Models loads one LLM encoder (llm_path): FLUX.1's T5 is no fit there, whatever its width.
    void loadModelsTakesOnlyALanguageModelEncoder() {
        const ModelShape flux1{4096, 16, {}, TextKind::T5, false};
        const ModelShape t5{{}, {}, 4096, TextKind::T5, {}};
        QCOMPARE(textEncoderFits(flux1, t5), Fit::Fits);
        QCOMPARE(loadableTextEncoderFits(flux1, t5), Fit::DoesNotFit);
        const ModelShape klein4b{7680, 32, {}, TextKind::Llm, false}, qwen4b{{}, {}, 2560, TextKind::Llm, {}};
        QCOMPARE(loadableTextEncoderFits(klein4b, qwen4b), Fit::Fits);
    }

    // A diffusers FLUX.2 names its text input context_embedder, as SD3 does, but reads an LLM.
    void aDiffusersFlux2ReadsALanguageModel() {
        writeSafetensors(file("flux2-diffusers.safetensors"), {{"context_embedder.weight", QJsonArray{3072, 7680}},
                                                               {"x_embedder.weight", QJsonArray{3072, 128}},
                                                               {"transformer_blocks.0.attn.to_q.weight", QJsonArray{3072, 3072}}});
        const ModelShape flux2 = shape(file("flux2-diffusers.safetensors"));
        QCOMPARE(flux2.textKind, std::optional<TextKind>(TextKind::Llm));
        QCOMPARE(flux2.textWidth, std::optional<int>(7680));
        writeSafetensors(file("sd3.safetensors"), {{"context_embedder.weight", QJsonArray{1536, 4096}},
                                                   {"joint_blocks.0.x_block.attn.qkv.weight", QJsonArray{4608, 1536}}});
        QCOMPARE(shape(file("sd3.safetensors")).textKind, std::optional<TextKind>(TextKind::T5));
    }

    void aShapeIsReadAgainOnlyWhenTheFileChanges() {
        writeSafetensors(file("vae.safetensors"), {{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}});
        ModelShapes shapes;
        QCOMPARE(shapes.of(file("vae.safetensors"))->latentChannels, std::optional<int>(16));
        writeSafetensors(file("vae.safetensors"), {{"decoder.conv_in.weight", QJsonArray{512, 32, 3, 3}},
                                                   {"decoder.norm_out.weight", QJsonArray{128}}}); // another size
        QCOMPARE(shapes.of(file("vae.safetensors"))->latentChannels, std::optional<int>(32));
    }

    // By hand: FUGUE_REAL_MODELS=<a ComfyUI models folder> prints what every file of the
    // diffusion model, text encoder and VAE folders reads as (nothing is asserted).
    void aRealModelsFolderReads() {
        const QString root = qEnvironmentVariable("FUGUE_REAL_MODELS");
        if (root.isEmpty())
            QSKIP("set FUGUE_REAL_MODELS to read a real models folder");
        const auto figure = [](const std::optional<int> &n) { return n ? QString::number(*n) : QStringLiteral("-"); };
        for (const char *folder : {"diffusion_models", "unet", "clip", "text_encoders", "vae", "checkpoints"}) {
            QDirIterator files(root + "/" + folder, {"*.gguf", "*.safetensors"}, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const QString path = files.next();
                QElapsedTimer clock;
                clock.start();
                const std::optional<ModelShape> read = readModelShape(path);
                qInfo("%-60s text %-6s latent %-4s hidden %-6s %lld ms", qPrintable(path.mid(root.size() + 1)),
                      qPrintable(read ? figure(read->textWidth) : "?"), qPrintable(read ? figure(read->latentChannels) : "?"),
                      qPrintable(read ? figure(read->hiddenSize) : "?"), clock.elapsed());
            }
        }
    }

private:
    QString file(const QString &name) const { return m_dir.filePath(name); }
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestModelShape)
#include "test_model_shape.moc"
