// Parameter sweep (#552, spec 13.1): planSweep and composeDocument, with no window and no GPU.

#include <QtTest>

#include "engine/registry.h"
#include "engine/sweep.h"

using namespace fugue;

namespace {

QJsonObject port(const char *name, const char *type, const QJsonObject &more = {}) {
    QJsonObject object{{"name", name}, {"type", type}};
    for (auto it = more.begin(); it != more.end(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

QJsonObject manifest(const char *id, const QJsonArray &inputs, const QJsonArray &outputs, const char *version = "1.0.0") {
    return {{"manifest", 1}, {"id", id}, {"version", version}, {"name", id}, {"runtime", QJsonObject{{"kind", "builtin"}}},
            {"inputs", inputs}, {"outputs", outputs}};
}

} // namespace

class TestSweep : public QObject {
    Q_OBJECT

    Registry m_registry;
    Document m_document;

    static Result<SweepPlan> plan(const Document &document, const QStringList &axes, const Registry &registry, const QString &to = "s") {
        QList<SweepAxis> parsed;
        for (const QString &text : axes) {
            const Result<SweepAxis> axis = parseSweepAxis(text);
            if (!axis.ok())
                return axis.errors();
            parsed << axis.value();
        }
        return planSweep(document, parsed, to, registry);
    }

private slots:
    void initTestCase() {
        const Errors made = m_registry.addManifest(manifest(
            "fugue/make",
            {port("steps", "number", {{"integer", true}, {"min", 1}, {"max", 100}, {"default", 20}}),
             port("mode", "text", {{"choices", QJsonArray{"fast", "slow"}}, {"default", "fast"}}),
             port("prompt", "text", {{"default", ""}}), port("source", "image", {{"optional", true}}),
             port("many", "text", {{"list", true}, {"optional", true}})},
            {port("image", "image")}));
        QVERIFY2(!hasErrors(made), qPrintable(made.value(0).message));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest("fugue/load-image", {port("file", "text")}, {port("image", "image")}, "1.1.0"))));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest("fugue/solid-mask", {port("value", "number"), port("width", "number"), port("height", "number")},
                                                           {port("mask", "image", {{"role", "mask"}})}, "1.0.1"))));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest("fugue/preview", {port("value", "any")}, {}))));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest("fugue/loader", {port("name", "text", {{"default", ""}})}, {port("model", "text")}))));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest(
            "fugue/image-grid",
            {port("images", "image", {{"list", true}}), port("columns", "number", {{"default", 0}}),
             port("labels", "text", {{"default", ""}}), port("column_labels", "text", {{"default", ""}}),
             port("row_labels", "text", {{"default", ""}})},
            {port("image", "image")}))));
        QVERIFY(!hasErrors(m_registry.addManifest(manifest(
            "fugue/save-image", {port("image", "image"), port("name", "text", {{"default", "fugue"}}), port("folder", "text", {{"optional", true}})},
            {port("file", "image")}, "1.1.0"))));
        const Result<Document> loaded = Document::load(
            R"({"fugue": 1, "graph": {"nodes": {"s": {"type": "fugue/make", "version": "1.0.0", "inputs": {"steps": 10}}}}})");
        QVERIFY(loaded.ok());
        m_document = loaded.value();
    }

    void valuesReadAsSetReadsThem() {
        const Result<SweepAxis> axis = parseSweepAxis(R"(s.prompt="a, b",7,true,plain)");
        QVERIFY(axis.ok());
        QCOMPARE(axis->node, QString("s"));
        QCOMPARE(axis->input, QString("prompt"));
        QCOMPARE(axis->values.size(), 4);
        QCOMPARE(axis->values.at(0), QJsonValue("a, b")); // a comma inside quotes stays
        QCOMPARE(axis->values.at(1), QJsonValue(7));
        QCOMPARE(axis->values.at(2), QJsonValue(true));
        QCOMPARE(axis->values.at(3), QJsonValue("plain"));
        QCOMPARE(axis->texts, (QStringList{"a, b", "7", "true", "plain"}));
        QCOMPARE(parseSweepAxis("s.prompt= a ,b").value().values.at(0), QJsonValue(" a ")); // not trimmed, as --set does not trim
        QVERIFY(!parseSweepAxis("steps=1,2").ok());
        QCOMPARE(parseSweepAxis("s.steps=1,,2").errors().value(0).code, QString("usage"));
    }

    void oneAxisBindsEachCellAndLeavesTheDocumentAlone() {
        const Result<SweepPlan> result = plan(m_document, {"s.steps=5,15,25"}, m_registry);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        QCOMPARE(result->cells.size(), 3);
        QCOMPARE(result->cells.at(1).bound.graph.nodes.value("s").inputs.value("steps").toJson(), QJsonValue(15));
        QCOMPARE(result->cells.at(2).label, QString("steps=25"));
        QCOMPARE(m_document.graph.nodes.value("s").inputs.value("steps").toJson(), QJsonValue(10));
        QCOMPARE(result->labels.columns, 2); // one axis wraps into a near-square sheet: 3 pictures, 2 columns
        QCOMPARE(result->labels.rows, 2);
        QCOMPARE(result->labels.cells, (QStringList{"steps=5", "steps=15", "steps=25"}));
        QVERIFY(result->labels.columnLabels.isEmpty());
    }

    void twoAxesGoRowByRowWithRowAndColumnLabels() {
        const Result<SweepPlan> result = plan(m_document, {"s.steps=5,15,25", "s.mode=fast,slow"}, m_registry);
        QVERIFY(result.ok());
        QCOMPARE(result->cells.size(), 6);
        const auto &fourth = result->cells.at(3).bound.graph.nodes.value("s").inputs;
        QCOMPARE(fourth.value("steps").toJson(), QJsonValue(5)); // the second row starts again at the first column
        QCOMPARE(fourth.value("mode").toJson(), QJsonValue("slow"));
        QCOMPARE(result->labels.columns, 3);
        QCOMPARE(result->labels.columnLabels, (QStringList{"steps=5", "steps=15", "steps=25"}));
        QCOMPARE(result->labels.rowLabels, (QStringList{"mode=fast", "mode=slow"}));
        QCOMPARE(result->cells.at(3).label, QString("steps=5, mode=slow"));
    }

    void refusalsCostNothing() {
        const auto code = [&](const QStringList &axes, const QString &to = "s") {
            const Result<SweepPlan> result = plan(m_document, axes, m_registry, to);
            return result.ok() ? QString("ok") : result.errors().value(0).code;
        };
        QCOMPARE(code({"nope.steps=1,2"}), QString("usage"));
        QCOMPARE(plan(m_document, {"nope.steps=1,2"}, m_registry).errors().value(0).valid, (QJsonArray{"s"}));
        QCOMPARE(code({"s.steps=1,2"}, "gone"), QString("usage"));
        QCOMPARE(code({"s.nope=1,2"}), QString("unknown_input"));
        QCOMPARE(code({"s.steps=1,abc"}), QString("type_mismatch"));
        QCOMPARE(code({"s.steps=1,500"}), QString("value_out_of_range"));
        QCOMPARE(code({"s.mode=fast,medium"}), QString("value_not_in_choices"));
        QCOMPARE(code({"s.source=1,2"}), QString("type_mismatch")); // a picture is not a literal
        QCOMPARE(code({"s.many=a,b"}), QString("type_mismatch"));   // nor a list
        QCOMPARE(code({"s.steps=1,2", "s.steps=3,4"}), QString("usage"));
        QCOMPARE(code({"s.steps=1", "s.mode=fast", "s.prompt=a"}), QString("usage")); // three axes
        QCOMPARE(code({}), QString("usage"));
        QCOMPARE(code({"s.steps=1,2"}, "s"), QString("ok"));
        QCOMPARE(code({"s.steps=1,2,3,4,5,6,7,8,9", "s.prompt=a,b,c,d,e,f,g,h"}), QString("value_out_of_range")); // 72 cells
        QCOMPARE(code({"s.steps=1,2,3,4,5,6,7,8", "s.prompt=a,b,c,d,e,f,g,h"}), QString("ok"));              // 64 do fit
    }

    void oneAxisWrapsSoSixteenSeedsStayInsideTheSheetLimit() {
        QStringList seeds;
        for (int i = 1; i <= 16; ++i)
            seeds << QString::number(i);
        const Result<SweepPlan> result = plan(m_document, {"s.steps=" + seeds.join(',')}, m_registry);
        QVERIFY(result.ok());
        QCOMPARE(result->labels.columns, 4);
        QCOMPARE(result->labels.rows, 4);
        QVERIFY(checkSheetSize(result->labels, QSize(1024, 1024)).isEmpty()); // 4 x 1024 fits
        const Result<SweepPlan> two = plan(m_document, {"s.steps=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", "s.prompt=a,b"}, m_registry);
        QVERIFY(two.ok());
        const Errors tooBig = checkSheetSize(two->labels, QSize(1024, 1024)); // 16 columns of 1024 px pass 16384
        QCOMPARE(tooBig.value(0).code, QString("value_out_of_range"));
        QVERIFY(checkSheetSize(two->labels, QSize(512, 512)).isEmpty());
        // The label bands count: 15 rows of 1060 px pass without labels and not with a line under each picture.
        SweepLabels tall;
        tall.columns = 1;
        tall.rows = 15;
        QVERIFY(checkSheetSize(tall, QSize(1000, 1060)).isEmpty());
        tall.cells = QStringList(15, "x");
        QCOMPARE(checkSheetSize(tall, QSize(1000, 1060)).value(0).code, QString("value_out_of_range"));
    }

    void aTargetMustMakeAPicture() {
        const Result<Document> doc = Document::load(
            R"({"fugue": 1, "graph": {"nodes": {"s": {"type": "fugue/make", "version": "1.0.0", "inputs": {}},
            "loader": {"type": "fugue/loader", "version": "1.0.0", "inputs": {}}, "view": {"type": "fugue/preview", "version": "1.0.0", "inputs": {}}}}})");
        QVERIFY(doc.ok());
        const auto code = [&](const QString &to) {
            const Result<SweepPlan> result = planSweep(doc.value(), {parseSweepAxis("s.steps=1,2").value()}, to, m_registry);
            return result.ok() ? QString("ok") : result.errors().value(0).code;
        };
        QCOMPARE(code("loader"), QString("usage")); // its output is a model, not a picture
        QCOMPARE(code("view"), QString("ok"));
        QCOMPARE(code("s"), QString("ok"));
    }

    void labelsStayOnOneLineAndTellSameNamesApart() {
        const auto labels = [&](const QStringList &axes) { return plan(m_document, axes, m_registry).value().labels; };
        // A newline in a value does not break the label lines; a long value is cut.
        const SweepLabels lines = labels({"s.prompt=\"one\\ntwo\",\"" + QString(80, 'x') + "\""});
        QCOMPARE(lines.cells.at(0), QString("prompt=one two"));
        QCOMPARE(lines.cells.size(), 2);
        QVERIFY(lines.cells.at(1).size() < 60 && lines.cells.at(1).endsWith(QChar(0x2026)));
        // Paths label as file names, with the folder when two share one.
        QCOMPARE(labels({"s.prompt=/a/red.png,/b/blue.png"}).cells, (QStringList{"prompt=red.png", "prompt=blue.png"}));
        QCOMPARE(labels({"s.prompt=/a/x.png,/b/x.png"}).cells, (QStringList{"prompt=a/x.png", "prompt=b/x.png"}));
        // Two axes of one input name (two nodes) are named node.input.
        const Result<Document> twin = Document::load(
            R"({"fugue": 1, "graph": {"nodes": {"a": {"type": "fugue/make", "version": "1.0.0", "inputs": {}}, "b": {"type": "fugue/make", "version": "1.0.0", "inputs": {}}}}})");
        const Result<SweepPlan> both = planSweep(twin.value(), {parseSweepAxis("a.steps=1,2").value(), parseSweepAxis("b.steps=3,4").value()}, "a", m_registry);
        QVERIFY(both.ok());
        QCOMPARE(both->labels.columnLabels, (QStringList{"a.steps=1", "a.steps=2"}));
        QCOMPARE(both->labels.rowLabels, (QStringList{"b.steps=3", "b.steps=4"}));
    }

    void onlyImageHandlesCountAsPictures() {
        // A list output whose key sorts before the picture's: its model handles are not pictures either.
        const QJsonObject outputs{{"a_models", QJsonArray{QJsonObject{{"resource", "fugue/image-model"}, {"path", "/m/y.safetensors"}}}},
                                  {"model", QJsonObject{{"resource", "fugue/image-model"}, {"path", "/m/x.safetensors"}}},
                                  {"file", QJsonObject{{"type", "image"}, {"path", "/p/a.png"}}}};
        QStringList any, images;
        collectMediaPaths(outputs, any);
        collectMediaPaths(outputs, images, "image");
        QCOMPARE(any.size(), 3);
        QCOMPARE(images, (QStringList{"/p/a.png"}));
    }

    void composeLaysTheCellsOutAndSavesTheSheet() {
        const Result<SweepPlan> result = plan(m_document, {"s.steps=5,15", "s.mode=fast,slow"}, m_registry);
        QVERIFY(result.ok());
        const Result<Document> sheet = composeDocument({"/a.png", "/b.png", QString(), "/d.png"}, result->labels, m_registry, {"/out", QSize(40, 30)});
        QVERIFY2(sheet.ok(), qPrintable(sheet.errors().value(0).message));
        const Nodes &nodes = sheet->graph.nodes;
        QCOMPARE(nodes.size(), 6); // four loads, the grid, the save
        // A cell with no picture is a plain tile made in the graph: no file.
        QCOMPARE(nodes.value("cell3").type, QString("fugue/solid-mask"));
        QCOMPARE(nodes.value("cell3").inputs.value("width").toJson(), QJsonValue(40));
        QCOMPARE(nodes.value("cell4").inputs.value("file").toJson(), QJsonValue("/d.png"));
        const auto &grid = nodes.value("grid").inputs;
        QCOMPARE(grid.value("images").items().size(), 4);
        QCOMPARE(grid.value("images").items().at(2).from().port, QString("mask"));
        QCOMPARE(grid.value("columns").toJson(), QJsonValue(2));
        QCOMPARE(grid.value("column_labels").toJson(), QJsonValue("steps=5\nsteps=15"));
        QCOMPARE(grid.value("row_labels").toJson(), QJsonValue("mode=fast\nmode=slow"));
        QCOMPARE(nodes.value("sheet").inputs.value("folder").toJson(), QJsonValue("/out"));
        QCOMPARE(nodes.value("sheet").inputs.value("image").from().node, QString("grid"));
        Registry empty;
        QCOMPARE(composeDocument({"/a.png"}, result->labels, empty).errors().value(0).code, QString("unknown_node_type"));
    }
};

QTEST_GUILESS_MAIN(TestSweep)
#include "test_sweep.moc"
