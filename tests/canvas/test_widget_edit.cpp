// Editing input values (spec 6.2, 3.2, 2.5) through the canvas model's widget editor
// (canvas/widget_edit.h): what each widget is told about its input, typed text turned into
// values and checked by the engine's rules, one undo step per commit, typing sessions,
// seeds, and the model picker's list. The window tests drive the same calls with keys.

#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/model_use.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "widget_fixture.h"
#include "workflow/model_downloads.h"
#include "../engine/fake_http.h"
#include "../engine/omarchy_fixture.h"

using namespace fugue;

namespace {

Document document(const char *json) {
    const Result<Document> loaded = Document::load(QByteArray(json));
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// One node of every widget, a Text linked into a Text Concat, and a placeholder.
const char *fixture = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "all": {"type": "io.example.widgets/all", "version": "1.0.0", "inputs": {"count": 7}},
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "join": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}},
    "gone": {"type": "io.example.gone/thing", "version": "1.0.0", "inputs": {"level": 3}}
  }},
  "layout": {"nodes": {"all": {"pos": [0, 0]}, "prompt": {"pos": [400, 0], "title": "Prompt"},
                       "join": {"pos": [700, 0]}, "gone": {"pos": [0, 600]}}}
})";

bool writeModel(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

struct Fixture {
    Registry registry;
    CanvasModel canvas{&registry};
    WidgetEdit &edits;
    Fixture() : edits(*canvas.widgets()) {
        nodes::addBuiltinNodeTypes(registry);
        widget_test::addWidgetType(registry);
        canvas.setDocument(document(fixture));
    }
    QJsonValue stored(const QString &node, const QString &input) const {
        return canvas.document().graph.nodes.value(node).inputs.value(input).toJson();
    }
    int steps() const { return canvas.undoStack()->count(); }
};

} // namespace

class TestWidgetEdit : public QObject {
    Q_OBJECT

private slots:
    void eachInputNamesItsWidgetAndConstraints() {
        Fixture f;
        const QVariantMap count = f.edits.spec("all", "count");
        QCOMPARE(count.value("widget").toString(), QString("spin"));
        QCOMPARE(count.value("min").toDouble(), 0.0);
        QCOMPARE(count.value("max").toDouble(), 100.0);
        QCOMPARE(count.value("integer").toBool(), true);
        QCOMPARE(count.value("default").toDouble(), 4.0);
        const QHash<QString, QString> widgets{{"amount", "slider"}, {"name", "line"},     {"notes", "multiline"},
                                              {"flag", "toggle"},   {"mode", "choice"},   {"file", "file"},
                                              {"folder", "folder"}, {"tint", "color"},    {"seed", "seed"},
                                              {"model", "model"}};
        for (auto it = widgets.begin(); it != widgets.end(); ++it)
            QCOMPARE(f.edits.spec("all", it.key()).value("widget").toString(), it.value());
        QCOMPARE(f.edits.spec("all", "mode").value("choices").toStringList(), (QStringList{"a", "b", "c"}));
        // Wire-only inputs and a placeholder's inputs have no widget.
        QVERIFY(f.edits.spec("all", "image").isEmpty());
        QVERIFY(f.edits.spec("gone", "level").isEmpty());
        QVERIFY(f.edits.spec("nobody", "count").isEmpty());
    }

    void typedNumbersAreCheckedByTheEngineAndOneStepEach() {
        Fixture f;
        QCOMPARE(f.edits.setText("all", "count", "12"), QString());
        QCOMPARE(f.stored("all", "count"), QJsonValue(12));
        QCOMPARE(f.steps(), 1);
        // Out of range, not whole, not a number: nothing is written and the reason is given.
        QVERIFY(f.edits.setText("all", "count", "101").contains("maximum"));
        QVERIFY(!f.edits.setText("all", "count", "2.5").isEmpty());
        QVERIFY(!f.edits.setText("all", "count", "lots").isEmpty());
        QCOMPARE(f.stored("all", "count"), QJsonValue(12));
        QCOMPARE(f.steps(), 1);
        // Typed numbers follow one rule (typed_number.h): a point always; a comma only where the language writes the
        // decimal point as one; never a thousands separator. Spaces around do not matter.
        const QLocale before;
        const auto restore = qScopeGuard([&] { QLocale::setDefault(before); });
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QVERIFY(f.edits.setText("all", "amount", " 0,25 ").contains("separator")); // never 25, never 0.25 in English
        QCOMPARE(f.edits.setText("all", "amount", " 0.25 "), QString());
        QCOMPARE(f.stored("all", "amount"), QJsonValue(0.25));
        for (const char *grouped : {"1,000", "12,500", "1.000,5", "1,000.5"}) {
            const QString why = f.edits.setText("all", "count", grouped);
            QVERIFY2(why.contains("separator"), qPrintable(QString::fromLatin1(grouped) + ": " + why));
        }
        QCOMPARE(f.stored("all", "count"), QJsonValue(12));
        QLocale::setDefault(QLocale(QLocale::Danish, QLocale::Denmark));
        QCOMPARE(f.edits.check("all", "amount", "0,5"), QString());
        QVERIFY(f.edits.setText("all", "count", "1.000,5").contains("separator"));
        // The same value again is no step.
        QCOMPARE(f.edits.setText("all", "amount", "0,25"), QString());
        QCOMPARE(f.steps(), 2);
        f.canvas.undo();
        QCOMPARE(f.stored("all", "amount"), QJsonValue(QJsonValue::Undefined));
    }

    void emptyTextUnsetsWhatCanBeUnset() {
        Fixture f;
        QCOMPARE(f.edits.setText("all", "width", "512"), QString());
        QCOMPARE(f.edits.setText("all", "width", ""), QString());
        QCOMPARE(f.stored("all", "width"), QJsonValue(QJsonValue::Undefined));
        // A number with a default goes back to it.
        QCOMPARE(f.edits.setText("all", "count", ""), QString());
        QVERIFY(!f.canvas.document().graph.nodes.value("all").inputs.contains("count"));
        // Text is text, empty included; an optional path with no default is unset.
        QCOMPARE(f.edits.setText("all", "name", ""), QString());
        QCOMPARE(f.stored("all", "name"), QJsonValue(""));
        QCOMPARE(f.edits.setText("all", "file", "/tmp/a.png"), QString());
        QCOMPARE(f.edits.setText("all", "file", ""), QString());
        QCOMPARE(f.stored("all", "file"), QJsonValue(QJsonValue::Undefined));
    }

    void valuesOfEachKind() {
        Fixture f;
        QCOMPARE(f.edits.setValue("all", "flag", true), QString());
        QCOMPARE(f.stored("all", "flag"), QJsonValue(true));
        QVERIFY(!f.edits.setValue("all", "flag", QStringLiteral("yes")).isEmpty());
        QCOMPARE(f.edits.setValue("all", "mode", QStringLiteral("c")), QString());
        QCOMPARE(f.stored("all", "mode"), QJsonValue("c"));
        QVERIFY(f.edits.setValue("all", "mode", QStringLiteral("d")).contains("not a valid"));
        QCOMPARE(f.edits.setText("all", "tint", "#00ff0080"), QString());
        QVERIFY(!f.edits.setText("all", "tint", "green").isEmpty());
        QCOMPARE(f.stored("all", "tint"), QJsonValue("#00ff0080"));
        QCOMPARE(f.edits.setColor("all", "tint", QColor(255, 0, 0)), QString());
        QCOMPARE(f.stored("all", "tint"), QJsonValue("#ff0000"));
        QCOMPARE(f.edits.setColor("all", "tint", QColor(255, 0, 0, 128)), QString());
        QCOMPARE(f.stored("all", "tint"), QJsonValue("#ff000080"));
        QCOMPARE(f.edits.setPath("all", "file", QUrl::fromLocalFile("/tmp/fox.png")), QString());
        QCOMPARE(f.stored("all", "file"), QJsonValue("/tmp/fox.png"));
        QCOMPARE(f.edits.setPath("all", "folder", QStringLiteral("/tmp/out")), QString());
        QCOMPARE(f.stored("all", "folder"), QJsonValue("/tmp/out"));
        QCOMPARE(f.steps(), 7);
        // A placeholder's input and a missing node are never written.
        QVERIFY(!f.edits.setText("gone", "level", "4").isEmpty());
        QVERIFY(!f.edits.setText("nobody", "count", "4").isEmpty());
        QCOMPARE(f.steps(), 7);
    }

    void aTypingSessionIsOneStep() {
        Fixture f;
        f.edits.begin("prompt", "text");
        QCOMPARE(f.edits.preview("a red"), QString());
        // Each valid value is in the document as it is typed.
        QCOMPARE(f.stored("prompt", "text"), QJsonValue("a red"));
        QCOMPARE(f.edits.preview("a red cat"), QString());
        f.edits.finish();
        QCOMPARE(f.steps(), 1);
        QCOMPARE(f.stored("prompt", "text"), QJsonValue("a red cat"));
        f.canvas.undo();
        QCOMPARE(f.stored("prompt", "text"), QJsonValue("a red fox"));

        // An invalid value is not written; the last valid one stays.
        f.edits.begin("all", "count");
        QCOMPARE(f.edits.preview("9"), QString());
        QVERIFY(!f.edits.preview("9x").isEmpty());
        QCOMPARE(f.stored("all", "count"), QJsonValue(9));
        f.edits.finish();
        QCOMPARE(f.canvas.undoStack()->index(), 1);

        // Cancel puts the value back and leaves no step.
        f.edits.begin("all", "count");
        QCOMPARE(f.edits.preview("50"), QString());
        f.edits.cancel();
        QCOMPARE(f.stored("all", "count"), QJsonValue(9));
        QCOMPARE(f.canvas.undoStack()->index(), 1);
        QVERIFY(!f.edits.editing());
    }

    void aSessionEndsWhenTheDocumentIsReplaced() {
        Fixture f;
        QSignalSpy dropped(&f.edits, &WidgetEdit::sessionEnded);
        f.edits.begin("prompt", "text");
        QCOMPARE(f.edits.preview("snow"), QString());
        f.canvas.setDocument(document(fixture)); // another tab, or a file opened
        QCOMPARE(dropped.count(), 1);
        QVERIFY(!f.edits.editing());
        // A late preview or finish writes nothing.
        QVERIFY(!f.edits.preview("rain").isEmpty());
        f.edits.finish();
        QCOMPARE(f.stored("prompt", "text"), QJsonValue("a red fox"));
        QCOMPARE(f.steps(), 0);
    }

    void anotherEditEndsTheSessionAsItsOwnStep() {
        Fixture f;
        QSignalSpy ended(&f.edits, &WidgetEdit::sessionEnded);
        f.edits.begin("prompt", "text");
        QCOMPARE(f.edits.preview("snow"), QString());
        // A toggle, a command, a seed advance: not merged into the typing.
        QCOMPARE(f.edits.setValue("all", "flag", true), QString());
        QCOMPARE(ended.count(), 1);
        QVERIFY(!f.edits.editing());
        QCOMPARE(f.steps(), 2);
        f.canvas.undo();
        QCOMPARE(f.stored("all", "flag"), QJsonValue(QJsonValue::Undefined));
        QCOMPARE(f.stored("prompt", "text"), QJsonValue("snow"));
        // The field's late finish does nothing.
        f.edits.finish();
        QCOMPARE(f.steps(), 2);

        f.edits.begin("all", "count");
        QCOMPARE(f.edits.preview("9"), QString());
        f.canvas.select({"all"});
        f.canvas.toggleCollapsed();
        QCOMPARE(f.canvas.undoStack()->index(), 3);
        f.canvas.undo();
        QCOMPARE(f.stored("all", "count"), QJsonValue(9));
    }

    void aSessionOnANodeThatGoesAwayWritesNothing() {
        Fixture f;
        f.edits.begin("prompt", "text");
        QCOMPARE(f.edits.preview("snow"), QString());
        f.canvas.undo(); // finishes the session's step and undoes it
        f.canvas.select({"prompt"});
        f.canvas.deleteSelection();
        QVERIFY(!f.edits.preview("rain").isEmpty());
        f.edits.finish();
        QVERIFY(!f.canvas.document().graph.nodes.contains("prompt"));
    }

    void stepsAreHeldInRangeAndRounded() {
        Fixture f;
        QCOMPARE(f.edits.stepped("all", "count", 7, 1), 8.0);
        QCOMPARE(f.edits.stepped("all", "count", 99, 5), 100.0);
        QCOMPARE(f.edits.stepped("all", "count", 1, -5), 0.0);
        QCOMPARE(f.edits.stepped("all", "amount", 0.1, 4), 0.3); // not 0.30000000000000004
        QCOMPARE(f.edits.stepped("all", "width", 512, 1), 576.0);
        QCOMPARE(f.edits.valueAt("all", "amount", 0.62), 0.6);
        QCOMPARE(f.edits.valueAt("all", "amount", 1.4), 1.0);
        QCOMPARE(f.edits.fraction("all", "amount", 0.25), 0.25);
        QCOMPARE(WidgetEdit::numberText(12), QString("12"));
        QCOMPARE(WidgetEdit::numberText(0.25), QString("0.25"));
    }

    // Spec Appendix A (#19): a node added on the canvas starts its seeds on randomize, as
    // ComfyUI's do; the manifest's default (and so the CLI) stays a fixed number.
    void aNodeAddedOnTheCanvasStartsItsSeedOnRandomize() {
        Fixture f;
        const QString id = f.canvas.addNode("io.example.widgets/all", 0, 400);
        QCOMPARE(f.edits.seedMode(id, "seed"), QString("random"));
        QCOMPARE(f.stored(id, "seed"), QJsonValue(QJsonObject{{"auto", "random"}}));
    }

    // Leaving randomize keeps the seed of the result the node shows, so the image the user
    // liked can be made again; with no result one is drawn.
    void leavingRandomizeKeepsTheSeedOfTheLastResult() {
        Fixture f;
        f.edits.setSeedMode("all", "seed", "random");
        f.canvas.setRunState("all", {{"state", "cached"}, {"values", QVariantMap{{"seed", "1234"}}}});
        f.edits.setSeedMode("all", "seed", "fixed");
        QCOMPARE(f.stored("all", "seed"), QJsonValue(1234));
    }

    void seedsAreFixedRandomOrIncrement() {
        Fixture f;
        QCOMPARE(f.edits.seedMode("all", "seed"), QString("fixed"));
        f.edits.setSeedMode("all", "seed", "random");
        QCOMPARE(f.stored("all", "seed"), QJsonValue(QJsonObject{{"auto", "random"}}));
        QCOMPARE(f.edits.seedMode("all", "seed"), QString("random"));
        // Back to a number: one is drawn, within the input's range.
        f.edits.setSeedMode("all", "seed", "increment");
        QVERIFY(f.stored("all", "seed").isDouble());
        QCOMPARE(f.edits.seedMode("all", "seed"), QString("increment"));
        const QJsonObject entry = f.canvas.document().layout.value("nodes").toObject().value("all").toObject();
        QCOMPARE(entry.value("increment").toArray(), QJsonArray{"seed"});
        QCOMPARE(f.steps(), 2);

        QCOMPARE(f.edits.setText("all", "seed", "41"), QString());
        f.edits.advanceSeeds();
        QCOMPARE(f.stored("all", "seed"), QJsonValue(42));
        QCOMPARE(f.steps(), 4);
        // Past the maximum it starts again at the minimum.
        QCOMPARE(f.edits.setText("all", "seed", "9007199254740991"), QString());
        f.edits.advanceSeeds();
        QCOMPARE(f.stored("all", "seed"), QJsonValue(0));

        // Fixed keeps the number and drops the increment.
        f.edits.setSeedMode("all", "seed", "fixed");
        QCOMPARE(f.stored("all", "seed"), QJsonValue(0));
        QVERIFY(!f.canvas.document().layout.value("nodes").toObject().value("all").toObject().contains("increment"));
        // The file keeps the choice.
        f.edits.setSeedMode("all", "seed", "increment");
        const Result<Document> reloaded = Document::load(f.canvas.document().save());
        QVERIFY(reloaded.ok());
        f.canvas.setDocument(reloaded.value());
        QCOMPARE(f.edits.seedMode("all", "seed"), QString("increment"));
        // Only a seed input has modes.
        f.edits.setSeedMode("all", "count", "random");
        QCOMPARE(f.stored("all", "count"), QJsonValue(7));
    }

    void theModelPickerListsModelsOfTheInputsType() {
        Fixture f;
        QString askedFor;
        std::function<void(QList<WidgetEdit::Model>)> answer;
        f.edits.setModelSource([&](const QString &type, const QStringList &, std::function<void(QList<WidgetEdit::Model>)> done) {
            askedFor = type;
            answer = std::move(done);
        });
        QSignalSpy changed(&f.edits, &WidgetEdit::modelsChanged);
        f.edits.requestModels("all", "model");
        QCOMPARE(askedFor, QString("fugue/image-model"));
        QVERIFY(f.edits.modelsLoading("all", "model"));
        const QJsonObject zimage{{"resource", "fugue/image-model"}, {"name", "z-image-turbo"}};
        answer({widget_test::served("z-image-turbo", zimage, "ready", "This machine")});
        QVERIFY(!f.edits.modelsLoading("all", "model"));
        QVERIFY(changed.count() >= 1);
        const QVariantList listed = f.edits.models("all", "model");
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().toMap().value("name").toString(), QString("z-image-turbo"));

        QCOMPARE(f.edits.setModel("all", "model", listed.first().toMap().value("handle").toMap()), QString());
        QCOMPARE(f.stored("all", "model"), QJsonValue(zimage));
        // A handle of another resource type is refused by the engine's check.
        QVERIFY(!f.edits.setModel("all", "model", QVariantMap{{"resource", "fugue/lora"}, {"name", "x"}}).isEmpty());
        // A typed name is a model served by that name.
        QCOMPARE(f.edits.setModelName("all", "model", " flux2-klein "), QString());
        QCOMPARE(f.stored("all", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux2-klein"}}));
        QCOMPARE(f.edits.value("all", "model").toMap().value("name").toString(), QString("flux2-klein"));
        // Unset: any ready model (8.4).
        f.edits.unset("all", "model");
        QCOMPARE(f.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
    }

    void thePickerListsModelFilesOnDiskByFolderBesideTheProvidersModels() {
        QTemporaryDir fugueModels, comfy;
        QVERIFY(writeModel(comfy.filePath("image-model/b.gguf"), "b"));
        QVERIFY(writeModel(fugueModels.filePath("image-model/sub/a.safetensors"), "a"));
        Fixture f;
        f.edits.setModelRoots({fugueModels.path(), comfy.path()});
        const QJsonObject served{{"resource", "fugue/image-model"}, {"name", "z-image-turbo"}};
        f.edits.setModelSource([&](const QString &, const QStringList &, std::function<void(QList<WidgetEdit::Model>)> done) {
            done({widget_test::served("z-image-turbo", served, "ready", "This machine")});
        });
        f.edits.requestModels("all", "model");
        const QVariantList listed = f.edits.models("all", "model");
        QCOMPARE(listed.size(), 3);
        // What providers offer first, then each folder's files in the folders' order.
        QCOMPARE(listed.at(0).toMap().value("provider").toString(), QString("This machine"));
        const QVariantMap a = listed.at(1).toMap();
        QCOMPARE(a.value("name").toString(), QString("sub/a.safetensors"));
        QCOMPARE(a.value("folder").toString(), fugueModels.filePath("image-model"));
        QCOMPARE(a.value("path").toString(), fugueModels.filePath("image-model/sub/a.safetensors"));
        QCOMPARE(a.value("size").toLongLong(), 1);
        QCOMPARE(a.value("provider").toString(), QString());
        // Not hashed yet: listing reads no model file, so the handle has no hash.
        QVERIFY(!a.value("handle").toMap().contains("hash"));
        QCOMPARE(listed.at(2).toMap().value("folder").toString(), comfy.filePath("image-model"));
    }

    void pickingAFileOnDiskHashesItInTheBackgroundOnceThenStoresItsHandle() {
        QTemporaryDir models;
        const QString path = models.filePath("image-model/flux/model.safetensors");
        QVERIFY(writeModel(path, QByteArray(3 * 1024 * 1024, 'w')));
        FileHashes running(FileHashes::defaultPath());
        Fixture f;
        f.edits.setModelRoots({models.path()});
        const int before = f.steps();
        QSignalSpy hashing(&f.edits, &WidgetEdit::hashingChanged);
        QCOMPARE(f.edits.setDiskModel("all", "model", path), QString());
        // Nothing is stored until the hash is known; meanwhile the input says it is reading.
        QCOMPARE(f.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
        QVERIFY(f.edits.hashState("all", "model").contains("progress"));
        QTRY_VERIFY(f.edits.hashState("all", "model").isEmpty());
        QVERIFY(hashing.count() >= 2);
        const QString hash = sha256(QByteArray(3 * 1024 * 1024, 'w'));
        QCOMPARE(f.stored("all", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux/model.safetensors"}, {"hash", hash}}));
        QCOMPARE(f.steps(), before + 1);
        // Remembered for every Fugue process, and listed with it. A run's store, open since
        // before the pick (the window's run thread), finds the file by it: the node gets its path.
        FileHashes stored(FileHashes::defaultPath());
        QCOMPARE(stored.known(path), hash);
        QCOMPARE(running.known(path), hash);
        const Result<QJsonValue> given = ModelLibrary({models.path()}, &f.registry)
                                             .withPaths(f.stored("all", "model"), running, ModelLibrary::Hashing::RememberedOnly);
        QVERIFY(given.ok());
        QCOMPARE(given.value().toObject().value("path").toString(), path);
        f.edits.requestModels("all", "model");
        QCOMPARE(f.edits.models("all", "model").last().toMap().value("handle").toMap().value("hash").toString(), hash);

        // A file whose hash is remembered is set at once.
        f.edits.unset("all", "model");
        QCOMPARE(f.edits.setDiskModel("all", "model", path), QString());
        QVERIFY(f.edits.hashState("all", "model").isEmpty());
        QCOMPARE(f.stored("all", "model").toObject().value("hash").toString(), hash);
    }

    // #605: a node whose model is not on disk, whose same weights are in another file, offers it; Use it is the pick of that file,
    // one undo step, and Undo gives the node its own model back.
    void aMissingModelOffersTheFileOfTheSameWeightsAndUseItIsOneUndoStep() {
        QTemporaryDir models;
        Fixture f;
        const QString type = QStringLiteral("fugue/diffusion-model");
        const QString folder = ModelLibrary({models.path()}, &f.registry).folders(*PortType::parse(type)).value(0);
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        file += header;
        const QString path = folder + "/z_image_turbo_bf16.safetensors";
        QVERIFY(writeModel(path, file));
        const QJsonObject wanted{{"resource", type}, {"name", "z_image_turbo_int8_convrot.safetensors"},
                                 {"hash", "sha256:" + QString(64, QLatin1Char('9'))}};
        f.canvas.setDocument(document(QByteArray(R"({"fugue": 1, "graph": {"nodes": {"models": {"type": "fugue/load-models", "version": "1.0.0",
            "inputs": {"diffusion_model": )" + QJsonDocument(wanted).toJson(QJsonDocument::Compact) + "}}}}}").constData()));
        f.edits.setModelRoots({models.path()});

        const QVariantList offers = f.edits.onDiskOffers("models");
        QCOMPARE(offers.size(), 1);
        QCOMPARE(offers.first().toMap().value("input").toString(), QString("diffusion_model"));
        QCOMPARE(offers.first().toMap().value("file").toString(), QString("z_image_turbo_bf16.safetensors"));
        QCOMPARE(offers.first().toMap().value("same").toString(), QString("same model, bf16"));

        const int before = f.steps();
        QCOMPARE(f.edits.setDiskModel("models", "diffusion_model", offers.first().toMap().value("path").toString()), QString());
        QTRY_COMPARE(f.stored("models", "diffusion_model").toObject().value("name").toString(), QString("z_image_turbo_bf16.safetensors"));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.edits.onDiskOffers("models").isEmpty()); // the file is here now: nothing to offer
        f.canvas.undoStack()->undo();
        QCOMPARE(f.stored("models", "diffusion_model").toObject(), wanted);
    }

    void anotherValueSetWhileAFileIsHashedWins() {
        QTemporaryDir models;
        const QString path = models.filePath("image-model/big.gguf");
        QVERIFY(writeModel(path, QByteArray(4 * 1024 * 1024, 'q')));
        Fixture f;
        f.edits.setModelRoots({models.path()});
        QCOMPARE(f.edits.setDiskModel("all", "model", path), QString());
        QCOMPARE(f.edits.setModelName("all", "model", "flux2-klein"), QString());
        QVERIFY(f.edits.hashState("all", "model").isEmpty());
        // The hash still arrives and is remembered, but does not replace the typed name.
        QTRY_COMPARE(FileHashes(FileHashes::defaultPath()).known(path), sha256(QByteArray(4 * 1024 * 1024, 'q')));
        QTest::qWait(50);
        QCOMPARE(f.stored("all", "model").toObject().value("name").toString(), QString("flux2-klein"));
    }

    void aPickWaitsForItsLevelAndIsDroppedWhenTheDocumentIsReplaced() {
        QTemporaryDir models;
        const QString path = models.filePath("image-model/large.gguf");
        const QByteArray bytes(6 * 1024 * 1024, 'g');
        QVERIFY(writeModel(path, bytes));
        Fixture f;
        f.edits.setModelRoots({models.path()});
        f.canvas.select({"prompt", "join"});
        const QString group = f.canvas.groups()->makeGroup();
        QVERIFY2(!group.isEmpty(), qPrintable(f.canvas.groups()->problem()));
        // Picked on the top level, then into a group while it is read: the group's nodes never
        // show it, and the handle is stored once the top level is back.
        QCOMPARE(f.edits.setDiskModel("all", "model", path), QString());
        QVERIFY(f.canvas.groups()->enterGroup(group));
        QVERIFY(f.edits.hashState("all", "model").isEmpty());
        QTRY_COMPARE(FileHashes(FileHashes::defaultPath()).known(path), sha256(bytes));
        QTest::qWait(50);
        QVERIFY(f.canvas.groups()->leaveGroup());
        QCOMPARE(f.stored("all", "model").toObject().value("hash").toString(), sha256(bytes));

        // Another document (a tab, a file opened) drops a pick still being read.
        const QString other = models.filePath("image-model/other.gguf");
        QVERIFY(writeModel(other, bytes + "o"));
        f.edits.unset("all", "model");
        QCOMPARE(f.edits.setDiskModel("all", "model", other), QString());
        f.canvas.setDocument(document(fixture));
        QVERIFY(f.edits.hashState("all", "model").isEmpty());
        QTRY_VERIFY(!FileHashes(FileHashes::defaultPath()).known(other).isEmpty());
        QTest::qWait(50);
        QCOMPARE(f.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
    }

    // ---- models to download from the catalog (#134) ----

    static ModelCatalog catalogOf(const QString &url, const QByteArray &bytes, bool commercial = true) {
        const QJsonObject entry{{"id", "cat"}, {"resource", "fugue/image-model"}, {"title", "Catalog model"},
                                {"description", "Good for tests."}, {"file", "cat.safetensors"}, {"url", url},
                                {"size", bytes.size()}, {"sha256", sha256(bytes).mid(7)}, {"scale", 4},
                                {"architecture", "ESRGAN"}, {"license", "MIT"}, {"commercial", commercial}};
        return ModelCatalog::parse(QJsonDocument(QJsonObject{{"catalog", 1}, {"models", QJsonArray{entry}}}).toJson(), "test");
    }

    void thePickerListsCatalogModelsNotOnDiskAndNamesWhatFiltersMatch() {
        QTemporaryDir models;
        Fixture f;
        f.edits.setModelRoots({models.path()});
        f.edits.setModelSource([](const QString &, const QStringList &, std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
        f.edits.setCatalog(catalogOf("https://example.org/cat.safetensors", "abc"));
        f.edits.requestModels("all", "model");
        QVariantList listed = f.edits.models("all", "model");
        QCOMPARE(listed.size(), 1);
        const QVariantMap entry = listed.first().toMap();
        QCOMPARE(entry.value("catalog").toBool(), true);
        QCOMPARE(entry.value("id").toString(), QString("cat"));
        QCOMPARE(entry.value("detail").toString(), QString("4x · ESRGAN · MIT · 1 MB"));
        QVERIFY(entry.value("keywords").toString().contains("esrgan", Qt::CaseInsensitive));
        QCOMPARE(entry.value("note").toString(), QString("Good for tests."));
        QCOMPARE(entry.value("size").toLongLong(), 3);
        // The question's facts: license, source host, size, and the folder it will be saved in.
        const QVariantMap info = f.edits.catalogInfo("all", "model", "cat");
        QCOMPARE(info.value("licenseLine").toString(), QString("MIT"));
        QCOMPARE(info.value("host").toString(), QString("example.org"));
        QCOMPARE(info.value("folder").toString(), models.filePath("image-model"));
        QVERIFY(f.edits.catalogInfo("all", "model", "nope").isEmpty());
        QVERIFY(f.edits.catalogInfo("all", "count", "cat").isEmpty());
        // Once a file of that name is on disk it is listed as a file, not offered again.
        QVERIFY(writeModel(models.filePath("image-model/cat.safetensors"), "abc"));
        f.edits.requestModels("all", "model");
        listed = f.edits.models("all", "model");
        QCOMPARE(listed.size(), 1);
        QVERIFY(!listed.first().toMap().value("catalog").toBool());
    }

    void downloadingACatalogModelFetchesItChecksItAndPutsItsHandleInTheInput() {
        QTemporaryDir models;
        const QByteArray bytes(200000, 'c');
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/cat.safetensors", [&](const fugue::test::HttpRequest &) {
            fugue::test::HttpResponse response;
            response.body = bytes;
            return response;
        });
        Fixture f;
        ModelDownloads downloads;
        f.edits.setDownloads(&downloads);
        f.edits.setModelRoots({models.path()});
        f.edits.setCatalog(catalogOf(QString::fromLatin1(server.origin()) + "/cat.safetensors", bytes));
        f.edits.requestModels("all", "model");
        const int before = f.steps();
        QCOMPARE(f.edits.downloadCatalogModel("all", "model", "cat"), QString());
        QVERIFY(downloads.busy());
        QCOMPARE(downloads.what(), QString("pick"));
        QTRY_VERIFY(!downloads.busy());
        QCOMPARE(f.stored("all", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "cat.safetensors"}, {"hash", sha256(bytes)}}));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(QFileInfo::exists(models.filePath("image-model/cat.safetensors")));
    }

    void aCatalogDownloadWhoseBytesDoNotMatchItsHashPutsNothingInTheInput() {
        QTemporaryDir models;
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/cat.safetensors", [](const fugue::test::HttpRequest &) {
            fugue::test::HttpResponse response;
            response.body = "something else entirely";
            return response;
        });
        Fixture f;
        ModelDownloads downloads;
        f.edits.setDownloads(&downloads);
        f.edits.setModelRoots({models.path()});
        f.edits.setCatalog(catalogOf(QString::fromLatin1(server.origin()) + "/cat.safetensors", "the bytes the catalog hashed"));
        QCOMPARE(f.edits.downloadCatalogModel("all", "model", "cat"), QString());
        QTRY_VERIFY(!downloads.busy());
        QVERIFY(!downloads.failure().isEmpty());
        QCOMPARE(f.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
        QVERIFY(!QFileInfo::exists(models.filePath("image-model/cat.safetensors")));
    }

    void aCatalogDownloadIsRefusedForAnotherTypeALinkOrNoDownloadLine() {
        QTemporaryDir models;
        Fixture f;
        f.edits.setModelRoots({models.path()});
        f.edits.setCatalog(catalogOf("https://example.org/cat.safetensors", "abc"));
        QVERIFY(!f.edits.downloadCatalogModel("all", "model", "cat").isEmpty()); // no download line
        ModelDownloads downloads;
        f.edits.setDownloads(&downloads);
        QVERIFY(!f.edits.downloadCatalogModel("all", "model", "nope").isEmpty());
        QVERIFY(!f.edits.downloadCatalogModel("all", "count", "cat").isEmpty());
        QVERIFY(!downloads.busy());
    }

    void aFileThatIsNotAModelOfTheInputsTypeIsRefused() {
        QTemporaryDir models;
        QVERIFY(writeModel(models.filePath("elsewhere/x.gguf"), "x"));
        Fixture f;
        f.edits.setModelRoots({models.path()});
        QVERIFY(!f.edits.setDiskModel("all", "model", models.filePath("elsewhere/x.gguf")).isEmpty());
        QVERIFY(!f.edits.setDiskModel("all", "count", models.filePath("elsewhere/x.gguf")).isEmpty());
        QCOMPARE(f.stored("all", "model"), QJsonValue(QJsonValue::Undefined));
    }

    void withoutProvidersJsonThePickerListsThePairedHostsModelsForTheNodesCapability() {
        // The owner's first run (spec 8.2): no providers.json, this machine sets up only text
        // models, and a paired host runs two image models. The picker asks the default
        // instances, and lists by instance what offers the node's capability.
        test::PairedHome home;
        Fixture f;
        f.canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "generate": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a fox"}},
            "edit": {"type": "fugue/edit-image", "version": "1.0.0", "inputs": {"instruction": "make it blue"}}}}})"));
        f.edits.requestModels("generate", "model");
        QVERIFY(f.edits.modelsLoading("generate", "model"));
        QTRY_VERIFY(!f.edits.modelsLoading("generate", "model"));
        const QVariantList listed = f.edits.models("generate", "model");
        QStringList seen;
        for (const QVariant &each : listed) {
            const QVariantMap m = each.toMap();
            seen << m.value("provider").toString() + ": " + m.value("title").toString() + " (" + m.value("status").toString() + ")";
        }
        // This machine offers no image model; lab is offline; neighbour is not paired.
        QCOMPARE(seen, (QStringList{"lab: Z-Image Turbo (unavailable)", "studio: FLUX.2 Klein 4B (ready)",
                                    "studio: Z-Image Turbo (ready)"}));
        const QVariantMap klein = listed.at(1).toMap();
        QCOMPARE(klein.value("name").toString(), QString("flux2-klein-4b-sdcpp-cuda"));
        QCOMPARE(klein.value("note").toString(), QString());
        QVERIFY(listed.at(0).toMap().value("note").toString().contains("lab is offline"));
        // Picking stores the handle, never the title.
        QCOMPARE(f.edits.setModel("generate", "model", klein.value("handle").toMap()), QString());
        QCOMPARE(f.stored("generate", "model"),
                 QJsonValue(QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux2-klein-4b-sdcpp-cuda"}}));

        // Edit Image asks for image.edit: only the model that edits.
        f.edits.requestModels("edit", "model");
        QTRY_VERIFY(!f.edits.modelsLoading("edit", "model"));
        const QVariantList edits = f.edits.models("edit", "model");
        QCOMPARE(edits.size(), 1);
        QCOMPARE(edits.first().toMap().value("title").toString(), QString("FLUX.2 Klein 4B"));
        QCOMPARE(edits.first().toMap().value("provider").toString(), QString("studio"));
    }

    void aProvidersFileWinsOverThePairedHosts() {
        test::PairedHome home;
        QVERIFY(test::writeFixtureFile(home.dir.filePath("config/fugue/providers.json"),
                                       R"({"providers": [{"name": "This machine", "type": "omarchy"}]})"));
        Fixture f;
        f.canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "generate": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a fox"}}}}})"));
        f.edits.requestModels("generate", "model");
        QTRY_VERIFY(!f.edits.modelsLoading("generate", "model"));
        QVERIFY(f.edits.models("generate", "model").isEmpty());
        QCOMPARE(f.edits.modelsEmptyText("generate", "model"),
                 QString("No image model available: start one in omarchy-local-ai or add a provider"));
    }

    // A text node lists the text models (text.generate), here the paired host's, and none of
    // the image models.
    void aTextNodesPickerListsTheTextModels() {
        test::PairedHome home;
        Fixture f;
        f.canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "enhance": {"type": "fugue/enhance-prompt", "version": "1.0.0", "inputs": {"prompt": "a fox"}}}}})"));
        f.edits.requestModels("enhance", "model");
        QTRY_VERIFY(!f.edits.modelsLoading("enhance", "model"));
        QStringList titles;
        for (const QVariant &entry : f.edits.models("enhance", "model"))
            if (entry.toMap().contains("handle"))
                titles.append(entry.toMap().value("title").toString() + " @ " + entry.toMap().value("provider").toString());
        QVERIFY2(titles.contains("Qwen3.5 2B @ studio"), qPrintable(titles.join(", ")));
        for (const QString &title : titles)
            QVERIFY2(!title.contains("FLUX") && !title.contains("Z-Image"), qPrintable(title));
        QCOMPARE(f.edits.modelsEmptyText("enhance", "model"),
                 QString("No text model available: start one in omarchy-local-ai or add a provider"));
    }

    void aLinkedInputShowsItsSource() {
        Fixture f;
        QCOMPARE(f.edits.source("join", "a"), QString("Prompt · Text"));
        QCOMPARE(f.edits.source("join", "b"), QString());
        QVERIFY(!f.edits.value("join", "a").isValid());
        // Typing into it is refused: unlink it first.
        QVERIFY(!f.edits.setText("join", "a", "x").isEmpty());
    }

    void theFaceSaysWhichWidgetEachRowHas() {
        Fixture f;
        const QVariantList inputs =
            f.canvas.nodes()->data(f.canvas.nodes()->index(0, 0), NodeList::Inputs).toList();
        QHash<QString, QVariantMap> byName;
        for (const QVariant &each : inputs)
            byName.insert(each.toMap().value("name").toString(), each.toMap());
        QCOMPARE(byName.value("seed").value("widgetType").toString(), QString("seed"));
        // A resource input is a value row with the model picker (2.5), not a wire-only slot.
        QCOMPARE(byName.value("model").value("widgetType").toString(), QString("model"));
        QVERIFY(byName.value("model").value("widget").toBool());
        QVERIFY(!byName.value("image").value("widget").toBool());
    }

    // #715: the text window lists a node's text inputs and types into the input through a session of its own.
    void theTextWindowListsTheTextInputsOfANode() {
        Fixture f;
        QStringList names;
        for (const QVariant &input : f.edits.textInputs("all"))
            names << input.toMap().value("name").toString() + ":" + input.toMap().value("widget").toString();
        QCOMPARE(names, (QStringList{"name:line", "notes:multiline"}));
        QVERIFY(f.edits.textInputs("gone").isEmpty()); // a placeholder has no widgets
        QVERIFY(f.canvas.textEditNode("").isEmpty()); // nothing selected
        f.canvas.select({"prompt"});
        QCOMPARE(f.canvas.textEditNode(""), QString("prompt"));
        QCOMPARE(f.canvas.textEditNode("all"), QString("all")); // the node a menu was opened on wins
        QVERIFY(f.canvas.textEditNode("gone").isEmpty());
    }

    // #715 slice 5: a double-click on a text result's excerpt opens the whole result: it says where the excerpt is, and what the text is.
    void aTextResultsExcerptIsWhereADoubleClickOpensItsWholeText() {
        Fixture f;
        QVERIFY(!f.canvas.resultText("prompt").value("has").toBool()); // before a run
        f.canvas.setRunState("prompt", {{"state", "cached"}, {"thumbnail", "data:,"}, {"thumbnailAspect", 0.3},
                                        {"thumbnailText", "a red fox\nin the snow"},
                                        {"texts", QVariantMap{{"text", "a red fox\nin the snow at dawn, soft light"}}}});
        const QVariantMap result = f.canvas.resultText("prompt");
        QVERIFY(result.value("has").toBool());
        QCOMPARE(result.value("output").toString(), QString("text"));
        QCOMPARE(result.value("label").toString(), QString("Text"));
        QCOMPARE(result.value("text").toString(), QString("a red fox\nin the snow at dawn, soft light"));
        QRectF excerpt; // in the world: the card's own rectangle moved to where the node is
        for (int row = 0; row < f.canvas.nodes()->rowCount(); ++row)
            if (f.canvas.nodes()->data(f.canvas.nodes()->index(row, 0), NodeList::NodeId).toString() == "prompt")
                excerpt = f.canvas.nodes()->data(f.canvas.nodes()->index(row, 0), NodeList::ThumbnailRect).toRectF().translated(f.canvas.position("prompt"));
        QVERIFY(!excerpt.isEmpty());
        QVERIFY(f.canvas.textResultAt("prompt", excerpt.center().x(), excerpt.center().y()));
        QVERIFY(!f.canvas.textResultAt("prompt", excerpt.left() - 30, excerpt.center().y())); // off the excerpt
        QVERIFY(!f.canvas.pictureAt("prompt", excerpt.center().x(), excerpt.center().y()));   // a text is not a picture
        // A placeholder, a number and a node with no text are not text results.
        f.canvas.setRunState("prompt", {{"state", "cached"}, {"thumbnail", "data:,"}, {"thumbnailAspect", 0.3},
                                        {"thumbnailText", "No result yet"}, {"thumbnailDim", true},
                                        {"texts", QVariantMap{{"text", "x"}}}});
        QVERIFY(!f.canvas.textResultAt("prompt", excerpt.center().x(), excerpt.center().y()));
        f.canvas.setRunState("prompt", {{"state", "cached"}, {"thumbnail", "data:,"}, {"thumbnailAspect", 0.3}, {"thumbnailText", "42"}});
        QVERIFY(!f.canvas.textResultAt("prompt", excerpt.center().x(), excerpt.center().y()));
        QVERIFY(!f.canvas.resultText("all").value("has").toBool());
        QVERIFY(!f.canvas.textResultAt("gone", 0, 0));
    }

    void aVisitToTheTextWindowIsOneStepAndAnInterruptedOneBeginsAnother() {
        Fixture f;
        QCOMPARE(f.edits.typeInto("all", "notes", "a"), QString());
        QCOMPARE(f.edits.typeInto("all", "notes", "a fox"), QString());
        QCOMPARE(f.edits.typeInto("all", "notes", "a fox\nin snow"), QString());
        QVERIFY(f.edits.editing());
        f.edits.finishInto("all", "name"); // not this input's session: it stays open
        QVERIFY(f.edits.editing());
        f.edits.finishInto("all", "notes");
        QVERIFY(!f.edits.editing());
        QCOMPARE(f.stored("all", "notes"), QJsonValue("a fox\nin snow"));
        QCOMPARE(f.steps(), 1);
        // Another field takes the session while the window is open (a drag on the canvas, a run advancing a seed): the next key
        // is a step of its own, written to the notes and never to the field that has the session.
        f.edits.typeInto("all", "notes", "a fox\nin snow!");
        f.edits.begin("all", "count");
        QCOMPARE(f.edits.typeInto("all", "notes", "a fox\nin snow!!"), QString());
        QCOMPARE(f.stored("all", "notes"), QJsonValue("a fox\nin snow!!"));
        QCOMPARE(f.stored("all", "count"), QJsonValue(7));
        f.edits.finishInto("all", "notes");
        QCOMPARE(f.steps(), 3);
        f.canvas.undo();
        QCOMPARE(f.stored("all", "notes"), QJsonValue("a fox\nin snow!"));
        // The window's text is checked as the inline field's: a linked input takes none.
        QVERIFY(!f.edits.typeInto("join", "a", "x").isEmpty());
    }

    void aLinkedInputShowsTheTextItsSourceMadeInItsLastRun() {
        Fixture f;
        QVERIFY(!f.edits.linkedText("prompt", "text").value("linked").toBool());
        QVariantMap before = f.edits.linkedText("join", "a");
        QVERIFY(before.value("linked").toBool());
        QCOMPARE(before.value("source").toString(), QString("prompt"));
        QCOMPARE(before.value("sourceTitle").toString(), QString("Prompt"));
        QVERIFY(!before.value("has").toBool()); // it has not run
        f.canvas.setRunState("prompt", {{"state", "cached"}, {"texts", QVariantMap{{"text", "a red fox\nin the snow"}}}});
        const QVariantMap after = f.edits.linkedText("join", "a");
        QVERIFY(after.value("has").toBool());
        QCOMPARE(after.value("text").toString(), QString("a red fox\nin the snow"));
        QVERIFY(!after.value("output").toString().isEmpty());
        f.canvas.setRunState("prompt", {{"state", "error"}, {"error", "boom"}});
        QCOMPARE(f.canvas.runError("prompt"), QString("boom"));
        QVERIFY(!f.edits.linkedText("join", "a").value("has").toBool());
    }

    void revealSelectsTheNodeAndBringsItIntoView() {
        Fixture f;
        QSignalSpy moved(&f.canvas, &CanvasModel::viewMoveRequested);
        f.canvas.reveal("prompt");
        QCOMPARE(f.canvas.selectedIds(), QStringList{"prompt"});
        QCOMPARE(moved.count(), 1);
        f.canvas.reveal("nowhere"); // nothing changes
        QCOMPARE(moved.count(), 1);
        QCOMPARE(f.canvas.selectedIds(), QStringList{"prompt"});
    }
};

QTEST_MAIN(TestWidgetEdit)
#include "test_widget_edit.moc"
