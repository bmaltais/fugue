// The Recommended mark among quantizations of one model (#370, engine/model_recommend.h).
//
// Seam: the rule over candidates with the header figures of the owner's Klein 9B and Qwen3 8B files, the
// device's memory and capability given, and a speed store in a temporary folder.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/model_recommend.h"

using namespace fugue;

namespace {

constexpr qint64 kGb = 1000000000;

ModelShape klein9() {
    ModelShape shape;
    shape.kind = ModelKind::DiffusionModel;
    shape.textWidth = 12288;
    shape.hidden = 4096;
    shape.latentChannels = 32;
    shape.videoVae = false;
    return shape;
}

ModelShape qwen8() {
    ModelShape shape;
    shape.kind = ModelKind::TextEncoder;
    shape.llmFamily = QStringLiteral("qwen3");
    shape.hiddenSize = 4096;
    shape.textKind = TextKind::Llm;
    return shape;
}

// Krea 2's header figures (#684): the real INT8 file's.
ModelShape krea2() {
    ModelShape shape;
    shape.kind = ModelKind::DiffusionModel;
    shape.textWidth = 2560;
    shape.hidden = 6144;
    shape.latentChannels = 16;
    shape.videoVae = true;
    shape.llmFamily = QStringLiteral("qwen3");
    return shape;
}

ComputeDevice cuda3090() { return {"cuda", "NVIDIA GeForce RTX 3090", 8.6, 24576}; }
ComputeDevice vulkan() { return {"vulkan", "AMD Radeon", std::nullopt, 24576}; }

} // namespace

class TestModelRecommend : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    // A file of `size` bytes on disk (its size is part of a speed's key), as a candidate whose header is `shape`.
    RecommendCandidate file(const QString &name, ModelShape shape, qint64 reported, int size = 10) {
        const QString path = m_dir.filePath(name);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(QByteArray(size, 'x')) != size)
            qFatal("cannot write %s", qPrintable(path));
        return {path, reported, shape};
    }
    QString at(const QString &name) const { return m_dir.filePath(name); }

private slots:
    // On a 3090 with no run measured, INT8 is the default; the kv file is another model, the base is another
    // family, and a file with no known family is in no group.
    void klein9bOnTheOwnersFolder() {
        const QList<RecommendCandidate> files{
            file("flux-2-klein-9b-Q8_0.gguf", klein9(), 9 * kGb),
            file("flux-2-klein-9b_int8_convrot.safetensors", klein9(), 9 * kGb, 11),
            file("flux-2-klein-9b-kv-Q8_0.gguf", klein9(), 9 * kGb),
            file("flux-2-klein-base-9b-Q8_0.gguf", klein9(), 9 * kGb),
            file("mystery.gguf", ModelShape{}, 9 * kGb),
        };
        const auto picked = recommend(files, {cuda3090(), 0, nullptr});
        QCOMPARE(picked.size(), 1);
        QVERIFY(picked.contains(at("flux-2-klein-9b_int8_convrot.safetensors")));
        QCOMPARE(picked.value(at("flux-2-klein-9b_int8_convrot.safetensors")).reason,
                 "Recommended: expected to be fastest here at Q8-level quality");
        // Vulkan has no INT8 default: GGUF Q8.
        const auto onVulkan = recommend(files, {vulkan(), 0, nullptr});
        QVERIFY(onVulkan.contains(at("flux-2-klein-9b-Q8_0.gguf")));
        QCOMPARE(onVulkan.size(), 1);
    }
    // Krea 2 Turbo (#684): the 25 GB bf16 file does not fit a 24 GB card, so the INT8 one is the pick; Raw is another model.
    void krea2TurboOnA3090() {
        const QList<RecommendCandidate> files{
            file("krea2_turbo_bf16.safetensors", krea2(), 25 * kGb),
            file("krea2_turbo_int8_convrot.safetensors", krea2(), 13492686496),
            file("krea2_raw_bf16.safetensors", krea2(), 25 * kGb),
        };
        const auto picked = recommend(files, {cuda3090(), 4858, nullptr});
        QCOMPARE(picked.size(), 1);
        QVERIFY(picked.contains(at("krea2_turbo_int8_convrot.safetensors")));
    }
    // 2 and 3: the memory is dropped on first, then the best tier left wins, whatever the speed.
    void memoryThenTier() {
        const QList<RecommendCandidate> files{
            file("flux-2-klein-9b-bf16.safetensors", klein9(), 22 * kGb),
            file("flux-2-klein-9b-Q8_0.gguf", klein9(), 9 * kGb),
            file("flux-2-klein-9b-Q4_K_M.gguf", klein9(), 5 * kGb),
            file("flux-2-klein-9b-fp8.safetensors", klein9(), 9 * kGb),
        };
        // Plenty of memory: full precision, though it is the slowest to run.
        auto picked = recommend(files, {{"cuda", "big", 8.6, 80000}, 0, nullptr});
        QVERIFY(picked.contains(at("flux-2-klein-9b-bf16.safetensors")));
        QCOMPARE(picked.size(), 1);
        // 24 GB: 22 GB plus a fifth does not fit; Q8 does.
        picked = recommend(files, {cuda3090(), 0, nullptr});
        QVERIFY(picked.contains(at("flux-2-klein-9b-Q8_0.gguf")));
        // The workflow's other models take memory too: Q8 no longer fits, fp8 neither, Q4 does.
        picked = recommend(files, {cuda3090(), 14000, nullptr});
        QVERIFY(picked.contains(at("flux-2-klein-9b-Q4_K_M.gguf")));
        QCOMPARE(picked.value(at("flux-2-klein-9b-Q4_K_M.gguf")).reason, "Recommended: best quality that fits here (Q4 level)");
        // Nothing fits: no mark.
        QVERIFY(recommend(files, {cuda3090(), 30000, nullptr}).isEmpty());
        // A card whose memory is not known drops nothing.
        QVERIFY(recommend(files, {{"vulkan", "x", std::nullopt, 0}, 0, nullptr}).contains(at("flux-2-klein-9b-bf16.safetensors")));
    }
    // Speed: a measurement of every file in the tier beats the default; one missing leaves the default.
    void measuredSpeedBeatsTheDefault() {
        const QList<RecommendCandidate> files{
            file("flux-2-klein-9b-Q8_0.gguf", klein9(), 9 * kGb),
            file("flux-2-klein-9b_int8_convrot.safetensors", klein9(), 9 * kGb, 11),
        };
        const QString int8 = at("flux-2-klein-9b_int8_convrot.safetensors");
        const QString q8 = at("flux-2-klein-9b-Q8_0.gguf");
        ModelSpeeds speeds(m_dir.filePath("speeds.json"));
        const ComputeDevice device = cuda3090();
        speeds.record(device.id(), q8, 0.6);
        // Only one measured: the default (INT8 on this card).
        QVERIFY(recommend(files, {device, 0, &speeds}).contains(int8));
        QVERIFY(recommend(files, {device, 0, &speeds}).value(int8).reason.contains("expected"));
        // Both measured, and GGUF turns out faster here: the measurement wins.
        speeds.record(device.id(), int8, 0.9);
        const auto picked = recommend(files, {device, 0, &speeds});
        QVERIFY(picked.contains(q8));
        QCOMPARE(picked.value(q8).reason, "Recommended: fastest here at Q8-level quality");
        // Another device has none of it: its default again.
        QVERIFY(recommend(files, {{"cuda", "other", 8.9, 24576}, 0, &speeds}).contains(int8));
    }
    // The store keeps the median of the last five, and survives a new object over the same file.
    void speedStore() {
        const RecommendCandidate one = file("store-model.gguf", klein9(), kGb);
        ModelSpeeds speeds(m_dir.filePath("store.json"));
        for (double seconds : {9.0, 1.0, 2.0, 3.0, 4.0, 5.0})
            speeds.record("cuda:a", one.path, seconds);
        QCOMPARE(*ModelSpeeds(m_dir.filePath("store.json")).speed("cuda:a", one.path), 3.0); // 1 2 3 4 5; the 9 aged out
        QVERIFY(!speeds.speed("cuda:b", one.path));
    }
    // Files stable-diffusion.cpp cannot load, and groups of one, get no mark; Qwen3 8B is grouped by the
    // text encoder's header and the stem.
    void unloadableAndAlone() {
        ModelShape blocked = qwen8();
        blocked.unloadable = QStringLiteral("GPTQ");
        QList<RecommendCandidate> files{
            file("Qwen3-8B-Q8_0.gguf", qwen8(), 8 * kGb),
            file("qwen_3_8b_fp8mixed.safetensors", blocked, 8 * kGb),
        };
        QVERIFY(recommend(files, {cuda3090(), 0, nullptr}).isEmpty()); // one loadable file is a group of one... plus one unloadable
        files << file("qwen_3_8b_int8_convrot.safetensors", qwen8(), 8 * kGb, 12);
        const auto picked = recommend(files, {cuda3090(), 0, nullptr});
        QCOMPARE(picked.size(), 1);
        QVERIFY(picked.contains(at("qwen_3_8b_int8_convrot.safetensors")));
    }
    void tiers() {
        QVERIFY(qualityTier("a-bf16.safetensors") > qualityTier("a-Q8_0.gguf"));
        QCOMPARE(qualityTier("a-Q8_0.gguf"), qualityTier("a_int8_convrot.safetensors"));
        QVERIFY(qualityTier("a-Q8_0.gguf") > qualityTier("a-fp8_scaled.safetensors"));
        QVERIFY(qualityTier("a-fp8_scaled.safetensors") > qualityTier("a-Q6_K.gguf"));
        QVERIFY(qualityTier("a-Q6_K.gguf") > qualityTier("a-Q5_K_M.gguf"));
        QVERIFY(qualityTier("a-Q5_K_M.gguf") > qualityTier("a-Q4_K_S.gguf"));
        QCOMPARE(qualityTier("a.gguf"), QualityTier::Unknown);
    }
};

QTEST_APPLESS_MAIN(TestModelRecommend)
#include "test_model_recommend.moc"
