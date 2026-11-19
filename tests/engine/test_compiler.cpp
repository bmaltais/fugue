// The compiler (spec 4.8, 9.1): a Document and a Registry of fake node types
// in, a flat CompiledGraph and structured errors (13.3) out.
//
// Seams: compile() with its CompileOptions, observed through
// CompiledGraph::toJson(), the dependency accessors and the errors; and
// CompiledGraph::fromJson() for reading compiled.json back.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

// JSON written with single quotes, so fixtures stay readable without raw
// string literals (which moc skips). No fixture text contains an apostrophe.
QJsonObject json(const char *text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s at %d in %s", qPrintable(error.errorString()), error.offset, text);
    return document.object();
}

QString dump(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"v", value}}).toJson(QJsonDocument::Compact));
}

QString dump(const Errors &errors) {
    return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson(QJsonDocument::Compact));
}

// The fake node types every test uses.
Registry registry() {
    Registry r;
    const char *manifests[] = {
        "{'manifest': 1, 'id': 'fugue/text', 'version': '1.0.0', 'name': 'Text',"
        " 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text'}],"
        " 'outputs': [{'name': 'text', 'type': 'text'}]}",

        "{'manifest': 1, 'id': 'fugue/generate-image', 'version': '1.0.0', 'name': 'Generate image',"
        " 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'prompt', 'type': 'text'},"
        "            {'name': 'image', 'type': 'image', 'optional': true},"
        "            {'name': 'strength', 'type': 'number', 'default': 1, 'min': 0, 'max': 1},"
        "            {'name': 'seed', 'type': 'number', 'integer': true, 'widget': 'seed', 'optional': true},"
        "            {'name': 'steps', 'type': 'number', 'integer': true, 'default': 4, 'min': 1, 'max': 50},"
        "            {'name': 'width', 'type': 'number', 'integer': true, 'default': 1024},"
        "            {'name': 'height', 'type': 'number', 'integer': true, 'default': 1024}],"
        " 'outputs': [{'name': 'image', 'type': 'image'}, {'name': 'seed', 'type': 'number'}]}",

        "{'manifest': 1, 'id': 'fugue/save-image', 'version': '1.0.0', 'name': 'Save image', 'sink': true,"
        " 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'image', 'type': 'image'}, {'name': 'name', 'type': 'text', 'default': 'out'}],"
        " 'outputs': []}",

        "{'manifest': 1, 'id': 'fugue/merge', 'version': '1.0.0', 'name': 'Merge',"
        " 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'images', 'type': 'image', 'list': true, 'min_items': 1, 'max_items': 3}],"
        " 'outputs': [{'name': 'image', 'type': 'image'}]}",

        "{'manifest': 1, 'id': 'fugue/batch', 'version': '1.0.0', 'name': 'Batch',"
        " 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'prompt', 'type': 'text'}],"
        " 'outputs': [{'name': 'images', 'type': 'image', 'list': true}]}",

        "{'manifest': 1, 'id': 'io.example.tools/shrink', 'version': '1.2.0', 'name': 'Shrink',"
        " 'aliases': ['io.example.tools/resize'], 'runtime': {'kind': 'command', 'entry': './shrink'},"
        " 'inputs': [{'name': 'image', 'type': 'image'},"
        "            {'name': 'method', 'type': 'text', 'default': 'bicubic',"
        "             'choices': ['nearest', 'bilinear', 'bicubic', 'lanczos', 'area']},"
        "            {'name': 'factor', 'type': 'number', 'default': 2, 'min': 1, 'max': 8}],"
        " 'outputs': [{'name': 'image', 'type': 'image'}]}",
    };
    for (const char *manifest : manifests) {
        const Errors errors = r.addManifest(json(manifest));
        if (hasErrors(errors))
            qFatal("bad fake manifest: %s", qPrintable(dump(errors)));
    }
    return r;
}

Document document(const QByteArray &text) {
    Errors errors;
    const std::optional<Document> doc = Document::fromJson(json(text.constData()), errors);
    if (!doc)
        qFatal("bad test document: %s", qPrintable(dump(errors)));
    return *doc;
}

// The 4.2 example.
const char *foxGraph =
    "{'fugue': 1, 'graph': {'nodes': {"
    " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a red fox in fresh snow'}},"
    " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0',"
    "         'inputs': {'prompt': {'from': 'prompt.text'}, 'width': 1024, 'height': 1024,"
    "                    'seed': {'auto': 'random'}}},"
    " 'save': {'type': 'fugue/save-image', 'version': '1.0.0',"
    "          'inputs': {'image': {'from': 'gen.image'}, 'name': 'fox'}}}}}";


QJsonObject nodesOf(const Compilation &result) {
    return result.graph ? result.graph->toJson().value("nodes").toObject() : QJsonObject();
}

QStringList codes(const Errors &errors) {
    QStringList list;
    for (const Error &error : errors)
        list << error.code;
    return list;
}

// The 4.4 example group, used as instance g7 in the 4.8 example.
const char *twoPassGroup =
    "'local/two-pass': {'version': '1.0.0', 'name': 'Two-pass generate',"
    " 'inputs': [{'name': 'prompt', 'targets': ['first.prompt', 'second.prompt']},"
    "            {'name': 'seed', 'targets': ['first.seed'], 'label': 'Seed'},"
    "            {'name': 'steps', 'targets': ['second.steps'], 'section': 'Advanced', 'min': 1, 'max': 12}],"
    " 'outputs': [{'name': 'image', 'from': 'second.image'}],"
    " 'nodes': {"
    "  'first': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {}},"
    "  'second': {'type': 'fugue/generate-image', 'version': '1.0.0',"
    "             'inputs': {'image': {'from': 'first.image'}, 'strength': 0.4}}}}";

QByteArray withGroups(const char *graph, const char *groups) {
    return QByteArray("{'fugue': 1, 'graph': {'nodes': {") + graph + "}}, 'groups': {" + groups + "}}";
}

// Two levels: local/outer holds a generate node and an instance of local/inner.
const char *nestedGroups =
    "'local/inner': {'version': '1.0.0', 'name': 'Inner',"
    " 'inputs': [{'name': 'image', 'targets': ['s2.image']}, {'name': 'method', 'targets': ['s2.method']}],"
    " 'outputs': [{'name': 'image', 'from': 's2.image'}],"
    " 'nodes': {'s2': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'factor': 4}}}},"
    "'local/outer': {'version': '1.0.0', 'name': 'Outer',"
    " 'inputs': [{'name': 'prompt', 'targets': ['gen.prompt']}, {'name': 'method', 'targets': ['inner3.method']}],"
    " 'outputs': [{'name': 'image', 'from': 'inner3.image'}],"
    " 'nodes': {"
    "  'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'steps': 8}},"
    "  'inner3': {'type': 'local/inner', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}}}}";

const char *nestedGraph =
    "'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
    "'g7': {'type': 'local/outer', 'version': '1.0.0',"
    "       'inputs': {'prompt': {'from': 'prompt.text'}, 'method': 'lanczos'}},"
    "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}}";

} // namespace

class TestCompiler : public QObject {
    Q_OBJECT

private slots:
    void flatGraphCompilesToTheCompiledForm();
    void groupsFlattenToPathIds();
    void nestedGroupsFlattenThroughEveryLevel();
    void valuesAreCheckedOnTheInnerNodeTheyReach();
    void unknownTypesAreReportedWithoutFollowOnErrors();
    void invalidLinksAreAllReported();
    void groupFaceProblemsAreReportedOnTheInstance();
    void missingInputsNameTheExposedInputToSet();
    void majorVersionMismatchNeedsReview();
    void olderMinorVersionWarnsAndDropsUnknownInputs();
    void aliasesCompileToTheInstalledType();
    void groupRecursionIsReportedAndDoesNotHang();
    void cyclesAreReported();
    void everyKindOfErrorIsReportedTogether();
    void exposedInputsNarrowConstraintsAndSupplyDefaults();
    void bypassCarriesALiteralThrough();
    void aLiteralCarriedThroughABypassIsCheckedAgainstTheInputItReaches();
    void bypassRewiresLinksToTheFirstInputOfTheSameType();
    void bypassWithNoMatchingInputEmptiesTheOutput();
    void bypassedGroupPassesItsExposedInputThrough();
    void mutedNodesAndTheirDependentsAreSkipped();
    void mutedGroupSkipsEveryInnerNode();
    void nodesNoSinkNeedsAreSkippedWithAWarning();
    void sinksInsideGroupsAreDropped();
    void runToHereKeepsOnlyWhatTheTargetNeeds();
    void unknownTargetIsAUsageError();
    void providerPinsAreResolvedAndChecked();
    void listInputsTakeItemsAndCheckTheirCount();
    void compiledGraphRoundTripsThroughJson();
    void compiledGraphJsonIsChecked();
};

void TestCompiler::flatGraphCompilesToTheCompiledForm() {
    const Document doc = document(foxGraph);
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QVERIFY2(result.errors.isEmpty(), qPrintable(dump(result.errors)));

    const QJsonObject expected = json(
        "{'fugue-compiled': 1, 'nodes': {"
        " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a red fox in fresh snow'}},"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0',"
        "         'inputs': {'prompt': {'from': 'prompt.text'}, 'width': 1024, 'height': 1024,"
        "                    'seed': {'auto': 'random'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'sink': true,"
        "          'inputs': {'image': {'from': 'gen.image'}, 'name': 'fox'}}},"
        " 'skipped': {}}");
    QJsonObject actual = result.graph->toJson();
    QCOMPARE(actual.take("source").toString(), doc.sourceHash());
    QCOMPARE(dump(actual), dump(expected));
}

// The 4.8 example: instance g7 of the 4.4 group.
void TestCompiler::groupsFlattenToPathIds() {
    const Document doc = document(withGroups(
        "'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
        "'g7': {'type': 'local/two-pass', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}}",
        twoPassGroup));
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(dump(nodesOf(result)),
             dump(json("{"
                       " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
                       " 'g7/first': {'type': 'fugue/generate-image', 'version': '1.0.0',"
                       "              'inputs': {'prompt': {'from': 'prompt.text'}}},"
                       " 'g7/second': {'type': 'fugue/generate-image', 'version': '1.0.0',"
                       "               'inputs': {'prompt': {'from': 'prompt.text'},"
                       "                          'image': {'from': 'g7/first.image'}, 'strength': 0.4}},"
                       " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'sink': true,"
                       "          'inputs': {'image': {'from': 'g7/second.image'}}}}")));
}

void TestCompiler::nestedGroupsFlattenThroughEveryLevel() {
    const Compilation result = compile(document(withGroups(nestedGraph, nestedGroups)), registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(dump(nodesOf(result)),
             dump(json("{"
                       " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
                       " 'g7/gen': {'type': 'fugue/generate-image', 'version': '1.0.0',"
                       "            'inputs': {'prompt': {'from': 'prompt.text'}, 'steps': 8}},"
                       " 'g7/inner3/s2': {'type': 'io.example.tools/shrink', 'version': '1.2.0',"
                       "                  'inputs': {'image': {'from': 'g7/gen.image'}, 'method': 'lanczos',"
                       "                             'factor': 4}},"
                       " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'sink': true,"
                       "          'inputs': {'image': {'from': 'g7/inner3/s2.image'}}}}")));

    // What the scheduler reads.
    const CompiledGraph &graph = *result.graph;
    QCOMPARE(graph.sinks(), QStringList{"save"});
    QCOMPARE(graph.dependencies("g7/inner3/s2"), QStringList{"g7/gen"});
    QCOMPARE(graph.consumers("g7/gen").size(), 1);
    QCOMPARE(graph.consumers("g7/gen").first().node, QString("g7/inner3/s2"));
    QVERIFY(graph.consumers("g7/gen").first().link == (CompiledLink{"image", -1, PortRef{"g7/gen", "image"}}));
    QCOMPARE(graph.order(), (QStringList{"prompt", "g7/gen", "g7/inner3/s2", "save"}));
}

// 13.3's example: a value given on the outer group's face is reported on the
// inner node it reaches, by path id, with every valid option.
void TestCompiler::valuesAreCheckedOnTheInnerNodeTheyReach() {
    QByteArray graph(nestedGraph);
    graph.replace("'method': 'lanczos'", "'method': 'lancos'");
    const Compilation result = compile(document(withGroups(graph, nestedGroups)), registry());
    QVERIFY(!result.ok());
    QCOMPARE(result.errors.size(), 1);
    const Error &error = result.errors.first();
    QCOMPARE(error.code, QString("value_not_in_choices"));
    QCOMPARE(error.node, QString("g7/inner3/s2"));
    QCOMPARE(error.input, QString("method"));
    QCOMPARE(error.valid, (QJsonArray{"nearest", "bilinear", "bicubic", "lanczos", "area"}));
    QCOMPARE(error.hint, QString("Did you mean \"lanczos\"?"));
}

void TestCompiler::unknownTypesAreReportedWithoutFollowOnErrors() {
    const Document doc = document(withGroups(
        "'gen': {'type': 'fugue/generate-imag', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}},"
        "'g7': {'type': 'local/box', 'version': '1.0.0', 'inputs': {}},"
        "'save2': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}}",
        "'local/box': {'version': '1.0.0', 'name': 'Box', 'inputs': [],"
        " 'outputs': [{'name': 'image', 'from': 'x.image'}],"
        " 'nodes': {'x': {'type': 'io.example.gone/thing', 'version': '1.0.0', 'inputs': {}}}}"));
    const Compilation result = compile(doc, registry());
    QVERIFY(!result.ok());
    QCOMPARE(codes(result.errors), (QStringList{"unknown_node_type", "unknown_node_type"}));

    const Error &inner = result.errors[0];
    QCOMPARE(inner.node, QString("g7/x"));
    QVERIFY(inner.message.contains("io.example.gone/thing"));
    const Error &top = result.errors[1];
    QCOMPARE(top.node, QString("gen"));
    QCOMPARE(top.hint, QString("Did you mean \"fugue/generate-image\"?"));
    // Every known type, in full: the registry's and the workflow's groups.
    QCOMPARE(top.valid, (QJsonArray{"fugue/batch", "fugue/generate-image", "fugue/merge", "fugue/save-image",
                                    "fugue/text", "io.example.tools/shrink", "local/box"}));
}

void TestCompiler::invalidLinksAreAllReported() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
        " 'batch': {'type': 'fugue/batch', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}}},"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0',"
        "         'inputs': {'prompt': {'from': 'promt.text'}, 'image': {'from': 'prompt.text'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.imag'}}},"
        " 'many': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'batch.images'}}},"
        " 'merge': {'type': 'fugue/merge', 'version': '1.0.0',"
        "           'inputs': {'images': [{'from': 'batch.images'}, {'from': 'gen.image'}]}},"
        " 'out': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'merge.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY(!result.ok());
    QCOMPARE(result.errors.size(), 4);

    // In path id order of the node the link is written on.
    const Error &source = result.errors[0];
    QCOMPARE(source.code, QString("unknown_link_source"));
    QCOMPARE(source.node, QString("gen"));
    QCOMPARE(source.input, QString("prompt"));
    QCOMPARE(source.valid, (QJsonArray{"batch", "gen", "many", "merge", "out", "prompt", "save"}));
    QCOMPARE(source.hint, QString("Did you mean \"prompt\"?"));

    const Error &mismatch = result.errors[1];
    QCOMPARE(mismatch.code, QString("type_mismatch"));
    QCOMPARE(mismatch.node, QString("gen"));
    QCOMPARE(mismatch.input, QString("image"));

    const Error &list = result.errors[2];
    QCOMPARE(list.code, QString("type_mismatch"));
    QCOMPARE(list.node, QString("many"));
    QVERIFY2(list.message.contains("list"), qPrintable(list.message));

    const Error &output = result.errors[3];
    QCOMPARE(output.code, QString("unknown_output"));
    QCOMPARE(output.node, QString("save"));
    QCOMPARE(output.input, QString("image"));
    QCOMPARE(output.valid, (QJsonArray{"image", "seed"}));
    QCOMPARE(output.hint, QString("Did you mean \"image\"?"));
}

void TestCompiler::groupFaceProblemsAreReportedOnTheInstance() {
    const Document doc = document(withGroups(
        "'g7': {'type': 'local/two-pass', 'version': '1.0.0',"
        "       'inputs': {'prompt': {'from': 'nowhere.text'}, 'sed': 3}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.picture'}}},"
        "'b': {'type': 'local/broken', 'version': '1.0.0', 'inputs': {}}",
        QByteArray(twoPassGroup) +
            ",'local/broken': {'version': '1.0.0', 'name': 'Broken',"
            " 'inputs': [{'name': 'what', 'targets': ['ghost.text']},"
            "            {'name': 'mixed', 'targets': ['t.text', 'g.steps']},"
            "            {'name': 'typo', 'targets': ['t.txt'], 'default': 'x'}],"
            " 'outputs': [{'name': 'image', 'from': 'g.picture'}],"
            " 'nodes': {'t': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'x'}},"
            "           'g': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': {'from': 't.text'}}}}}"));
    const Compilation result = compile(doc, registry());
    QVERIFY(!result.ok());

    QStringList seen;
    for (const Error &error : result.errors)
        seen << QStringLiteral("%1 %2 %3%4").arg(error.code, error.node, error.input, error.output);
    // The bad outer link reaches two inner nodes but is reported once, where it is written.
    QCOMPARE(seen, (QStringList{
                       "unknown_input b what",          // target names no inner node
                       "type_mismatch b mixed",         // targets of different types
                       "unknown_input b typo",          // once: not again on b/t for its default
                       "unknown_output b image",        // the group output leads nowhere
                       "unknown_input g7 sed",          // not an exposed input
                       "unknown_link_source g7 prompt", // written on g7, reported once
                       "unknown_output save image",     // g7 has no output picture
                   }));
    QCOMPARE(result.errors[4].valid, (QJsonArray{"prompt", "seed", "steps"}));
    QCOMPARE(result.errors[4].hint, QString("Did you mean \"seed\"?"));
    QCOMPARE(result.errors[6].valid, QJsonArray{"image"});
}

void TestCompiler::missingInputsNameTheExposedInputToSet() {
    const Document doc = document(withGroups(
        "'g7': {'type': 'local/two-pass', 'version': '1.0.0', 'inputs': {}},"
        "'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}},"
        "'save2': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}}",
        twoPassGroup));
    const Compilation result = compile(doc, registry());
    QStringList seen;
    for (const Error &error : result.errors)
        seen << QStringLiteral("%1 %2 %3").arg(error.code, error.node, error.input);
    QCOMPARE(seen, (QStringList{"missing_input g7/first prompt", "missing_input g7/second prompt",
                                "missing_input gen prompt"}));
    QCOMPARE(result.errors[0].hint, QString("Set exposed input prompt on g7."));
}

void TestCompiler::majorVersionMismatchNeedsReview() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'old': {'type': 'io.example.tools/shrink', 'version': '0.9.0', 'inputs': {'image': {'from': 'x.y'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'old.renamed'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QCOMPARE(result.errors.size(), 1); // nothing about its inputs or outputs, which may have changed
    const Error &error = result.errors.first();
    QCOMPARE(error.code, QString("node_version_mismatch"));
    QCOMPARE(error.node, QString("old"));
    QCOMPARE(error.valid, QJsonArray{"1.2.0"});
    QVERIFY2(error.hint.contains("1.2.0"), qPrintable(error.hint));
}

// 3.6: a workflow made with a newer minor version loads; inputs the installed
// version lacks are flagged and not used.
void TestCompiler::olderMinorVersionWarnsAndDropsUnknownInputs() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'shrink': {'type': 'io.example.tools/shrink', 'version': '1.4.0',"
        "            'inputs': {'image': {'from': 'gen.image'}, 'sharpen': true}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'shrink.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.errors.size(), 1);
    const Error &warning = result.errors.first();
    QCOMPARE(warning.severity, Error::Severity::Warning);
    QCOMPARE(warning.code, QString("unknown_input"));
    QCOMPARE(warning.node, QString("shrink"));
    QCOMPARE(warning.input, QString("sharpen"));
    const QJsonObject shrink = nodesOf(result).value("shrink").toObject();
    QCOMPARE(shrink.value("version").toString(), QString("1.2.0"));
    QVERIFY(!shrink.value("inputs").toObject().contains("sharpen"));
}

void TestCompiler::aliasesCompileToTheInstalledType() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'resize': {'type': 'io.example.tools/resize', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'resize.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QVERIFY2(result.errors.isEmpty(), qPrintable(dump(result.errors)));
    const QJsonObject resize = nodesOf(result).value("resize").toObject();
    QCOMPARE(resize.value("type").toString(), QString("io.example.tools/shrink"));
    QCOMPARE(resize.value("version").toString(), QString("1.2.0"));
}

void TestCompiler::groupRecursionIsReportedAndDoesNotHang() {
    const Document doc = document(withGroups(
        "'a': {'type': 'local/self', 'version': '1.0.0', 'inputs': {}},"
        "'b': {'type': 'local/ping', 'version': '1.0.0', 'inputs': {}},"
        "'c': {'type': 'local/holder', 'version': '1.0.0', 'inputs': {}}",
        "'local/self': {'version': '1.0.0', 'name': 'Self', 'inputs': [], 'outputs': [],"
        " 'nodes': {'me': {'type': 'local/self', 'version': '1.0.0', 'inputs': {}}}},"
        "'local/ping': {'version': '1.0.0', 'name': 'Ping', 'inputs': [], 'outputs': [],"
        " 'nodes': {'p': {'type': 'local/pong', 'version': '1.0.0', 'inputs': {}}}},"
        "'local/pong': {'version': '1.0.0', 'name': 'Pong', 'inputs': [], 'outputs': [],"
        " 'nodes': {'p': {'type': 'local/ping', 'version': '1.0.0', 'inputs': {}}}},"
        "'local/holder': {'version': '1.0.0', 'name': 'Holder', 'inputs': [], 'outputs': [],"
        " 'nodes': {'h': {'type': 'local/ping', 'version': '1.0.0', 'inputs': {}}}}"));
    const Compilation result = compile(doc, registry());
    QCOMPARE(codes(result.errors), (QStringList{"group_recursion", "group_recursion"}));
    QCOMPARE(result.errors[0].valid, (QJsonArray{"local/ping", "local/pong"}));
    QCOMPARE(result.errors[1].valid, QJsonArray{"local/self"});
}

void TestCompiler::cyclesAreReported() {
    const Document doc = document(withGroups(
        "'a': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'b.image'}}},"
        "'b': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'a.image'}}},"
        "'self': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'self.image'}}},"
        "'g': {'type': 'local/pass', 'version': '1.0.0', 'inputs': {'image': {'from': 'g.image'}}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'a.image'}}}",
        "'local/pass': {'version': '1.0.0', 'name': 'Pass', 'inputs': [{'name': 'image', 'targets': ['s.image']}],"
        " 'outputs': [{'name': 'image', 'from': 's.image'}],"
        " 'nodes': {'s': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {}}}}"));
    const Compilation result = compile(doc, registry());
    QVERIFY(!result.ok());
    QStringList seen;
    for (const Error &error : result.errors)
        seen << QStringLiteral("%1 %2").arg(error.code, dump(error.valid));
    QCOMPARE(seen, (QStringList{QString("cycle ") + dump(QJsonArray{"a", "b"}),
                                QString("cycle ") + dump(QJsonArray{"g/s"}),
                                QString("cycle ") + dump(QJsonArray{"self"})}));
}

void TestCompiler::everyKindOfErrorIsReportedTogether() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'a': {'type': 'io.example.tools/shrink', 'version': '1.2.0',"
        "       'inputs': {'image': {'from': 'b.image'}, 'method': 'cubic'}},"
        " 'b': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'a.image'}}},"
        " 'c': {'type': 'io.example.gone/thing', 'version': '1.0.0', 'inputs': {}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'c.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY(!result.ok());
    QCOMPARE(codes(result.errors), (QStringList{"unknown_node_type", "value_not_in_choices", "cycle"}));
}

void TestCompiler::exposedInputsNarrowConstraintsAndSupplyDefaults() {
    QByteArray groups(twoPassGroup);
    groups.replace("{'name': 'seed', 'targets': ['first.seed'], 'label': 'Seed'}",
                   "{'name': 'seed', 'targets': ['first.seed'], 'label': 'Seed', 'default': 7}");
    const char *graph =
        "'g7': {'type': 'local/two-pass', 'version': '1.0.0', 'inputs': {'prompt': 'a fox', 'steps': 20}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}}";
    Compilation result = compile(document(withGroups(graph, groups.constData())), registry());
    // fugue/generate-image allows 50 steps; the group narrows it to 12.
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().code, QString("value_out_of_range"));
    QCOMPARE(result.errors.first().node, QString("g7/second"));
    QCOMPARE(result.errors.first().input, QString("steps"));

    QByteArray fixed(graph);
    fixed.replace("'steps': 20", "'steps': 12");
    result = compile(document(withGroups(fixed.constData(), groups.constData())), registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    const QJsonObject first = nodesOf(result).value("g7/first").toObject().value("inputs").toObject();
    QCOMPARE(first.value("seed").toInt(), 7); // the group's default
    QCOMPARE(first.value("prompt").toString(), QString("a fox"));
    QCOMPARE(nodesOf(result).value("g7/second").toObject().value("inputs").toObject().value("steps").toInt(), 12);
}

void TestCompiler::bypassCarriesALiteralThrough() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'mode': 'bypass', 'inputs': {'text': 'a fox'}},"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(dump(nodesOf(result).value("gen").toObject().value("inputs")), dump(json("{'prompt': 'a fox'}")));
}

// A link is only type-checked (text into text); once a bypass turns it into the literal
// behind it, that literal must fit the limits of the input it now sits in (9.1).
void TestCompiler::aLiteralCarriedThroughABypassIsCheckedAgainstTheInputItReaches() {
    const char *graph =
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'how': {'type': 'fugue/text', 'version': '1.0.0', 'mode': 'bypass', 'inputs': {'text': '%1'}},"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'small': {'type': 'io.example.tools/shrink', 'version': '1.2.0',"
        "           'inputs': {'image': {'from': 'gen.image'}, 'method': {'from': 'how.text'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'small.image'}}}}}}";
    const Compilation bad = compile(document(QString::fromLatin1(graph).arg("cubic").toUtf8()), registry());
    QVERIFY(!bad.ok());
    QCOMPARE(bad.errors.size(), 1);
    const Error error = bad.errors.first();
    QCOMPARE(error.code, QStringLiteral("value_not_in_choices"));
    QCOMPARE(error.node, QStringLiteral("small"));
    QCOMPARE(error.input, QStringLiteral("method"));
    QVERIFY(error.valid.contains("lanczos"));
    QVERIFY2(error.message.contains("how"), qPrintable(error.message)); // where the value comes from

    const Compilation good = compile(document(QString::fromLatin1(graph).arg("lanczos").toUtf8()), registry());
    QVERIFY2(good.ok(), qPrintable(dump(good.errors)));
    QCOMPARE(good.graph->nodes.value("small").inputs.value("method").literal().toString(), QStringLiteral("lanczos"));
}

void TestCompiler::bypassRewiresLinksToTheFirstInputOfTheSameType() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'a': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'mode': 'bypass',"
        "       'inputs': {'image': {'from': 'gen.image'}}},"
        " 'b': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'mode': 'bypass',"
        "       'inputs': {'image': {'from': 'a.image'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'b.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QVERIFY2(result.errors.isEmpty(), qPrintable(dump(result.errors)));
    const QJsonObject nodes = nodesOf(result);
    QCOMPARE(nodes.keys(), (QStringList{"gen", "save"}));
    QCOMPARE(dump(nodes.value("save").toObject().value("inputs")), dump(json("{'image': {'from': 'gen.image'}}")));
    QVERIFY(result.graph->skipped.isEmpty()); // bypassed nodes are not listed
}

void TestCompiler::bypassWithNoMatchingInputEmptiesTheOutput() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'mode': 'bypass', 'inputs': {'prompt': 'a fox'}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}},"
        " 'after': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'gen.image'}}},"
        " 'save2': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'after.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    // gen's first image input, `image`, is unset and has no default: its image output is empty.
    QVERIFY(nodesOf(result).isEmpty());
    QCOMPARE(result.graph->skipped,
             (QMap<QString, QString>{{"after", "upstream_empty"}, {"save", "upstream_empty"}, {"save2", "upstream_empty"}}));
}

void TestCompiler::bypassedGroupPassesItsExposedInputThrough() {
    QByteArray graph(nestedGraph);
    graph.replace("'type': 'local/outer', 'version': '1.0.0',", "'type': 'local/outer', 'version': '1.0.0', 'mode': 'bypass',");
    // local/outer exposes no image input, so a bypassed instance's image output is empty...
    Compilation result = compile(document(withGroups(graph, nestedGroups)), registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->skipped.value("save"), QString("upstream_empty"));
    QVERIFY(!result.graph->nodes.contains("g7/gen")); // nothing inside a bypassed group runs

    // ...while a bypassed local/inner hands its exposed image input straight on.
    const Document inner = document(withGroups(
        "'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        "'i': {'type': 'local/inner', 'version': '1.0.0', 'mode': 'bypass', 'inputs': {'image': {'from': 'gen.image'}}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'i.image'}}}",
        nestedGroups));
    result = compile(inner, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->nodes.keys(), (QStringList{"gen", "save"}));
    QCOMPARE(result.graph->dependencies("save"), QStringList{"gen"});
}

void TestCompiler::mutedNodesAndTheirDependentsAreSkipped() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'old-branch': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'mode': 'mute',"
        "                'inputs': {'image': {'from': 'gen.image'}}},"
        " 'after': {'type': 'io.example.tools/shrink', 'version': '1.2.0', 'inputs': {'image': {'from': 'old-branch.image'}}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'after.image'}}},"
        " 'keep': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->nodes.keys(), (QStringList{"gen", "keep"}));
    QCOMPARE(result.graph->skipped,
             (QMap<QString, QString>{{"old-branch", "muted"}, {"after", "upstream_muted"}, {"save", "upstream_muted"}}));
    QVERIFY2(result.errors.isEmpty(), qPrintable(dump(result.errors)));
}

void TestCompiler::mutedGroupSkipsEveryInnerNode() {
    QByteArray graph(nestedGraph);
    graph.replace("'type': 'local/outer', 'version': '1.0.0',", "'type': 'local/outer', 'version': '1.0.0', 'mode': 'mute',");
    const Compilation result = compile(document(withGroups(graph, nestedGroups)), registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->skipped, (QMap<QString, QString>{{"g7/gen", "muted"},
                                                          {"g7/inner3/s2", "muted"},
                                                          {"prompt", "not_part_of_output"},
                                                          {"save", "upstream_muted"}}));
}

void TestCompiler::nodesNoSinkNeedsAreSkippedWithAWarning() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}},"
        " 'lonely': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'unused'}}"
        "}}}");
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->skipped, (QMap<QString, QString>{{"lonely", "not_part_of_output"}}));
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().code, QString("not_part_of_output"));
    QCOMPARE(result.errors.first().severity, Error::Severity::Warning);
    QCOMPARE(result.errors.first().node, QString("lonely"));
    QVERIFY(!hasErrors(result.errors));
}

void TestCompiler::sinksInsideGroupsAreDropped() {
    const Document doc = document(withGroups(
        "'g': {'type': 'local/with-save', 'version': '1.0.0', 'inputs': {}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g.image'}}}",
        "'local/with-save': {'version': '1.0.0', 'name': 'With save', 'inputs': [],"
        " 'outputs': [{'name': 'image', 'from': 'gen.image'}],"
        " 'nodes': {'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'x'}},"
        "           'inner-save': {'type': 'fugue/save-image', 'version': '1.0.0',"
        "                          'inputs': {'image': {'from': 'gen.image'}}}}}"));
    const Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->nodes.keys(), (QStringList{"g/gen", "save"}));
    QVERIFY(result.graph->skipped.isEmpty());
    QCOMPARE(result.graph->sinks(), QStringList{"save"});
}

void TestCompiler::runToHereKeepsOnlyWhatTheTargetNeeds() {
    CompileOptions options;
    options.target = "g7/gen";
    const Compilation result = compile(document(withGroups(nestedGraph, nestedGroups)), registry(), options);
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QVERIFY2(result.errors.isEmpty(), qPrintable(dump(result.errors))); // no warnings for the rest
    QCOMPARE(result.graph->nodes.keys(), (QStringList{"g7/gen", "prompt"}));
    QCOMPARE(result.graph->target, QString("g7/gen"));
    QCOMPARE(result.graph->toJson().value("target").toString(), QString("g7/gen"));
    QCOMPARE(result.graph->skipped.value("save"), QString("not_part_of_output"));
    QVERIFY(result.graph->sinks().isEmpty());
}

void TestCompiler::unknownTargetIsAUsageError() {
    CompileOptions options;
    options.target = "g7/gem";
    const Compilation result = compile(document(withGroups(nestedGraph, nestedGroups)), registry(), options);
    QVERIFY(!result.ok());
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().code, QString("usage"));
    QCOMPARE(result.errors.first().valid, (QJsonArray{"g7/gen", "g7/inner3/s2", "prompt", "save"}));
    QCOMPARE(result.errors.first().hint, QString("Did you mean \"g7/gen\"?"));
}

void TestCompiler::providerPinsAreResolvedAndChecked() {
    QByteArray text = withGroups(
        "'g7': {'type': 'local/two-pass', 'version': '1.0.0', 'provider': 'Studio', 'inputs': {'prompt': 'a fox'}},"
        "'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        "'save': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'g7.image'}}},"
        "'save2': {'type': 'fugue/save-image', 'version': '1.0.0', 'provider': 'Laptop',"
        "          'inputs': {'image': {'from': 'gen.image'}}}",
        twoPassGroup);
    text.replace("'graph': {'nodes'", "'graph': {'provider': 'This machine', 'nodes'");
    const Document doc = document(text);

    // Not checked when the caller does not say which instances exist.
    Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->nodes.value("g7/first").provider, QString("Studio")); // the instance's pin
    QCOMPARE(result.graph->nodes.value("gen").provider, QString("This machine")); // the workflow's
    QCOMPARE(result.graph->nodes.value("save2").provider, QString("Laptop"));     // its own
    QCOMPARE(nodesOf(result).value("gen").toObject().value("provider").toString(), QString("This machine"));

    CompileOptions options;
    options.provider = "Studio"; // fugue run --provider
    options.providers = QStringList{"This machine", "Studio"};
    result = compile(doc, registry(), options);
    QVERIFY(!result.ok());
    QCOMPARE(result.errors.size(), 1);
    const Error &error = result.errors.first();
    QCOMPARE(error.code, QString("unknown_provider"));
    QCOMPARE(error.node, QString("save2"));
    QCOMPARE(error.valid, (QJsonArray{"This machine", "Studio"}));

    options.providers = QStringList{"This machine", "Studio", "Laptop"};
    result = compile(doc, registry(), options);
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    QCOMPARE(result.graph->nodes.value("gen").provider, QString("Studio"));
}

void TestCompiler::listInputsTakeItemsAndCheckTheirCount() {
    const Document doc = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'batch': {'type': 'fugue/batch', 'version': '1.0.0', 'inputs': {'prompt': 'foxes'}},"
        " 'one': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': {'from': 'gen.image'}}},"
        " 'two': {'type': 'fugue/merge', 'version': '1.0.0',"
        "         'inputs': {'images': [{'from': 'gen.image'}, {'from': 'batch.images'}]}},"
        " 'save1': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'one.image'}}},"
        " 'save2': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': {'from': 'two.image'}}}"
        "}}}");
    Compilation result = compile(doc, registry());
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    const QList<CompiledLink> links = result.graph->nodes.value("two").links();
    QCOMPARE(links.size(), 2);
    QVERIFY(links[1] == (CompiledLink{"images", 1, PortRef{"batch", "images"}}));
    QCOMPARE(result.graph->dependencies("two"), (QStringList{"batch", "gen"}));

    const Document bad = document(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'gen': {'type': 'fugue/generate-image', 'version': '1.0.0', 'inputs': {'prompt': 'a fox'}},"
        " 'none': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': []}},"
        " 'many': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': ["
        "   {'from': 'gen.image'}, {'from': 'gen.image'}, {'from': 'gen.image'}, {'from': 'gen.image'}]}},"
        " 'text': {'type': 'fugue/merge', 'version': '1.0.0', 'inputs': {'images': ['a.png']}},"
        " 'single': {'type': 'fugue/save-image', 'version': '1.0.0', 'inputs': {'image': [{'from': 'gen.image'}]}}"
        "}}}");
    result = compile(bad, registry());
    QStringList seen;
    for (const Error &error : result.errors)
        seen << QStringLiteral("%1 %2 %3").arg(error.code, error.node, error.input);
    QCOMPARE(seen, (QStringList{"value_out_of_range many images", "value_out_of_range none images",
                                "type_mismatch single image", "type_mismatch text images"}));
}

void TestCompiler::compiledGraphRoundTripsThroughJson() {
    CompileOptions options;
    options.target = "g7/inner3/s2";
    const Compilation result = compile(document(withGroups(nestedGraph, nestedGroups)), registry(), options);
    QVERIFY2(result.ok(), qPrintable(dump(result.errors)));
    CompiledGraph graph = *result.graph;
    graph.nodes["g7/gen"].frozen = "sha256:00";
    graph.nodes["g7/gen"].inputs["seed"] = InputValue::autoRandom();
    graph.nodes["g7/gen"].inputs["model"] = InputValue::resource(QJsonObject{{"resource", "fugue/lora"}, {"name", "x"}});

    Errors errors;
    const std::optional<CompiledGraph> back = CompiledGraph::fromJson(graph.toJson(), errors);
    QVERIFY2(back, qPrintable(dump(errors)));
    QCOMPARE(dump(back->toJson()), dump(graph.toJson()));
    QCOMPARE(back->order(), graph.order());
}

void TestCompiler::compiledGraphJsonIsChecked() {
    Errors errors;
    QVERIFY(!CompiledGraph::fromJson(json("{'fugue-compiled': 2, 'source': 'x', 'nodes': {}}"), errors));
    QCOMPARE(codes(errors), QStringList{"unsupported_format_version"});

    errors.clear();
    QVERIFY(!CompiledGraph::fromJson(
        json("{'fugue-compiled': 1, 'source': 'x', 'nodes': {"
             " 'g7/a': {'type': 'fugue/text', 'version': '1.0', 'inputs': {'text': null}},"
             " 'B': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {}}},"
             " 'skipped': {'c': 'tired'}}"),
        errors));
    QStringList seen;
    for (const Error &error : errors)
        seen << QStringLiteral("%1 %2 %3").arg(error.code, error.node, error.input);
    QCOMPARE(seen, (QStringList{"invalid_document B ", "invalid_document g7/a ", "invalid_document g7/a text",
                                "invalid_document c "}));
}

QTEST_GUILESS_MAIN(TestCompiler)
#include "test_compiler.moc"
