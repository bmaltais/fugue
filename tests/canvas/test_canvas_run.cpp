// Runs from the canvas (#19, spec 9.8, 10.3, 12.4, 12.7).
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns and RunService as main() makes
// them, keys pressed on the window, and what the nodes show read from the canvas model's
// roles (NodeList). The run thread's providers are one FakeProvider, driven from the test;
// every folder (runs, queue, lock, cache, thumbnails) is in the test's temporary folder.

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QImage>
#include <QJsonDocument>
#include <QTimer>
#include <QUndoStack>
#include <QtTest>

#include <atomic>

#include "canvas_app.h"
#include "engine/fake_provider.h"
#include "engine/queue.h"
#include "engine/run_folder.h"
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
    "grp": {"pos": [300, 700]}, "show2": {"pos": [600, 700]}
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

// While set, a SlowProvider's describe() never answers: a paired host that takes its whole
// transfer timeout. Set and read on the test's thread and the run thread.
std::atomic<bool> g_describeHangs{false};

// A provider whose description takes a while, as a real one over HTTP does: checkGraph waits
// for it on an event loop of its own.
class SlowProvider : public FakeProvider {
public:
    using FakeProvider::FakeProvider;
    void describe(fugue::Reply<fugue::Description> reply) override {
        if (g_describeHangs.load()) {
            m_unanswered.append(std::move(reply));
            return;
        }
        QTimer::singleShot(300, this, [this, reply = std::move(reply)]() mutable { FakeProvider::describe(std::move(reply)); });
    }
    // A cancel is answered from the event loop, as over HTTP, never inside the call.
    void cancel(const QString &job, fugue::Reply<QString> reply) override {
        QTimer::singleShot(0, this, [this, job, reply = std::move(reply)]() mutable { FakeProvider::cancel(job, std::move(reply)); });
    }

private:
    QList<fugue::Reply<fugue::Description>> m_unanswered;
};

// Until it goes, describe() hangs.
struct DescribeHangs {
    DescribeHangs() { g_describeHangs.store(true); }
    ~DescribeHangs() { g_describeHangs.store(false); }
};

// The window with its runs. The fake provider is made on the run thread, where providers
// live, and stays the same one for every run.
struct RunApp {
    App app;
    std::shared_ptr<FakeProvider> fake;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;

    explicit RunApp(const QByteArray &graph, bool slow = false, const fugue::Description &offers = imageDescription())
        : app(load(graph)) {
        RunSetup setup;
        setup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
        setup.addKernels = fugue::nodes::addBuiltinKernels;
        auto *self = this;
        setup.registerProviders = [self, slow, offers](fugue::Registry &registry) {
            if (!self->fake)
                self->fake = slow ? std::make_shared<SlowProvider>(offers)
                                  : std::make_shared<FakeProvider>(offers);
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
        runs = std::make_unique<CanvasRuns>(&app.model, &app.workflows, service.get(), &app.registry);
        runs->registerCommands(app.commands);
        app.runs = runs.get();
    }
    ~RunApp() { close(); }
    // The window goes before the runs it shows, as in main(), which makes its engine last.
    // The test's folder stays until the RunApp goes.
    void close() {
        if (!service)
            return;
        qDeleteAll(app.engine.rootObjects());
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

// Leaves a check of the window waiting on a describe() that never answers. The checks
// asked before it (the one when the window opened: 300 ms pause, 300 ms describe) are over
// first, so this one is the one that hangs.
void hangACheck(RunApp &r) {
    QTest::qWait(1000);
    const DescribeHangs hangs;
    r.runs->checkNow();
    QTest::qWait(200); // the check is waiting on describe
}

} // namespace

const QByteArray kLoadImage = R"({
  "fugue": 1,
  "graph": {"nodes": {"src": {"type": "fugue/load-image", "version": "1.0.0"}}},
  "layout": {"nodes": {"src": {"pos": [100, 100]}}, "view": {"center": [300, 300], "zoom": 1}}
})";

// Generate Image into a Preview and a Save Image (into the test's folder), and a number shown.
QByteArray sinksGraph(const QString &folder) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a cat"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "words.text"}, "seed": 5}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "gen.image"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0",
             "inputs": {"image": {"from": "gen.image"}, "name": "cat", "folder": "%1", "embed": "none"}},
    "size": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 768}},
    "show-size": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "size.value"}}}
  }},
  "layout": {"nodes": {"words": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}, "show": {"pos": [600, 0]},
                       "save": {"pos": [600, 300]}, "size": {"pos": [0, 300]}, "show-size": {"pos": [300, 300]}},
             "view": {"center": [400, 300], "zoom": 0.6}}
})").arg(folder).toUtf8();
}

class TestCanvasRun : public QObject {
    Q_OBJECT

    QTemporaryDir pictures;

private slots:
    void initTestCase() {
        // Pasted images go to <pictures>/Fugue/inputs (paths.h inputsDir).
        qputenv("XDG_PICTURES_DIR", pictures.path().toUtf8());
    }

    // Load Image shows its file before any run (spec 12.7): a paste puts the picture on the
    // node, undo takes it away again.
    void pastingIntoLoadImageShowsItsPictureAndUndoTakesItAway() {
        RunApp r(kLoadImage);
        QVERIFY(r.app.open());
        QCOMPARE(r.role("src", NodeList::Thumbnail).toString(), QString());
        r.app.model.select({"src"});
        QImage picture(64, 32, QImage::Format_RGB32);
        picture.fill(Qt::red);
        auto *data = new QMimeData;
        data->setImageData(picture);
        QGuiApplication::clipboard()->setMimeData(data);
        const qreal height = r.role("src", NodeList::Height).toReal();
        r.app.key(Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(r.role("src", NodeList::Thumbnail).toString().startsWith("file://"));
        QCOMPARE(r.role("src", NodeList::ThumbnailInfo).toString(), QStringLiteral("64 \u00d7 32"));
        QVERIFY(r.role("src", NodeList::Height).toReal() > height); // the node grew to hold it
        QQuickItem *card = item(r.app.window->contentItem(), "node-src");
        QVERIFY(card);
        QTRY_VERIFY(item(card, "thumbnail") && item(card, "thumbnail")->property("active").toBool());

        r.app.key(Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(r.role("src", NodeList::Thumbnail).toString(), QString());
        QTRY_COMPARE(r.role("src", NodeList::Height).toReal(), height);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // A node nothing can serve says so before any run, and how to fix it (8.5); a run that
    // reaches it fails with the same words.
    void aNodeNoProviderCanServeShowsWhatIsMissing() {
        RunApp r(kGraph, false, fugue::Description{});
        QVERIFY(r.app.open());
        QTRY_VERIFY(r.role("gen", NodeList::Notice).toString().contains("Nothing can serve image.generate right now."));
        QVERIFY(r.role("gen", NodeList::Notice).toString().contains("add a provider"));
        QVERIFY(r.role("words", NodeList::Notice).toString().isEmpty());
        QQuickItem *card = item(r.app.window->contentItem(), "node-gen");
        QVERIFY(card);
        QTRY_VERIFY(item(card, "noticeChip") && item(card, "noticeChip")->isVisible());

        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.state("gen"), QString("error"));
        QVERIFY(r.role("gen", NodeList::Error).toString().contains("add a provider"));
        QVERIFY(!item(card, "noticeChip")->isVisible()); // the error chip says it now
        QCOMPARE(r.role("gen", NodeList::Notice).toString(), QString());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // A click on a node's error chip (4.7) opens the run pill's error list and brings the node
    // to the middle of the view, selected, as a click on its row in the list does.
    void aClickOnAnErrorChipListsTheErrorsAndShowsTheNode() {
        RunApp r(kGraph, false, fugue::Description{});
        QVERIFY(r.app.open());
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.state("gen"), QString("error"));
        r.app.model.select({"words"});
        QQuickItem *card = item(r.app.window->contentItem(), "node-gen");
        QVERIFY(card);
        QQuickItem *chip = nullptr;
        QTRY_VERIFY((chip = item(card, "errorChip")) && chip->isVisible());
        QQuickItem *list = item(r.app.window->contentItem(), "runErrors");
        QVERIFY(!list || !list->isVisible());
        const QPointF before = r.app.pan();
        { QQuickItem *g = item(r.app.window->contentItem(), "node-grp");
          qWarning() << "DBG chip" << chip->mapRectToScene(QRectF(0,0,chip->width(),chip->height())) << "grp" << (g ? g->mapRectToScene(QRectF(0,0,g->width(),g->height())) : QRectF()) << "gen" << card->mapRectToScene(QRectF(0,0,card->width(),card->height())); }

        QTest::mouseClick(r.app.window, Qt::LeftButton, {},
                          chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_VERIFY((list = item(r.app.window->contentItem(), "runErrors")) && list->isVisible());
        QCOMPARE(r.app.model.selectedIds(), QStringList{"gen"});
        QVERIFY(r.app.pan() != before);
        // The node's header is in the middle of the canvas now.
        const QPointF header = r.app.model.position("gen") + QPointF(30 * r.app.model.unit(), 10 * r.app.model.unit());
        const QPoint middle = r.app.at(r.app.canvas->width() / 2, r.app.canvas->height() / 2);
        QVERIFY2((r.app.screen(header.x(), header.y()) - middle).manhattanLength() <= 2,
                 qPrintable(QStringLiteral("%1,%2").arg(r.app.screen(header.x(), header.y()).x()).arg(middle.x())));
        QCOMPARE(r.app.model.undoStack()->count(), 0); // showing is not an edit
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The run's state of `node` in run `run` of this window, from its run folder.
    static QJsonObject ranNode(const RunApp &r, const QString &run, const QString &node, QString *status = nullptr) {
        const fugue::Result<fugue::RunFolder> folder = fugue::RunFolder::open(r.app.dir.path() + "/runs", run);
        if (!folder.ok())
            return {};
        if (status)
            *status = folder->state().value("status").toString();
        return folder->state().value("nodes").toObject().value(node).toObject();
    }

    // A saved workflow keeps its images in `inputs` beside it under relative paths (Appendix A),
    // so its folder can move or be shared: reopened from where it went, it runs on the same
    // images with the same keys, shows them before the run, and is not changed by opening.
    void aSavedWorkflowsFolderMovesWithItsImages() {
        const QByteArray graph = R"({
  "fugue": 1,
  "graph": {"nodes": {"src": {"type": "fugue/load-image", "version": "1.0.0"},
                      "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "src.image"}}}}},
  "layout": {"nodes": {"src": {"pos": [100, 100]}, "show": {"pos": [400, 100]}}, "view": {"center": [300, 300], "zoom": 1}}
})";
        QTemporaryDir place;
        QVERIFY(QDir().mkpath(place.filePath("first")));
        QString key;
        QString name;
        {
            RunApp r(graph);
            QVERIFY(r.app.open());
            r.app.model.select({"src"});
            auto *data = new QMimeData;
            data->setData("image/png", png(20, 10));
            QGuiApplication::clipboard()->setMimeData(data);
            r.app.key(Qt::Key_V, Qt::ControlModifier);
            QVERIFY(r.app.workflows.saveAs(place.filePath("first/flow.fugue")));
            const QString file = r.app.model.document().graph.nodes.value("src").inputs.value("file").literal().toString();
            QVERIFY2(file.startsWith("inputs/") && !file.contains(place.path()), qPrintable(file));
            name = file.mid(7);
            QVERIFY(QFileInfo(place.filePath("first/inputs/" + name)).isFile());

            QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
            r.app.key(Qt::Key_Return, Qt::ControlModifier);
            QTRY_COMPARE(finished.size(), 1);
            QString status;
            key = ranNode(r, finished.first().at(0).toString(), "src", &status).value("key").toString();
            QCOMPARE(status, QString("ok"));
            QVERIFY(!key.isEmpty());
        }
        QFile saved(place.filePath("first/flow.fugue"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray bytes = saved.readAll();
        saved.close();
        QVERIFY(QDir(place.path()).rename("first", "moved"));

        RunApp r(kLoadImage); // a node of the same id in the tab it opens beside
        QVERIFY(r.app.open());
        QVERIFY(r.app.workflows.openPath(place.filePath("moved/flow.fugue")));
        QCOMPARE(r.app.model.document().graph.nodes.value("src").inputs.value("file").literal().toString(), "inputs/" + name);
        QVERIFY(!r.app.workflows.unsaved());
        // Its picture shows before any run, from the moved folder.
        QTRY_VERIFY(r.role("src", NodeList::Thumbnail).toString().startsWith("file://"));
        QCOMPARE(r.role("src", NodeList::ThumbnailInfo).toString(), QStringLiteral("20 × 10"));
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        QString status;
        QCOMPARE(ranNode(r, finished.first().at(0).toString(), "src", &status).value("key").toString(), key);
        QCOMPARE(status, QString("ok"));
        QVERIFY(r.app.runs->errors().isEmpty());
        // Saving it where it is now writes the same bytes: nothing was made absolute.
        QVERIFY(r.app.workflows.save());
        QFile again(place.filePath("moved/flow.fugue"));
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), bytes);
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // An unsaved workflow has no folder: a relative file is an error before a run, and the run
    // says why.
    void aRelativeFileInAnUnsavedWorkflowSaysWhy() {
        RunApp r(kLoadImage);
        QVERIFY(r.app.open());
        r.app.workflows.newWorkflow();
        QVERIFY(r.app.workflows.openText(QString::fromUtf8(R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "inputs/a.png"}},
            "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "src.image"}}}}}})")));
        QTRY_COMPARE(r.state("src"), QStringLiteral("error"));
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(r.state("src"), QStringLiteral("error"));
        QVERIFY2(r.role("src", NodeList::Error).toString().contains("not saved"), qPrintable(r.role("src", NodeList::Error).toString()));
    }

    // A file that is not there shows a quiet placeholder, not an error.
    void aMissingFileShowsAPlaceholder() {
        RunApp r(kLoadImage);
        QVERIFY(r.app.open());
        QVERIFY(r.app.model.widgets()->setText("src", "file", r.app.dir.filePath("gone.png")).isEmpty());
        QTRY_COMPARE(r.role("src", NodeList::ThumbnailText).toString(), QStringLiteral("File not found"));
        QVERIFY(r.role("src", NodeList::ThumbnailDim).toBool());
        QVERIFY(r.app.warnings.isEmpty());
    }

    // Preview and Save Image show a dimmed "No result yet" before a run, then the image (Save:
    // with the file it wrote); a Preview of a number shows the number.
    void previewAndSaveImageShowTheirResultInTheSlot() {
        QTemporaryDir out;
        RunApp r(sinksGraph(out.path()));
        QVERIFY(r.app.open());
        QCOMPARE(r.role("show", NodeList::ThumbnailText).toString(), QStringLiteral("No result yet"));
        QVERIFY(r.role("show", NodeList::ThumbnailDim).toBool());
        QCOMPARE(r.role("save", NodeList::ThumbnailText).toString(), QStringLiteral("No result yet"));

        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(320, 240)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        QVERIFY(r.role("show", NodeList::Thumbnail).toString().startsWith("file://"));
        QCOMPARE(r.role("show", NodeList::ThumbnailInfo).toString(), QStringLiteral("320 \u00d7 240")); // never a cache name
        QCOMPARE(r.role("show", NodeList::ThumbnailText).toString(), QString());
        QVERIFY(!r.role("show", NodeList::ThumbnailDim).toBool());
        QVERIFY(r.role("save", NodeList::Thumbnail).toString().startsWith("file://"));
        const QString info = r.role("save", NodeList::ThumbnailInfo).toString();
        QCOMPARE(info, QStringLiteral("320 \u00d7 240"));
        QVERIFY(r.role("save", NodeList::ThumbnailFile).toString().startsWith("cat"));
        QVERIFY(r.role("show", NodeList::ThumbnailFile).toString().isEmpty());
        QCOMPARE(r.role("show-size", NodeList::ThumbnailText).toString(), QStringLiteral("768"));
        QVERIFY(r.app.warnings.isEmpty());
    }


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
        // What depends on it was skipped: the run's record says so (the canvas shows that only
        // until the check after the run makes it stale, 10.3, which a busy machine may reach
        // before this line).
        const fugue::Result<fugue::RunFolder> folder =
            fugue::RunFolder::open(r.app.dir.path() + "/runs", finished.first().at(0).toString());
        QVERIFY(folder.ok());
        QCOMPARE(folder->state().value("nodes").toObject().value("show2").toObject().value("state").toString(),
                 QStringLiteral("skipped"));
        // The check after the run keeps the error: the seed advanced at the start is no edit
        // of the user's, and the run is newer than it.
        QSignalSpy checked(r.runs.get(), &CanvasRuns::checkApplied);
        r.runs->checkNow();
        QTRY_VERIFY(checked.size() >= 1);
        QCOMPARE(r.state("grp"), QStringLiteral("error"));
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

    // An edit's check that a newer check replaces before it is answered (only the latest is
    // answered) still counts as the edit's: the last run's error goes.
    void anEditsCheckReplacedByANewerOneStillClearsTheLastRunsErrors() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.fail(f.jobs().at(0), fugue::makeError("node_failed", "The model fell over.")); });
        QTRY_COMPARE(r.jobs().size(), 2);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(1), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(r.state("gen"), QStringLiteral("error"));

        QSignalSpy checked(r.runs.get(), &CanvasRuns::checkApplied);
        QVERIFY(r.app.model.widgets()->setText("words", "text", "a dog").isEmpty());
        r.runs->checkNow(); // the edit's
        r.runs->checkNow(); // replaces it before the run thread answers it
        QTRY_VERIFY(checked.size() >= 1);
        QCOMPARE(r.state("gen"), QStringLiteral("stale"));
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
        // Not part of this run: Preview has no result, and a check says it would run. (The
        // check after the seed edit's pause may land before or after the run ends, so its
        // state is read after a check of its own.)
        QVERIFY(r.state("show") != QLatin1String("cached") && r.state("show") != QLatin1String("running"));
        r.runs->checkNow();
        QTRY_COMPARE(r.state("show"), QStringLiteral("stale"));
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

    // A run started while a check waits on a slow provider waits for the check, then runs.
    void aRunStartedDuringACheckRunsAfterIt() {
        RunApp r(kGraph, true);
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.runs->checkNow();
        QTest::qWait(50); // the check is waiting on describe
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(r.jobs().size(), 1, 10000);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE_WITH_TIMEOUT(r.jobs().size(), 2, 10000);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(1), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
    }

    // A run asked for while a check waits on a provider that does not answer starts at once:
    // the check gives way, and is made again later.
    void aRunAskedForDuringAHangingCheckDoesNotWaitForIt() {
        RunApp r(kGraph, true);
        QVERIFY(r.app.open());
        hangACheck(r);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(r.jobs().size(), 1, 5000);
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // A window run whose turn comes during a check that hangs starts at once, so the CLI runs
    // queued behind it do not wait either.
    void aWindowRunsTurnDuringAHangingCheckDoesNotWaitForIt() {
        RunApp r(kGraph, true);
        QVERIFY(r.app.open());
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        QVERIFY(queue.enqueue("cli-run", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-run"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(queue.entries().size(), 2);
        hangACheck(r);
        QVERIFY(queue.remove(QStringLiteral("cli-run")));
        worker.release();
        QTRY_COMPARE_WITH_TIMEOUT(r.jobs().size(), 1, 5000);
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // A window run whose entry goes just as its turn comes (a `fugue runs cancel` between the
    // window reading the queue and activating its entry) leaves the engine lock free: here
    // the entry cannot be activated at all.
    void aTurnThatCannotBeTakenLeavesTheEngineLockFree() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        const QString queueDir = r.app.dir.path() + "/queue";
        const fugue::Queue queue(queueDir);
        QVERIFY(queue.enqueue("cli-run", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-run"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(queue.entries().size(), 2);
        QVERIFY(queue.remove(QStringLiteral("cli-run")));
        // No entry in the folder can be renamed (activated) from now on.
        const QFileDevice::Permissions was = QFile::permissions(queueDir);
        QVERIFY(QFile::setPermissions(queueDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        worker.release();
        QTest::qWait(200); // the window has tried its turn a few times
        bool free = false;
        for (int i = 0; i < 20 && !free; ++i) {
            free = worker.tryAcquire();
            if (!free)
                QTest::qWait(20);
        }
        worker.release();
        QVERIFY(QFile::setPermissions(queueDir, was));
        QVERIFY(free);
        QVERIFY(r.jobs().isEmpty());
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // `fugue runs cancel` of a waiting window run takes its entry, then ends its folder. A
    // window that looks in between ends it as cancelled, never with the status it had.
    void aWaitingRunCancelledFromOutsideEndsCancelled() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        QVERIFY(queue.enqueue("cli-run", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-run"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(queue.entries().size(), 2);
        const QString run = r.runs->runs().value(0);
        QVERIFY(queue.remove(run)); // and the canceller has not ended the folder yet
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        const fugue::Result<fugue::RunFolder> folder = fugue::RunFolder::open(r.app.dir.path() + "/runs", run);
        QVERIFY(folder.ok());
        QCOMPARE(fugue::runStatusName(folder->status()), QStringLiteral("cancelled"));
        QVERIFY(queue.remove(QStringLiteral("cli-run")));
    }

    // Closing the window while a check waits on a provider neither hangs nor crashes.
    void closingDuringACheckIsClean() {
        auto r = std::make_unique<RunApp>(kGraph, true);
        QVERIFY(r->app.open());
        r->runs->checkNow();
        QTest::qWait(50);
        r.reset();
    }

    // Closing the window while a node runs cancels the run, which ends as cancelled: its
    // folder says so (run.done, `ended`), the queue is empty (`fugue runs` lists nothing) and
    // the file-hash memo is saved.
    void closingDuringARunIsClean() {
        auto r = std::make_unique<RunApp>(kGraph, true);
        QVERIFY(r->app.open());
        r->app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r->state("gen"), QStringLiteral("running"));
        const QString run = r->runs->runs().value(0);
        QVERIFY(!run.isEmpty());
        const QString root = r->app.dir.path();
        r->close();

        const fugue::Result<fugue::RunFolder> folder = fugue::RunFolder::open(root + "/runs", run);
        QVERIFY(folder.ok());
        QCOMPARE(fugue::runStatusName(folder->status()), QStringLiteral("cancelled"));
        QVERIFY(folder->state().contains("ended"));
        QCOMPARE(folder->state().value("status").toString(), QStringLiteral("cancelled"));
        const QList<QJsonObject> events = folder->events();
        QVERIFY(std::any_of(events.cbegin(), events.cend(),
                            [](const QJsonObject &e) { return e.value("event") == QLatin1String("run.done"); }));
        QVERIFY(fugue::Queue(root + "/queue").entries().isEmpty());
        QVERIFY(QFileInfo::exists(root + "/hashes.json"));
        r.reset();
    }

    // Escape cancels a run only when nothing else wants it: open node search takes it first.
    void escapeClosesSearchBeforeItCancelsTheRun() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.state("gen"), QStringLiteral("running"));
        QObject *popup = r.app.engine.rootObjects().first()->findChild<QObject *>("nodeSearch");
        QVERIFY(popup);
        QTest::mouseMove(r.app.window, r.app.screen(100, 500));
        r.app.key(Qt::Key_Tab);
        QTRY_VERIFY(popup->property("opened").toBool());
        r.app.key(Qt::Key_Escape);
        // Closed, its 90 ms fade (docs/canvas-look.md 15) done: keys during the fade are still
        // the popup's, and a busy machine stretched a fixed 100 ms wait past it.
        QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY(r.runs->running());
        QCOMPARE(r.state("gen"), QStringLiteral("running"));
        r.app.key(Qt::Key_Escape); // now it is the run's
        QTRY_VERIFY(!r.runs->busy());
    }

    // A frozen node (10.4) shows frozen, between runs and after one.
    void aFrozenNodeShowsFrozen() {
        QFile file(QStringLiteral(FUGUE_SOURCE_DIR "/examples/demo.fugue"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        RunApp r(file.readAll());
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        const QString run = finished.first().at(0).toString();
        const fugue::Result<fugue::RunFolder> folder = fugue::RunFolder::open(r.app.dir.path() + "/runs", run);
        QVERIFY(folder.ok());
        const QString key = folder->state().value("nodes").toObject().value("subject").toObject().value("key").toString();
        QVERIFY(!key.isEmpty());

        fugue::Document document = r.app.model.document();
        document.graph.nodes["subject"].frozen = key;
        r.app.model.setDocument(document);
        // The check of the new document says so. (A check due from before may land first with
        // the old states, so wait for the state, not for any check.)
        QTRY_COMPARE(r.state("subject"), QStringLiteral("frozen"));
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(r.state("subject"), QStringLiteral("frozen"));
        QCOMPARE(r.state("prompt"), QStringLiteral("cached"));
    }

    // Run first (Ctrl+Shift+Enter) goes ahead of the waiting runs, the CLI's included.
    void runFirstGoesAheadOfTheWaitingRuns() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        QVERIFY(queue.enqueue("cli-active", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-active"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        QVERIFY(queue.enqueue("cli-waiting", "/w/b.fugue", false));

        r.app.key(Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(queue.entries().size(), 3);
        const QList<fugue::QueueEntry> entries = queue.entries();
        QCOMPARE(entries.at(0).run, QStringLiteral("cli-active"));
        QCOMPARE(entries.at(1).run, r.runs->runs().value(0));
        QCOMPARE(entries.at(2).run, QStringLiteral("cli-waiting"));
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // 300 nodes take their states from one check without holding the window up: the UI
    // thread only applies them, and only to nodes whose state changed.
    void threeHundredNodesTakeTheirStatesQuickly() {
        QJsonObject nodes, layout;
        for (int i = 0; i < 150; ++i) {
            const QString n = QStringLiteral("n%1").arg(i), p = QStringLiteral("p%1").arg(i);
            nodes.insert(n, QJsonObject{{"type", "fugue/number"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"value", i}}}});
            nodes.insert(p, QJsonObject{{"type", "fugue/preview"}, {"version", "1.0.0"},
                                        {"inputs", QJsonObject{{"value", QJsonObject{{"from", n + ".value"}}}}}});
            layout.insert(n, QJsonObject{{"pos", QJsonArray{(i % 10) * 600, (i / 10) * 200}}});
            layout.insert(p, QJsonObject{{"pos", QJsonArray{(i % 10) * 600 + 300, (i / 10) * 200}}});
        }
        const QJsonObject document{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}},
                                   {"layout", QJsonObject{{"nodes", layout}}}};
        RunApp r(QJsonDocument(document).toJson());
        QVERIFY(r.app.open());
        QSignalSpy checked(r.runs.get(), &CanvasRuns::checkApplied);
        // CPU time of the whole process, the run thread's check included (tests/cpu_time.h).
        qint64 start = test_support::cpuMs();
        r.runs->checkNow();
        QTRY_COMPARE(checked.size(), 1);
        const qint64 first = test_support::cpuMs() - start;
        QCOMPARE(r.state("p149"), QStringLiteral("stale"));
        start = test_support::cpuMs();
        r.runs->checkNow(); // the same states again: nothing to lay out
        QTRY_COMPARE(checked.size(), 2);
        qInfo("first check of 300 nodes applied in %lld ms CPU, the same again in %lld ms", first,
              test_support::cpuMs() - start);
        QVERIFY(first < 1500);
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
