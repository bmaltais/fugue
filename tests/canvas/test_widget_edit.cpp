// Editing input values (spec 6.2, 3.2, 2.5) through the canvas model's widget editor
// (canvas/widget_edit.h): what each widget is told about its input, typed text turned into
// values and checked by the engine's rules, one undo step per commit, typing sessions,
// seeds, and the model picker's list. The window tests drive the same calls with keys.

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "widget_fixture.h"

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
        // A comma works as the decimal separator too; spaces around do not matter.
        QCOMPARE(f.edits.setText("all", "amount", " 0,25 "), QString());
        QCOMPARE(f.stored("all", "amount"), QJsonValue(0.25));
        // The same value again is no step.
        QCOMPARE(f.edits.setText("all", "amount", "0.25"), QString());
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
        f.edits.setModelSource([&](const QString &type, std::function<void(QList<WidgetEdit::Model>)> done) {
            askedFor = type;
            answer = std::move(done);
        });
        QSignalSpy changed(&f.edits, &WidgetEdit::modelsChanged);
        f.edits.requestModels("all", "model");
        QCOMPARE(askedFor, QString("fugue/image-model"));
        QVERIFY(f.edits.modelsLoading("all", "model"));
        const QJsonObject zimage{{"resource", "fugue/image-model"}, {"name", "z-image-turbo"}};
        answer({{"z-image-turbo", zimage, "ready", "This machine"}});
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
};

QTEST_MAIN(TestWidgetEdit)
#include "test_widget_edit.moc"
