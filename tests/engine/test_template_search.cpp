// The templates window's search and the model families it filters by (#369, spec 11.10):
// word-start matching, source:/family:/tag: words, and the family read from a model's file name.
// Seam: templateMatchesSearch over template maps, modelFamily over handles. No window.

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "engine/model_family.h"
#include "engine/template_search.h"

using namespace fugue;

namespace {

QVariantMap templ(const QString &id, const QString &title, const QStringList &tags = {}, const QVariantMap &source = {},
                  const QStringList &families = {}) {
    return {{"id", id}, {"title", title}, {"description", "Update the picture"}, {"categoryTitle", "Editing"},
            {"tags", tags}, {"source", source}, {"families", families}};
}

} // namespace

class TestTemplateSearch : public QObject {
    Q_OBJECT

private slots:
    void aWordMatchesWhereAWordStartsNotInsideOne() {
        QVERIFY(matchesWordStart("DA workflow", "da"));
        QVERIFY(matchesWordStart("FLUX.2 Klein 9B", "9b"));
        QVERIFY(matchesWordStart("flux2-klein-9b-da-edit", "da"));
        QVERIFY(!matchesWordStart("Update the picture", "da"));
        QVERIFY(!matchesWordStart("candy", "dy"));
    }

    void everyWordMustMatchAndEmptyMatchesAll() {
        const QVariantMap t = templ("edit", "Edit pictures", {"Klein"});
        QVERIFY(templateMatchesSearch("", t));
        QVERIFY(templateMatchesSearch("  edit  KLEIN ", t));
        QVERIFY(!templateMatchesSearch("edit fox", t));
        // Only the description has "update": "da" no longer finds it.
        QVERIFY(templateMatchesSearch("update", t));
        QVERIFY(!templateMatchesSearch("da", t));
    }

    void searchReadsTheSourceTheIdAndTheFamilies() {
        const QVariantMap t = templ("flux2-klein-9b-da-edit", "Edit", {}, {{"name", "DA flux2 klein-9b distilled union v10"}, {"author", "DemonAlone"}},
                                    {"FLUX.2 Klein 9B"});
        QVERIFY(templateMatchesSearch("demonalone", t));
        QVERIFY(templateMatchesSearch("union", t));
        QVERIFY(templateMatchesSearch("da", t));
        QVERIFY(templateMatchesSearch("klein 9b", t));
        QVERIFY(!templateMatchesSearch("seedvr2", t));
    }

    void keyedWordsLimitTheSearchToOneField() {
        const QVariantMap t = templ("a", "Seedvr2 restore", {"Restore"}, {{"name", "DA workflow"}, {"author", "DemonAlone"}}, {"SeedVR2", "FLUX.2 Klein 9B"});
        QVERIFY(templateMatchesSearch("source:demon", t));
        QVERIFY(!templateMatchesSearch("source:restore", t)); // the tag, not the source
        QVERIFY(templateMatchesSearch("tag:restore", t));
        QVERIFY(!templateMatchesSearch("tag:demon", t));
        QVERIFY(templateMatchesSearch("family:klein", t));
        QVERIFY(templateMatchesSearch("family:\"klein 9b\" source:da", t));
        QVERIFY(!templateMatchesSearch("family:\"klein 4b\"", t));
        QVERIFY(!templateMatchesSearch("family:restore", t)); // the title and tag are not families
        QVERIFY(templateMatchesSearch("Source:DA", t));
        QVERIFY(templateMatchesSearch("source:", templ("b", "No source"))); // empty value: no word, so no limit
        QVERIFY(!templateMatchesSearch("path:x", t));                          // not a key: a plain word
    }

    void familiesComeFromTheModelsNotTheText() {
        QCOMPARE(modelFamily("fugue/diffusion-model", "flux-2-klein-9b-Q8_0.gguf"), QStringLiteral("FLUX.2 Klein 9B"));
        QCOMPARE(modelFamily("fugue/diffusion-model", "flux-2-klein-base-9b-Q8_0.gguf"), QStringLiteral("FLUX.2 Klein 9B"));
        QCOMPARE(modelFamily("diffusion-model", "flux-2-klein-4b-fp8.safetensors"), QStringLiteral("FLUX.2 Klein 4B"));
        QCOMPARE(modelFamily("fugue/diffusion-model", "z_image_turbo_int8_convrot.safetensors"), QStringLiteral("Z-Image"));
        QCOMPARE(modelFamily("fugue/diffusion-model", "qwen_image_2512_fp8.safetensors"), QStringLiteral("Qwen-Image"));
        QCOMPARE(modelFamily("fugue/diffusion-model", "Wan2.2-I2V-A14B-LowNoise-Q4_K_S.gguf"), QStringLiteral("Wan 2.2"));
        QCOMPARE(modelFamily("fugue/seedvr2-model", "seedvr2_7b_int8_convrot.safetensors"), QStringLiteral("SeedVR2"));
        QCOMPARE(modelFamily("fugue/upscale-model", "RealESRGAN_x4plus.safetensors"), QStringLiteral("Real-ESRGAN"));
        // A text encoder has a picker family (#370) but is never one of a template's (modelFamilies, below); a VAE
        // and an unknown diffusion model have none.
        QCOMPARE(modelFamily("fugue/text-encoder", "Qwen3-8B-Q8_0.gguf"), QStringLiteral("Qwen3 8B"));
        QVERIFY(modelFamily("fugue/vae", "z-image-vae.safetensors").isEmpty());
        QVERIFY(modelFamily("fugue/diffusion-model", "mystery.gguf").isEmpty());
    }

    void aTemplatesFamiliesAreEachOnceInOrder() {
        const auto model = [](const QString &resource, const QString &name) {
            return QJsonObject{{"resource", resource}, {"name", name}};
        };
        const QJsonArray models{model("fugue/text-encoder", "Qwen3-8B-Q8_0.gguf"),
                                model("fugue/diffusion-model", "flux-2-klein-9b-Q8_0.gguf"),
                                model("fugue/diffusion-model", "flux-2-klein-9b-Q4.gguf"),
                                model("fugue/seedvr2-model", "seedvr2.safetensors")};
        QCOMPARE(modelFamilies(models), (QStringList{"FLUX.2 Klein 9B", "SeedVR2"}));
        QVERIFY(knownModelFamilies().contains("Z-Image"));
    }
};

QTEST_GUILESS_MAIN(TestTemplateSearch)
#include "test_template_search.moc"
