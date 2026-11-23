// The canvas actions' runner (#231, docs/canvas-actions-design.md 5, 12): CanvasActions with the window's RunService, a
// FakeProvider on the run thread and the session's canvas store in the test's own FUGUE_HOME, no window. Generate binds
// the action's workflow and queues one ordinary run, whose Canvas Write lands one layer on the head; progress, cancel and
// errors come back in plain words; a fully cached Write after an undo is placed again; CanvasRuns ignores these runs.

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas_actions_rig.h"
#include "canvas/mask_layer.h"
#include "engine/canvas_store.h"

using namespace actions_test;

class TestCanvasActions : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    canvas::CanvasStore store() const { return canvas::CanvasStore::forSession(); }
    QString head() const { return store().info(m_canvas)->head; }
    int layers(const QString &revision) const {
        return int(store().tree(store().revision(m_canvas, revision)->tree)->layers.size());
    }
    QString topLayer(const QString &revision) const {
        return store().tree(store().revision(m_canvas, revision)->tree)->layers.back().name;
    }
    QVariantMap press(Rig &r, const QVariantMap &extra = {}) {
        QVariantMap request{{"action", "test-fill"}, {"canvas", m_canvas}, {"revision", head()}, {"prompt", "a red umbrella"},
                            {"rect", rect(96, 96, 64, 64)}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            request.insert(it.key(), it.value());
        return r.actions->generate(request);
    }
    // One press through to its layer: the job answered with a square picture.
    void pressAndFinish(Rig &r, const QVariantMap &extra = {}) {
        const int before = int(r.jobs().size());
        const QVariantMap started = press(r, extra);
        QVERIFY2(started.value("ok").toBool(), qPrintable(started.value("reason").toString()));
        QTRY_COMPARE(r.jobs().size(), before + 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(before), {{"image.png", png(64, 64)}}); });
        QTRY_VERIFY(!r.actions->busy());
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        for (const char *variable : {"FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR"})
            qunsetenv(variable);
        qputenv("FUGUE_HOME", m_dir.filePath("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath("config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", m_dir.filePath("data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", m_dir.filePath("state").toLocal8Bit());
        qputenv("XDG_CACHE_HOME", m_dir.filePath("cache").toLocal8Bit());
        QDir().mkpath(m_dir.filePath("run"));
        qputenv("XDG_RUNTIME_DIR", m_dir.filePath("run").toLocal8Bit());
    }

    void init() {
        QDir(m_dir.filePath("home")).removeRecursively();
        PaintDocument d(256, 256);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(256, 256, {30, 60, 200, 255}));
        const auto made = store().create("Actions", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    void theActionsAreListedWithWhatIsBrokenAndWhatIsMissing() {
        Rig r;
        const QVariantMap fill = r.row("generative-fill");
        QCOMPARE(fill.value("name").toString(), QStringLiteral("Generative fill"));
        QCOMPARE(fill.value("broken").toBool(), false);
        QCOMPARE(fill.value("missing").toInt(), 3); // Klein 4B, Qwen3 4B and the VAE are not in the test's model folders
        QVERIFY(fill.value("download").toString().startsWith("Download 3 models"));
        QCOMPARE(r.row("test-fill").value("missing").toInt(), 0);
        QCOMPARE(r.row("broken").value("broken").toBool(), true);
        QVERIFY(r.row("broken").value("reason").toString().contains("has no node nowhere"));
        // Generate on an action whose models are missing says so and offers the download; nothing is queued.
        const QVariantMap refused = press(r, {{"action", "generative-fill"}});
        QVERIFY(!refused.value("ok").toBool());
        QVERIFY(r.actions->error().contains("needs models"));
        QCOMPARE(r.actions->errorAction(), QStringLiteral("generative-fill"));
        QVERIFY(!r.actions->busy());
        const QVariantMap broken = press(r, {{"action", "broken"}});
        QVERIFY(broken.value("reason").toString().contains("has no node nowhere"));
    }

    // Actions from the template library (#231, design 8): a template whose workflow has a valid block is an action; a plain
    // one is not; one with a broken block is listed as broken; a template wins over the same id compiled into Fugue.
    void aTemplateWithAnActionBlockIsAnAction() {
        Rig r;
        QVERIFY(r.row("lib-fill").isEmpty());
        r.addTemplate("lib-fill", kTestFill);
        QCOMPARE(r.row("lib-fill").value("name").toString(), QStringLiteral("Test fill"));
        QCOMPARE(r.row("lib-fill").value("broken").toBool(), false);
        r.addTemplate("lib-plain", R"({"fugue": 1, "graph": {"nodes": {}}})");
        QVERIFY(r.row("lib-plain").isEmpty()); // an ordinary workflow is not an action
        r.addTemplate("lib-broken", kBroken);
        QVERIFY(r.row("lib-broken").value("broken").toBool());
        QVERIFY(r.row("lib-broken").value("reason").toString().contains("has no node nowhere"));
        // A template of the library runs like a file one: one press, one layer.
        QVariantMap request{{"action", "lib-fill"}, {"canvas", m_canvas}, {"revision", head()}, {"prompt", "a hat"}, {"rect", rect(96, 96, 64, 64)}};
        QVERIFY2(r.actions->generate(request).value("ok").toBool(), qPrintable(r.actions->error()));
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_VERIFY(!r.actions->busy());
        QCOMPARE(layers(head()), 2);
    }

    void aTemplateReplacesTheActionCompiledIntoFugueOfTheSameId() {
        Rig r;
        QCOMPARE(r.row("test-fill").value("name").toString(), QStringLiteral("Test fill")); // the file of the roots
        QByteArray renamed = kTestFill;
        renamed.replace("Test fill", "House fill");
        r.addTemplate("test-fill", renamed);
        QCOMPARE(r.row("test-fill").value("name").toString(), QStringLiteral("House fill"));
        int same = 0;
        for (const QVariant &row : r.actions->actions())
            same += row.toMap().value("id") == "test-fill";
        QCOMPARE(same, 1);
        // A plain template of that id hides nothing.
        QDir(r.templatesDir + "/test-fill").removeRecursively();
        r.addTemplate("test-fill", R"({"fugue": 1, "graph": {"nodes": {}}})");
        QCOMPARE(r.row("test-fill").value("name").toString(), QStringLiteral("Test fill"));
    }

    // Edit workflow (design 7): the action's graph opens as a tab with no file; nothing is written.
    void editWorkflowOpensTheActionsGraphAsAnUnsavedTab() {
        Rig r;
        r.addTemplate("lib-fill", kTestFill, "Library fill");
        const QVariantMap library = r.actions->editWorkflow("lib-fill");
        QVERIFY2(library.value("ok").toBool(), qPrintable(library.value("reason").toString()));
        QCOMPARE(r.app.workflows.count(), 1); // the blank tab of a fresh window is replaced
        QVariantMap tab = r.app.workflows.tabs().at(r.app.workflows.current()).toMap();
        QCOMPARE(tab.value("title").toString(), QStringLiteral("Library fill"));
        QVERIFY(tab.value("path").toString().isEmpty());
        QVERIFY(tab.value("unsaved").toBool());
        // One compiled into Fugue (a file of the roots) opens as a copy named after the action.
        const QVariantMap file = r.actions->editWorkflow("test-fill");
        QVERIFY(file.value("ok").toBool());
        QCOMPARE(r.app.workflows.count(), 2);
        tab = r.app.workflows.tabs().at(r.app.workflows.current()).toMap();
        QCOMPARE(tab.value("title").toString(), QStringLiteral("Test fill"));
        QVERIFY(tab.value("path").toString().isEmpty());
        // "auto" is the action Auto falls back to; one that is not there says so in words.
        QVERIFY(r.actions->editWorkflow("auto").value("ok").toBool());
        const QVariantMap none = r.actions->editWorkflow("nowhere");
        QVERIFY(!none.value("ok").toBool());
        QCOMPARE(r.app.workflows.count(), 3);
    }

    void generateQueuesOneRunThatLandsALayerOnTheHead() {
        Rig r;
        const QVariantMap started = press(r);
        QVERIFY2(started.value("ok").toBool(), qPrintable(started.value("reason").toString()));
        QVERIFY(r.actions->busy());
        QCOMPARE(r.actions->canvas(), m_canvas);
        QTRY_COMPARE(r.jobs().size(), 1);
        // The bound copy: the prompt, a seed of the runner's, the shown head in full.
        QJsonObject params;
        r.onFake([&](FakeProvider &f) { params = f.submitted(f.jobs().at(0)).params; });
        QCOMPARE(params.value("prompt").toString(), QStringLiteral("a red umbrella"));
        QVERIFY(params.contains("seed"));
        QFile workflow(r.app.dir.path() + "/runs/" + started.value("run").toString() + "/workflow.json");
        QVERIFY(workflow.open(QIODevice::ReadOnly));
        const QJsonObject region = QJsonDocument::fromJson(workflow.readAll()).object().value("graph").toObject()
                                       .value("nodes").toObject().value("region").toObject().value("inputs").toObject();
        QCOMPARE(region.value("canvas").toString(), m_canvas);
        QCOMPARE(region.value("revision").toString(), m_head);
        QCOMPARE(region.value("x").toInt(), 96);
        QCOMPARE(region.value("padding").toInt(), 64); // the router's: half the longer side, at least 64
        QVERIFY(region.value("scale").toDouble() > 1.0); // a small region is brought up toward 1 MP
        // Progress from the provider's node.
        r.onFake([&](FakeProvider &f) { f.progress(f.jobs().at(0), 0.45, "sampling"); });
        QTRY_COMPARE(r.actions->progress(), 0.45);
        QVERIFY(r.actions->running());
        QVERIFY(!r.runs->running()); // CanvasRuns ignores a run of no tab
        QVERIFY(!r.runs->busy());
        QSignalSpy finished(r.actions.get(), &CanvasActions::finished);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        QVERIFY(!r.actions->busy());
        QCOMPARE(r.actions->error(), QString());
        QVERIFY(head() != m_head);
        QCOMPARE(layers(head()), 2);
        QCOMPARE(topLayer(head()), QStringLiteral("a red umbrella")); // named from the prompt
        QCOMPARE(r.runs->lastStatus(), QString());
        QCOMPARE(r.actions->history("test-fill"), QStringList{"a red umbrella"});
    }

    void cancelEndsTheRunAndWritesNothing() {
        Rig r;
        QVERIFY(press(r).value("ok").toBool());
        QTRY_COMPARE(r.jobs().size(), 1);
        QSignalSpy finished(r.actions.get(), &CanvasActions::finished);
        r.actions->cancel();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        QCOMPARE(r.actions->error(), QString()); // a cancel is not news
        QCOMPARE(head(), m_head);
    }

    void aFailedRunSaysItInPlainWords() {
        Rig r;
        QVERIFY(press(r).value("ok").toBool());
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(0), makeError("out_of_memory", "CUDA out of memory: tried to allocate 2 GiB.")); });
        QTRY_VERIFY(!r.actions->busy());
        QCOMPARE(r.actions->error(), QStringLiteral("Not enough GPU memory. Try a smaller selection."));
        QCOMPARE(r.actions->errorCode(), QStringLiteral("out_of_memory"));
        QCOMPARE(head(), m_head);
        QCOMPARE(CanvasActions::plainWords(makeError("mask_empty", "x"), "Fill"), QStringLiteral("The selection is empty. Draw one, then Generate."));
        QCOMPARE(CanvasActions::plainWords(makeError("capability_unavailable", "x"), "Generative fill"),
                 QStringLiteral("No model on this machine can run Generative fill."));
    }

    void whatCannotRunIsRefusedBeforeAnythingIsQueued() {
        Rig r;
        QCOMPARE(press(r, {{"rect", QVariantMap()}}).value("reason").toString(), QStringLiteral("Select an area first."));
        QCOMPARE(press(r, {{"rect", rect(10, 10, 4, 4)}}).value("reason").toString(), QStringLiteral("The selection is empty. Draw one, then Generate."));
        QCOMPARE(r.actions->errorCode(), QStringLiteral("mask_empty"));
        QCOMPARE(press(r, {{"prompt", ""}}).value("reason").toString(), QStringLiteral("Type what to put in the selection, then Generate."));
        // Auto: the router picks Generative fill for a selection and a prompt; with no prompt it would be Remove.
        QCOMPARE(r.actions->route(m_canvas, head(), rect(96, 96, 64, 64), true).value("action").toString(), QStringLiteral("generative-fill"));
        QCOMPARE(press(r, {{"action", "auto"}, {"prompt", ""}}).value("reason").toString(), QStringLiteral("Type what to put in the selection, then Generate."));
        QCOMPARE(press(r, {{"action", "auto"}, {"rect", QVariantMap()}, {"prompt", ""}}).value("reason").toString(),
                 QStringLiteral("Select an area first, or type what to change in the whole picture."));
        QCOMPARE(r.jobs().size(), 0);
        QVERIFY(!r.actions->busy());
    }

    // Design decision 5: the same prompt, seed, selection and head after an undo is a cache hit to the end, Write
    // included; the runner places the revision it landed again, so Generate always shows a result.
    void aFullyCachedWriteAfterAnUndoIsPlacedAgain() {
        Rig r;
        pressAndFinish(r, {{"seed", 7}});
        const QString landed = head();
        QVERIFY(landed != m_head);
        QVERIFY(store().undo(m_canvas, landed)->moved);
        QCOMPARE(head(), m_head);
        QSignalSpy finished(r.actions.get(), &CanvasActions::finished);
        QVERIFY(press(r, {{"seed", 7}}).value("ok").toBool());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(r.jobs().size(), 1); // nothing ran again
        QCOMPARE(head(), landed);     // the layer is back
        QCOMPARE(r.actions->error(), QString());
        // Read from the same revision again while the layer is still there (what slice 4's "generate again" pins): the
        // cache is right, and the bar says why nothing new came.
        QVERIFY(press(r, {{"seed", 7}, {"revision", m_head}}).value("ok").toBool());
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(head(), landed);
        QVERIFY2(r.actions->notice().startsWith("Same prompt, seed and selection as “a red umbrella”"), qPrintable(r.actions->notice()));
        // Another seed is a new layer.
        pressAndFinish(r, {{"seed", 8}});
        QCOMPARE(layers(head()), 3);
    }

    // A Quick Mask selection goes in by its hash (Region 1.2), with its bounds as the rectangle.
    void aQuickMaskSelectionIsStoredByContentAndRuns() {
        Rig r;
        MaskLayer selection;
        selection.setImageSize(QSize(256, 256));
        QVERIFY(!r.actions->storeSelection(&selection).value("ok").toBool()); // nothing painted
        selection.fillRect(100, 110, 40, 40, false); // square, as the fake answers
        const QVariantMap stored = r.actions->storeSelection(&selection);
        QVERIFY2(stored.value("ok").toBool(), qPrintable(stored.value("reason").toString()));
        QVERIFY(stored.value("hash").toString().startsWith("sha256:"));
        QCOMPARE(stored.value("width").toInt(), 40);
        QVERIFY(store().selection(stored.value("hash").toString()));
        pressAndFinish(r, {{"selection", stored.value("hash")}, {"rect", rect(100, 110, 40, 40)}});
        QCOMPARE(r.actions->error(), QString());
        QCOMPARE(layers(head()), 2);
    }

    void thePromptHistoryIsPerActionNewestFirstAndClearable() {
        Rig r;
        pressAndFinish(r, {{"prompt", "one"}});
        pressAndFinish(r, {{"prompt", "two"}});
        pressAndFinish(r, {{"prompt", "one"}});
        QCOMPARE(r.actions->history("test-fill"), (QStringList{"one", "two"}));
        QCOMPARE(r.actions->history("generative-fill"), QStringList());
        r.actions->clearHistory();
        QCOMPARE(r.actions->history("test-fill"), QStringList());
    }
};

QTEST_MAIN(TestCanvasActions)
#include "test_canvas_actions.moc"
