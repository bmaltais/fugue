// Group edits (spec 5, 4.4): Make group, Ungroup, Make unique, exposing inputs and outputs and
// a group's face, as Document-to-Document functions (engine/groups.h).
//
// Seams: the functions of groups.h, observed through the document they return, compile() of
// it (a group must flatten to what the nodes compiled to before), and a run of a nested group
// made and saved this way.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/groups.h"
#include "engine/registry.h"
#include "scheduler_support.h"

using namespace fugue;
using fugue::test::json;

namespace {

QString dump(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"v", value}}).toJson(QJsonDocument::Compact));
}

QString dump(const Errors &errors) {
    return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson(QJsonDocument::Compact));
}

Registry registry() {
    Registry r;
    const char *manifests[] = {
        "{'manifest': 1, 'id': 'fugue/text', 'version': '1.0.0', 'name': 'Text', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text', 'label': 'Text'}], 'outputs': [{'name': 'text', 'type': 'text'}]}",
        "{'manifest': 1, 'id': 'fugue/generate', 'version': '1.0.0', 'name': 'Generate', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'prompt', 'type': 'text'}, {'name': 'negative', 'type': 'text', 'default': ''},"
        "            {'name': 'seed', 'type': 'number', 'integer': true, 'default': 0},"
        "            {'name': 'steps', 'type': 'number', 'integer': true, 'default': 4, 'min': 1, 'max': 50}],"
        " 'outputs': [{'name': 'image', 'type': 'image'}]}",
        "{'manifest': 1, 'id': 'fugue/merge', 'version': '1.0.0', 'name': 'Merge', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'images', 'type': 'image', 'list': true}], 'outputs': [{'name': 'image', 'type': 'image'}]}",
        "{'manifest': 1, 'id': 'fugue/save', 'version': '1.0.0', 'name': 'Save', 'sink': true, 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'image', 'type': 'image'}], 'outputs': []}",
    };
    for (const char *manifest : manifests) {
        const Errors errors = r.addManifest(json(manifest));
        if (hasErrors(errors))
            qFatal("bad fake manifest: %s", qPrintable(dump(errors)));
    }
    return r;
}

Document document(const QByteArray &text) {
    const Result<Document> doc = Document::fromJson(json(text));
    if (!doc)
        qFatal("bad test document: %s", qPrintable(dump(doc.errors())));
    return *doc;
}

// prompt -> gen -> save, and a second generate fed by the same prompt.
const char *graph =
    "{'fugue': 1, 'graph': {'nodes': {"
    " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
    " 'gen': {'type': 'fugue/generate', 'version': '1.0.0',"
    "         'inputs': {'prompt': {'from': 'prompt.text'}, 'negative': {'from': 'prompt.text'}, 'steps': 8}},"
    " 'gen2': {'type': 'fugue/generate', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}}},"
    " 'save': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}},"
    " 'save2': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen2.image'}}}}},"
    " 'layout': {'nodes': {'prompt': {'pos': [0, 0]}, 'gen': {'pos': [300, 0]}, 'gen2': {'pos': [300, 200]},"
    "                      'save': {'pos': [600, 0], 'color': 'accent'}, 'save2': {'pos': [600, 200]}}}}";

// The compiled nodes with the instance's prefix taken off their path ids, so a graph and the
// same graph with some nodes in a group compare equal.
QJsonObject flat(const Document &doc, const Registry &r, const QString &prefix = {}) {
    const Compilation compiled = compile(doc, r);
    if (!compiled.ok())
        qFatal("does not compile: %s", qPrintable(dump(compiled.errors())));
    QByteArray text = QJsonDocument(compiled->toJson().value("nodes").toObject()).toJson(QJsonDocument::Compact);
    if (!prefix.isEmpty())
        text.replace(prefix.toUtf8() + '/', "");
    return QJsonDocument::fromJson(text).object();
}

} // namespace

class TestGroups : public QObject {
    Q_OBJECT

private slots:
    void makeGroupTurnsTheBoundaryIntoTheFace() {
        const Registry r = registry();
        const Document before = document(graph);
        const Result<GroupMade> made = makeGroup(before, {"gen", "gen2"}, r);
        QVERIFY2(made.ok(), qPrintable(dump(made.errors())));
        const Document &doc = made->document;
        QCOMPARE(made->group, QStringLiteral("local/group"));

        const GroupDefinition *group = doc.group(made->group);
        QVERIFY(group);
        QCOMPARE(group->name, QStringLiteral("Group"));
        QCOMPARE(group->version.toString(), QStringLiteral("1.0.0"));
        QCOMPARE(QStringList(group->nodes.keys()), (QStringList{"gen", "gen2"}));
        // One prompt output drives three text inputs: one exposed input.
        QCOMPARE(group->inputs.size(), 1);
        QCOMPARE(group->inputs[0].name, QStringLiteral("negative"));
        QCOMPARE(group->inputs[0].targets.size(), 3);
        QCOMPARE(group->outputs.size(), 2);
        QCOMPARE(group->outputs[0].from.toString(), QStringLiteral("gen.image"));
        QCOMPARE(group->outputs[1].from.toString(), QStringLiteral("gen2.image"));
        QCOMPARE(group->outputs[1].name, QStringLiteral("image_2"));

        // The instance sits where the nodes were; they keep their layout inside.
        const Node instance = doc.graph.nodes.value(made->instance);
        QCOMPARE(instance.type, made->group);
        QCOMPARE(instance.inputs.value("negative").from().toString(), QStringLiteral("prompt.text"));
        QCOMPARE(doc.graph.nodes.value("save").inputs.value("image").from().toString(),
                 made->instance + QStringLiteral(".image"));
        QCOMPARE(doc.graph.nodes.value("save2").inputs.value("image").from().toString(),
                 made->instance + QStringLiteral(".image_2"));
        QCOMPARE(dump(doc.layout.value("nodes").toObject().value(made->instance)), dump(json("{'pos': [300, 0]}")));
        QCOMPARE(dump(doc.layout.value("groups").toObject().value(made->group)),
                 dump(json("{'nodes': {'gen': {'pos': [0, 0]}, 'gen2': {'pos': [0, 200]}}}")));

        // It runs as the nodes did.
        QCOMPARE(dump(flat(doc, r, made->instance)), dump(flat(before, r)));
    }

    void outputsOfDifferentTypesAreNotMerged() {
        const Registry r = registry();
        const Document before = document(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'x'}},"
            " 'merge': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': [{'from': 'gen.image'}]}},"
            " 'gen': {'type': 'fugue/generate', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}}},"
            " 'save': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'image': {'from': 'merge.image'}}}}}}");
        // A list input fed from outside becomes an exposed list input carrying the list.
        const Result<GroupMade> made = makeGroup(before, {"merge"}, r);
        QVERIFY2(made.ok(), qPrintable(dump(made.errors())));
        QCOMPARE(made->document.graph.nodes.value(made->instance).inputs.value("images").kind(),
                 InputValue::Kind::List);
        QCOMPARE(dump(flat(made->document, r, made->instance)), dump(flat(before, r)));
    }

    void aListFedFromBothSidesCannotBeSplit() {
        const Registry r = registry();
        const Document before = document(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'a': {'type': 'fugue/generate', 'version': '1.0.0', 'inputs': {'prompt': 'x'}},"
            " 'b': {'type': 'fugue/generate', 'version': '1.0.0', 'inputs': {'prompt': 'y'}},"
            " 'merge': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': [{'from': 'a.image'}, {'from': 'b.image'}]}}}}}");
        const Result<GroupMade> made = makeGroup(before, {"a", "merge"}, r);
        QVERIFY(!made.ok());
        QCOMPARE(made.error().code, QStringLiteral("group_invalid"));
        QVERIFY(!made.error().hint.isEmpty());
        QCOMPARE(makeGroup(before, {}, r).error().code, QStringLiteral("group_invalid"));
    }

    void ungroupPutsEverythingBack() {
        const Registry r = registry();
        const Document before = document(graph);
        const Result<GroupMade> made = makeGroup(before, {"gen", "gen2"}, r);
        QVERIFY(made.ok());
        const Result<Ungrouped> back = ungroup(made->document, made->instance);
        QVERIFY2(back.ok(), qPrintable(dump(back.errors())));
        QCOMPARE(back->nodes, (QStringList{"gen", "gen2"}));
        Document doc = back->document;
        dropUnusedGroups(doc, {made->group});
        QVERIFY(doc.groups.isEmpty());
        QCOMPARE(dump(doc.toJson().value("graph")), dump(before.toJson().value("graph")));
        QCOMPARE(dump(doc.layout.value("nodes")), dump(before.layout.value("nodes")));
        QVERIFY(!doc.layout.contains("groups"));
    }

    void ungroupGivesTheFaceValuesToTheInputsTheyDrive() {
        const Registry r = registry();
        Document doc = document(graph);
        Result<GroupMade> made = makeGroup(doc, {"gen", "gen2"}, r);
        QVERIFY(made.ok());
        doc = made->document;
        // A literal on the face, a default on the group, ids that clash with the level's.
        doc.graph.nodes[made->instance].inputs.insert("negative", InputValue::literal("blurry"));
        doc = *exposeInput(doc, made->group, {"gen", "steps"}, r);
        doc.groups[made->group].inputs.last().defaultValue = InputValue::literal(20);
        doc.graph.nodes.insert("gen", Node{"fugue/text", Version{1, 0, 0}, {{"text", InputValue::literal("t")}}, {}, {}, {}, {}});

        const Result<Ungrouped> back = ungroup(doc, made->instance);
        QVERIFY2(back.ok(), qPrintable(dump(back.errors())));
        QCOMPARE(back->nodes.size(), 2);
        QVERIFY(back->nodes[0] != QStringLiteral("gen"));
        QCOMPARE(back->nodes[1], QStringLiteral("gen2"));
        const Node gen = back->document.graph.nodes.value(back->nodes[0]);
        QCOMPARE(gen.inputs.value("negative").literal().toString(), QStringLiteral("blurry"));
        QCOMPARE(gen.inputs.value("prompt").literal().toString(), QStringLiteral("blurry"));
        QCOMPARE(gen.inputs.value("steps").literal().toInt(), 20);
        QCOMPARE(back->document.graph.nodes.value("save").inputs.value("image").from().toString(),
                 back->nodes[0] + QStringLiteral(".image"));
        QCOMPARE(back->document.graph.nodes.value("gen").type, QStringLiteral("fugue/text"));
    }

    void theFaceIsTypedThroughNestedGroups() {
        const Registry r = registry();
        Result<GroupMade> inner = makeGroup(document(graph), {"gen"}, r);
        QVERIFY(inner.ok());
        Result<GroupMade> outer = makeGroup(inner->document, {inner->instance, "prompt"}, r);
        QVERIFY2(outer.ok(), qPrintable(dump(outer.errors())));
        QCOMPARE(outer->group, QStringLiteral("local/group-2"));
        Document doc = outer->document;
        // Expose the inner group's steps, then that on the outer group.
        QString name;
        doc = *exposeInput(doc, inner->group, {"gen", "steps"}, r, {}, &name);
        QCOMPARE(name, QStringLiteral("steps"));
        doc = *exposeInput(doc, outer->group, {inner->instance, "steps"}, r, {}, &name);
        doc = *editExposedInput(doc, outer->group, "steps", {"steps", "Steps", "Advanced", 2, 10, {}, {}}, r);

        const NodeType face = groupFace(doc, r, outer->group);
        QCOMPARE(face.id, outer->group);
        // The inner group's image goes to save; the prompt's text to gen2.
        QCOMPARE(face.outputs.size(), 2);
        QVERIFY(face.output("image") && face.output("text"));
        QCOMPARE(face.output("image")->type.toString(), QStringLiteral("image"));
        QCOMPARE(face.output("text")->type.toString(), QStringLiteral("text"));
        const InputSpec *steps = face.input("steps");
        QVERIFY(steps);
        QCOMPARE(steps->type.toString(), QStringLiteral("number"));
        QVERIFY(steps->integer);
        QCOMPARE(*steps->min, 2.0);
        QCOMPARE(*steps->max, 10.0);
        // The inner node's own value is what an unset face input falls back to.
        QCOMPARE(steps->defaultValue.toInt(), 8);
        QCOMPARE(steps->label, QStringLiteral("Steps"));
        QVERIFY(steps->advanced);
        QVERIFY(groupContains(doc, outer->group, inner->group));
        QVERIFY(!groupContains(doc, inner->group, outer->group));
        QVERIFY(groupFace(doc, r, "local/none").id.isEmpty());
    }

    void exposingChecksTheTargetAndOneToManyChecksTheType() {
        const Registry r = registry();
        Result<GroupMade> made = makeGroup(document(graph), {"gen", "gen2"}, r);
        QVERIFY(made.ok());
        const Document doc = made->document;
        QCOMPARE(exposeInput(doc, made->group, {"gen", "nope"}, r).error().code, QStringLiteral("group_invalid"));
        QCOMPARE(exposeInput(doc, made->group, {"gen", "prompt"}, r).error().code, QStringLiteral("group_invalid"));
        QString name;
        Document one = *exposeInput(doc, made->group, {"gen", "seed"}, r, {}, &name);
        QCOMPARE(name, QStringLiteral("seed"));
        // One seed for both: same type.
        one = *exposeInput(one, made->group, {"gen2", "seed"}, r, "seed");
        QCOMPARE(one.group(made->group)->inputs.last().targets.size(), 2);
        // A text input cannot join a number input.
        const Result<Document> wrong = exposeInput(one, made->group, {"gen2", "negative"}, r, "seed");
        QCOMPARE(wrong.error().code, QStringLiteral("type_mismatch"));
    }

    void faceEditsReachEveryInstance() {
        const Registry r = registry();
        Result<GroupMade> made = makeGroup(document(graph), {"gen"}, r);
        QVERIFY(made.ok());
        Document doc = made->document;
        // A second instance, inside another group.
        Result<GroupMade> holder = makeGroup(doc, {"save2"}, r);
        doc = holder->document;
        doc.groups[holder->group].nodes.insert(
            "copy", Node{made->group, Version{1, 0, 0}, {{"negative", InputValue::literal("x")}}, {}, {}, {}, {}});
        doc.groups[holder->group].nodes["save2"].inputs.insert("image", InputValue::link({"copy", "image"}));

        doc = *editExposedInput(doc, made->group, "negative", {"words", {}, {}, {}, {}, {}, {}}, r);
        QCOMPARE(doc.graph.nodes.value(made->instance).inputs.value("words").from().toString(), QStringLiteral("prompt.text"));
        QCOMPARE(doc.group(holder->group)->nodes.value("copy").inputs.value("words").literal().toString(), QStringLiteral("x"));
        QCOMPARE(editExposedInput(doc, made->group, "words", {"Bad name", {}, {}, {}, {}, {}, {}}, r).error().code,
                 QStringLiteral("group_invalid"));

        doc = *renameExposedOutput(doc, made->group, "image", "picture");
        QCOMPARE(doc.graph.nodes.value("save").inputs.value("image").from().toString(), made->instance + ".picture");
        QCOMPARE(doc.group(holder->group)->nodes.value("save2").inputs.value("image").from().toString(),
                 QStringLiteral("copy.picture"));

        doc = *unexposeOutput(doc, made->group, "picture");
        QVERIFY(!doc.graph.nodes.value("save").inputs.contains("image"));
        QVERIFY(!doc.group(holder->group)->nodes.value("save2").inputs.contains("image"));

        // The last target of an exposed input goes: so does every instance's value for it.
        const QList<PortRef> targets = doc.group(made->group)->inputs[0].targets;
        for (const PortRef &target : targets)
            doc = *unexposeInput(doc, made->group, target);
        QVERIFY(doc.group(made->group)->inputs.isEmpty());
        QVERIFY(!doc.graph.nodes.value(made->instance).inputs.contains("words"));
        QVERIFY(!doc.group(holder->group)->nodes.value("copy").inputs.contains("words"));
    }

    void reorderAndPrune() {
        const Registry r = registry();
        Result<GroupMade> made = makeGroup(document(graph), {"gen", "gen2"}, r);
        Document doc = *exposeInput(made->document, made->group, {"gen", "seed"}, r);
        doc = *exposeInput(doc, made->group, {"gen2", "steps"}, r);
        doc = *moveExposed(doc, made->group, false, "steps", 0);
        QCOMPARE(doc.group(made->group)->inputs[0].name, QStringLiteral("steps"));
        doc = *moveExposed(doc, made->group, true, "image_2", 0);
        QCOMPARE(doc.group(made->group)->outputs[0].name, QStringLiteral("image_2"));

        // gen2 is deleted inside the group: what named it goes.
        doc.groups[made->group].nodes.remove("gen2");
        doc.graph.nodes[made->instance].inputs.insert("steps", InputValue::literal(3));
        QVERIFY(pruneGroup(doc, made->group, r));
        const GroupDefinition *group = doc.group(made->group);
        QCOMPARE(group->outputs.size(), 1);
        QCOMPARE(group->inputs.size(), 2);
        QCOMPARE(group->inputs[0].targets.size(), 2); // negative: gen.prompt, gen.negative
        QVERIFY(!doc.graph.nodes.value(made->instance).inputs.contains("steps"));
        QVERIFY(!doc.graph.nodes.value("save2").inputs.contains("image"));
        QVERIFY(!pruneGroup(doc, made->group, r));
    }

    void makeUniqueCopiesTheDefinition() {
        const Registry r = registry();
        Result<GroupMade> made = makeGroup(document(graph), {"gen"}, r);
        Document doc = made->document;
        const Result<GroupMade> unique = makeUnique(doc, made->instance, r);
        QVERIFY(unique.ok());
        QCOMPARE(unique->group, QStringLiteral("local/group-2"));
        QCOMPARE(unique->document.graph.nodes.value(made->instance).type, unique->group);
        QVERIFY(unique->document.group(made->group));
        QCOMPARE(dump(unique->document.layout.value("groups").toObject().value(unique->group)),
                 dump(doc.layout.value("groups").toObject().value(made->group)));
        QCOMPARE(makeUnique(doc, "prompt", r).error().code, QStringLiteral("group_invalid"));
    }

    void sinksAreFoundAndIdsAreFree() {
        const Registry r = registry();
        const Document doc = document(graph);
        QCOMPARE(sinksAmong(doc, r, {"gen", "save", "save2"}), (QStringList{"save", "save2"}));
        QCOMPARE(uniqueGroupId(doc, r, "Two Pass!"), QStringLiteral("local/two-pass"));
        QCOMPARE(uniqueGroupId(doc, r, "9"), QStringLiteral("local/group"));
    }

    // Done when (#26): nested groups save, load, flatten and run.
    void nestedGroupsSaveLoadFlattenAndRun() {
        fugue::test::World world;
        QStringList saved;
        world.kernel("fugue/save", [&saved](const NodeContext &context) -> Result<Values> {
            saved << context.input("value").text();
            return Values{};
        });
        const Document before = document(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'a': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'fox'}},"
            " 'b': {'type': 'fugue/join', 'version': '1.0.0', 'inputs': {'a': {'from': 'a.text'}, 'b': ' in snow'}},"
            " 'c': {'type': 'fugue/join', 'version': '1.0.0', 'inputs': {'a': {'from': 'b.text'}, 'b': '!'}},"
            " 's': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'value': {'from': 'c.text'}}}}}}");
        Result<GroupMade> inner = makeGroup(before, {"b"}, world.registry);
        QVERIFY(inner.ok());
        Result<GroupMade> outer = makeGroup(inner->document, {inner->instance, "c"}, world.registry);
        QVERIFY2(outer.ok(), qPrintable(dump(outer.errors())));

        const Result<Document> loaded = Document::load(outer->document.save());
        QVERIFY2(loaded.ok(), qPrintable(dump(loaded.errors())));
        QCOMPARE(loaded->save(), outer->document.save());
        QCOMPARE(loaded->groups.size(), 2);

        const Compilation compiled = compile(*loaded, world.registry);
        QVERIFY2(compiled.ok(), qPrintable(dump(compiled.errors())));
        QVERIFY(compiled->nodes.contains(outer->instance + "/" + inner->instance + "/b"));
        QVERIFY(compiled->nodes.contains(outer->instance + "/c"));
        const fugue::test::Ran ran = world.run(*compiled);
        QVERIFY2(ran.outcome.errors.isEmpty(), qPrintable(dump(ran.outcome.errors)));
        QCOMPARE(saved, QStringList{"fox in snow!"});
    }
};

QTEST_GUILESS_MAIN(TestGroups)
#include "test_groups.moc"
