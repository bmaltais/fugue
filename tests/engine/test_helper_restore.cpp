// What the helper's restore op decides before it loads anything (helper/src/restore.h, #346): the size and
// tiling it refuses, the files it recognises by their tensor names, the paths it will not put in a program,
// the program's literals and the one bar it reports. The engine's checker runs the program in
// helper/test/restore_test.cpp (it needs the engine, which the app's tests do not link).

#include <QtTest>

#include <set>
#include <string>

#include "../../helper/src/restore.h"

using namespace fugue_helper;

class TestHelperRestore : public QObject {
    Q_OBJECT

private slots:
    void aPictureTooSmallOrTooBigIsRefusedWithTheLimit() {
        QVERIFY(restoreGeometryError(512, 512).empty());
        QVERIFY(restoreGeometryError(16, 16).empty());
        QVERIFY(restoreGeometryError(4098, 4098).empty()); // what was measured to finish
        QVERIFY(restoreGeometryError(15, 600).find("at least 16") != std::string::npos);
        QVERIFY(restoreGeometryError(600, 4).find("at least 16") != std::string::npos);
        const std::string big = restoreGeometryError(4200, 4200);
        QVERIFY2(big.find("17.6 MP") != std::string::npos && big.find("at most 16.8 MP") != std::string::npos, big.c_str());
        QVERIFY(!restoreGeometryError(16385, 100).empty()); // one side past the limit, few pixels
        QVERIFY(!restoreGeometryError(2'000'000'000, 2'000'000'000).empty()); // no overflow
    }

    void tileAndOverlapAreWholeNumbersInRange() {
        QVERIFY(restoreTilingError(512, 64).empty());
        QVERIFY(restoreTilingError(64, 32).empty());
        QVERIFY(restoreTilingError(16384, 0).empty());
        for (const auto &[tile, overlap] : {std::pair<int64_t, int64_t>{63, 0}, {16385, 0}, {512, -1}, {512, 257}, {-5, 0}})
            QVERIFY2(!restoreTilingError(tile, overlap).empty(), (std::to_string(tile) + "/" + std::to_string(overlap)).c_str());
    }

    void pathsMustBeUtf8() {
        QVERIFY(isValidUtf8("/models/seedvr2.safetensors"));
        QVERIFY(isValidUtf8("/modèles/日本語.safetensors"));
        QVERIFY(!isValidUtf8(std::string("/models/\xff.safetensors")));
        QVERIFY(!isValidUtf8(std::string("/models/\xc3"))); // cut short
        QVERIFY(!isValidUtf8(std::string("/models/\xc0\x80"))); // overlong
    }

    void aDiffusionModelMustCarryItsEmbeddings() {
        const std::set<std::string> comfy{"vid_in.proj.weight", "positive_conditioning", "negative_conditioning", "blocks.0.attn.proj_qkv.vid.weight"};
        QVERIFY(restoreModelError(comfy).empty());
        QVERIFY(restoreModelError({"model.diffusion_model.vid_in.proj.weight", "model.diffusion_model.positive_conditioning"}).empty());
        // The GGUF and the numz fp8 files have the layers and no embedding.
        const std::string noEmbedding = restoreModelError({"vid_in.proj.weight", "blocks.0.attn.rope.rope.freqs"});
        QVERIFY2(noEmbedding.find("positive_conditioning") != std::string::npos, noEmbedding.c_str());
        // A Klein model is no SeedVR2 at all.
        QVERIFY(restoreModelError({"double_blocks.0.img_attn.qkv.weight", "txt_in.weight"}).find("not a SeedVR2") != std::string::npos);
        QVERIFY(!restoreModelError({}).empty());
    }

    void aVaeMustBeTheSeedvr2One() {
        QVERIFY(restoreVaeError({"encoder.conv_out.bias", "decoder.up_blocks.0.upsamplers.0.upscale_conv.weight"}).empty());
        QVERIFY(!restoreVaeError({"encoder.conv_in.weight", "decoder.conv_in.weight"}).empty()); // a Flux or SD VAE
    }

    void theProgramHasTheRequestsLiteralsAndTheTiling() {
        const std::string program = restoreScript("\"/a b/m.safetensors\"", "\"/v.safetensors\"", 256, 0.125, 42);
        for (const char *wanted : {"dit_path = \"/a b/m.safetensors\"", "vae_path = \"/v.safetensors\"", "tile = 256", "overlap = 0.125",
                                   "seed = 42", "target_overlap = overlap", "posterior = \"mode\"", "tiling = tiles", "seedvr2::positive_embedding"})
            QVERIFY2(program.find(wanted) != std::string::npos, wanted);
        // One euler step from sigma 1 to 0, as the workflow's single step.
        QVERIFY(program.find("tensor::vector([1.0, 0.0])") != std::string::npos);
        QVERIFY(program.find("method = \"euler\"") != std::string::npos);
    }

    void theBarNeverGoesBackAndEachStageHasItsSpan() {
        RestoreProgress bar;
        double last = 0;
        const auto step = [&](double expectedAtLeast, double expectedBelow) {
            QVERIFY(bar.value() >= last);
            QVERIFY2(bar.value() >= expectedAtLeast && bar.value() <= expectedBelow, std::to_string(bar.value()).c_str());
            last = bar.value();
        };
        bar.stage("loading", 0, 100);
        step(0, 0.001);
        bar.stage("loading", 50, 100);
        step(0.09, 0.11);
        bar.stage("loading", 100, 100);
        step(0.2, 0.2);
        bar.stage("vae_encode", 0, 0);
        bar.tiles(1, 4);
        step(0.25, 0.26);
        bar.tiles(4, 4);
        step(0.4, 0.4);
        bar.stage("loading", 7, 10); // the DiT loads here, after the encode: not the first span again
        bar.tiles(4, 4);
        step(0.4, 0.4);
        bar.stage("sampling", 0, 1);
        step(0.4, 0.4);
        bar.stage("sampling", 1, 1);
        step(0.55, 0.55);
        bar.stage("vae_decode", 0, 0);
        bar.stage("loading", 1, 2); // and the VAE again
        bar.tiles(2, 4);
        step(0.77, 0.78);
        bar.tiles(1, 4); // a late, smaller report does not move it back
        step(0.77, 0.78);
        bar.tiles(4, 4);
        step(1.0, 1.0);
    }

    void tilesOutsideATiledStageAreIgnored() {
        RestoreProgress bar;
        bar.stage("sampling", 0, 1);
        bar.tiles(3, 4); // the legacy callback also reports sampling steps: not tiles
        QVERIFY(bar.value() <= 0.41);
        bar.stage("something_new", 5, 10); // a stage the bar does not know leaves it alone
        QVERIFY(bar.value() <= 0.41);
    }
};

QTEST_GUILESS_MAIN(TestHelperRestore)
#include "test_helper_restore.moc"
