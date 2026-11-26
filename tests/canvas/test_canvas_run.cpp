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
#include <QProcess>
#include <QTemporaryDir>
#include <QImage>
#include <QJsonDocument>
#include <QTimer>
#include <QUndoStack>
#include <QtTest>

#include <atomic>

#include "canvas_app.h"
#include "../disk_pack.h"
#include "../test_home_guard.h"
#include "engine/fake_provider.h"
#include "engine/pack_loader.h"
#include "engine/trust_store.h"
#include "engine/queue.h"
#include "engine/run_folder.h"
#include "run/canvas_runs.h"
#include "run/run_bar.h"
#include "../window_grab.h"
#include "../readme_shot.h"
#include "run/run_queue.h"
#include "run/run_service.h"
#include "../test_shard.h"

using namespace canvas_test;
using pack_fixture::writeSayPack;
using pack_fixture::writeToolsPack;
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

// An Expression whose Number goes to a Preview, and one that nothing reads.
const QByteArray kFormula = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "calc": {"type": "fugue/expression", "version": "1.0.0", "inputs": {"formula": "a + b"}},
    "loose": {"type": "fugue/expression", "version": "1.0.0", "inputs": {"formula": "1 +"}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "calc.number"}}}
  }},
  "layout": {"nodes": {"calc": {"pos": [0, 0]}, "show": {"pos": [300, 0]}, "loose": {"pos": [0, 300]}}}
})";

// A Generate Image, a Resize of it and a Preview of that: the middle one only repeats the picture before it.
const QByteArray kChain = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a cat"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "words.text"}, "seed": 5}},
    "rz": {"type": "fugue/resize", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "width": 160, "height": 120}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "rz.image"}}}
  }},
  "layout": {"nodes": {"words": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}, "rz": {"pos": [600, 0]}, "show": {"pos": [900, 0]}}}
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
    std::unique_ptr<fugue::RunBar> bar;
    std::unique_ptr<fugue::RunQueue> queue;

    // `address` makes the one provider instance an http service at that address ("Cloud"), off this machine when it is public (#404).
    explicit RunApp(const QByteArray &graph, bool slow = false, const fugue::Description &offers = imageDescription(),
                    const Look &look = {}, const QString &address = {})
        : app(load(graph), {}, look) {
        RunSetup setup;
        // As main() fills it: the built-ins, then the test's own (XDG) packs folder.
        setup.addNodeTypes = [](fugue::Registry &registry) {
            fugue::nodes::addBuiltinNodeTypes(registry);
            fugue::loadInstalledPacks(registry);
        };
        setup.addKernels = fugue::nodes::addBuiltinKernels;
        auto *self = this;
        setup.registerProviders = [self, slow, offers, address](fugue::Registry &registry) {
            if (!self->fake)
                self->fake = slow ? std::make_shared<SlowProvider>(offers)
                                  : std::make_shared<FakeProvider>(offers);
            fugue::ProviderInstance instance;
            instance.name = address.isEmpty() ? QStringLiteral("This machine") : QStringLiteral("Cloud");
            instance.type = address.isEmpty() ? QStringLiteral("fake") : QStringLiteral("http");
            instance.address = address;
            return registry.setProviders({fugue::ProviderEntry{instance, self->fake}});
        };
        const QString root = app.dir.path();
        setup.runsRoot = root + "/runs";
        setup.queueDir = root + "/queue";
        setup.lockPath = root + "/engine.lock";
        setup.cacheRoot = root + "/results";
        setup.hashesPath = root + "/hashes.json";
        setup.consentPath = root + "/remote-consent.json";
        setup.thumbnailDir = root + "/thumbnails";
        setup.collectGarbage = false;
        service = std::make_unique<RunService>(setup);
        runs = std::make_unique<CanvasRuns>(&app.model, &app.workflows, service.get(), &app.registry);
        runs->registerCommands(app.commands);
        app.runs = runs.get();
        // The run toolbar and the queue panel, as main() makes them, with the test's own folders.
        bar = std::make_unique<fugue::RunBar>(runs.get(), root + "/run-bar.json", &app.chrome);
        bar->registerCommands(app.commands);
        queue = std::make_unique<fugue::RunQueue>(fugue::RunQueuePaths{root + "/runs", root + "/queue", root + "/engine.lock"},
                                                  service.get());
        queue->setOpenUrl([self](const QUrl &url) { self->openedFolders << url; });
        app.runBar = bar.get();
        app.runQueue = queue.get();
    }
    QList<QUrl> openedFolders;
    ~RunApp() { close(); }
    // The window goes before the runs it shows, as in main(), which makes its engine last.
    // The test's folder stays until the RunApp goes.
    void close() {
        if (!service)
            return;
        qDeleteAll(app.engine.rootObjects());
        queue.reset();
        bar.reset();
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

// Leaves a check of the window waiting on a describe() that never answers. A check asked
// and answered first (it replaces the one the open and the edit pause asked) is over, so
// this one is the one that hangs. With a run waiting for its turn a check is never applied
// (a run since it was asked knows better), so there the pause and the describe (300 ms each)
// are waited out.
void hangACheck(RunApp &r, bool runWaiting = false) {
    if (runWaiting) {
        QTest::qWait(700);
    } else {
        QSignalSpy applied(r.runs.get(), &fugue::CanvasRuns::checkApplied);
        r.runs->checkNow();
        QVERIFY(applied.wait(5000));
    }
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

// A Load Video and a Load Audio into Save Video, Save Audio and a Preview of the clip; the files are
// made with ffmpeg (no media is committed) and the saves go into the test's folder.
QByteArray mediaGraph(const QString &clip, const QString &song, const QString &folder) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "clip": {"type": "fugue/load-video", "version": "1.0.0", "inputs": {"file": "%1"}},
    "song": {"type": "fugue/load-audio", "version": "1.0.0", "inputs": {"file": "%2"}},
    "save-clip": {"type": "fugue/save-video", "version": "1.0.0",
                  "inputs": {"video": {"from": "clip.video"}, "name": "fox", "format": "webm", "folder": "%3", "embed": "none"}},
    "save-song": {"type": "fugue/save-audio", "version": "1.0.0",
                  "inputs": {"audio": {"from": "song.audio"}, "name": "tune", "format": "flac", "folder": "%3", "embed": "none"}},
    "show-clip": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "clip.video"}}}
  }},
  "layout": {"nodes": {"clip": {"pos": [0, 0]}, "song": {"pos": [0, 300]}, "save-clip": {"pos": [300, 0]},
                       "save-song": {"pos": [300, 300]}, "show-clip": {"pos": [600, 0]}},
             "view": {"center": [400, 300], "zoom": 0.6}}
})").arg(clip, song, folder).toUtf8();
}

bool makeMedia(const QStringList &args) {
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", QStringList{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"} + args);
    return ffmpeg.waitForFinished(60000) && ffmpeg.exitCode() == 0;
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

    // Node pictures (#263): Smart shows no picture on a node that only changes the one before it, and its thumbnail is
    // not made; the node's own Show picture brings it back (stored in the layout) and Hide picture takes one away; the
    // View > Picture commands set the mode.
    void aTransformShowsNoPictureUnderSmartAndAnOverrideOrTheModeBringsItBack() {
        RunApp r(kChain);
        QVERIFY(r.app.open());
        QSignalSpy events(r.service.get(), &RunService::event);
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        const auto run = [&](int count) {
            r.app.key(Qt::Key_Return, Qt::ControlModifier);
            QTRY_COMPARE(finished.size(), count);
            QCOMPARE(finished.last().at(1).toString(), QStringLiteral("ok"));
        };
        const auto thumbnailsOf = [&](const QString &node) {
            QJsonObject found;
            for (const QList<QVariant> &args : events) {
                const QJsonObject e = args.at(1).toJsonObject();
                if (e.value("node").toString() == node && e.value("event").toString().startsWith("node.done"))
                    found = e.value("thumbnails").toObject();
            }
            return found;
        };
        const auto height = [&](const QString &node) { return r.role(node, NodeList::Height).toReal(); };
        const auto layoutPicture = [&](const QString &node) {
            return r.app.model.document().layout.value("nodes").toObject().value(node).toObject().value("picture").toString();
        };

        QTRY_VERIFY(r.app.commands.trigger("view.picture-smart"));
        QCOMPARE(r.app.model.pictureMode(), fugue::PictureMode::Smart);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(320, 240)}}); });
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(r.role("gen", NodeList::Thumbnail).toString().startsWith("file://"));
        QVERIFY(r.role("show", NodeList::Thumbnail).toString().startsWith("file://"));
        QCOMPARE(r.role("rz", NodeList::Thumbnail).toString(), QString());
        QVERIFY(!thumbnailsOf("gen").isEmpty());
        QVERIFY(thumbnailsOf("rz").isEmpty()); // not made at all
        const qreal bare = height("rz");

        // Show picture: stored in the layout; the picture comes with the next run (a cached one is quick).
        r.app.model.select({"rz"});
        QVERIFY(r.app.commands.isEnabled("node.picture-show"));
        QVERIFY(!r.app.commands.isEnabled("node.picture-hide"));
        QVERIFY(!r.app.commands.isEnabled("node.picture-default"));
        QVERIFY(r.app.commands.trigger("node.picture-show"));
        QCOMPARE(layoutPicture("rz"), QString("show"));
        QVERIFY(!r.app.commands.isEnabled("node.picture-show"));
        QVERIFY(r.app.commands.isEnabled("node.picture-default"));
        run(2);
        QVERIFY(r.role("rz", NodeList::Thumbnail).toString().startsWith("file://"));
        QVERIFY(height("rz") > bare);

        // Hide picture on Generate Image: its picture goes at once, the node is shorter.
        const qreal tall = height("gen");
        r.app.model.select({"gen"});
        QVERIFY(r.app.commands.trigger("node.picture-hide"));
        QCOMPARE(layoutPicture("gen"), QString("hide"));
        QTRY_COMPARE(r.role("gen", NodeList::Thumbnail).toString(), QString());
        QVERIFY(height("gen") < tall);
        QVERIFY(r.app.commands.trigger("node.picture-default"));
        QVERIFY(layoutPicture("gen").isEmpty());
        QTRY_VERIFY(r.role("gen", NodeList::Thumbnail).toString().startsWith("file://")); // what it showed, back

        // Default again: Smart hides the transform's picture.
        r.app.model.select({"rz"});
        QVERIFY(r.app.commands.trigger("node.picture-default"));
        QTRY_COMPARE(r.role("rz", NodeList::Thumbnail).toString(), QString());
        QCOMPARE(height("rz"), bare);
        r.app.key(Qt::Key_Z, Qt::ControlModifier); // undo: one step
        QCOMPARE(layoutPicture("rz"), QString("show"));
        QTRY_VERIFY(r.role("rz", NodeList::Thumbnail).toString().startsWith("file://"));
        r.app.key(Qt::Key_Z, Qt::ControlModifier); // the step before: Generate Image's Default picture
        QCOMPARE(layoutPicture("gen"), QString("hide"));
        QTRY_COMPARE(r.role("gen", NodeList::Thumbnail).toString(), QString());
        r.app.key(Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(r.role("gen", NodeList::Thumbnail).toString().startsWith("file://"));
        r.app.model.select({"rz"});
        QVERIFY(r.app.commands.trigger("node.picture-default"));
        QTRY_COMPARE(r.role("rz", NodeList::Thumbnail).toString(), QString());

        // View > Picture > All: the mode is the setting, and a run makes the pictures.
        QVERIFY(r.app.commands.trigger("view.picture-all"));
        QCOMPARE(r.app.model.pictureMode(), fugue::PictureMode::All);
        QCOMPARE(r.app.settings.pictureMode(), QString("all"));
        QVERIFY(r.app.commands.entry("view.picture-all").value("checked").toBool());
        QVERIFY(!r.app.commands.entry("view.picture-smart").value("checked").toBool());
        run(3);
        QVERIFY(r.role("rz", NodeList::Thumbnail).toString().startsWith("file://"));
        // Sinks and inputs only: a generator loses its picture too; the Preview keeps it.
        QVERIFY(r.app.commands.trigger("view.picture-sinks"));
        QTRY_COMPARE(r.role("gen", NodeList::Thumbnail).toString(), QString());
        QTRY_COMPARE(r.role("rz", NodeList::Thumbnail).toString(), QString());
        QVERIFY(r.role("show", NodeList::Thumbnail).toString().startsWith("file://"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // A group that shows a picture keeps its "N nodes" footer under it, not over it.
    void aGroupsPictureLeavesRoomForItsFooter() {
        RunApp r(R"({
          "fugue": 1,
          "groups": {"local/pic": {
            "version": "1.0.0", "name": "Picture",
            "inputs": [{"name": "picture", "targets": ["load.file"]}],
            "outputs": [{"name": "image", "from": "size.image"}],
            "nodes": {"load": {"type": "fugue/load-image", "version": "1.0.0"},
                      "size": {"type": "fugue/resize-megapixels", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}}}}},
          "graph": {"nodes": {"grp": {"type": "local/pic", "version": "1.0.0"}}},
          "layout": {"nodes": {"grp": {"pos": [100, 100]}}, "view": {"center": [300, 300], "zoom": 1}}
        })");
        QVERIFY(r.app.open());
        const qreal bare = r.role("grp", NodeList::Height).toReal();
        // After a run the group shows its image output's picture (here set directly, as the look boards do).
        QImage picture(64, 32, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QVERIFY(picture.save(QDir(QDir::tempPath()).filePath("fugue-footer-test.png")));
        r.app.model.setRunState("grp", {{"state", "cached"}, {"thumbnail", QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath("fugue-footer-test.png")).toString()},
                                        {"thumbnailAspect", 0.5}, {"thumbnailInfo", "64 \u00d7 32"}});
        QTRY_VERIFY(r.role("grp", NodeList::Thumbnail).toString().startsWith("file://"));
        const QRectF slot = r.role("grp", NodeList::ThumbnailRect).toRectF();
        const qreal height = r.role("grp", NodeList::Height).toReal();
        QVERIFY(height > bare);
        // The footer row (drawn at the bottom) is wholly below the picture.
        QVERIFY2(slot.bottom() + r.app.model.rowHeight() <= height, qPrintable(QString("slot bottom %1, row %2, height %3").arg(slot.bottom()).arg(r.app.model.rowHeight()).arg(height)));
    }

    // A group that exposes a Load Image's file on its face shows that picture before any run, as a Load Image does,
    // in every picture mode; Hide picture takes it away and Show picture brings it back. After a run it shows the
    // image its group makes, and not when hidden.
    void aGroupExposingALoadImagesFileShowsThePicture() {
        RunApp r(R"({
          "fugue": 1,
          "groups": {"local/pic": {
            "version": "1.0.0", "name": "Picture",
            "inputs": [{"name": "picture", "targets": ["load.file"]}],
            "outputs": [{"name": "image", "from": "size.image"}],
            "nodes": {"load": {"type": "fugue/load-image", "version": "1.0.0"},
                      "size": {"type": "fugue/resize-megapixels", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}}}}},
          "graph": {"nodes": {"grp": {"type": "local/pic", "version": "1.0.0"},
            "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "grp.image"}}}}},
          "layout": {"nodes": {"grp": {"pos": [100, 100]}, "show": {"pos": [500, 100]}}, "view": {"center": [300, 300], "zoom": 1}}
        })");
        QVERIFY(r.app.open());
        const QString file = r.app.dir.filePath("face.png");
        QImage picture(64, 32, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QVERIFY(picture.save(file));
        QVERIFY(r.app.model.widgets()->setText("grp", "picture", file).isEmpty());
        const auto shown = [&] { return r.role("grp", NodeList::Thumbnail).toString().startsWith("file://"); };
        for (const char *mode : {"view.picture-smart", "view.picture-sinks", "view.picture-all"}) {
            QVERIFY(r.app.commands.trigger(mode));
            QTRY_VERIFY2(shown(), mode);
        }
        QVERIFY(r.app.commands.trigger("view.picture-smart"));
        r.app.model.select({"grp"});
        QVERIFY(r.app.commands.trigger("node.picture-hide"));
        QTRY_VERIFY(!shown());
        QVERIFY(r.app.commands.trigger("node.picture-show"));
        QTRY_VERIFY(shown());

        // A run: the picture is now what the group makes (the resized image), not the file's.
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        QTRY_COMPARE(r.role("grp", NodeList::ThumbnailInfo).toString(), QStringLiteral("1456 \u00d7 720"));
        QVERIFY(r.app.commands.trigger("node.picture-hide"));
        QTRY_VERIFY(!shown());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The live-frame overlays showing (visible) under an item.
    static int overlays(QQuickItem *from) {
        int n = from->objectName() == "liveFrame" && from->isVisible() ? 1 : 0;
        for (QQuickItem *child : from->childItems())
            n += overlays(child);
        return n;
    }

    // A Sampler makes a latent, not a picture, yet its step previews (node.preview) show on its card while
    // it runs, at the top level and inside a group, and the group's own card shows its inside's preview.
    void aSamplersStepPreviewShowsOnItsCardAlsoInsideAGroup() {
        RunApp r(R"({
          "fugue": 1,
          "groups": {"local/s": {"version": "1.0.0", "name": "S", "inputs": [], "outputs": [],
            "nodes": {"inner": {"type": "fugue/sampler", "version": "1.2.0"}}}},
          "graph": {"nodes": {"top": {"type": "fugue/sampler", "version": "1.2.0"},
                              "grp": {"type": "local/s", "version": "1.0.0"}}},
          "layout": {"nodes": {"top": {"pos": [0, 0]}, "grp": {"pos": [400, 0]}}, "view": {"center": [300, 300], "zoom": 1}}
        })");
        QVERIFY(r.app.open());
        const QString file = r.app.dir.filePath("step.png");
        QVERIFY(QImage(64, 32, QImage::Format_RGB32).save(file));
        const QString run = r.runs->start();
        QVERIFY(!run.isEmpty());
        const auto preview = [&](const QString &node) {
            const QJsonObject thumbnail{{"url", QUrl::fromLocalFile(file).toString()}, {"width", 64}, {"height", 32}, {"source", file}};
            emit r.service->event(run, {{"event", "node.running"}, {"node", node}});
            emit r.service->event(run, {{"event", "node.preview"}, {"node", node}, {"thumbnails", QJsonObject{{"preview", thumbnail}}}});
        };
        const auto shown = [&](const QString &node) { return r.role(node, NodeList::Thumbnail).toString().startsWith("file://"); };
        preview("top");
        QTRY_VERIFY(shown("top"));
        QCOMPARE(overlays(r.app.window->contentItem()), 0); // the card has the slot: no overlay
        preview("grp/inner");
        QTRY_VERIFY(shown("grp"));
        QVERIFY(r.app.model.groups()->enterGroup("grp"));
        QTRY_VERIFY(shown("inner"));
        QQuickItem *card = item(r.app.window->contentItem(), "node-inner");
        QVERIFY(card);
        QTRY_VERIFY(item(card, "thumbnail") && item(card, "thumbnail")->property("active").toBool());
    }

    // The live frame shows even where the picture mode hides the node's picture (a Sampler in Sinks
    // mode, or inside a group) as an overlay beside the card, never in it (the card's size does not
    // change), and fades out when the node ends; the node's own Hide picture wins.
    void aLiveFrameShowsInSinksModeUntilTheNodeEnds() {
        RunApp r(R"({
          "fugue": 1,
          "groups": {"local/s": {"version": "1.0.0", "name": "S", "inputs": [], "outputs": [],
            "nodes": {"inner": {"type": "fugue/sampler", "version": "1.2.0"}}}},
          "graph": {"nodes": {"top": {"type": "fugue/sampler", "version": "1.2.0"},
                              "hid": {"type": "fugue/sampler", "version": "1.2.0"},
                              "grp": {"type": "local/s", "version": "1.0.0"}}},
          "layout": {"nodes": {"top": {"pos": [0, 0]}, "hid": {"pos": [0, 300], "picture": "hide"}, "grp": {"pos": [400, 0]}}, "view": {"center": [300, 300], "zoom": 1}}
        })");
        QVERIFY(r.app.open());
        QVERIFY(r.app.commands.trigger("view.picture-sinks"));
        const QString file = r.app.dir.filePath("step.png");
        QVERIFY(QImage(64, 32, QImage::Format_RGB32).save(file));
        const QString run = r.runs->start();
        QVERIFY(!run.isEmpty());
        // The events are the test's, checked at once: the real run fails soon (no model) and would end them.
        const qreal bare = r.role("top", NodeList::Height).toReal();
        const auto shown = [&](const QString &node) { return r.role(node, NodeList::Thumbnail).toString().startsWith("file://"); };
        const auto event = [&](const char *kind, const QString &node, bool frame) {
            QJsonObject e{{"event", kind}, {"node", node}};
            if (frame)
                e.insert("thumbnails", QJsonObject{{"preview", QJsonObject{{"url", QUrl::fromLocalFile(file).toString()},
                                                                           {"width", 64}, {"height", 32}, {"source", file}}}});
            emit r.service->event(run, e);
        };
        for (const QString node : {"top", "hid", "grp/inner"}) {
            event("node.running", node, false);
            event("node.preview", node, true);
        }
        QCOMPARE(overlays(r.app.window->contentItem()), 2); // top and grp, not hid
        QVERIFY(!shown("top"));
        QVERIFY(!shown("grp"));
        QCOMPARE(r.role("top", NodeList::Height).toReal(), bare);
        for (const QString node : {"top", "hid", "grp/inner"})
            event("node.done", node, false);
        QTRY_COMPARE(overlays(r.app.window->contentItem()), 0);
        QCOMPARE(r.role("top", NodeList::Height).toReal(), bare);
    }

    // The service makes a thumbnail for a live frame of a node whose picture is hidden (the key of the live
    // preview in Sinks mode), and none for its result.
    void aHiddenPicturesLiveFrameGetsAThumbnailButItsResultDoesNot() {
        RunApp r(kChain);
        QVERIFY(r.app.open());
        QVERIFY(r.app.commands.trigger("view.picture-sinks"));
        QSignalSpy events(r.service.get(), &RunService::event);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.preview(f.jobs().at(0), png(64, 32)); });
        const auto thumbnails = [&](const char *kind) {
            for (const QList<QVariant> &args : events) {
                const QJsonObject e = args.at(1).toJsonObject();
                if (e.value("node").toString() == "gen" && e.value("event").toString() == kind)
                    return e.value("thumbnails").toObject();
            }
            return QJsonObject{{"none", true}};
        };
        QTRY_VERIFY(!thumbnails("node.preview").contains("none"));
        QVERIFY(!thumbnails("node.preview").value("preview").toObject().isEmpty());
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(320, 240)}}); });
        QTRY_VERIFY(!thumbnails("node.done").contains("none"));
        QVERIFY(thumbnails("node.done").isEmpty());
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
            QSignalSpy ev(r.service.get(), &RunService::event);
            r.app.key(Qt::Key_Return, Qt::ControlModifier);
            QTRY_COMPARE(finished.size(), 1);
            for (auto &a : ev) qWarning().noquote() << QJsonDocument(a.at(1).toJsonObject()).toJson(QJsonDocument::Compact).left(400);
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

    // Save Video, Save Audio and a Preview of a video show a player in the slot (#116): the label
    // before a run, the result's file and kind after, a bar with the play button, the time and, for
    // a video, the button that opens the large view, which plays the same file.
    void saveVideoSaveAudioAndPreviewShowAPlayerOnTheirFace() {
        QTemporaryDir out;
        const QString clip = out.filePath("in.webm"), song = out.filePath("in.wav");
        QVERIFY(makeMedia({"-f", "lavfi", "-i", "testsrc=size=96x64:rate=12:duration=2", "-c:v", "libvpx", clip}));
        QVERIFY(makeMedia({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=22050:duration=2", song}));
        RunApp r(mediaGraph(clip, song, out.filePath("saved")));
        QVERIFY(r.app.open());
        QCOMPARE(r.role("save-clip", NodeList::ThumbnailText).toString(), QStringLiteral("No result yet"));
        QVERIFY(r.role("save-clip", NodeList::Media).toMap().isEmpty());

        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));

        const QVariantMap saved = r.role("save-clip", NodeList::Media).toMap();
        QCOMPARE(saved.value("type").toString(), QStringLiteral("video"));
        QCOMPARE(saved.value("file").toString(), QStringLiteral("fox.webm"));
        QCOMPARE(QUrl(saved.value("url").toString()).toLocalFile(), out.filePath("saved/fox.webm"));
        QCOMPARE(r.role("save-clip", NodeList::ThumbnailText).toString(), QStringLiteral("Video \u00b7 0:02"));
        const QVariantMap sound = r.role("save-song", NodeList::Media).toMap();
        QCOMPARE(sound.value("type").toString(), QStringLiteral("audio"));
        QCOMPARE(sound.value("file").toString(), QStringLiteral("tune.flac"));
        // A Preview of the clip plays the clip itself, and names no file.
        const QVariantMap shown = r.role("show-clip", NodeList::Media).toMap();
        QCOMPARE(QUrl(shown.value("url").toString()).toLocalFile(), clip);
        QVERIFY(shown.value("file").toString().isEmpty());
        // A sound's slot is a bar, not a picture.
        const QRectF slot = r.role("save-song", NodeList::ThumbnailRect).toRectF();
        QVERIFY(slot.width() > 0 && slot.height() < 0.3 * slot.width());

        QQuickItem *card = item(r.app.window->contentItem(), "node-save-clip");
        QVERIFY(card);
        QQuickItem *player = nullptr;
        QTRY_VERIFY((player = item(card, "mediaPlayer")));
        QCOMPARE(player->property("kind").toString(), QStringLiteral("video"));
        QTRY_VERIFY(player->property("duration").toReal() > 1500);
        // The play button starts it and pauses it; Space does the same once the player has the focus.
        QVERIFY(!player->property("playing").toBool());
        click(r, item(player, "mediaPlay"));
        QTRY_VERIFY(player->property("playing").toBool());
        click(r, item(player, "mediaPlay"));
        QTRY_VERIFY(!player->property("playing").toBool());
        player->forceActiveFocus();
        r.app.key(Qt::Key_Space);
        QTRY_VERIFY(player->property("playing").toBool());
        r.app.key(Qt::Key_Space);
        QTRY_VERIFY(!player->property("playing").toBool());
        QCOMPARE(item(player, "mediaTime")->property("text").toString(), QStringLiteral("0:00 / 0:02"));
        // Volume: Down and Up change it by a tenth, setting it above zero unmutes.
        QCOMPARE(player->property("volume").toReal(), 1.0);
        r.app.key(Qt::Key_Down);
        QTRY_COMPARE(player->property("volume").toReal(), 0.9);
        player->setProperty("muted", true);
        r.app.key(Qt::Key_Up);
        QTRY_COMPARE(player->property("volume").toReal(), 1.0);
        QVERIFY(!player->property("muted").toBool());
        QVERIFY(item(player, "mediaExpand")->isVisible());
        QQuickItem *songPlayer = item(item(r.app.window->contentItem(), "node-save-song"), "mediaPlayer");
        QVERIFY(songPlayer);
        QCOMPARE(songPlayer->property("kind").toString(), QStringLiteral("audio"));
        QVERIFY(!item(songPlayer, "mediaExpand")->isVisible());

        // The expand button opens the large view on the same file, with no zoom buttons; closing stops it.
        click(r, item(player, "mediaExpand"));
        QQuickItem *dialog = nullptr;
        QTRY_VERIFY((dialog = named(r, "mediaViewer")));
        QTRY_COMPARE(QUrl(dialog->property("source").toString()).toLocalFile(), out.filePath("saved/fox.webm"));
        QTRY_VERIFY(dialog->property("playing").toBool()); // it starts at once
        QVERIFY(!named(r, "mediaZoom-fit")->isVisible());
        QVERIFY(named(r, "mediaClose")->isVisible());
        click(r, named(r, "mediaClose"));
        QTRY_VERIFY(dialog->property("source").toString().isEmpty());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
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


    // 8.2, #404: a node routed to a service off this machine says so on its face; a run that would send data there asks first
    // (nothing is sent meanwhile); the question opens on No, so Enter and Escape cancel the run and Tab then Enter is Yes, which is kept for the next run.
    void aServiceOffThisMachineIsMarkedAndAsksBeforeAnythingIsSent() {
        RunApp r(kGraph, false, imageDescription(), {}, "https://api.example.com/v1");
        QVERIFY(r.app.open());
        QTRY_COMPARE(r.role("gen", NodeList::RunsOn).toString(), QStringLiteral("Cloud (api.example.com)"));
        QCOMPARE(r.role("words", NodeList::RunsOn).toString(), QString());
        QTRY_COMPARE(r.runs->runsOn(), QStringLiteral("Cloud (api.example.com)"));
        QObject *ask = r.app.window->findChild<QObject *>("remoteConsentAsk");
        QVERIFY(ask);

        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!r.runs->remoteQuestion().isEmpty());
        QTRY_VERIFY(ask->property("opened").toBool());
        const QVariantMap service = r.runs->remoteQuestion().value("services").toList().first().toMap();
        QCOMPARE(service.value("instance").toString(), QStringLiteral("Cloud"));
        QVERIFY(service.value("text").toString().contains("prompts"));
        QVERIFY(r.jobs().isEmpty());
        // The question opens on No: a stray Enter cancels the run and sends nothing.
        QTRY_VERIFY(ask->property("opened").toBool());
        r.app.key(Qt::Key_Return);
        QTRY_VERIFY(r.runs->remoteQuestion().isEmpty());
        QTRY_VERIFY(!r.runs->busy());
        QVERIFY(r.jobs().isEmpty());

        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(ask->property("opened").toBool());
        r.app.key(Qt::Key_Escape); // No, by keys alone
        QTRY_VERIFY(r.runs->remoteQuestion().isEmpty());
        QTRY_VERIFY(!r.runs->busy());
        QVERIFY(r.jobs().isEmpty());

        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(ask->property("opened").toBool());
        r.app.key(Qt::Key_Tab); // Yes takes Enter only once the focus is on it
        r.app.key(Qt::Key_Return);
        QTRY_COMPARE(r.jobs().size(), 1);
        QVERIFY(QFile::exists(r.app.dir.path() + "/remote-consent.json"));
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
        r.app.key(Qt::Key_Return, Qt::ControlModifier); // remembered: no question this time
        QTRY_VERIFY(r.runs->busy());
        QVERIFY(r.runs->remoteQuestion().isEmpty());
        r.runs->cancel();
    }

    // A batch (several starts at once) asks once and keeps its order; No cancels all of it.
    void aBatchAsksOnceAndKeepsItsOrder() {
        RunApp r(kGraph, false, imageDescription(), {}, "https://api.example.com/v1");
        QVERIFY(r.app.open());
        QTRY_COMPARE(r.runs->runsOn(), QStringLiteral("Cloud (api.example.com)"));
        QStringList queued, cancelled;
        QSignalSpy asked(r.service.get(), &RunService::remoteConsentNeeded);
        connect(r.service.get(), &RunService::event, this, [&](const QString &run, const QJsonObject &event) {
            if (event.value("event").toString() == QLatin1String("run.queued"))
                queued << run;
            else if (event.value("event").toString() == QLatin1String("run.done") && event.value("status").toString() == QLatin1String("cancelled"))
                cancelled << run;
        });
        fugue::RunRequest request;
        request.document = r.app.model.document();
        const auto startThree = [&] {
            QStringList ids;
            for (int i = 0; i < 3; ++i)
                ids << r.service->start(request);
            return ids;
        };

        QStringList ids = startThree();
        QTRY_VERIFY(!r.runs->remoteQuestion().isEmpty());
        QTest::qWait(300); // the other two reach the question the first one asked (they have no event to wait on)
        QCOMPARE(asked.size(), 1);
        QVERIFY(queued.isEmpty());
        r.runs->answerRemote(false);
        QTRY_COMPARE(cancelled, ids);
        QVERIFY(queued.isEmpty());

        cancelled.clear();
        asked.clear();
        ids = startThree();
        QTRY_VERIFY(!r.runs->remoteQuestion().isEmpty());
        QTest::qWait(300);
        QCOMPARE(asked.size(), 1);
        r.runs->answerRemote(true);
        QTRY_COMPARE(queued, ids); // all three, in the order they were started
        QCOMPARE(asked.size(), 1);
        for (const QString &id : ids)
            r.service->cancel(id);
        QTRY_COMPARE(cancelled.size(), 3);
    }

    // The window closes while the check that precedes the question is under way: nothing is queued or written, and nothing crashes.
    void closingTheWindowDuringTheOffMachineCheckQueuesNothing() {
        RunApp r(kGraph, true, imageDescription(), {}, "https://api.example.com/v1");
        QVERIFY(r.app.open());
        QTRY_COMPARE(r.runs->runsOn(), QStringLiteral("Cloud (api.example.com)"));
        fugue::RunRequest request;
        request.document = r.app.model.document();
        r.service->start(request); // its check waits 300 ms on the slow provider's describe
        r.close();                 // shutdown arrives inside that check
        QVERIFY(QDir(r.app.dir.path() + "/runs").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(QDir(r.app.dir.path() + "/queue").entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
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
        // The dot travels: its place changes between two looks and stays on the wire's box.
        {
            const auto dotAt = [&] {
                QQuickItem *delegate = nullptr;
                QMetaObject::invokeMethod(dots, "itemAt", Q_RETURN_ARG(QQuickItem *, delegate), Q_ARG(int, 0));
                QQuickItem *ball = delegate && !delegate->childItems().isEmpty() ? delegate->childItems().last() : nullptr;
                return ball ? ball->mapToScene(QPointF(0, 0)) : QPointF();
            };
            QPointF first;
            QTRY_VERIFY((first = dotAt()) != QPointF());
            QTRY_VERIFY2(dotAt() != first, "the running dot does not move along its wire");
            // It follows the wire when a node moves under the run (a thumbnail also grows nodes): its path is the
            // wire's live one, not the one it started on.
            QQuickItem *delegate = nullptr;
            QMetaObject::invokeMethod(dots, "itemAt", Q_RETURN_ARG(QQuickItem *, delegate), Q_ARG(int, 0));
            QVERIFY(delegate);
            const int row = r.runs->runningWires().first().toMap().value("row").toInt();
            const auto livePath = [&] { return r.app.model.links()->data(r.app.model.links()->index(row), LinkList::Path).toString(); };
            QCOMPARE(delegate->property("wirePath").toString(), livePath());
            const QString before = livePath();
            r.app.model.beginMove(QStringLiteral("words"));
            r.app.model.moveBy(180, 120);
            QTRY_VERIFY(livePath() != before);
            QTRY_COMPARE(delegate->property("wirePath").toString(), livePath());
            r.app.model.moveBy(0, 0); // back where it was: no edit is left to stale the run
            r.app.model.endMove();
        }

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

    // #600: a kernel's own check() error (Expression's formula that does not parse) shows on its node
    // after a committed edit, without a run, and goes when the value is fixed.
    void aKernelsCheckErrorShowsOnItsNodeWithoutARun() {
        RunApp r(kFormula);
        QVERIFY(r.app.open());
        QSignalSpy checked(r.runs.get(), &CanvasRuns::checkApplied);
        QVERIFY(r.app.model.widgets()->setText("calc", "formula", "a +").isEmpty());
        QTRY_VERIFY(checked.size() >= 1);
        QTRY_COMPARE(r.state("calc"), QStringLiteral("error"));
        QVERIFY(r.role("calc", NodeList::Error).toString().contains("Column"));
        const QVariantList errors = r.runs->errors();
        QCOMPARE(errors.size(), 2); // calc, and loose which nothing reads
        for (const QVariant &error : errors) {
            QCOMPARE(error.toMap().value("input").toString(), QStringLiteral("formula"));
            QCOMPARE(error.toMap().value("code").toString(), QStringLiteral("formula_invalid"));
        }
        QVERIFY(r.role("calc", NodeList::Error).toString().startsWith("Column 4"));
        QCOMPARE(r.state("loose"), QStringLiteral("error"));
        QVERIFY(r.jobs().isEmpty()); // no run, no provider call
        // Fixed: the error goes at the next check.
        checked.clear();
        QVERIFY(r.app.model.widgets()->setText("calc", "formula", "a + 1").isEmpty());
        QTRY_VERIFY(checked.size() >= 1);
        QVERIFY(r.state("calc") != QStringLiteral("error"));
        QVERIFY(r.role("calc", NodeList::Error).toString().isEmpty());
        QCOMPARE(r.runs->errors().size(), 1);
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
        hangACheck(r, true);
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

    // #264: a node of a pack that is not installed, plain or inside a group instance, says in words
    // which pack it needs (the display name the workflow records), with the full text in the chip's
    // tooltip detail, and its Install… button asks for the pack as the banner's does.
    void aNodeOfAMissingPackSaysWhichPackAndOffersToInstall() {
        const QString packs = fugue::packsDir();
        FUGUE_REQUIRE_TEST_HOME(packs);
        QDir(packs).removeRecursively();
        RunApp r(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "plain": {"type": "io.example.tools/say", "version": "1.0.0"},
    "wrap": {"type": "local/wrap", "version": "1.0.0"}
  }},
  "groups": {"local/wrap": {"name": "Wrap", "version": "1.0.0",
    "inputs": [{"name": "threshold", "label": "Threshold", "section": "Advanced", "targets": ["find.threshold"]}],
    "outputs": [],
    "nodes": {"find": {"type": "io.example.tools/say", "version": "1.0.0"}}}},
  "requires": {"packs": {"io.example.tools": {"version": "1.0.0", "source": "example.com:tools.git", "name": "Say Words"}}},
  "layout": {"nodes": {"plain": {"pos": [0, 0]}, "wrap": {"pos": [300, 0]}}}
})");
        QVERIFY(r.app.open());
        r.app.workflows.dismissPendingTrust(); // the question about the packs is not under test
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.runs->start();
        QTRY_COMPARE(finished.size(), 1);
        const QStringList both{"plain", "wrap"};
        for (const QString &node : both) {
            QTRY_COMPARE(r.role(node, NodeList::Error).toString(), QStringLiteral("Needs the Say Words pack"));
            const QVariantMap pack = r.role(node, NodeList::ErrorPack).toMap();
            QCOMPARE(pack.value("id").toString(), QStringLiteral("io.example.tools"));
            QVERIFY(pack.value("installable").toBool());
            QVERIFY2(r.role(node, NodeList::ErrorDetail).toString().contains("is not installed"),
                     qPrintable(r.role(node, NodeList::ErrorDetail).toString()));
        }
        // The group's own detail names the inner node, as the technical text did.
        QVERIFY(r.role("wrap", NodeList::ErrorDetail).toString().contains("wrap/find"));

        QSignalSpy install(&r.app.workflows, &fugue::Workflows::packInstallRequested);
        QQuickItem *card = item(r.app.window->contentItem(), "node-wrap");
        QVERIFY(card);
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = item(card, "installPackChip")) && button->isVisible());
        QTest::mouseClick(r.app.window, Qt::LeftButton, {},
                          button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QCOMPARE(install.size(), 1);
        QCOMPARE(install.first().at(0).toString(), QStringLiteral("example.com:tools.git"));
        QCOMPARE(install.first().at(1).toString(), QString());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // #70: a pack installed between two runs is what the next run runs, with no restart, and so
    // is its update (a new version, trusted again): the run thread reloads between runs.
    void aReloadBetweenTwoRunsRunsTheNewTypes() {
        const QString packs = fugue::packsDir();
        FUGUE_REQUIRE_TEST_HOME(packs);
        QDir(packs).removeRecursively();
        RunApp r(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "say": {"type": "io.example.tools/say", "version": "1.0.0"},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "say.text"}}}
  }},
  "layout": {"nodes": {"say": {"pos": [0, 0]}, "show": {"pos": [300, 0]}}}
})");
        QVERIFY(r.app.open());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.runs->start();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("failed")); // not installed

        fugue::TrustStore trust;
        QVERIFY(writeSayPack(packs, "first"));
        QVERIFY(trust.trust("io.example.tools", "1.0.0").isEmpty());
        r.app.reloader->reload();
        QVERIFY(!r.role("say", NodeList::Missing).toBool());
        r.runs->start();
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished.at(1).at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(r.role("show", NodeList::ThumbnailText).toString(), QStringLiteral("first"));

        // The update: new files at a new version. The workflow still records 1.0.0, a minor
        // step behind, which loads and runs (3.6).
        QVERIFY(writeSayPack(packs, "second", "1.1.0"));
        QVERIFY(trust.trust("io.example.tools", "1.1.0").isEmpty());
        r.app.reloader->reload();
        r.runs->start();
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(finished.at(2).at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(r.role("show", NodeList::ThumbnailText).toString(), QStringLiteral("second"));

        // Updated while a run waits its turn: the run was compiled and trusted for 1.1.0, so it
        // fails rather than run 1.2.0's files, which nobody trusted (3.6, 6.7).
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        QVERIFY(queue.enqueue("cli-run", "/w/a.fugue", false));
        QVERIFY(queue.activate("cli-run"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        r.runs->start();
        QTRY_COMPARE(queue.entries().size(), 2);
        QVERIFY(writeSayPack(packs, "third", "1.2.0"));
        r.app.reloader->reload();
        QVERIFY(queue.remove(QStringLiteral("cli-run")));
        worker.release();
        QTRY_COMPARE(finished.size(), 4);
        QCOMPARE(finished.at(3).at(1).toString(), QStringLiteral("failed"));
        QTRY_VERIFY(!r.runs->errors().isEmpty());
        QCOMPARE(r.runs->errors().first().toMap().value("code").toString(), QStringLiteral("node_version_mismatch"));
        QCOMPARE(r.role("show", NodeList::ThumbnailText).toString(), QStringLiteral("second"));
        QVERIFY(QFile::exists(packs + "/io.example.tools/say/run.sh"));
        r.close();
        QDir(packs).removeRecursively();
    }

    // #65: a command node's `preview` line (7.2) is its picture while it runs, at full strength;
    // its result has no picture, so the preview stays. When it runs again, that picture is the
    // last run's and is drawn at half strength until the new run's preview comes.
    void aCommandNodesPreviewShowsOnTheNodeWhileItRuns() {
        const QString packs = fugue::packsDir();
        FUGUE_REQUIRE_TEST_HOME(packs);
        QDir(packs).removeRecursively();
        // Run n waits for the file preview<n> in the node's folder, shows step.png, then waits
        // for result<n>: the test decides when each step happens.
        const QByteArray script = QByteArray(R"(cat > /dev/null
n=$(( $(cat "$FUGUE_NODE_DIR/count" 2>/dev/null || echo 0) + 1 )); echo $n > "$FUGUE_NODE_DIR/count"
wait_for() { for i in $(seq 1 400); do [ -e "$FUGUE_NODE_DIR/$1" ] && return; sleep 0.05; done; }
wait_for preview$n
echo iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg== | base64 -d > step$n.png
echo '{"op": "preview", "image": {"path": "step'$n'.png"}}'
wait_for result$n
echo '{"outputs": {"text": "painted"}}'
)");
        QVERIFY(writeToolsPack(packs, "paint", script, "1.0.0", {{"deterministic", false}}));
        fugue::TrustStore trust;
        QVERIFY(trust.trust("io.example.tools", "1.0.0").isEmpty());
        RunApp r(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "paint": {"type": "io.example.tools/paint", "version": "1.0.0"},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "paint.text"}}}
  }},
  "layout": {"nodes": {"paint": {"pos": [0, 0]}, "show": {"pos": [300, 0]}}}
})");
        QVERIFY(r.app.open());
        const QString dir = packs + "/io.example.tools/paint/";
        auto touch = [&](const QString &name) { QVERIFY(pack_fixture::writeFile(dir + name, "")); };
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);

        r.runs->start();
        QTRY_COMPARE(r.state("paint"), QStringLiteral("running"));
        QVERIFY(r.role("paint", NodeList::Thumbnail).toString().isEmpty());
        touch("preview1");
        QTRY_VERIFY(!r.role("paint", NodeList::Thumbnail).toString().isEmpty());
        const QString first = r.role("paint", NodeList::Thumbnail).toString();
        QVERIFY(!QImage(QUrl(first).toLocalFile()).isNull());
        QCOMPARE(r.state("paint"), QStringLiteral("running"));
        QVERIFY(!r.role("paint", NodeList::ThumbnailDim).toBool());
        touch("result1");
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(r.role("paint", NodeList::Thumbnail).toString(), first);
        QVERIFY(!r.role("paint", NodeList::ThumbnailDim).toBool());

        r.runs->start();
        QTRY_COMPARE(r.state("paint"), QStringLiteral("running"));
        QCOMPARE(r.role("paint", NodeList::Thumbnail).toString(), first);
        QVERIFY(r.role("paint", NodeList::ThumbnailDim).toBool());
        touch("preview2");
        QTRY_VERIFY(r.role("paint", NodeList::Thumbnail).toString() != first);
        QVERIFY(!r.role("paint", NodeList::ThumbnailDim).toBool());
        touch("result2");
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(r.app.warnings.isEmpty());
        r.close();
        QDir(packs).removeRecursively();
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

    // ---- The run toolbar and the queue panel (#60) ----

    static void click(const RunApp &r, QQuickItem *target) {
        QVERIFY(target);
        QTest::mouseClick(r.app.window, Qt::LeftButton, {},
                          target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
    }
    // An item by name in the window, the overlay where menus draw included.
    static QQuickItem *named(const RunApp &r, const QString &name) {
        QQuickItem *root = r.app.window->contentItem();
        while (root->parentItem())
            root = root->parentItem();
        return item(root, name);
    }

    // Run and Cancel on the toolbar start and end the window's run as the keys do; the toolbar
    // floats at the top edge, centered, and its Run is the one highlighted button.
    void theToolbarStartsARunAndCancelsIt() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QQuickItem *toolbar = qvariant_cast<QQuickItem *>(named(r, "runToolbar")->property("surface"));
        QVERIFY(toolbar);
        QCOMPARE(toolbar->y(), 4 * r.app.theme.look()->property("unit").toReal());
        QVERIFY(qAbs(toolbar->x() + toolbar->width() / 2 - toolbar->parentItem()->width() / 2) <= 1);
        QVERIFY(!named(r, "runCancelButton")->isEnabled());
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        click(r, named(r, "runButton"));
        QTRY_COMPARE(r.jobs().size(), 1);
        QTRY_VERIFY(named(r, "runCancelButton")->isEnabled());
        click(r, named(r, "runCancelButton"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        QTRY_VERIFY(!named(r, "runCancelButton")->isEnabled());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The split menu is the Run menu's rows: Run first and Run to here start the run as their keys do.
    void theSplitMenuHoldsTheRunCommands() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        r.app.model.select({"gen"});
        click(r, named(r, "runMenuButton"));
        QQuickItem *menu = nullptr;
        QTRY_VERIFY((menu = named(r, "menuItem-run.to-here")) && menu->isVisible());
        QVERIFY(named(r, "menuItem-run.start"));
        QVERIFY(named(r, "menuItem-run.start-first"));
        QVERIFY(named(r, "menuItem-run.cancel"));
        QVERIFY(menu->isEnabled());
        click(r, menu);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
    }

    // The batch count makes that many runs of one Run, in order, each with the seed shown then.
    void theBatchCountMakesSeveralRunsInOrder() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        click(r, named(r, "runBatchMore"));
        click(r, named(r, "runBatchMore"));
        QCOMPARE(r.runs->batch(), 3);
        QCOMPARE(named(r, "runBatchCount")->property("text").toString(), QString::fromUtf8("\u00d73"));
        click(r, named(r, "runButton"));
        QCOMPARE(r.runs->count(), 3);
        QCOMPARE(r.runs->runs().size(), 3);
        QTRY_COMPARE(r.jobs().size(), 1);
        // The count is kept for the next time the window opens.
        QFile saved(r.app.dir.path() + "/run-bar.json");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(saved.readAll()).object().value("batch").toInt(), 3);
        for (int i = 0; i < 3 && r.runs->busy(); ++i) {
            r.runs->cancel();
            QTest::qWait(100);
        }
        QTRY_VERIFY(!r.runs->busy());

        // Up to the one node, each run has its own seed, in the order they were asked for.
        r.app.model.widgets()->setSeedMode("gen", "seed", "increment");
        r.app.model.select({"gen"});
        const int base = r.app.model.widgets()->value("gen", "seed").toInt();
        const int before = r.jobs().size();
        r.app.key(Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(r.runs->count(), 3);
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(r.jobs().size(), before + i + 1);
            QJsonObject params;
            r.onFake([&](FakeProvider &f) { params = f.submitted(f.jobs().at(before + i)).params; });
            QCOMPARE(params.value("seed").toInt(), base + i);
            r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(before + i), {{"image.png", png(64, 64)}}); });
        }
        QTRY_VERIFY(!r.runs->busy());
        r.runs->setBatch(1);
        QCOMPARE(r.runs->batch(), 1);
        click(r, named(r, "runBatchLess"));
        QCOMPARE(r.runs->batch(), 1); // not below one
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The toolbar moves anywhere in the window, across as well as up and down, and the place is kept
    // in the state file as fractions of its room; the keys move it from the grip, Shift in bigger
    // steps; the View menu's command hides it and shows it again (#120).
    void theToolbarMovesFreelyAndRemembersItsPlace() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QQuickItem *grip = named(r, "runToolbarGrip");
        QVERIFY(grip);
        QQuickItem *toolbar = qvariant_cast<QQuickItem *>(named(r, "runToolbar")->property("surface"));
        QVERIFY(toolbar);
        QQuickItem *area = toolbar->parentItem();
        const qreal unit = r.app.theme.look()->property("unit").toReal();
        QCOMPARE(r.bar->place(), QPointF(0.5, 0));
        const QPoint from = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
        QTest::mousePress(r.app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(r.app.window, from + QPoint(-30 * i, 20 * i));
        QTest::mouseRelease(r.app.window, Qt::LeftButton, {}, from + QPoint(-300, 200));
        // Left and down of where it was, both axes remembered (the drag may be pulled to an edge).
        QTRY_VERIFY2(r.bar->place().x() < 0.5 && r.bar->place().y() > 0, qPrintable(QStringLiteral("%1,%2").arg(r.bar->place().x()).arg(r.bar->place().y())));
        QVERIFY(toolbar->x() < (area->width() - toolbar->width()) / 2);
        QVERIFY(toolbar->y() > 4 * unit);
        QFile saved(r.app.dir.path() + "/run-bar.json");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject kept = QJsonDocument::fromJson(saved.readAll()).object();
        QVERIFY(!kept.contains("dock"));
        QCOMPARE(kept.value("place").toObject().value("x").toDouble(), r.bar->place().x());

        // Dragged far outside, it stays inside the window, 4u from the edge.
        const QPoint grip2 = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
        QTest::mousePress(r.app.window, Qt::LeftButton, {}, grip2);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(r.app.window, grip2 + QPoint(-200 * i, 0));
        QTest::mouseRelease(r.app.window, Qt::LeftButton, {}, grip2 + QPoint(-2000, 0));
        QTRY_COMPARE(r.bar->place().x(), 0.0);
        QCOMPARE(toolbar->x(), 4 * unit);

        // The keys from the grip: Right moves it by 2u, Shift by 10u; Down likewise.
        grip->forceActiveFocus();
        const qreal x0 = toolbar->x(), y0 = toolbar->y();
        r.app.key(Qt::Key_Right);
        QTRY_COMPARE(toolbar->x(), x0 + 2 * unit);
        r.app.key(Qt::Key_Right, Qt::ShiftModifier);
        QTRY_COMPARE(toolbar->x(), x0 + 12 * unit);
        r.app.key(Qt::Key_Down);
        QTRY_COMPARE(toolbar->y(), y0 + 2 * unit);
        r.app.key(Qt::Key_Up, Qt::ShiftModifier);
        QTRY_COMPARE(toolbar->y(), qMax(4 * unit, y0 - 8 * unit));
        // Far enough to the right and it stops at the edge.
        for (int i = 0; i < 200 && toolbar->x() + toolbar->width() + 4 * unit < area->width(); ++i)
            r.app.key(Qt::Key_Right, Qt::ShiftModifier);
        QTRY_COMPARE(r.bar->place().x(), 1.0);
        QCOMPARE(toolbar->x() + toolbar->width(), area->width() - 4 * unit);

        // A smaller window keeps the bar inside, at the same relative place.
        const QSize before = r.app.window->size();
        r.app.window->resize(before.width() * 3 / 4, before.height() * 3 / 4);
        // The area is the canvas: the window less the tab strip on its left (#159).
        QTRY_COMPARE(area->width(), qreal(before.width() * 3 / 4) - area->mapToScene(QPointF(0, 0)).x());
        QTRY_COMPARE(toolbar->x() + toolbar->width(), area->width() - 4 * unit);
        QVERIFY(toolbar->y() + toolbar->height() <= area->height());
        r.app.window->resize(before);

        QVERIFY(r.app.commands.trigger("view.toggle-run-toolbar"));
        QTRY_VERIFY(!named(r, "runToolbar"));
        QVERIFY(r.app.commands.trigger("view.toggle-run-toolbar"));
        QTRY_VERIFY(named(r, "runToolbar"));
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // Not a check: with FUGUE_SHOT_DIR set, saves a finished text-to-image workflow with its pictures
    // on the nodes as <dir>/results.png and results-light.png (#94). The provider is the fake one, and
    // the picture it hands back is the one the z-image-turbo template ships as its preview (made with
    // Fugue from this very prompt, no one's own).
    void savesFinishedWorkflowsWhenAsked_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("file");
        QTest::newRow("dark") << QString() << "results.png";
        QTest::newRow("light") << "catppuccin-latte" << "results-light.png";
    }
    void savesFinishedWorkflowsWhenAsked() {
        const QString dir = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the README screenshot");
        QFETCH(QString, theme);
        QFETCH(QString, file);
        const auto picture = [](const QString &template_) {
            const QImage image(QFINDTESTDATA("../../templates/" + template_ + "/preview.webp"));
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const QByteArray fox = picture("z-image-turbo");
        QVERIFY(!fox.isEmpty());
        RunApp r(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "fox-prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox in fresh snow, morning light, photo"}},
    "fox": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "fox-prompt.text"}, "seed": 7}},
    "fox-show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "fox.image"}}}
  }},
  "layout": {"nodes": {"fox-prompt": {"pos": [0, 0]}, "fox": {"pos": [320, 0]}, "fox-show": {"pos": [640, 0]}}}
})", false, imageDescription(), Look{theme, 8});
        QVERIFY(r.app.open());
        r.app.window->resize(1600, 1000);
        r.app.workflows.dismissPendingTrust();
        r.app.workflows.dismissNotice();
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        r.app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", fox}}); });
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        QTest::qWait(300);
        r.app.model.select({});
        r.app.model.requestFit();
        QTest::qWait(1200); // the thumbnails load
        QVERIFY(readme_shot::graphOnly(r.app.window, r.app.canvas).save(dir + "/" + file));
    }

    // Not a check: with FUGUE_SHOT_DIR set, saves the run toolbar with Cancel enabled and disabled
    // on a dark and a light theme, as <dir>/runbar-<theme>.png (#120).
    void savesTheToolbarWhenAsked() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the toolbar's picture");
        for (const QString &theme : {QStringLiteral("tokyo-night"), QStringLiteral("catppuccin-latte"), QStringLiteral("rose-pine")}) {
            RunApp r(kGraph, false, imageDescription(), Look{theme, -1});
            QVERIFY(r.app.open());
            QQuickItem *bar = qvariant_cast<QQuickItem *>(named(r, "runToolbar")->property("surface"));
            QTest::qWait(300);
            const QRect box(bar->mapToScene(QPointF(0, 0)).toPoint(), QSize(bar->width() + 8, bar->height() + 8));
            const QString base = QString::fromLocal8Bit(dir) + "/runbar-" + theme;
            QVERIFY(test_support::grab(r.app.window).copy(box.adjusted(-4, -4, 4, 4)).save(base + "-idle.png"));
            named(r, "runToolbar")->setProperty("canCancel", true);
            named(r, "runToolbar")->setProperty("pending", 2);
            QTest::qWait(200);
            QVERIFY(test_support::grab(r.app.window).copy(box.adjusted(-4, -4, 4, 4)).save(base + "-running.png"));
            QTest::mouseMove(r.app.window, named(r, "runCancelButton")->mapToScene(QPointF(10, 10)).toPoint());
            QTest::qWait(200);
            QVERIFY(test_support::grab(r.app.window).copy(box.adjusted(-4, -4, 4, 4)).save(base + "-hover.png"));
        }
    }

    // The queue panel stays attached to the bar wherever it is: over it when the bar is low, under
    // it when the bar is high, and inside the window when the bar is at a side (#120).
    void theQueuePanelFollowsTheBar() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        r.bar->setPlace({0.5, 1});
        QQuickItem *bar = qvariant_cast<QQuickItem *>(named(r, "runToolbar")->property("surface"));
        QTRY_VERIFY(bar->y() > bar->parentItem()->height() / 2);
        click(r, named(r, "runQueueButton"));
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = named(r, "runQueuePanel")) && panel->isVisible());
        QTRY_VERIFY(panel->y() + panel->height() <= bar->y());
        QVERIFY(panel->y() >= 0);

        r.bar->setPlace({0, 0.5});
        QTRY_COMPARE(bar->x(), 4 * r.app.theme.look()->property("unit").toReal());
        QTRY_VERIFY(panel->x() >= 0);
        QVERIFY(panel->x() + panel->width() <= panel->parentItem()->width());
        QVERIFY(panel->y() >= 0 && panel->y() + panel->height() <= panel->parentItem()->height());
        QVERIFY(panel->y() + panel->height() <= bar->y() || panel->y() >= bar->y() + bar->height());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // The panel lists this window's runs and a CLI run in one queue; "move to front" reorders the
    // waiting ones, "cancel" takes a run out whoever started it.
    void theQueuePanelListsCliRunsAndReordersAndCancels() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        const QString runs = r.app.dir.path() + "/runs";
        const fugue::Queue queue(r.app.dir.path() + "/queue");
        // A CLI run is going: its worker holds the engine lock. Another waits behind it.
        const auto cliRun = [&](const QString &id) {
            const fugue::Result<fugue::RunFolder> folder = fugue::RunFolder::create(runs, id);
            QVERIFY(folder.ok());
            QVERIFY(folder->queued("/w/cli.fugue"));
            QVERIFY(queue.enqueue(id, "/w/cli.fugue", false));
        };
        cliRun("cli-going");
        QVERIFY(queue.activate("cli-going"));
        fugue::Lock worker(r.app.dir.path() + "/engine.lock");
        QVERIFY(worker.tryAcquire());
        cliRun("cli-waiting");

        click(r, named(r, "runButton"));
        click(r, named(r, "runButton"));
        QTRY_COMPARE(queue.entries().size(), 4);
        QCOMPARE(r.runs->runs().size(), 2);
        const QString first = r.runs->runs().at(0), second = r.runs->runs().at(1);

        QVERIFY(!named(r, "runQueuePanel"));
        click(r, named(r, "runQueueButton"));
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = named(r, "runQueuePanel")) && panel->isVisible());
        QTRY_VERIFY(named(r, "queueEntry-cli-going"));
        QVERIFY(named(r, "queueEntry-cli-waiting"));
        QVERIFY(named(r, "queueEntry-" + first));
        QVERIFY(named(r, "queueEntry-" + second));
        // The panel hangs from the bar, under it at the top of the window.
        {
            QQuickItem *bar = qvariant_cast<QQuickItem *>(named(r, "runToolbar")->property("surface"));
            QVERIFY(qAbs(panel->x() + panel->width() / 2 - (bar->x() + bar->width() / 2)) <= 1);
            QVERIFY(panel->y() >= bar->y() + bar->height());
        }
        QCOMPARE(named(r, "queueEntry-cli-going")->findChild<QQuickItem *>("queueDetail")->property("text").toString(),
                 QStringLiteral("Starting · from the command line"));
        QVERIFY(named(r, "queueFront-" + first)->isVisible() && !named(r, "queueFront-cli-going")->isVisible());
        QCOMPARE(r.queue->count(), 4);

        // Move the last run to the front of the waiting ones.
        QVERIFY(!named(r, "queueFront-cli-waiting")->isEnabled()); // already first of the waiting ones
        QVERIFY(named(r, "queueFront-" + second)->isEnabled());
        click(r, named(r, "queueFront-" + second));
        QTRY_COMPARE(queue.entries().at(1).run, second);
        QCOMPARE(queue.entries().at(0).run, QStringLiteral("cli-going"));
        // Cancel the CLI run that waits: its entry goes and its folder ends cancelled.
        click(r, named(r, "queueCancel-cli-waiting"));
        QTRY_VERIFY(!queue.find("cli-waiting"));
        QCOMPARE(fugue::RunFolder::open(runs, "cli-waiting")->status(), fugue::RunStatus::Cancelled);
        QTRY_VERIFY(!named(r, "queueEntry-cli-waiting"));
        // And the window's own first run.
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        // The list lays its rows out when the window next polishes (a frame): a click before that lands where the row was
        // (the panel was still 28 px taller, the row 96 px higher), on nothing. One frame first.
        r.app.window->grabWindow();
        click(r, named(r, "queueCancel-" + first));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), first);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));

        // The panel closes with Escape and with the toolbar's button.
        QTRY_VERIFY(named(r, "runQueuePanel")->hasActiveFocus() || named(r, "runQueuePanel")->isVisible());
        named(r, "runQueuePanel")->forceActiveFocus();
        r.app.key(Qt::Key_Escape);
        QTRY_VERIFY(!named(r, "runQueuePanel"));
        QVERIFY(!r.queue->watching());

        QVERIFY(queue.remove(QStringLiteral("cli-going")));
        worker.release();
        QTRY_COMPARE(r.jobs().size(), 1);
        r.runs->cancel();
        QTRY_VERIFY(!r.runs->busy());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }

    // A finished run is in the history; a click on it shows its results on the nodes of the saved
    // workflow it came from, and its folder button opens the run folder. A run of another file
    // shows nothing and says so.
    void theHistoryShowsAFinishedRunsResultsAndOpensItsFolder() {
        RunApp r(kGraph);
        QVERIFY(r.app.open());
        QTemporaryDir place;
        QVERIFY(r.app.workflows.saveAs(place.filePath("flow.fugue")));
        QSignalSpy finished(r.runs.get(), &CanvasRuns::finished);
        click(r, named(r, "runButton"));
        QTRY_COMPARE(r.jobs().size(), 1);
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(r.jobs().size(), 2); // the group's
        r.onFake([&](FakeProvider &f) { f.finish(f.jobs().at(1), {{"image.png", png(64, 64)}}); });
        QTRY_COMPARE(finished.size(), 1);
        const QString run = finished.first().at(0).toString();
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("ok"));
        // A later edit makes the nodes stale; showing the run puts its results back.
        r.app.model.widgets()->setValue("words", "text", "a dog");
        r.runs->checkNow();
        QTRY_COMPARE(r.state("gen"), QStringLiteral("stale"));

        click(r, named(r, "runQueueButton"));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = named(r, "historyEntry-" + run)) && row->isVisible());
        QVERIFY(row->findChild<QQuickItem *>("historyDetail")->property("text").toString().startsWith("Done"));
        // The rows may still be settling as the panel opens: press until the press lands.
        for (int i = 0; i < 20 && r.openedFolders.isEmpty(); ++i) {
            click(r, named(r, "historyFolder-" + run));
            QTest::qWait(50);
        }
        QCOMPARE(r.openedFolders.mid(0, 1), QList<QUrl>{QUrl::fromLocalFile(r.app.dir.path() + "/runs/" + run)});
        click(r, named(r, "historyResults-" + run));
        QTRY_VERIFY(!r.role("show", NodeList::Thumbnail).toString().isEmpty());
        QCOMPARE(r.state("gen"), QStringLiteral("cached")); // what it made, as when the run ended

        // The row names the run by what it made and the same press opens the media browser on that
        // run alone, in the workflows panel, whatever workflow is open.
        QQuickItem *lead = named(r, "historyLead-" + run);
        QVERIFY(lead && lead->isVisible());
        QVERIFY(lead->property("item").toMap().value("kind").toString() == "image");
        QTRY_VERIFY(named(r, "workflowsResults")); // opened by the click above
        QVERIFY(r.app.workflows.openText(QString::fromUtf8(kGraph)));
        click(r, named(r, "historyResults-" + run));
        QTRY_VERIFY(named(r, "workflowsResultsAllRuns")->isVisible());
        QVERIFY(named(r, "workflowsResultsAllRuns")->isVisible());
        const QVariantList shown = named(r, "workflowsResults")->property("items").toList();
        QVERIFY(!shown.isEmpty());
        for (const QVariant &item : shown)
            QCOMPARE(item.toMap().value("run").toString(), run);
        click(r, named(r, "workflowsResultsAllRuns"));
        QTRY_VERIFY(!named(r, "workflowsResultsAllRuns")->isVisible());
        QVERIFY2(r.app.warnings.isEmpty(), qPrintable(r.app.warnings.join('\n')));
    }
};

FUGUE_TEST_MAIN(TestCanvasRun, QGuiApplication)
#include "test_canvas_run.moc"
