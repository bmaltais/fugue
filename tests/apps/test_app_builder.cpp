// The app builder's store (#393, apps/app_builder.h), no window: a new workflow becomes an app in a few model calls, the block is
// written as one undo step, validation says in words what Apply would refuse, and an app already there loads into the builder and
// goes back unchanged. The dialog and the click-to-expose layer are test_app_builder_window.

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QUndoStack>
#include <QtTest>

#include "apps/app_builder.h"
#include "canvas/canvas_model.h"
#include "engine/app_binding.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

const char *workflow = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/sample.png"}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "an old family portrait"}},
    "grow": {"type": "fugue/grow-mask", "version": "1.0.1", "inputs": {"tapered_corners": true}},
    "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 3, "steps": 3, "conditioning": {"from": "encode.conditioning"}}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "encode": {"pos": [400, 0]}, "grow": {"pos": [0, 400]}, "sample": {"pos": [400, 400]}, "save": {"pos": [800, 0]}}}
})";

struct Fixture {
    Registry registry;
    CanvasModel canvas{&registry};
    AppBuilder &builder;
    explicit Fixture(const QByteArray &json = workflow) : builder(*canvas.appBuilder()) {
        nodes::addBuiltinNodeTypes(registry);
        const Result<Document> loaded = Document::load(json);
        if (!loaded.ok())
            qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
        canvas.setDocument(loaded.value());
    }
    QJsonValue block() const { return canvas.document().extra.value(QStringLiteral("app")); }
};

QStringList keysOf(const QVariantList &rows) {
    QStringList keys;
    for (const QVariant &row : rows)
        keys << row.toMap().value("key").toString();
    return keys;
}

} // namespace

class TestAppBuilder : public QObject {
    Q_OBJECT

private slots:
    void aNewWorkflowBecomesAnAppInAFewCallsAsOneUndoStep() {
        Fixture f;
        QVERIFY(!f.builder.hasBlock());
        QVERIFY(f.builder.begin(QStringLiteral("Restyle")));
        QCOMPARE(f.builder.title(), QStringLiteral("Restyle"));
        QVERIFY(!f.builder.editing());
        QVERIFY(f.builder.exposeInput("load", "file"));
        QVERIFY(f.builder.exposeInput("encode", "prompt"));
        QVERIFY(f.builder.exposeInput("sample", "seed"));
        QVERIFY(f.builder.exposeInput("sample", "steps"));
        QVERIFY(!f.builder.problems().isEmpty()); // no output yet
        QVERIFY(f.builder.addOutput("save"));
        QVERIFY(f.builder.editInput("steps", {{"label", "Passes"}, {"min", "1"}, {"max", "8"}, {"advanced", true}}));
        QVERIFY(f.builder.moveInput("steps", 0));
        QVERIFY2(f.builder.problems().isEmpty(), qPrintable(f.builder.problems().join('\n')));
        QCOMPARE(f.canvas.undoStack()->count(), 0); // nothing touches the workflow before Apply
        QVERIFY(!f.block().isObject());

        QSignalSpy applied(&f.builder, &AppBuilder::applied);
        QVERIFY(f.builder.apply(true));
        QCOMPARE(applied.count(), 1);
        QVERIFY(applied.first().first().toBool());
        QVERIFY(!f.builder.isOpen());
        QCOMPARE(f.canvas.undoStack()->count(), 1);
        QCOMPARE(f.canvas.undoStack()->undoText(), QStringLiteral("Make app"));

        const Result<std::optional<app::App>> read = app::appOf(f.canvas.document());
        QVERIFY(read.ok() && read.value());
        const app::App &app = *read.value();
        QCOMPARE(app.title, QStringLiteral("Restyle"));
        QCOMPARE(app.defaultView, QStringLiteral("app"));
        QCOMPARE(app.inputs.size(), 4);
        QCOMPARE(app.inputs.at(0).key(), QStringLiteral("steps"));
        QCOMPARE(app.inputs.at(0).label, QStringLiteral("Passes"));
        QCOMPARE(*app.inputs.at(0).min, 1.0);
        QCOMPARE(*app.inputs.at(0).max, 8.0);
        QCOMPARE(app.inputs.at(0).section, QStringLiteral("Advanced"));
        QCOMPARE(app.inputs.at(1).role, QStringLiteral("image"));
        QCOMPARE(app.inputs.at(2).role, QStringLiteral("prompt"));
        QCOMPARE(app.inputs.at(3).role, QStringLiteral("seed"));
        QCOMPARE(app.outputs.size(), 1);
        QVERIFY(app::checkApp(app, f.canvas.document(), f.registry).isEmpty());

        f.canvas.undo(); // one step: the block goes
        QVERIFY(!f.block().isObject());
        QVERIFY(!f.builder.hasBlock());
        f.canvas.redo();
        QVERIFY(f.block().isObject());
    }

    void validationSaysInWordsWhatApplyWouldRefuse() {
        Fixture f;
        QVERIFY(f.builder.begin());
        QVERIFY(f.builder.problems().join(' ').contains("title"));
        f.builder.setTitle("Restyle");
        QVERIFY(f.builder.exposeInput("encode", "prompt"));
        QVERIFY(f.builder.problems().join(' ').contains("at least one output"));
        QVERIFY(!f.builder.apply());
        QVERIFY(f.builder.isOpen()); // a refused apply keeps the builder open
        QVERIFY(f.builder.addOutput("encode")); // not a sink: in the list, and a problem in words
        const QString problems = f.builder.problems().join('\n');
        QVERIFY2(problems.contains("not a Save or Preview sink"), qPrintable(problems));
        QVERIFY(!f.builder.apply());
        QCOMPARE(f.canvas.undoStack()->count(), 0);
        QVERIFY(f.builder.removeOutput("encode"));
        QVERIFY(f.builder.addOutput("save"));
        QVERIFY(f.builder.problems().isEmpty());
        QVERIFY(!f.builder.addOutput("save")); // twice
        QVERIFY(!f.builder.addOutput("nowhere"));
        f.builder.cancel();
        QVERIFY(!f.block().isObject());
    }

    void refusesWhatTheFormCannotAsk() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        QVERIFY(!f.builder.exposeInput("sample", "conditioning")); // linked
        QVERIFY(f.builder.notice().contains("linked"));
        QVERIFY(!f.builder.exposeInput("sample", "nope"));
        QVERIFY(!f.builder.exposeInput("nowhere", "file"));
        QVERIFY(f.builder.exposeInput("sample", "steps"));
        QVERIFY(!f.builder.exposeInput("sample", "steps")); // already in
        QVERIFY(f.builder.notice().contains("already"));
        // One input can drive several of its type (5.4), never another type.
        QVERIFY(!f.builder.alsoDrive("steps", "load", "file"));
        QVERIFY(f.builder.notice().contains("not"));
        QStringList could;
        for (const QVariant &candidate : f.builder.driveCandidates("steps"))
            could << candidate.toMap().value("node").toString() + "." + candidate.toMap().value("input").toString();
        QVERIFY(could.contains("sample.seed"));
        QVERIFY(!could.contains("load.file")); // another type
        QVERIFY(f.builder.alsoDrive("steps", "sample", "seed"));
        const QVariantMap row = f.builder.inputs().first().toMap();
        QCOMPARE(row.value("targets").toStringList(), QStringList({"sample.steps", "sample.seed"}));
        QVERIFY(f.builder.removeInput("steps"));
        QVERIFY(f.builder.inputs().isEmpty());
    }

    void aSecondPictureKeepsItsRoleUnderAName() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        QVERIFY(f.builder.exposeInput("load", "file"));
        QVERIFY(f.builder.exposeInput("load", "mask"));
        QCOMPARE(keysOf(f.builder.inputs()), QStringList({"image", "mask"}));
    }

    void aSecondClickTakesAnInputOutAgain() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        QVERIFY(f.builder.toggleInput("encode", "prompt"));
        QCOMPARE(f.builder.inputs().size(), 1);
        QVERIFY(f.builder.toggleInput("encode", "prompt"));
        QVERIFY(f.builder.inputs().isEmpty());
        QVERIFY(f.builder.toggleOutput("save"));
        QCOMPARE(f.builder.outputs().size(), 1);
        QVERIFY(f.builder.toggleOutput("save"));
        QVERIFY(f.builder.outputs().isEmpty());
    }

    void aClickOnTheCanvasPicksTheRowOrTheSink() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        f.builder.pickAt(0, 0); // not picking yet: nothing
        QVERIFY(f.builder.inputs().isEmpty());
        f.builder.startPicking();
        QVERIFY(f.builder.picking());
        const QRectF row = f.canvas.inputRowRect("encode", "prompt");
        QVERIFY(!row.isEmpty());
        f.builder.pickAt(row.center().x(), row.center().y());
        QCOMPARE(keysOf(f.builder.inputs()), QStringList({"prompt"}));
        const QRectF sink = f.canvas.nodeRect("save");
        f.builder.pickAt(sink.center().x(), sink.top() + 1); // the header: no input row, a sink
        QCOMPARE(f.builder.outputs().size(), 1);
        const QRectF grow = f.canvas.nodeRect("grow");
        f.builder.pickAt(grow.center().x(), grow.top() + 1); // a node that is no sink: a sentence, nothing added
        QCOMPARE(f.builder.outputs().size(), 1);
        QVERIFY(f.builder.notice().contains("no result"));
        QCOMPARE(f.builder.marks().size(), 2);
        f.builder.finishPicking();
        QVERIFY(!f.builder.picking());
        QVERIFY(f.builder.isOpen());
    }

    void anExistingBlockLoadsUnchangedAndAppliesAsNothing() {
        QByteArray text(workflow);
        text.replace(R"("fugue": 1,)", R"("fugue": 1,
  "app": {"format": 1, "title": "Restore", "description": "Cleans a photo.", "defaultView": "graph",
          "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                     {"name": "steps", "targets": ["sample.steps"], "label": "Times", "min": 1, "max": 4, "section": "Advanced", "default": 2}],
          "outputs": [{"node": "save", "label": "Result"}]},)");
        Fixture f(text);
        const QJsonValue before = f.block();
        QVERIFY(before.isObject());
        QVERIFY(f.builder.hasBlock());
        QVERIFY(f.builder.begin("ignored"));
        QVERIFY(f.builder.editing());
        QCOMPARE(f.builder.title(), QStringLiteral("Restore")); // the block's own, not the suggestion
        QCOMPARE(f.builder.defaultView(), QStringLiteral("graph"));
        QCOMPARE(f.builder.block(), before.toObject());
        QVERIFY(f.builder.problems().isEmpty());
        QCOMPARE(f.builder.inputs().size(), 2);
        QCOMPARE(f.builder.outputs().first().toMap().value("label").toString(), QStringLiteral("Result"));
        QVERIFY(f.builder.apply());
        QCOMPARE(f.block(), before); // written back as it was
        QCOMPARE(f.canvas.undoStack()->count(), 0); // and no undo step for nothing
    }

    void editingChangesTheBlockInOneStepAndRemoveAppTakesItOut() {
        QByteArray text(workflow);
        text.replace(R"("fugue": 1,)", R"("fugue": 1,
  "app": {"format": 1, "title": "Restore", "defaultView": "app",
          "inputs": [{"role": "prompt", "targets": ["encode.prompt"]}], "outputs": [{"node": "save"}]},)");
        Fixture f(text);
        QVERIFY(f.builder.begin());
        f.builder.setTitle("Restore a photo");
        QVERIFY(f.builder.apply());
        QCOMPARE(f.canvas.undoStack()->undoText(), QStringLiteral("Edit app"));
        QCOMPARE(f.block().toObject().value("title").toString(), QStringLiteral("Restore a photo"));
        QVERIFY(f.builder.begin());
        QVERIFY(f.builder.removeApp());
        QVERIFY(!f.block().isObject());
        QCOMPARE(f.canvas.undoStack()->count(), 2);
        f.canvas.undo();
        QVERIFY(f.block().isObject());
    }

    void whatTheWorkflowLostIsLeftOutWithASentence() {
        QByteArray text(workflow);
        text.replace(R"("fugue": 1,)", R"("fugue": 1,
  "app": {"format": 1, "title": "Restore", "defaultView": "app",
          "inputs": [{"role": "prompt", "targets": ["gone.prompt"]}, {"role": "seed", "targets": ["sample.seed"]}],
          "outputs": [{"node": "save"}]},)");
        Fixture f(text);
        QVERIFY(f.builder.begin());
        QCOMPARE(keysOf(f.builder.inputs()), QStringList({"seed"}));
        QVERIFY(f.builder.notice().contains("gone"));
    }

    void theChoicesListWhatAKeyboardCanAdd() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        QVariantList choices = f.builder.inputChoices();
        QStringList wanted;
        for (const QVariant &choice : choices)
            wanted << choice.toMap().value("node").toString() + "." + choice.toMap().value("input").toString();
        QVERIFY(wanted.contains("encode.prompt"));
        QVERIFY(wanted.contains("sample.steps"));
        QVERIFY(!wanted.contains("sample.conditioning")); // linked
        QCOMPARE(f.builder.outputChoices().size(), 1);
        QCOMPARE(f.builder.outputChoices().first().toMap().value("node").toString(), QStringLiteral("save"));
    }

    void thePreviewIsTheAppViewsFormOfTheDraft() {
        Fixture f;
        QVERIFY(f.builder.begin("A"));
        AppView *preview = f.builder.preview();
        QVERIFY(preview);
        QCOMPARE(preview->fields().size(), 0);
        QVERIFY(f.builder.exposeInput("sample", "steps"));
        QCOMPARE(preview->fields().size(), 1);
        QVERIFY(f.builder.editInput("steps", {{"label", "Passes"}}));
        QCOMPARE(preview->fields().first().toMap().value("label").toString(), QStringLiteral("Passes"));
        QVERIFY(f.builder.removeInput("steps"));
        QCOMPARE(preview->fields().size(), 0);
    }
};

QTEST_MAIN(TestAppBuilder)
#include "test_app_builder.moc"
