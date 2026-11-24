// The text-encoder rows of the family table (#370, engine/model_family.h): the pickers' family filter
// names an LLM encoder by its file, as it names a diffusion model.

#include <QtTest>

#include "engine/model_family.h"

using namespace fugue;

class TestModelFamilyEncoders : public QObject {
    Q_OBJECT
private slots:
    void qwen3Encoders() {
        QCOMPARE(modelFamily("fugue/text-encoder", "Qwen3-8B-Q8_0.gguf"), "Qwen3 8B");
        QCOMPARE(modelFamily("fugue/text-encoder", "qwen_3_8b_int8_convrot.safetensors"), "Qwen3 8B");
        QCOMPARE(modelFamily("fugue/text-encoder", "qwen_3_4b.safetensors"), "Qwen3 4B");
        QCOMPARE(modelFamily("fugue/text-encoder", "Qwen3.5-9B-Q4_1.gguf"), "Qwen3.5 9B");
        QVERIFY(modelFamily("fugue/text-encoder", "t5xxl_fp16.safetensors").isEmpty());
        // A diffusion model of that name is not an encoder.
        QVERIFY(modelFamily("fugue/diffusion-model", "Qwen3-8B-Q8_0.gguf").isEmpty());
        QVERIFY(knownModelFamilies().contains("Qwen3 8B"));
    }
};

QTEST_APPLESS_MAIN(TestModelFamilyEncoders)
#include "test_model_family_encoders.moc"
