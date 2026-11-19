// Runs from the canvas (#19, spec 9.8, 10.3, 12.4, 12.7).
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns and RunService as main() makes
// them, keys pressed on the window, and what the nodes show read from the canvas model's
// roles (NodeList). The run thread's providers are one FakeProvider, driven from the test;
// every folder (runs, queue, lock, cache, thumbnails) is in the test's temporary folder.

#include <QBuffer>
#include <QImage>
#include <QtTest>

#include "canvas_app.h"
#include "engine/fake_provider.h"
#include "engine/queue.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::FakeProvider;
using fugue::RunService;
using fugue::RunSetup;

namespace {

// Words into a Generate Image and a Preview of it; the same again inside a group.
const QByteArray kGraph = R"({
  "fugue": 1,
  "groups": {
    "local/gen": {
      "version": "1.0.0", "name": "Gen",
      "inputs": [{"name": "prompt", "targets": ["inner.prompt"]}],
      "outputs": [{"name": "image", "from": "inner.image"}],
      "nodes": {"inner": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"seed": 1}}}
    }
  },
  "graph": {"nodes": {
    "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a cat"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "words.text"}, "seed": 5}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "gen.image"}}},
    "grp": {"type": "local/gen", "version": "1.0.0", "inputs": {"prompt": {"from": "words.text"}}},
    "show2": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "grp.image"}}}
  }},
  "layout": {"nodes": {
    "words": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}, "show": {"pos": [600, 0]},
    "grp": {"pos": [300, 300]}, "show2": {"pos": [600, 300]}
  }}
})";

QByteArray png(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

fugue::Description imageDescription() {
    fugue::OfferedModel model;
    model.handle = QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux"}};
    model.status = fugue::ModelStatus::Ready;
    fugue::CapabilityOffer offer;
    offer.capability = QStringLiteral("image.generate");
    offer.models.append(model);
    fugue::Description description;
    description.engine = QStringLiteral("fake");
    description.capabilities.append(offer);
    return description;
}

// The window with its runs. The fake provider is made on the run thread, where providers
// live, and stays the same one for every run.
struct RunApp {
    App app;
    std::shared_ptr<FakeProvider> fake;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;

    explicit RunApp(const QByteArray &graph) : app(load(graph)) {
        RunSetup setup;
        setup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
        setup.addKernels = fugue::nodes::addBuiltinKernels;
        auto *self = this;
        setup.registerProviders = [self](fugue::Registry &registry) {
            if (!self->fake)
                self->fake = std::make_shared<FakeProvider>(imageDescription());
            fugue::ProviderInstance instance;
            instance.name = QStringLiteral("This machine");
            instance.type = QStringLiteral("fake");
            return registry.setProviders({fugue::ProviderEntry{instance, self->fake}});
        };
        const QString root = app.dir.path();
        setup.runsRoot = root + "/runs";
        setup.queueDir = root + "/queue";
        setup.lockPath = root + "/engine.lock";
        setup.cacheRoot = root + "/results";
        setup.hashesPath = root + "/hashes.json";
        setup.thumbnailDir = root + "/thumbnails";
        setup.collectGarbage = false;
        service = std::make_unique<RunService>(setup);
        runs = std::make_unique<CanvasRuns>(&app.model, &app.workflows, service.get());
        runs->registerCommands(app.commands);
        app.engine.rootContext()->setContextProperty("runs", runs.get());
    }
    ~RunApp() {
        runs.reset();
        service.reset(); // stops the run thread before the fake goes
    }

    QVariant role(const QString &node, int role) const {
        const NodeList *nodes = app.model.nodes();
        for (int row = 0; row < nodes->rowCount(); ++row)
            if (nodes->data(nodes->index(row), NodeList::NodeId).toString() == node)
                return nodes->data(nodes->index(row), role);
        return {};
    }
    QString state(const QString &node) const { return role(node, NodeList::RunState).toString(); }

    // The fake's jobs, read on the run thread where it lives.
    QStringList jobs() {
        QStringList list;
        service->onRunThread([&] { list = fake ? fake->jobs() : QStringList(); });
        return list;
    }
    void onFake(const std::function<void(FakeProvider &)> &call) {
        service->onRunThread([&] { call(*fake); });
    }
};

} // namespace

class TestCanvasRun : public QObject {
    Q_OBJECT

private slots:
    // Ctrl+Enter runs the shown workflow: progress shows on the running node, a finished
    // node shows cached with its time, values and thumbnail, a failure inside a group shows
    // on the group's node with the inner node's path id, and its downstream is skipped.
    void ctrlEnterRunsWithLiveProgressValuesAndAnErrorThroughAGroup() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(r.runs->busy());
        QVERIFY(r.app.commands.contextActive("run"));

        QTRY_COMPARE(r.jobs().size(), 1); // one local GPU job at a time: gen first
        const QString first = r.jobs().first();
        QTRY_COMPARE(r.state("gen"), QStringLiteral("running"));
        r.onFake([&](FakeProvider &f) { f.progress(first, 0.5, "sampling"); });
        QTRY_COMPARE(r.role("gen", NodeList::Progress).toReal(), 0.5);
        QTRY_COMPARE(r.runs->progress(), 0.5);
        QCOMPARE(r.runs->runningNodes(), QStringList{"gen"});
        QCOMPARE(r.runs->runningWires().size(), 1); // words -> gen
        // The run pill says so, and a dot travels the wire into the running node.
        QQuickItem *pillText = item(r.app.window->contentItem(), "runPillText");
        QVERIFY(pillText);
        QTRY_VERIFY(pillText->property("text").toString().endsWith("50%"));
        QQuickItem *dots = item(r.app.window->contentItem(), "runningDots");
        QVERIFY(dots);
        QCOMPARE(dots->property("count").toInt(), 1);

        r.onFake([&](FakeProvider &f) { f.finish(first, {{"image.png", png(640, 480)}}); });
        QTRY_COMPARE(r.state("gen"), QStringLiteral("cached"));
        QVERIFY(r.role("gen", NodeList::Seconds).toReal() >= 0);
        QVERIFY(r.role("gen", NodeList::Thumbnail).toString().startsWith("file://"));
        QCOMPARE(r.role("gen", NodeList::ThumbnailInfo).toString(), QStringLiteral("640 × 480"));
        QCOMPARE(r.app.model.previewSize("gen"), QSizeF(640, 480));
        // The thumbnail is a small copy, not the result itself.
        QImage thumbnail(QUrl(r.role("gen", NodeList::Thumbnail).toString()).toLocalFile());
        QVERIFY(!thumbnail.isNull());
        QVERIFY(thumbnail.width() <= 512);
        QTRY_COMPARE(r.state("show"), QStringLiteral("cached"));
        QVERIFY(!r.role("show", NodeList::Thumbnail).toString().isEmpty());
        // The seed it used is the last value on its port.
        bool seedShown = false;
        for (const QVariant &port : r.role("gen", NodeList::Outputs).toList())
            seedShown |= port.toMap().value("name") == "seed" && port.toMap().value("value") == "5";
        QVERIFY(seedShown);

        QTRY_COMPARE(r.jobs().size(), 2); // then the group's inner node
        r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(1), fugue::makeError("node_failed", "The model fell over.")); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("failed"));
        QCOMPARE(r.state("grp"), QStringLiteral("error"));
        QVERIFY(r.role("grp", NodeList::Error).toString().startsWith("grp/inner: "));
        QCOMPARE(r.state("show2"), QStringLiteral("skipped"));
        const QVariantList errors = r.runs->errors();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toMap().value("node").toString(), QStringLiteral("grp/inner"));
        QCOMPARE(errors.first().toMap().value("shown").toString(), QStringLiteral("grp"));
        QVERIFY(!r.runs->busy());
        QVERIFY(!r.app.commands.contextActive("run"));
        QTRY_COMPARE(dots->property("count").toInt(), 0);
        // After the run the pill counts its errors; a click lists them by path id.
        pillText = item(r.app.window->contentItem(), "runPillText");
        QTRY_COMPARE(pillText->property("text").toString(), QStringLiteral("1 error"));
        QQuickItem *box = item(r.app.window->contentItem(), "runPillBox");
        QTest::mouseClick(r.app.window, Qt::LeftButton, {},
                          box->mapToScene(QPointF(box->width() / 2, box->height() / 2)).toPoint());
        QQuickItem *list = item(r.app.window->contentItem(), "runErrors");
        QTRY_VERIFY(list && list->isVisible());
        QVERIFY(r.app.warnings.isEmpty());
    }

    // 10.3 between runs: what is in the cache is cached, what would run is stale; an edit
    // upstream makes its downstream stale again.
    void nodesShowTheirCacheStateBetweenRuns() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QSignalSpy checked(r.runs.get(), &CanvasRuns::checkApplied);
        r.runs->checkNow();
        QTRY_COMPARE(checked.size(), 1);
        QCOMPARE(r.state("words"), QStringLiteral("stale"));
        QCOMPARE(r.state("gen"), QStringLiteral("stale"));

        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(r.jobs().size(), 2);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(1), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));

        r.runs->checkNow();
        QTRY_COMPARE(checked.size(), 2);
        QCOMPARE(r.state("words"), QStringLiteral("cached"));
        QCOMPARE(r.state("gen"), QStringLiteral("cached"));
        QCOMPARE(r.state("grp"), QStringLiteral("cached"));

        // A new text: everything that reads it would run.
        QVERIFY(r.app.model.widgets()->setText("words", "text", "a dog").isEmpty());
        QTRY_COMPARE(checked.size(), 3); // after the edit pause
        QCOMPARE(r.state("words"), QStringLiteral("stale"));
        QCOMPARE(r.state("gen"), QStringLiteral("stale"));
        QVERIFY(!r.role("gen", NodeList::Thumbnail).toString().isEmpty()); // the last image stays
    }

    // Escape while a run is going cancels it (12.4): the provider's job is cancelled, the
    // node that was running would run again.
    void escapeCancelsTheRun() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        QTRY_COMPARE(r.state("gen"), QStringLiteral("running"));
        r.app.key(Qt::Key_Escape);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        QCOMPARE(r.state("gen"), QStringLiteral("stale"));
        QVERIFY(!r.app.commands.contextActive("run"));
    }

    // Shift+Enter runs up to the selected node only, adding no Preview; the seed set to
    // increment is the one shown for this run and one more for the next.
    void runToHereRunsOnlyWhatTheSelectedNodeNeedsAndSeedsAdvance() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        r.app.model.widgets()->setSeedMode("gen", "seed", "increment");
        r.app.model.select({"gen"});
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        QJsonObject params;
        r.onFake([&](FakeProvider &f) { params = f.submitted(f.jobs().at(0)).params; });
        QCOMPARE(params.value("seed").toInt(), 5);
        QCOMPARE(r.app.model.widgets()->value("gen", "seed").toInt(), 6);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(r.jobs().size(), 1); // the group never ran
        QCOMPARE(r.state("show"), QString()); // not part of this run
    }

    // A run of the window waits in the queue the CLI's workers share, behind theirs.
    void aWindowRunWaitsItsTurnInTheSharedQueue() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        // A CLI run is active: its worker holds the engine lock.
        QVERIFY(queue.enqueue("cli-run", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-run"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());

        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(queue.entries().size(), 2);
        QTest::qWait(200);
        QVERIFY(r.jobs().isEmpty()); // still waiting
        QVERIFY(r.state("gen") != QLatin1String("running"));
        QVERIFY(r.runs->busy() && !r.runs->running());

        QVERIFY(queue.remove(QStringLiteral("cli-run")));
        worker.release();
        QTRY_COMPARE(r.jobs().size(), 1);
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // The demo workflow that ships (examples/demo.fugue) runs from the window: CPU nodes only.
    void theDemoRuns() {
        QFile file(QStringLiteral(FUGUE_SOURCE_DIR "/examples/demo.fugue"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        RunApp r(file.readAll());
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        for (const char *node : {"subject", "style", "prompt", "size", "show-prompt", "show-size"})
            QCOMPARE(r.state(node), QStringLiteral("cached"));
        bool shown = false;
        for (const QVariant &port : r.role("size", NodeList::Outputs).toList())
            shown |= port.toMap().value("value") == "768";
        QVERIFY(shown);
        QVERIFY(r.app.warnings.isEmpty());
    }
};

QTEST_MAIN(TestCanvasRun)
#include "test_canvas_run.moc"
