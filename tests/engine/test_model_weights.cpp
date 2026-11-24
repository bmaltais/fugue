// Which files are the same weights (#370, engine/model_weights.h, and the text-encoder rows of
// engine/model_family.h).
//
// Seam: ModelShape values with the figures of the owner's files (read from their headers on 2026-11-24).

#include <QtTest>

#include "engine/model_weights.h"

using namespace fugue;

namespace {

ModelShape diffusion(int text, int block, int channels) {
    ModelShape shape;
    shape.kind = ModelKind::DiffusionModel;
    shape.textWidth = text;
    shape.hidden = block;
    shape.latentChannels = channels;
    return shape;
}

} // namespace

class TestModelWeights : public QObject {
    Q_OBJECT
private slots:
    void stems() {
        QCOMPARE(modelStem("flux-2-klein-9b-Q8_0.gguf"), modelStem("flux_2_klein_9b.int8_convrot.safetensors"));
        QCOMPARE(modelStem("Qwen3-8B-Q8_0.gguf"), modelStem("qwen_3_8b_int8_convrot.safetensors"));
        QCOMPARE(modelStem("Qwen_Image_Edit-Q6_K.gguf"), modelStem("qwen-image-edit-Q5_K_M.gguf"));
        QCOMPARE(modelStem("z_image_bf16.safetensors"), modelStem("z_image-Q8_0.gguf"));
    }
    // Distilled and base Klein share every shape; the stem, not the header, tells them apart. So do the
    // kv file, a point release and a fine-tune.
    void differentWeights() {
        const auto klein = std::optional<ModelShape>(diffusion(12288, 4096, 32));
        const QString plain = weightsKey(klein, "flux-2-klein-9b-Q8_0.gguf");
        QVERIFY(!plain.isEmpty());
        QCOMPARE(weightsKey(klein, "flux-2-klein-9b_int8_convrot.safetensors"), plain);
        QVERIFY(weightsKey(klein, "flux-2-klein-base-9b-Q8_0.gguf") != plain);
        QVERIFY(weightsKey(klein, "flux-2-klein-9b-kv-Q8_0.gguf") != plain);
        QVERIFY(weightsKey(klein, "qwen-image-edit-2509-Q5_K_M.gguf") != weightsKey(klein, "qwen-image-edit-2511-Q4_K_M.gguf"));
        QVERIFY(modelStem("Qwen3-8B-Q8_0.gguf") != modelStem("qwen3-8b-abliterated-q8_0.gguf"));
        // Another header under the same name is another model.
        QVERIFY(weightsKey(std::optional<ModelShape>(diffusion(7680, 3072, 32)), "flux-2-klein-9b-Q8_0.gguf") != plain);
    }
    // No header, no family, no key: such a file is never grouped.
    void unknownIsNeverGrouped() {
        QVERIFY(weightsKey(std::nullopt, "flux-2-klein-9b-Q8_0.gguf").isEmpty());
        QVERIFY(weightsKey(std::optional<ModelShape>(ModelShape{}), "flux-2-klein-9b-Q8_0.gguf").isEmpty());
        QVERIFY(weightsKey(std::optional<ModelShape>(diffusion(1, 2, 3)), "mystery-Q8_0.gguf").isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestModelWeights)
#include "test_model_weights.moc"
