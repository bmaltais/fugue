// The rows a model picker shows (workflow/model_picker.h): the engine's name as the family of a file the table has none for, and
// the line under a file the engine does not recognise (#703).

#include <QtTest>

#include "workflow/model_picker.h"

using namespace fugue;

class TestModelPicker : public QObject {
    Q_OBJECT

    static ListedModel file(const QString &name) {
        ListedModel model;
        model.name = name;
        model.status = QStringLiteral("ready");
        model.path = QStringLiteral("/models/") + name;
        model.folder = QStringLiteral("/models");
        model.size = 10;
        return model;
    }

private slots:
    void theEnginesNameIsTheFamilyWhenTheTableHasNone() {
        const QString type = QStringLiteral("fugue/diffusion-model");
        const ListedModel mystery = file("mystery.safetensors");
        const ListedModel klein = file("flux-2-klein-9b.gguf");
        const ListedModel odd = file("odd.gguf");
        const QHash<QString, ModelMark> marks{{mystery.path, ModelMark{QStringLiteral("Qwen Image"), false, {}, false}},
                                              {klein.path, ModelMark{QStringLiteral("FLUX.2 Klein 9B"), false, {}, false}},
                                              {odd.path, ModelMark{{}, false, {}, true}}};
        QCOMPARE(modelRow(mystery, type, marks).value("family").toString(), QStringLiteral("Qwen Image"));
        QVERIFY(!modelRow(mystery, type, marks).contains("engineNote"));
        QCOMPARE(modelRow(klein, type, marks).value("family").toString(), QStringLiteral("FLUX.2 Klein 9B"));
        const QVariantMap unrecognised = modelRow(odd, type, marks);
        QCOMPARE(unrecognised.value("family").toString(), QString());
        QCOMPARE(unrecognised.value("engineNote").toString(), QStringLiteral("stable-diffusion.cpp does not recognise this model"));
        QVERIFY(!modelRow(mystery, type, {}).contains("engineNote")); // no helper, no marks: as before
    }
};

QTEST_MAIN(TestModelPicker)
#include "test_model_picker.moc"
