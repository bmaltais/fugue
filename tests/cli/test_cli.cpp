// The `fugue` command line (spec 13), driven as the real binary.
//
// Seam: the built `fugue` executable run as a subprocess with no display
// (QT_QPA_PLATFORM, DISPLAY and WAYLAND_DISPLAY unset). Tests assert on what a
// caller sees: the JSON on stdout, stderr, and the exit code. The shipped binary
// has no node types until M1, so most tests drive fugue_with_fakes: the same
// command line, whose test-only main() seeds the registry from the folder of
// node.json files that FUGUE_NODE_MANIFESTS names (through cli::Hooks). Run folders are made with RunFolder under a temporary
// XDG_CACHE_HOME, the same files a worker writes.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/document.h"
#include "engine/run_folder.h"

using namespace fugue;

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    // Stdout as the single JSON object 13.2 promises; fails the test if it is not one.
    QJsonObject json() const {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(out, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning("stdout is not exactly one JSON object: %s", out.constData());
            return {};
        }
        return document.object();
    }
    QString firstCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
    QString firstMessage() const { return json().value("errors").toArray().at(0).toObject().value("message").toString(); }
};

QString manifest(const QString &id, const QString &name, const QString &inType, const QString &outType) {
    const QJsonObject json{
        {"manifest", 1},
        {"id", id},
        {"version", "1.0.0"},
        {"name", name},
        {"description", name + "."},
        {"category", "Test/Fakes"},
        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./x.sh"}}},
        {"inputs", QJsonArray{QJsonObject{{"name", "in"}, {"type", inType}}}},
        {"outputs", QJsonArray{QJsonObject{{"name", "out"}, {"type", outType}}}},
    };
    return QString::fromUtf8(QJsonDocument(json).toJson());
}

QString plainManifest(const QString &id, bool sink, bool hasInput, bool hasOutput) {
    QJsonObject json{
        {"manifest", 1},
        {"id", id},
        {"version", "1.0.0"},
        {"name", id},
        {"runtime", QJsonObject{{"kind", "builtin"}}},
        {"inputs", hasInput ? QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}} : QJsonArray()},
        {"outputs", hasOutput ? QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}} : QJsonArray()},
    };
    if (sink)
        json.insert("sink", true);
    return QString::fromUtf8(QJsonDocument(json).toJson());
}

QByteArray flow(const char *nodes) {
    QByteArray bytes = "{\"fugue\": 1, \"graph\": {\"nodes\": {";
    bytes += nodes;
    bytes += "}}}";
    return bytes;
}

} // namespace

class TestCli : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString cache() const { return m_dir.filePath("cache"); }
    QString runsRoot() const { return cache() + "/fugue/runs"; }

    void write(const QString &path, const QByteArray &bytes) const {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
        file.write(bytes);
    }

    Outcome fugue(const QStringList &args, const QString &binary = FUGUE_FAKE_BINARY) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_CACHE_HOME", cache());
        env.insert("XDG_STATE_HOME", m_dir.filePath("state"));
        env.insert("FUGUE_NODE_MANIFESTS", m_dir.filePath("manifests"));
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(binary, args);
        Outcome outcome;
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    QString workflow(const QString &name = "flow.fugue") const {
        const QString path = m_dir.filePath(name);
        if (!QFile::exists(path)) {
            Errors errors;
            Document document;
            if (!document.saveFile(path, errors))
                qFatal("cannot write the fixture workflow");
        }
        return path;
    }

    static QStringList ids(const QJsonArray &nodes) {
        QStringList list;
        for (const QJsonValue &node : nodes)
            list << node.toObject().value("id").toString();
        return list;
    }

    // A finished run with a few events, as a worker would leave it.
    void makeRun(const QString &id, const QString &status, bool withDone = true) const {
        auto folder = RunFolder::create(runsRoot(), id);
        QVERIFY(folder.ok());
        folder->writeState(QJsonObject{{"status", status}, {"workflow", "/w/a.fugue"}});
        folder->appendEvent(QJsonObject{{"event", "run.queued"}});
        folder->appendEvent(QJsonObject{{"event", "run.started"}});
        folder->appendEvent(QJsonObject{{"event", "node.running"}, {"node", "a"}});
        folder->appendEvent(QJsonObject{{"event", "node.progress"}, {"node", "a"}, {"value", 0.5}});
        folder->appendEvent(QJsonObject{{"event", "node.done"},
                                        {"node", "a"},
                                        {"outputs", QJsonObject{{"image", QJsonObject{{"path", "/x/a.png"}}}}},
                                        {"seconds", 1.5}});
        folder->appendEvent(QJsonObject{{"event", "node.running"}, {"node", "b"}});
        if (withDone)
            folder->appendEvent(QJsonObject{{"event", "run.done"}, {"status", status}, {"errors", QJsonArray()}});
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        write(m_dir.filePath("manifests/sharpen/node.json"),
              manifest("io.github.alice.tools/sharpen", "Sharpen", "image", "image").toUtf8());
        write(m_dir.filePath("manifests/shout/node.json"),
              manifest("fugue/shout", "Shout", "text", "text").toUtf8());
        write(m_dir.filePath("manifests/note/node.json"), plainManifest("fugue/note", false, false, true).toUtf8());
        write(m_dir.filePath("manifests/show/node.json"), plainManifest("fugue/show", true, true, false).toUtf8());
        // note -> show, and a stray shout that feeds nothing.
        write(m_dir.filePath("good.fugue"),
              flow("\"a\": {\"type\": \"fugue/note\", \"version\": \"1.0.0\"},"
                   "\"b\": {\"type\": \"fugue/show\", \"version\": \"1.0.0\", \"inputs\": {\"text\": {\"from\": \"a.text\"}}},"
                   "\"c\": {\"type\": \"fugue/shout\", \"version\": \"1.0.0\", \"inputs\": {\"in\": \"hi\"}}"));
        write(m_dir.filePath("broken.fugue"),
              flow("\"b\": {\"type\": \"fugue/show\", \"version\": \"1.0.0\"},"
                   "\"x\": {\"type\": \"fugue/nope\", \"version\": \"1.0.0\"}"));
    }

    // --- the compiler behind validate, compile and run ---

    void validateRunsTheCompiler() {
        const Outcome o = fugue({"validate", m_dir.filePath("broken.fugue"), "--json"});
        QCOMPARE(o.exitCode, 1);
        QStringList codes;
        for (const QJsonValue &error : o.json().value("errors").toArray())
            codes << error.toObject().value("code").toString();
        QVERIFY(codes.contains("unknown_node_type"));
        QVERIFY(codes.contains("missing_input"));
    }

    void validateAndCompilePrintWarningsOnSuccess() {
        for (const char *command : {"validate", "compile"}) {
            const Outcome o = fugue({QString(command), m_dir.filePath("good.fugue"), "--json"});
            QVERIFY2(o.exitCode == 0, qPrintable(command));
            const QJsonObject json = o.json();
            QVERIFY2(json.value("ok").toBool(), qPrintable(command));
            const QJsonArray warnings = json.value("warnings").toArray();
            QVERIFY2(warnings.size() == 1, qPrintable(command));
            QCOMPARE(warnings.at(0).toObject().value("code").toString(), QString("not_part_of_output"));
            QCOMPARE(warnings.at(0).toObject().value("node").toString(), QString("c"));
            QVERIFY2(o.err.contains("not_part_of_output"), qPrintable(command));
        }
    }

    void compilePrintsTheCompiledGraph() {
        const Outcome o = fugue({"compile", m_dir.filePath("good.fugue"), "--json"});
        const QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(data.value("fugue-compiled").toInt(), 1);
        QCOMPARE(data.value("nodes").toObject().keys(), (QStringList{"a", "b"}));
        QCOMPARE(data.value("nodes").toObject().value("b").toObject().value("sink").toBool(), true);
    }

    void runCompilesWithTargetAndProviderOptions() {
        // Compile errors come first; then the options are checked against the graph.
        QCOMPARE(fugue({"run", m_dir.filePath("broken.fugue"), "--json"}).firstCode(), QString("unknown_node_type"));
        QCOMPARE(fugue({"run", m_dir.filePath("good.fugue"), "--to", "nope", "--json"}).firstCode(), QString("usage"));
        QCOMPARE(fugue({"run", m_dir.filePath("good.fugue"), "--provider", "ghost", "--json"}).firstCode(),
                 QString("unknown_provider"));
    }

    void workflowDescribeReportsSinksAndWhatIsMissing() {
        Outcome o = fugue({"workflow", "describe", m_dir.filePath("good.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0);
        QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(data.value("sinks").toArray().at(0).toString(), QString("b"));
        QCOMPARE(data.value("missing").toArray().size(), 0);

        o = fugue({"workflow", "describe", m_dir.filePath("broken.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0); // describing is not failing
        data = o.json().value("data").toObject();
        QStringList missing;
        for (const QJsonValue &item : data.value("missing").toArray())
            missing << item.toObject().value("code").toString() + ":" + item.toObject().value("node").toString();
        QVERIFY(missing.contains("missing_input:b"));
        QVERIFY(missing.contains("unknown_node_type:x"));
    }

    // --- arguments and the envelope ---

    void unknownCommandIsAUsageErrorOnStdoutWithJson() {
        const Outcome o = fugue({"bogus", "--json"});
        QCOMPARE(o.exitCode, 1);
        const QJsonObject json = o.json();
        QCOMPARE(json.value("ok").toBool(true), false);
        QCOMPARE(o.firstCode(), QString("usage"));
        QCOMPARE(json.value("errors").toArray().at(0).toObject().value("severity").toString(), QString("error"));
    }

    void usageErrorWithoutJsonGoesToStderr() {
        const Outcome o = fugue({"bogus"});
        QCOMPARE(o.exitCode, 1);
        QVERIFY(o.out.isEmpty());
        QVERIFY(o.err.contains("usage"));
    }

    void jsonFlagMayComeAnywhere() {
        const Outcome o = fugue({"--json", "node", "list"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("ok").toBool(), true);
    }

    void unknownOptionAndMissingArgumentsAreUsageErrors() {
        for (const QStringList &args : QList<QStringList>{
                 {"node", "list", "--bogus"},
                 {"node", "list", "extra"},
                 {"node", "list", "--type"},
                 {"node", "describe"},
                 {"validate"},
                 {"validate", "a", "b"},
                 {"run"},
                 {"runs"},
                 {"runs", "status"},
                 {"runs", "bogus"},
                 {"workflow"},
                 {"workflow", "bogus", "x"},
                 {"run", "x.fugue", "--set", "no-equals-sign"},
             }) {
            const Outcome o = fugue(args + QStringList{"--json"});
            QVERIFY2(o.exitCode == 1, qPrintable(args.join(' ')));
            QVERIFY2(o.firstCode() == "usage", qPrintable(args.join(' ')));
        }
    }

    void runAndRunsAreDifferentCommands() {
        // `runs list` needs no file; `run` needs one.
        QCOMPARE(fugue({"runs", "list", "--json"}).exitCode, 0);
        QCOMPARE(fugue({"run", "--json"}).firstCode(), QString("usage"));
    }

    void helpAndVersionPrintAndExitZero() {
        QVERIFY(fugue({"--help"}, FUGUE_BINARY).out.contains("usage: fugue"));
        QCOMPARE(fugue({"--version"}, FUGUE_BINARY).exitCode, 0);
    }

    void noDisplayNeeded() {
        // initTestCase's env has no display and no QT_QPA_PLATFORM; this must not abort.
        const Outcome o = fugue({"node", "list", "--json"}, FUGUE_BINARY);
        QCOMPARE(o.exitCode, 0);
        QVERIFY2(!o.err.contains("xcb") && !o.err.contains("platform plugin"), o.err.constData());
    }

    // --- nodes ---

    void nodeListShowsTheRegistry() {
        const Outcome o = fugue({"node", "list", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(ids(data.value("nodes").toArray()),
                 (QStringList{"fugue/note", "fugue/shout", "fugue/show", "io.github.alice.tools/sharpen"}));
        const QJsonObject first = data.value("nodes").toArray().at(3).toObject();
        QCOMPARE(first.value("name").toString(), QString("Sharpen"));
        QCOMPARE(first.value("version").toString(), QString("1.0.0"));
    }

    void pluralIsAnAliasForNode() {
        QCOMPARE(fugue({"nodes", "list", "--json"}).out, fugue({"node", "list", "--json"}).out);
        QCOMPARE(fugue({"nodes", "describe", "fugue/shout", "--json"}).out,
                 fugue({"node", "describe", "fugue/shout", "--json"}).out);
    }

    void nodeListFiltersByPortType() {
        const Outcome o = fugue({"node", "list", "--type", "image", "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(ids(o.json().value("data").toObject().value("nodes").toArray()),
                 (QStringList{"io.github.alice.tools/sharpen"}));
        QCOMPARE(fugue({"node", "list", "--type", "not a type!", "--json"}).firstCode(), QString("usage"));
    }

    void nodeDescribeGivesFullManifestsInOrderGiven() {
        const Outcome o = fugue({"node", "describe", "io.github.alice.tools/sharpen", "fugue/shout", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonArray nodes = o.json().value("data").toObject().value("nodes").toArray();
        QCOMPARE(nodes.size(), 2);
        QCOMPARE(nodes.at(0).toObject().value("id").toString(), QString("io.github.alice.tools/sharpen"));
        QCOMPARE(nodes.at(0).toObject().value("inputs").toArray().at(0).toObject().value("name").toString(),
                 QString("in"));
        QCOMPARE(nodes.at(1).toObject().value("id").toString(), QString("fugue/shout"));
    }

    void nodeDescribeUnknownTypeNamesIt() {
        const Outcome o = fugue({"node", "describe", "fugue/shout", "fugue/nope", "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("unknown_node_type"));
        QVERIFY(o.firstMessage().contains("fugue/nope"));
    }

    void emptyRegistryListsNothing() {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("FUGUE_NODE_MANIFESTS", m_dir.filePath("nowhere"));
        process.setProcessEnvironment(env);
        process.start(FUGUE_FAKE_BINARY, {"node", "list", "--json"});
        QVERIFY(process.waitForFinished(20000));
        const QJsonObject json = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(json.value("data").toObject().value("nodes").toArray().size(), 0);
    }

    // The manifest folder is a test hook; the app people install does not have it.
    void theShippedBinaryLoadsNoNodeTypesFromTheEnvironment() {
        const Outcome o = fugue({"node", "list", "--json"}, FUGUE_BINARY); // FUGUE_NODE_MANIFESTS is set
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("data").toObject().value("nodes").toArray().size(), 0);
    }

    // --- workflows ---

    void validateReportsDocumentErrors() {
        Outcome o = fugue({"validate", m_dir.filePath("missing.fugue"), "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("file_not_found"));

        write(m_dir.filePath("bad.fugue"), "{not json");
        o = fugue({"validate", m_dir.filePath("bad.fugue"), "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("invalid_json"));

        write(m_dir.filePath("future.fugue"), "{\"fugue\": 99, \"graph\": {\"nodes\": {}}}");
        o = fugue({"validate", m_dir.filePath("future.fugue"), "--json"});
        QCOMPARE(o.firstCode(), QString("unsupported_format_version"));
    }

    void validateAcceptsAGoodDocument() {
        const Outcome o = fugue({"validate", workflow(), "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("ok").toBool(), true);
        QCOMPARE(o.json().value("data").toObject().value("valid").toBool(), true);
        QVERIFY(!o.json().contains("warnings")); // none to report
    }

    void workflowValidateIsTheSameCommand() {
        QCOMPARE(fugue({"workflow", "validate", workflow(), "--json"}).out,
                 fugue({"validate", workflow(), "--json"}).out);
    }

    void workflowNewWritesAnEmptyDocumentAndRefusesToOverwrite() {
        const QString path = m_dir.filePath("made.fugue");
        Outcome o = fugue({"workflow", "new", path, "--json"});
        QCOMPARE(o.exitCode, 0);
        QVERIFY(QFile::exists(path));
        QCOMPARE(fugue({"validate", path, "--json"}).exitCode, 0);
        o = fugue({"workflow", "new", path, "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("usage"));
    }

    void workflowDescribeSummarisesNodesAndLinks() {
        QJsonObject doc{{"fugue", 1},
                        {"graph", QJsonObject{{"nodes", QJsonObject{
                                                   {"a", QJsonObject{{"type", "fugue/shout"}, {"version", "1.0.0"}}},
                                                   {"b", QJsonObject{{"type", "fugue/shout"},
                                                                     {"version", "1.0.0"},
                                                                     {"inputs", QJsonObject{{"in", QJsonObject{{"from", "a.out"}}}}}}}}}}}};
        write(m_dir.filePath("two.fugue"), QJsonDocument(doc).toJson());
        const Outcome o = fugue({"workflow", "describe", m_dir.filePath("two.fugue"), "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(data.value("nodes").toArray().size(), 2);
        QCOMPARE(data.value("links").toArray().size(), 1);
    }

    void workflowExportIsNotBuiltYet() {
        const Outcome o = fugue({"workflow", "export", workflow(), "--share", "--no-prompts", "--no-layout", "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("internal"));
    }

    // --- later milestones ---

    void laterMilestoneCommandsAreParsedAndSayNotAvailable() {
        for (const QStringList &args : QList<QStringList>{
                 {"pack", "list"}, {"packs", "add", "x"}, {"pack", "trust", "x"}, {"pack", "remove", "x"},
                 {"pack", "update"}, {"provider", "list"}, {"providers", "list"}, {"model", "list", "--type", "t"},
                 {"models", "list"}, {"cache", "status"}, {"cache", "clear", "--workflow", "f", "--node", "n"},
                 {"command", "list"}, {"skill", "install"}, {"node", "new", "x"}, {"node", "test", "x"},
             }) {
            const Outcome o = fugue(args + QStringList{"--json"});
            QVERIFY2(o.exitCode == 1, qPrintable(args.join(' ')));
            QVERIFY2(o.firstCode() == "usage", qPrintable(args.join(' ')));
            QVERIFY2(o.firstMessage().contains("not available in this version"), qPrintable(args.join(' ')));
        }
        // A wrong subcommand is still a plain usage error.
        QVERIFY(!fugue({"cache", "bogus", "--json"}).firstMessage().contains("not available"));
    }

    // --- runs ---

    void runsListIsEmptyWithNoRuns() {
        const Outcome o = fugue({"runs", "list", "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("data").toObject().value("runs").toArray().size(), 0);
    }

    void unknownRunIsReportedByEveryRunsCommand() {
        for (const char *sub : {"status", "events", "result", "cancel"}) {
            const Outcome o = fugue({"runs", QString(sub), "nope-1", "--json"});
            QVERIFY2(o.exitCode == 1, qPrintable(sub));
            QVERIFY2(o.firstCode() == "unknown_run", qPrintable(sub));
        }
    }

    void runsListShowsNewestFirstWithStatus() {
        makeRun("20261118-100000-000-aaaa", "ok");
        makeRun("20261119-100000-000-bbbb", "failed");
        const Outcome o = fugue({"runs", "list", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonArray runs = o.json().value("data").toObject().value("runs").toArray();
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs.at(0).toObject().value("id").toString(), QString("20261119-100000-000-bbbb"));
        QCOMPARE(runs.at(0).toObject().value("status").toString(), QString("failed"));
        QCOMPARE(runs.at(1).toObject().value("status").toString(), QString("ok"));
    }

    void runsStatusShowsStateAndPerNodeProgress() {
        const Outcome o = fugue({"runs", "status", "20261118-100000-000-aaaa", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(data.value("id").toString(), QString("20261118-100000-000-aaaa"));
        QCOMPARE(data.value("status").toString(), QString("ok"));
        const QJsonObject nodes = data.value("nodes").toObject();
        QCOMPARE(nodes.value("a").toObject().value("state").toString(), QString("done"));
        QCOMPARE(nodes.value("b").toObject().value("state").toString(), QString("running"));
    }

    void runsEventsReturnsEventsInOrder() {
        const Outcome o = fugue({"runs", "events", "20261118-100000-000-aaaa", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonArray events = o.json().value("data").toObject().value("events").toArray();
        QCOMPARE(events.size(), 7);
        QCOMPARE(events.at(0).toObject().value("event").toString(), QString("run.queued"));
        QCOMPARE(events.at(6).toObject().value("event").toString(), QString("run.done"));
    }

    void runsEventsWithoutJsonPrintsOneEventPerLine() {
        const Outcome o = fugue({"runs", "events", "20261118-100000-000-aaaa"});
        QCOMPARE(o.exitCode, 0);
        const QList<QByteArray> lines = o.out.trimmed().split('\n');
        QCOMPARE(lines.size(), 7);
        QCOMPARE(QJsonDocument::fromJson(lines.at(2)).object().value("event").toString(), QString("node.running"));
    }

    void runsEventsFollowStreamsLinesEvenWithJsonAndEndsAtRunDone() {
        const Outcome o = fugue({"runs", "events", "20261118-100000-000-aaaa", "--follow", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QList<QByteArray> lines = o.out.trimmed().split('\n');
        QCOMPARE(lines.size(), 7);
        QCOMPARE(QJsonDocument::fromJson(lines.last()).object().value("event").toString(), QString("run.done"));
    }

    void runsFollowOfAFailedRunExitsOne() {
        const Outcome o = fugue({"runs", "events", "20261119-100000-000-bbbb", "--follow"});
        QCOMPARE(o.exitCode, 1);
    }

    void runsResultListsNodeOutputs() {
        const Outcome o = fugue({"runs", "result", "20261118-100000-000-aaaa", "--json"});
        QCOMPARE(o.exitCode, 0);
        const QJsonObject data = o.json().value("data").toObject();
        QCOMPARE(data.value("status").toString(), QString("ok"));
        QCOMPARE(data.value("outputs").toObject().value("a").toObject().value("image").toObject().value("path").toString(),
                 QString("/x/a.png"));
    }

    // run.done is appended once by whoever ended the run; run.json is rewritten as the run
    // goes, so a late write there must not bring an ended run back to life.
    void runDoneDecidesTheStatusWhateverRunJsonSays() {
        makeRun("20261122-100000-000-eeee", "running", false);
        auto folder = RunFolder::open(runsRoot(), "20261122-100000-000-eeee");
        folder->appendEvent(QJsonObject{{"event", "run.done"}, {"status", "cancelled"}, {"errors", QJsonArray()}});
        const Outcome o = fugue({"runs", "status", "20261122-100000-000-eeee", "--json"});
        QCOMPARE(o.json().value("data").toObject().value("status").toString(), QString("cancelled"));
    }

    void runsCancelOfARunWithNoWorkerMarksItFailed() {
        makeRun("20261120-100000-000-cccc", "running", false);
        const Outcome o = fugue({"runs", "cancel", "20261120-100000-000-cccc", "--json"});
        QCOMPARE(o.exitCode, 1);
        QCOMPARE(o.firstCode(), QString("internal"));
        const Outcome status = fugue({"runs", "status", "20261120-100000-000-cccc", "--json"});
        QCOMPARE(status.json().value("data").toObject().value("status").toString(), QString("failed"));
        QVERIFY(!status.json().value("data").toObject().value("ended").toString().isEmpty());
    }

    void runsResultKeepsOnlyTheSinksOfTheCompiledGraph() {
        auto folder = RunFolder::create(runsRoot(), "20261121-100000-000-dddd");
        QVERIFY(folder.ok());
        folder->writeState(QJsonObject{{"status", "ok"}});
        const QJsonObject node{{"type", "fugue/show"}, {"version", "1.0.0"}, {"inputs", QJsonObject()}};
        QJsonObject sink = node;
        sink.insert("sink", true);
        folder->writeCompiled(QJsonObject{{"fugue-compiled", 1}, {"source", "sha256:" + QString(64, '0')},
                                          {"nodes", QJsonObject{{"a", node}, {"b", sink}}}, {"skipped", QJsonObject()}});
        for (const char *id : {"a", "b"})
            folder->appendEvent(QJsonObject{{"event", "node.done"}, {"node", QString(id)},
                                            {"outputs", QJsonObject{{"text", QString(id)}}}, {"seconds", 0.1}});
        const Outcome o = fugue({"runs", "result", "20261121-100000-000-dddd", "--json"});
        QCOMPARE(o.exitCode, 0);
        QCOMPARE(o.json().value("data").toObject().value("outputs").toObject().keys(), (QStringList{"b"}));
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "test_cli.moc"
