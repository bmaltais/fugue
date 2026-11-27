// Group editing through the canvas model's group editor (canvas/group_edit.h), no window: the
// level shown (enter, leave, a breadcrumb jump), the interface of a group (expose, unexpose,
// rename, reorder), and the problem and notice lines. The window tests (test_canvas_group_window)
// keep what needs a pointer or a dialog; test_canvas_groups has the document-level cases.

#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_model.h"
#include "canvas/group_edit.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

// prompt -> gen -> save, all placed.
const char *chain = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "prompt.text"}, "steps": 8, "seed": 3}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "name": "fox"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 100]}, "gen": {"pos": [300, 80]}, "save": {"pos": [640, 120]}}}
})";

QStringList namesOf(const QVariantList &ports) {
    QStringList names;
    for (const QVariant &port : ports)
        names << port.toMap().value("name").toString();
    return names;
}

struct Fixture {
    Registry registry;
    CanvasModel canvas{&registry};
    GroupEdit &groups;
    QString instance;
    Fixture() : groups(*canvas.groups()) {
        nodes::addBuiltinNodeTypes(registry);
        const Result<Document> loaded = Document::load(QByteArray(chain));
        if (!loaded.ok())
            qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
        canvas.setDocument(loaded.value());
        canvas.select({"gen"});
        instance = groups.makeGroup();
    }
    int steps() const { return canvas.undoStack()->count(); }
};

} // namespace

class TestGroupEdit : public QObject {
    Q_OBJECT

private slots:
    void enteringAndLeavingMoveBetweenLevels() {
        Fixture f;
        QVERIFY2(!f.instance.isEmpty(), qPrintable(f.groups.problem()));
        QVERIFY(!f.groups.enterGroup("prompt")); // not a group
        QVERIFY(!f.groups.leaveGroup());         // already at the top
        const int steps = f.steps();

        QSignalSpy scope(&f.canvas, &CanvasModel::scopeChanged);
        QVERIFY(f.groups.enterGroup(f.instance));
        QVERIFY(f.canvas.inGroup());
        QCOMPARE(f.canvas.breadcrumb().size(), 1);
        QVERIFY(f.groups.leaveGroup());
        QVERIFY(!f.canvas.inGroup());
        QCOMPARE(f.canvas.selectedIds(), QStringList{f.instance}); // the instance is selected again
        QCOMPARE(scope.count(), 2);

        // A breadcrumb jump to the top, and to the level already shown, change nothing more.
        QVERIFY(f.groups.enterGroup(f.instance));
        f.groups.leaveTo(1);
        QVERIFY(f.canvas.inGroup());
        f.groups.leaveTo(0);
        QVERIFY(!f.canvas.inGroup());
        QCOMPARE(f.steps(), steps); // entering and leaving are not edits
    }

    void interfaceEditsAreOneUndoStepEach() {
        Fixture f;
        QVERIFY(f.groups.groupInterface().isEmpty()); // at the top there is no interface
        QVERIFY(f.groups.enterGroup(f.instance));
        QCOMPARE(namesOf(f.groups.groupInterface().value("inputs").toList()), QStringList{"prompt"});

        int steps = f.steps();
        QVERIFY2(f.groups.exposeInput("gen", "seed"), qPrintable(f.groups.problem()));
        QVERIFY(f.groups.exposeInput("gen", "steps"));
        QCOMPARE(f.steps(), steps + 2);
        QCOMPARE(namesOf(f.groups.groupInterface().value("inputs").toList()), (QStringList{"prompt", "seed", "steps"}));
        QCOMPARE(f.groups.exposedName("gen", "seed", false), QStringLiteral("seed"));
        QCOMPARE(f.groups.exposedName("gen", "cfg", false), QString());

        // Reorder: seed to the front; renaming an input and the output; each one step.
        steps = f.steps();
        QVERIFY(f.groups.moveExposed(false, "seed", 0));
        QCOMPARE(namesOf(f.groups.groupInterface().value("inputs").toList()), (QStringList{"seed", "prompt", "steps"}));
        QVERIFY(f.groups.editExposedInput("steps", {{"name", "quality"}, {"label", "Quality"}}));
        QVERIFY(f.groups.renameExposedOutput("image", "picture"));
        QCOMPARE(f.steps(), steps + 3);
        const QVariantMap face = f.groups.groupInterface();
        QCOMPARE(namesOf(face.value("inputs").toList()), (QStringList{"seed", "prompt", "quality"}));
        QCOMPARE(namesOf(face.value("outputs").toList()), QStringList{"picture"});

        // The label on the face is not the port's name (#72): relabel, the name stays.
        QCOMPARE(f.groups.portToRename("gen", "seed", false).value("name").toString(), QStringLiteral("seed"));
        QVERIFY(f.groups.renamePort("gen", "seed", false, "Random seed"));
        QCOMPARE(f.groups.portToRename("gen", "seed", false).value("label").toString(), QStringLiteral("Random seed"));

        // Stop exposing: one target of an input, a whole input, an output.
        QVERIFY(!f.groups.unexposeInput("gen", "cfg")); // not exposed
        QVERIFY(f.groups.unexposeInput("gen", "steps"));
        QVERIFY(f.groups.removeExposedInput("seed"));
        QVERIFY(f.groups.unexposeOutput("picture"));
        QCOMPARE(namesOf(f.groups.groupInterface().value("inputs").toList()), QStringList{"prompt"});
        QVERIFY(f.groups.groupInterface().value("outputs").toList().isEmpty());
        QVERIFY(f.groups.exposeOutput("gen", "image"));
        QCOMPARE(namesOf(f.groups.groupInterface().value("outputs").toList()), QStringList{"image"});

        // Undo walks back one edit at a time.
        f.canvas.undo();
        QVERIFY(f.groups.groupInterface().value("outputs").toList().isEmpty());
    }

    void exposeCandidatesAreTheInputsOfTheSameTypeNotExposedYet() {
        Fixture f;
        QVERIFY(f.groups.enterGroup(f.instance));
        QVERIFY(f.groups.exposeInput("gen", "steps"));
        const QVariantList candidates = f.groups.exposeCandidates("steps");
        QVERIFY(!candidates.isEmpty());
        for (const QVariant &candidate : candidates) {
            QCOMPARE(candidate.toMap().value("node").toString(), QStringLiteral("gen"));
            QVERIFY(candidate.toMap().value("input").toString() != QStringLiteral("steps"));
        }
        QVERIFY(f.groups.exposeCandidates("nothing").isEmpty());
    }

    // The editor says what an exposed port does in words, with the inner nodes' titles and the ports' labels, and lists
    // what is left to add.
    void theInterfaceSaysWhatEachPortSetsInWords() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas{&registry};
        GroupEdit &groups = *canvas.groups();
        const Result<Document> loaded = Document::load(QByteArray(R"({"fugue": 1,
          "graph": {"nodes": {
            "loader": {"type": "fugue/load-models", "version": "1.2.0", "inputs": {}},
            "encode": {"type": "fugue/text-encode", "version": "1.2.0", "inputs": {"model": {"from": "loader.model"}}},
            "sample": {"type": "fugue/sampler", "version": "1.5.0",
                       "inputs": {"model": {"from": "loader.model"}, "conditioning": {"from": "encode.conditioning"}}},
            "picture": {"type": "fugue/vae-decode", "version": "1.0.0", "inputs": {"latent": {"from": "sample.latent"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "picture.image"}}}}},
          "layout": {"nodes": {"loader": {"pos": [0, 0]}, "encode": {"pos": [300, 0], "title": "Encode"},
                               "sample": {"pos": [600, 0], "title": "Sample"}, "picture": {"pos": [900, 0], "title": "Picture"},
                               "save": {"pos": [1200, 0]}}}})"));
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());
        canvas.select({"encode", "sample", "picture"});
        const QString instance = groups.makeGroup();
        QVERIFY2(!instance.isEmpty(), qPrintable(groups.problem()));
        QVERIFY(groups.enterGroup(instance));

        QVariantMap face = groups.groupInterface();
        QCOMPARE(face.value("inputs").toList().value(0).toMap().value("sets").toString(),
                 QStringLiteral("Sets Model on Encode and Sample"));
        const QVariantMap output = face.value("outputs").toList().value(0).toMap();
        QCOMPARE(output.value("comesFrom").toString(), QStringLiteral("From Image on Picture"));
        QCOMPARE(output.value("fromNode").toString(), QStringLiteral("picture"));
        // With no label of its own a port shows the inner one, as the card does.
        QCOMPARE(face.value("inputs").toList().value(0).toMap().value("shown").toString(), QStringLiteral("Model"));
        QCOMPARE(output.value("shown").toString(), QStringLiteral("Image"));

        // Two labels on one node: each says where it goes.
        QVERIFY(groups.exposeInput("sample", "seed"));
        QVERIFY2(groups.exposeInput("sample", "variation_seed", "seed"), qPrintable(groups.problem()));
        face = groups.groupInterface();
        QCOMPARE(face.value("inputs").toList().value(1).toMap().value("sets").toString(),
                 QStringLiteral("Sets Seed on Sample and Variation seed on Sample"));

        // What is left to add: never a port exposed already.
        QStringList inputs, outputs;
        for (const QVariant &choice : groups.exposeChoices(false)) {
            const QVariantMap map = choice.toMap();
            inputs << map.value("node").toString() + "." + map.value("port").toString();
            if (inputs.last() == "sample.steps")
                QCOMPARE(map.value("title").toString(), QStringLiteral("Sample \u203a Steps"));
        }
        for (const QVariant &choice : groups.exposeChoices(true))
            outputs << choice.toMap().value("node").toString() + "." + choice.toMap().value("port").toString();
        QVERIFY(inputs.contains("sample.steps"));
        for (const char *exposed : {"encode.model", "sample.model", "sample.seed", "sample.variation_seed"})
            QVERIFY2(!inputs.contains(exposed), exposed);
        QVERIFY(outputs.contains("sample.latent"));
        QVERIFY(!outputs.contains("picture.image"));
    }

    // The editor relabels the shown group's output, also one that comes from a group nested in it: never the nested
    // group's own definition (whose instances are elsewhere too).
    void relabelingAnOutputFromANestedGroupChangesTheShownGroupOnly() {
        Fixture f; // gen is a group (inner)
        const QString inner = f.canvas.document().graph.nodes.value(f.instance).type;
        f.canvas.select({"prompt", f.instance});
        const QString outer = f.groups.makeGroup();
        QVERIFY2(!outer.isEmpty(), qPrintable(f.groups.problem()));
        QVERIFY(f.groups.enterGroup(outer));
        const QVariantMap output = f.groups.groupInterface().value("outputs").toList().value(0).toMap();
        QCOMPARE(output.value("fromNode").toString(), f.instance);
        QVERIFY2(f.groups.relabelExposedOutput(output.value("name").toString(), "Picture"), qPrintable(f.groups.problem()));
        const QVariantMap after = f.groups.groupInterface().value("outputs").toList().value(0).toMap();
        QCOMPARE(after.value("label").toString(), QStringLiteral("Picture"));
        QCOMPARE(after.value("shown").toString(), QStringLiteral("Picture"));
        for (const GroupOutput &port : f.canvas.document().groups.value(inner).outputs)
            QVERIFY2(port.label.isEmpty(), qPrintable(port.label)); // the nested group is untouched
        QVERIFY(f.groups.relabelExposedOutput(output.value("name").toString(), "")); // empty: the inner label again
        QCOMPARE(f.groups.groupInterface().value("outputs").toList().value(0).toMap().value("label").toString(), QString());
    }

    void aRefusalSetsTheProblemAndAnEditThatWorksClearsIt() {
        Fixture f;
        QSignalSpy changed(&f.groups, &GroupEdit::problemChanged);
        QVERIFY(!f.groups.exposeInput("gen", "seed")); // at the top there is no group to edit
        QVERIFY(f.groups.enterGroup(f.instance));
        QVERIFY(f.groups.exposeInput("gen", "seed"));
        QVERIFY(!f.groups.exposeInput("gen", "seed")); // exposed already
        QVERIFY(!f.groups.problem().isEmpty());
        QVERIFY(changed.count() >= 1);
        QVERIFY(f.groups.exposeInput("gen", "steps")); // an edit that works clears it
        QCOMPARE(f.groups.problem(), QString());

        QVERIFY(!f.groups.editExposedInput("steps", {{"name", "Not A Name"}}));
        QVERIFY(!f.groups.problem().isEmpty());
        f.groups.dismiss();
        QCOMPARE(f.groups.problem(), QString());
    }

    void makeGroupWithNothingSelectedSaysWhy() {
        Fixture f;
        f.canvas.select({});
        QCOMPARE(f.groups.makeGroup(), QString());
        QVERIFY(!f.groups.problem().isEmpty());
        QCOMPARE(f.groups.makeUnique(), QString()); // nothing selected: no group, and no complaint to add
    }

    void aLibraryResultAnnouncedElsewhereShowsAsANoticeOrAProblem() {
        Fixture f;
        QSignalSpy notice(&f.groups, &GroupEdit::noticeChanged);
        f.groups.announceLibraryResult("Deleted local/group", {});
        QCOMPARE(f.groups.notice(), QStringLiteral("Deleted local/group"));
        QCOMPARE(f.groups.problem(), QString());
        f.groups.announceLibraryResult("Deleted local/group", "Could not delete it.");
        QCOMPARE(f.groups.notice(), QString()); // a failure shows the problem alone
        QCOMPARE(f.groups.problem(), QStringLiteral("Could not delete it."));
        QCOMPARE(notice.count(), 2);
        f.groups.dismiss();
        QCOMPARE(f.groups.problem(), QString());
    }
};

QTEST_MAIN(TestGroupEdit)
#include "test_group_edit.moc"
