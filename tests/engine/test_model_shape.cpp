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

// Shapes as the reader gives them, by role.
ModelShape diffusionModel(int textWidth, int latentChannels, TextKind reads = TextKind::Llm, bool videoVae = false) {
    ModelShape shape;
    shape.textWidth = textWidth;
    shape.latentChannels = latentChannels;
    shape.textKind = reads;
    shape.videoVae = videoVae;
    shape.kind = ModelKind::DiffusionModel;
    return shape;
}
ModelShape textEncoder(int hiddenSize, TextKind kind = TextKind::Llm) {
    ModelShape shape;
    shape.hiddenSize = hiddenSize;
    shape.textKind = kind;
    shape.kind = ModelKind::TextEncoder;
    return shape;
}
ModelShape vae(int latentChannels, bool video = false) {
    ModelShape shape;
    shape.latentChannels = latentChannels;
    shape.videoVae = video;
    shape.kind = ModelKind::Vae;
    return shape;
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
        const ModelShape zImage = diffusionModel(2560, 16), klein4b = diffusionModel(7680, 32),
                         klein9b = diffusionModel(12288, 32);
        const ModelShape qwen4b = textEncoder(2560), qwen8b = textEncoder(4096);
        QCOMPARE(textEncoderFits(zImage, qwen4b), Fit::Fits);
        QCOMPARE(textEncoderFits(zImage, qwen8b), Fit::DoesNotFit);
        QCOMPARE(textEncoderFits(klein4b, qwen4b), Fit::Fits);
        QCOMPARE(textEncoderFits(klein4b, qwen8b), Fit::DoesNotFit);
        QCOMPARE(textEncoderFits(klein9b, qwen8b), Fit::Fits);
        QCOMPARE(textEncoderFits(klein9b, qwen4b), Fit::DoesNotFit); // 12288 is 4.8 x 2560
        const ModelShape ae = vae(16), flux2vae = vae(32);
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
        const ModelShape klein9b = diffusionModel(12288, 32);
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

    // Load Models loads one LLM encoder (llm_path): FLUX.1's T5 is no fit there, whatever its
    // width.
    void loadModelsTakesOnlyALanguageModelEncoder() {
        const ModelShape flux1 = diffusionModel(4096, 16, TextKind::T5);
        const ModelShape t5 = textEncoder(4096, TextKind::T5);
        QCOMPARE(textEncoderFits(flux1, t5), Fit::Fits);
        QCOMPARE(loadableTextEncoderFits(flux1, t5), Fit::DoesNotFit);
        const ModelShape klein4b = diffusionModel(7680, 32), qwen4b = textEncoder(2560);
        QCOMPARE(loadableTextEncoderFits(klein4b, qwen4b), Fit::Fits);
    }

    // Wan (#105): text_embedding beside a 3D patch_embedding, as its GGUF names them. It reads UMT5
    // (a T5) and wants a video VAE, and its UMT5 is the one T5 Load Models takes (t5xxl_path).
    void wanReadsUmt5AndTakesAVideoVae() {
        Gguf().text("general.architecture", "wan")
            .tensor("patch_embedding.weight", {5120, 16, 1, 2, 2})
            .tensor("text_embedding.0.weight", {5120, 4096})
            .write(file("wan2.2-t2v-low.gguf"));
        writeSafetensors(file("umt5.safetensors"), {{"shared.weight", QJsonArray{256384, 4096}},
                                                    {"encoder.block.0.layer.0.SelfAttention.q.weight", QJsonArray{4096, 4096}}});
        writeSafetensors(file("qwen_3_8b.safetensors"), {{"model.embed_tokens.weight", QJsonArray{151936, 4096}},
                                                         {"model.layers.0.self_attn.q_proj.weight", QJsonArray{4096, 4096}}});
        writeSafetensors(file("wan_2.1_vae.safetensors"), {{"decoder.conv1.weight", QJsonArray{384, 16, 3, 3, 3}}});
        writeSafetensors(file("ae.safetensors"), {{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}});
        const ModelShape wan = shape(file("wan2.2-t2v-low.gguf")), umt5 = shape(file("umt5.safetensors"));
        QCOMPARE(wan.textKind, std::optional<TextKind>(TextKind::T5));
        QCOMPARE(wan.videoVae, std::optional<bool>(true));
        QCOMPARE(wan.textWidth, std::optional<int>(4096));
        QCOMPARE(wan.latentChannels, std::optional<int>(16));
        // The image-to-video one takes 36 channels in (noise, mask and the first frame) and makes 16.
        Gguf().text("general.architecture", "wan")
            .tensor("patch_embedding.weight", {5120, 36, 1, 2, 2})
            .tensor("text_embedding.0.weight", {5120, 4096})
            .write(file("wan2.2-i2v-low.gguf"));
        QCOMPARE(shape(file("wan2.2-i2v-low.gguf")).latentChannels, std::optional<int>(16));
        QCOMPARE(loadableTextEncoderFits(wan, umt5), Fit::Fits);
        QCOMPARE(loadableTextEncoderFits(wan, shape(file("qwen_3_8b.safetensors"))), Fit::DoesNotFit); // an LLM, not T5
        QCOMPARE(vaeFits(wan, shape(file("wan_2.1_vae.safetensors"))), Fit::Fits);
        QCOMPARE(vaeFits(wan, shape(file("ae.safetensors"))), Fit::DoesNotFit); // 16 channels, but an image VAE
        // Only Wan's: FLUX.1's T5 stays out of Load Models.
        QCOMPARE(loadableTextEncoderFits(diffusionModel(4096, 16, TextKind::T5), umt5), Fit::DoesNotFit);
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

    // #86: what a file is, for a folder of one kind. Names as in the owner's LoRA folders.
    void aFilesKindComesFromItsTensorNames() {
        writeSafetensors(file("klein-lora.safetensors"),
                         {{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{16, 3072}},
                          {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{3072, 16}}});
        writeSafetensors(file("sdxl-lora.safetensors"),
                         {{"lora_te1_text_model_encoder_layers_0_mlp_fc1.lora_down.weight", QJsonArray{8, 768}},
                          {"lora_unet_down_blocks_0_attentions_0_proj_in.lora_up.weight", QJsonArray{320, 8}}});
        writeSafetensors(file("lokr.safetensors"), {{"diffusion_model.double_blocks.0.img_attn.proj.lokr_w1", QJsonArray{4, 4}},
                                                    {"diffusion_model.txt_in.weight", QJsonArray{3072, 7680}}}); // not read as a model
        writeSafetensors(file("sdxl.safetensors"), {{"model.diffusion_model.input_blocks.0.0.weight", QJsonArray{320, 4, 3, 3}},
                                                    {"first_stage_model.decoder.conv_in.weight", QJsonArray{512, 4, 3, 3}}});
        writeSafetensors(file("unet.safetensors"), {{"txt_in.weight", QJsonArray{3072, 7680}}, {"img_in.weight", QJsonArray{3072, 128}}});
        writeSafetensors(file("vae.safetensors"), {{"decoder.conv_in.weight", QJsonArray{512, 16, 3, 3}}});
        writeSafetensors(file("encoder.safetensors"), {{"model.embed_tokens.weight", QJsonArray{151936, 2560}}});
        const auto kind = [&](const char *name) { return shape(file(QString::fromLatin1(name))).kind; };
        QCOMPARE(kind("klein-lora.safetensors"), std::optional<ModelKind>(ModelKind::Lora));
        QCOMPARE(kind("sdxl-lora.safetensors"), std::optional<ModelKind>(ModelKind::Lora));
        QCOMPARE(kind("lokr.safetensors"), std::optional<ModelKind>(ModelKind::Lora));
        QCOMPARE(shape(file("lokr.safetensors")).textWidth, std::optional<int>());
        QCOMPARE(kind("sdxl.safetensors"), std::optional<ModelKind>(ModelKind::Checkpoint));
        QCOMPARE(kind("unet.safetensors"), std::optional<ModelKind>(ModelKind::DiffusionModel));
        QCOMPARE(kind("vae.safetensors"), std::optional<ModelKind>(ModelKind::Vae));
        QCOMPARE(kind("encoder.safetensors"), std::optional<ModelKind>(ModelKind::TextEncoder));
    }

    // #86: a LoRA fits a model whose layers it changes are there, as wide. Names and widths as in
    // the owner's files (Klein 9B is 4096 wide, Klein 4B 3072).
    void aLoraFitsTheModelWhoseLayersItChanges() {
        Gguf().text("general.architecture", "flux")
            .tensor("txt_in.weight", {4096, 12288})
            .tensor("img_in.weight", {4096, 128})
            .tensor("double_blocks.0.img_attn.proj.weight", {4096, 4096})
            .tensor("double_blocks.0.img_mlp.0.weight", {24576, 4096})
            .write(file("klein-9b.gguf"));
        writeSafetensors(file("klein9b-turbo.safetensors"),
                         {{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{128, 4096}},
                          {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{4096, 128}}});
        writeSafetensors(file("klein4b-rhc.safetensors"),
                         {{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{32, 3072}},
                          {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{3072, 32}}});
        // kohya's names: underscores for dots, a lora_unet_ prefix.
        writeSafetensors(file("kohya.safetensors"),
                         {{"lora_unet_double_blocks_0_img_mlp_0.lora_down.weight", QJsonArray{16, 4096}},
                          {"lora_unet_double_blocks_0_img_mlp_0.lora_up.weight", QJsonArray{24576, 16}},
                          {"lora_te1_text_model_encoder_layers_0_mlp_fc1.lora_down.weight", QJsonArray{16, 768}}});
        // A diffusers FLUX.1 LoRA: named so nothing in the model matches, but 3072 wide.
        writeSafetensors(file("flux1-diffusers.safetensors"),
                         {{"transformer.single_transformer_blocks.0.attn.to_k.lora_A.weight", QJsonArray{16, 3072}},
                          {"transformer.single_transformer_blocks.0.attn.to_k.lora_B.weight", QJsonArray{3072, 16}}});
        // ... and one as wide as Klein 9B: nothing is told.
        writeSafetensors(file("diffusers-4096.safetensors"),
                         {{"transformer.single_transformer_blocks.0.attn.to_k.lora_A.weight", QJsonArray{16, 4096}},
                          {"transformer.single_transformer_blocks.0.attn.to_k.lora_B.weight", QJsonArray{4096, 16}}});
        // A kohya SDXL LoRA: a UNet's layers, and Klein is a transformer.
        writeSafetensors(file("sdxl-kohya.safetensors"),
                         {{"lora_unet_down_blocks_1_attentions_0_proj_in.lora_down.weight", QJsonArray{8, 640}},
                          {"lora_unet_down_blocks_1_attentions_0_proj_in.lora_up.weight", QJsonArray{640, 8}}});
        // LoKr (Klein LoRAs in the owner's folder): 4x4 (x) 1024x1024 is 4096x4096; LoHa: w1_a, w1_b.
        writeSafetensors(file("klein-lokr.safetensors"),
                         {{"diffusion_model.double_blocks.0.img_attn.proj.lokr_w1", QJsonArray{4, 4}},
                          {"diffusion_model.double_blocks.0.img_attn.proj.lokr_w2", QJsonArray{1024, 1024}}});
        writeSafetensors(file("sdxl-loha.safetensors"),
                         {{"lora_unet_input_blocks_4_1_proj_in.hada_w1_a", QJsonArray{640, 8}},
                          {"lora_unet_input_blocks_4_1_proj_in.hada_w1_b", QJsonArray{8, 640}}});
        const ModelShape klein9b = shape(file("klein-9b.gguf"));
        QCOMPARE(loraFits(klein9b, shape(file("klein9b-turbo.safetensors"))), Fit::Fits);
        QCOMPARE(loraFits(klein9b, shape(file("klein4b-rhc.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(loraFits(klein9b, shape(file("kohya.safetensors"))), Fit::Fits);
        QCOMPARE(loraFits(klein9b, shape(file("flux1-diffusers.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(loraFits(klein9b, shape(file("diffusers-4096.safetensors"))), Fit::Unknown);
        QCOMPARE(loraFits(klein9b, shape(file("sdxl-kohya.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(loraFits(klein9b, shape(file("klein-lokr.safetensors"))), Fit::Fits);
        // FLUX.1 (19 double blocks) on a model as wide with fewer: block 18 is not there.
        writeSafetensors(file("flux1-deep.safetensors"),
                         {{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{16, 4096}},
                          {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{4096, 16}},
                          {"diffusion_model.double_blocks.18.img_attn.proj.lora_A.weight", QJsonArray{16, 4096}},
                          {"diffusion_model.double_blocks.18.img_attn.proj.lora_B.weight", QJsonArray{4096, 16}}});
        QCOMPARE(loraFits(klein9b, shape(file("flux1-deep.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(loraFits(klein9b, shape(file("sdxl-loha.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(layerKey("lora_unet_double_blocks_0_img_mlp_0"), layerKey("model.diffusion_model.double_blocks.0.img_mlp.0"));
    }

    // What the owner's runs crashed the helper on (2026-11-20): ComfyUI's mixed quantization, and a
    // Qwen3.5 encoder as wide as Klein 9B's Qwen3-8B.
    void formatsTheHelperCannotLoadAndLlmFamiliesAreTold() {
        writeSafetensors(file("qwen3.5_9b_int8_convrot.safetensors"),
                         {{"model.embed_tokens.weight", QJsonArray{248320, 4096}},
                          {"model.layers.0.linear_attn.in_proj_qkv.weight", QJsonArray{8192, 4096}},
                          {"model.layers.0.linear_attn.in_proj_qkv.comfy_quant", QJsonArray{3}}});
        writeSafetensors(file("qwen_3_8b.safetensors"), {{"model.embed_tokens.weight", QJsonArray{151936, 4096}},
                                                         {"model.layers.0.self_attn.q_norm.weight", QJsonArray{128}}});
        writeSafetensors(file("qwen_3_8b_fp8mixed.safetensors"),
                         {{"model.embed_tokens.weight", QJsonArray{151936, 4096}},
                          {"model.layers.0.self_attn.q_norm.weight", QJsonArray{128}},
                          {"model.layers.0.self_attn.q_proj.comfy_quant", QJsonArray{3}}});
        writeSafetensors(file("qwen3.5_9b.safetensors"), {{"model.embed_tokens.weight", QJsonArray{248320, 4096}},
                                                          {"model.layers.0.linear_attn.A_log", QJsonArray{32}}});
        writeSafetensors(file("svdq-z-image.safetensors"), {{"layers.0.attention.qkv.qweight", QJsonArray{4, 4}},
                                                            {"layers.0.attention.qkv.proj_down", QJsonArray{4, 4}},
                                                            {"cap_embedder.1.weight", QJsonArray{3840, 2560}}});
        const ModelShape klein9b = [] {
            ModelShape shape = diffusionModel(12288, 32);
            shape.llmFamily = QStringLiteral("qwen3");
            return shape;
        }();
        QVERIFY(shape(file("qwen3.5_9b_int8_convrot.safetensors")).unloadable.contains("ComfyUI"));
        QCOMPARE(loadableTextEncoderFits(klein9b, shape(file("qwen3.5_9b_int8_convrot.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(loadableTextEncoderFits(klein9b, shape(file("qwen_3_8b_fp8mixed.safetensors"))), Fit::DoesNotFit);
        QCOMPARE(shape(file("qwen_3_8b.safetensors")).llmFamily, QStringLiteral("qwen3"));
        QCOMPARE(loadableTextEncoderFits(klein9b, shape(file("qwen_3_8b.safetensors"))), Fit::Fits);
        QCOMPARE(shape(file("qwen3.5_9b.safetensors")).llmFamily, QStringLiteral("qwen3.5"));
        QCOMPARE(loadableTextEncoderFits(klein9b, shape(file("qwen3.5_9b.safetensors"))), Fit::DoesNotFit); // 4096 wide too
        QVERIFY(shape(file("svdq-z-image.safetensors")).unloadable.contains("SVDQuant"));
        // A Z-Image LoRA, named as ComfyUI saves one, on Klein: none of its layers are there.
        writeSafetensors(file("z-image-lora.safetensors"), {{"diffusion_model.layers.0.attention.to_k.lora_A.weight", QJsonArray{16, 3840}},
                                                            {"diffusion_model.layers.0.attention.to_k.lora_B.weight", QJsonArray{3840, 16}}});
        ModelShape klein = klein9b;
        klein.layers.insert(layerKey("double_blocks.0.img_attn.proj"), ModelShape::Widths{4096, 4096});
        QCOMPARE(loraFits(klein, shape(file("z-image-lora.safetensors"))), Fit::DoesNotFit);
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
