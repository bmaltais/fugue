// The text-encoder rows of the family table (#370, engine/model_family.h): the pickers' family filter
// names an LLM encoder by its file, as it names a diffusion model.

#include <QJsonArray>
#include <QJsonObject>
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

    // Krea 2 (#684), Turbo and Raw, and the Qwen3-VL 4B it reads, which the Qwen3 4B pattern would also match.
    void krea2AndItsEncoder() {
        for (const char *name : {"krea2_turbo_int8_convrot.safetensors", "krea2_raw_bf16.safetensors", "Krea-2-Turbo-Q8_0.gguf"})
            QCOMPARE(modelFamily("fugue/diffusion-model", name), "Krea 2");
        QCOMPARE(modelFamily("fugue/text-encoder", "qwen3vl_4b_int8_convrot.safetensors"), "Qwen3-VL 4B");
        QCOMPARE(modelFamily("fugue/text-encoder", "qwen3vl_4b_fp8_scaled.safetensors"), "Qwen3-VL 4B");
        QCOMPARE(modelFamily("fugue/text-encoder", "Qwen3-VL-4B-Instruct-Q8_0.gguf"), "Qwen3-VL 4B");
        QCOMPARE(modelFamily("fugue/text-encoder", "qwen_3_4b.safetensors"), "Qwen3 4B"); // unchanged
        QVERIFY(knownModelFamilies().contains("Krea 2"));
        QVERIFY(knownModelFamilies().contains("Qwen3-VL 4B"));
        QCOMPARE(modelFamilies(QJsonArray{QJsonObject{{"resource", "fugue/diffusion-model"}, {"name", "krea2_turbo_int8_convrot.safetensors"}}}),
                 QStringList{"Krea 2"});
    }
};

QTEST_APPLESS_MAIN(TestModelFamilyEncoders)
#include "test_model_family_encoders.moc"
