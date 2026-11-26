// What the built-in nodes can tell from their written values before a run (NodeKernel::check, ProviderKernel::check;
// spec 10.4 and the "Also before a run" column of 14).
//
// Seam: the registered kernels, as the scheduler's dry run calls them. One table a node family: the written values
// in, the error (code:input) out, and none for a good value or for one that is not known yet (left out of the inputs,
// as a wired or not-yet-run input is). The same errors under run are the nodes' own tests'.

#include <QtTest>

#include <QElapsedTimer>

#include "engine/node_kernel.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

class TestNodeChecks : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;

    QString file(const QString &name, const QByteArray &content) {
        const QString path = m_dir.filePath(name);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(content) != content.size())
            qFatal("cannot write %s", qPrintable(path));
        return path;
    }

    // The kernel's check of `written` over the type's defaults, `absent` left out as a wired input is. Each error as code:input.
    QString check(const QString &type, const QVariantMap &written, const QStringList &absent = {}) {
        NodeContext context;
        context.node = QStringLiteral("n");
        context.type = m_registry.nodeType(type);
        if (!context.type)
            qFatal("no node type %s", qPrintable(type));
        for (const InputSpec &spec : context.type->inputs)
            if (!spec.defaultValue.isUndefined())
                context.inputs.insert(spec.name, Value(spec.defaultValue));
        for (auto it = written.begin(); it != written.end(); ++it)
            context.inputs.insert(it.key(), Value(QJsonValue::fromVariant(it.value())));
        for (const QString &name : absent)
            context.inputs.remove(name);
        Errors errors;
        if (const auto kernel = m_kernels.node(type))
            errors = kernel->check(context);
        else
            errors = m_kernels.provider(type)->check(context);
        QStringList found;
        for (const Error &error : errors)
            found << error.code + ":" + error.input;
        return found.join(",");
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    // Load Image, Video and Audio, Paint Mask and Paint: a file named that is not there. A blank path is the
    // missing input the scheduler reports; a path not known yet is not judged.
    void filesNamedThatAreNotThere_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QString>("expected");
        const QString there = file("there.bin", "x");
        const QString gone = m_dir.filePath("gone.bin");
        for (const char *type : {"fugue/load-image", "fugue/load-video", "fugue/load-audio"}) {
            QTest::newRow(qPrintable(QString(type) + " there")) << type << QVariantMap{{"file", there}} << "";
            QTest::newRow(qPrintable(QString(type) + " gone")) << type << QVariantMap{{"file", gone}} << "file_not_found:file";
            QTest::newRow(qPrintable(QString(type) + " blank")) << type << QVariantMap{{"file", ""}} << "";
        }
        QTest::newRow("load-image painted mask gone") << "fugue/load-image" << QVariantMap{{"file", there}, {"mask", gone}} << "file_not_found:mask";
        QTest::newRow("load-image both gone") << "fugue/load-image" << QVariantMap{{"file", gone}, {"mask", gone}} << "file_not_found:file,file_not_found:mask";
        QTest::newRow("load-image painted mask there") << "fugue/load-image" << QVariantMap{{"file", there}, {"mask", there}} << "";
        QTest::newRow("paint-mask gone") << "fugue/paint-mask" << QVariantMap{{"mask", gone}} << "file_not_found:mask";
        QTest::newRow("paint-mask there") << "fugue/paint-mask" << QVariantMap{{"mask", there}} << "";
        QTest::newRow("paint-mask none") << "fugue/paint-mask" << QVariantMap{{"mask", ""}} << "";
        QTest::newRow("paint gone") << "fugue/paint" << QVariantMap{{"paint", gone}} << "file_not_found:paint";
        QTest::newRow("paint there") << "fugue/paint" << QVariantMap{{"paint", there}} << "";
        QTest::newRow("paint none") << "fugue/paint" << QVariantMap{{"paint", ""}} << "";
    }
    void filesNamedThatAreNotThere() {
        QFETCH(QString, type);
        QFETCH(QVariantMap, written);
        QFETCH(QString, expected);
        QCOMPARE(check(type, written), expected);
        if (written.contains("file"))
            QCOMPARE(check(type, {}, {"file"}), QString()); // a file not known yet
    }

    // Apply LUT: the file is there and its first lines are a good header; the rows are not read when the file is long.
    void applyLut_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        const auto row = [&](const char *name, const QString &path, const char *expected) { QTest::newRow(name) << path << QString(expected); };
        row("missing", m_dir.filePath("no.cube"), "file_not_found:file");
        row("good 3D", file("good.cube", "TITLE \"x\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"), "");
        row("good 1D", file("good1.cube", "# c\nLUT_1D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n0 0 0\n1 1 1\n"), "");
        row("unknown keyword", file("kw.cube", "LUT_3D_SIZE 2\nFOO 1\n"), "input_invalid:file");
        row("row before size", file("early.cube", "0 0 0\nLUT_3D_SIZE 2\n"), "input_invalid:file");
        row("size too small", file("size.cube", "LUT_3D_SIZE 1\n0 0 0\n"), "input_invalid:file");
        row("second size", file("two.cube", "LUT_3D_SIZE 2\nLUT_1D_SIZE 2\n"), "input_invalid:file");
        row("empty domain", file("domain.cube", "LUT_1D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n1 1 1\n"), "input_invalid:file");
        row("short of rows", file("short.cube", "LUT_1D_SIZE 4\n0 0 0\n1 1 1\n"), "input_invalid:file");
        row("no size", file("nosize.cube", "# nothing\n"), "input_invalid:file");
    }
    void applyLut() {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        QCOMPARE(check("fugue/apply-lut", {{"file", path}}), expected);
        QCOMPARE(check("fugue/apply-lut", {}, {"file"}), QString());
    }

    // A long file: only its first KB are read, so a bad keyword up there is told, a bad row far down is a run's, and
    // it stays fast.
    void applyLutReadsOnlyTheStartOfALongFile() {
        QByteArray rows;
        for (int i = 0; i < 40000; ++i)
            rows += "0.5 0.5 0.5\n";
        const QString good = file("long.cube", "# " + QByteArray(9000, 'c') + "\nLUT_3D_SIZE 34\n" + rows);
        const QString badKeyword = file("long-bad.cube", "LUT_3D_SIZE 34\nBOGUS 1\n" + rows);
        const QString badRowFarDown = file("long-row.cube", "LUT_3D_SIZE 34\n" + rows + "not a row\n");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(check("fugue/apply-lut", {{"file", good}}), QString()); // comments past the cut: nothing known, nothing said
        QCOMPARE(check("fugue/apply-lut", {{"file", badKeyword}}), QString("input_invalid:file"));
        QCOMPARE(check("fugue/apply-lut", {{"file", badRowFarDown}}), QString());
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    }

    // Pad: a target of one side only, or a target with a side; an input not known yet is not judged.
    void pad_data() {
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QStringList>("absent");
        QTest::addColumn<QString>("expected");
        QTest::newRow("sides only") << QVariantMap{{"left", 10}, {"bottom", 4}} << QStringList() << "";
        QTest::newRow("target only") << QVariantMap{{"target_width", 512}, {"target_height", 512}} << QStringList() << "";
        QTest::newRow("width alone") << QVariantMap{{"target_width", 512}} << QStringList() << "input_invalid:target_height";
        QTest::newRow("height alone") << QVariantMap{{"target_height", 512}} << QStringList() << "input_invalid:target_width";
        QTest::newRow("target and a side") << QVariantMap{{"target_width", 512}, {"target_height", 512}, {"right", 8}} << QStringList() << "input_invalid:right";
        QTest::newRow("target, height wired") << QVariantMap{{"target_width", 512}} << QStringList{"target_height"} << "";
        QTest::newRow("target, a side wired") << QVariantMap{{"target_width", 512}, {"target_height", 512}, {"top", 3}} << QStringList{"left"} << "input_invalid:top";
    }
    void pad() {
        QFETCH(QVariantMap, written);
        QFETCH(QStringList, absent);
        QFETCH(QString, expected);
        QCOMPARE(check("fugue/pad", written, absent), expected);
    }

    // Number Math: a division by zero from `b` and the operation alone; a result that is no number needs all three.
    void numberMath_data() {
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QStringList>("absent");
        QTest::addColumn<QString>("expected");
        QTest::newRow("add") << QVariantMap{{"a", 1}, {"b", 0}, {"operation", "add"}} << QStringList() << "";
        QTest::newRow("divide by zero") << QVariantMap{{"a", 1}, {"b", 0}, {"operation", "divide"}} << QStringList() << "value_out_of_range:b";
        QTest::newRow("modulo by zero") << QVariantMap{{"a", 1}, {"b", 0}, {"operation", "modulo"}} << QStringList() << "value_out_of_range:b";
        QTest::newRow("divide by zero, a wired") << QVariantMap{{"b", 0}, {"operation", "divide"}} << QStringList{"a"} << "value_out_of_range:b";
        QTest::newRow("divide by two, a wired") << QVariantMap{{"b", 2}, {"operation", "divide"}} << QStringList{"a"} << "";
        QTest::newRow("power of zero, a wired") << QVariantMap{{"b", -1}, {"operation", "power"}} << QStringList{"a"} << "";
        QTest::newRow("divide, b wired") << QVariantMap{{"a", 1}, {"operation", "divide"}} << QStringList{"b"} << "";
        QTest::newRow("not finite") << QVariantMap{{"a", 0}, {"b", -1}, {"operation", "power"}} << QStringList() << "value_out_of_range:a";
        QTest::newRow("operation wired") << QVariantMap{{"a", 1}, {"b", 0}} << QStringList{"operation"} << "";
    }
    void numberMath() {
        QFETCH(QVariantMap, written);
        QFETCH(QStringList, absent);
        QFETCH(QString, expected);
        QCOMPARE(check("fugue/number-math", written, absent), expected);
    }

    // Save Image, Video and Audio: the name and folder written are ones a save can use.
    void saveNodes_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QString>("expected");
        QDir().mkpath(outputDir());
        for (const char *type : {"fugue/save-image", "fugue/save-video", "fugue/save-audio"}) {
            const auto row = [&](const char *what, const QVariantMap &written, const char *expected) {
                QTest::newRow(qPrintable(QString(type) + " " + what)) << type << written << QString(expected);
            };
            row("plain name", {{"name", "fugue"}}, "");
            row("tokens and a subfolder", {{"name", "{date}/{model}-{time}"}}, "");
            row("empty name", {{"name", ""}}, "node_failed:name");
            row("absolute name", {{"name", "/etc/x"}}, "node_failed:name");
            row("dot part", {{"name", "a/../b"}}, "node_failed:name");
            row("backslash", {{"name", "a\\b"}}, "node_failed:name");
            row("too long", {{"name", QString(300, 'x')}}, "node_failed:name");
            row("folder inside", {{"name", "ok"}, {"folder", "sub/folder"}}, "");
            row("folder absolute", {{"name", "ok"}, {"folder", "/tmp"}}, "");
            row("folder leaves the output", {{"name", "ok"}, {"folder", "../elsewhere"}}, "permission_denied:folder");
        }
    }
    void saveNodes() {
        QFETCH(QString, type);
        QFETCH(QVariantMap, written);
        QFETCH(QString, expected);
        QCOMPARE(check(type, written), expected);
        QCOMPARE(check(type, {{"name", "/etc/x"}}, {"name"}), QString()); // a name not known yet
    }

    // Canvas Write: both Onto fields. Canvas Region: a canvas written is an id; an empty one is named at the run.
    void canvasNodes_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QString>("expected");
        QTest::newRow("write: neither") << "fugue/canvas-write" << QVariantMap{} << "";
        QTest::newRow("write: onto only") << "fugue/canvas-write" << QVariantMap{{"onto", "Mask"}} << "";
        QTest::newRow("write: pixels only") << "fugue/canvas-write" << QVariantMap{{"onto_pixels", "Layer"}} << "";
        QTest::newRow("write: both") << "fugue/canvas-write" << QVariantMap{{"onto", "Mask"}, {"onto_pixels", "Layer"}} << "input_invalid:onto_pixels";
        QTest::newRow("write: blanks") << "fugue/canvas-write" << QVariantMap{{"onto", "  "}, {"onto_pixels", "Layer"}} << "";
        QTest::newRow("region: id") << "fugue/canvas-region" << QVariantMap{{"canvas", "canvas-0123456789ab"}} << "";
        QTest::newRow("region: empty") << "fugue/canvas-region" << QVariantMap{{"canvas", ""}} << "";
        QTest::newRow("region: not an id") << "fugue/canvas-region" << QVariantMap{{"canvas", "my canvas"}} << "input_invalid:canvas";
    }
    void canvasNodes() {
        QFETCH(QString, type);
        QFETCH(QVariantMap, written);
        QFETCH(QString, expected);
        QCOMPARE(check(type, written), expected);
    }
    void canvasWriteWithAnOntoNotKnownIsNotJudged() {
        QCOMPARE(check("fugue/canvas-write", {{"onto", "Mask"}, {"onto_pixels", "Layer"}}, {"onto_pixels"}), QString());
    }

    // Provider nodes: Detail Regions' boxes, Restore Image's overlap, Extend and Repaint Song's seconds.
    void providerNodes_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("written");
        QTest::addColumn<QString>("expected");
        const auto row = [&](const char *name, const char *type, const QVariantMap &written, const char *expected) {
            QTest::newRow(name) << type << written << QString(expected);
        };
        row("boxes: none", "fugue/detail-regions", {{"boxes", ""}}, "");
        row("boxes: good", "fugue/detail-regions", {{"boxes", "10 20 110 220\n\n5.5 6 70 80"}}, "");
        row("boxes: three numbers", "fugue/detail-regions", {{"boxes", "10 20 110"}}, "input_invalid:boxes");
        row("boxes: text", "fugue/detail-regions", {{"boxes", "10 20 110 220\nleft top right bottom"}}, "input_invalid:boxes");
        row("restore: overlap fits", "fugue/restore-image", {{"tile", 1024}, {"overlap", 512}}, "");
        row("restore: overlap too big", "fugue/restore-image", {{"tile", 1024}, {"overlap", 600}}, "value_out_of_range:overlap");
        row("extend: after", "fugue/extend-song", {{"before", 0}, {"after", 10}}, "");
        row("extend: before", "fugue/extend-song", {{"before", 5}, {"after", 0}}, "");
        row("extend: neither", "fugue/extend-song", {{"before", 0}, {"after", 0}}, "value_out_of_range:after");
        row("extend: both", "fugue/extend-song", {{"before", 5}, {"after", 5}}, "value_out_of_range:after");
        row("extend: negative", "fugue/extend-song", {{"before", -1}, {"after", 0}}, "value_out_of_range:before");
        row("repaint: part", "fugue/repaint-song", {{"start", 5}, {"end", 20}}, "");
        row("repaint: to the end", "fugue/repaint-song", {{"start", 5}, {"end", 0}}, "");
        row("repaint: backwards", "fugue/repaint-song", {{"start", 20}, {"end", 5}}, "value_out_of_range:end");
        row("repaint: empty", "fugue/repaint-song", {{"start", 5}, {"end", 5}}, "value_out_of_range:end");
    }
    void providerNodes() {
        QFETCH(QString, type);
        QFETCH(QVariantMap, written);
        QFETCH(QString, expected);
        QCOMPARE(check(type, written), expected);
    }
    void providerNodesWithAnInputNotKnownAreNotJudged() {
        QCOMPARE(check("fugue/detail-regions", {}, {"boxes"}), QString());
        QCOMPARE(check("fugue/restore-image", {{"tile", 1024}, {"overlap", 600}}, {"tile"}), QString());
        QCOMPARE(check("fugue/extend-song", {{"before", 5}, {"after", 5}}, {"after"}), QString());
        QCOMPARE(check("fugue/repaint-song", {{"start", 20}, {"end", 5}}, {"start"}), QString());
    }
};

QTEST_GUILESS_MAIN(TestNodeChecks)
#include "test_node_checks.moc"
