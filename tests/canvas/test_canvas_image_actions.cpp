// Open, Copy and Save Image on a node's context menu (#57, spec 12.3).
//
// Seam: the real window (Main.qml, offscreen) with CanvasRuns, RunService and ImageActions as
// main() makes them, a right-click on a node, the menu's rows. A FakeProvider gives the run
// its picture; every folder is in the test's temporary folder; the viewer is a stand-in.

#include <QQuickStyle>
#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QImage>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTimer>
#include <QtTest>

#include "canvas_app.h"
#include "engine/fake_provider.h"
#include "engine/queue.h"
#include "engine/run_folder.h"
#include "menu/image_actions.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

using namespace canvas_test;
using fugue::CanvasRuns;
using fugue::FakeProvider;
using fugue::RunService;
using fugue::RunSetup;

namespace {

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

    explicit RunApp(const QByteArray &graph, const fugue::Description &offers = imageDescription())
        : app(load(graph)) {
        RunSetup setup;
        setup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
        setup.addKernels = fugue::nodes::addBuiltinKernels;
        auto *self = this;
        setup.registerProviders = [self, offers](fugue::Registry &registry) {
            if (!self->fake)
                self->fake = std::make_shared<FakeProvider>(offers);
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
    ~RunApp() {
        // The window goes before the runs it shows, as in main(), which makes its engine last.
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

const QByteArray kLoadImage = R"({
  "fugue": 1,
  "graph": {"nodes": {"src": {"type": "fugue/load-image", "version": "1.0.0"}}},
  "layout": {"nodes": {"src": {"pos": [100, 100]}}, "view": {"center": [300, 300], "zoom": 1}}
})";

// Generate Image into a Preview, and a number shown by another.
const QByteArray kSinks = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a cat"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "words.text"}, "seed": 5}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "gen.image"}}},
    "size": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 768}},
    "show-size": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "size.value"}}}
  }},
  "layout": {"nodes": {"words": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}, "show": {"pos": [600, 0]},
                       "size": {"pos": [0, 300]}, "show-size": {"pos": [300, 300]}},
             "view": {"center": [400, 300], "zoom": 0.6}}
})";

// The window with the image actions of main(); `opened` is what Open Image gave the viewer.
struct ActionApp : RunApp {
    QList<QUrl> opened;
    fugue::ImageActions actions;
    QStringList messages;

    explicit ActionApp(const QByteArray &graph)
        : RunApp(graph), actions(app.commands, app.model, *runs, [this](const QUrl &url) {
              opened << url;
              return true;
          }) {
        QObject::connect(&actions, &fugue::ImageActions::message, [this](const QString &text) { messages << text; });
        app.imageActions = &actions;
    }

    // Runs the graph and gives the Generate Image a picture too big for a thumbnail.
    void runWithPicture(const QByteArray &picture) {
        QSignalSpy finished(runs.get(), &CanvasRuns::finished);
        app.key(Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(jobs().size(), 1);
        onFake([&](FakeProvider &f) { f.finish(f.jobs().at(0), {{"image.png", picture}}); });
        QTRY_COMPARE(finished.size(), 1);
    }
    // Right-clicks a node's header, which opens its menu.
    void openMenu(const QString &node) {
        const QPointF pos = app.model.position(node);
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(pos.x() + 20, pos.y() + 10));
        QTRY_VERIFY(menuOpen());
    }
    bool menuOpen() const {
        QObject *menu = app.window->findChild<QObject *>("contextMenuPopup");
        return menu && menu->property("visible").toBool();
    }
    QQuickItem *menuItem(const char *command) const {
        return app.window->findChild<QQuickItem *>(QStringLiteral("menuItem-") + command);
    }
    void closeMenu() {
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!menuOpen());
    }
    // Whether the row is on the node's menu and enabled.
    bool menuItemEnabled(const QString &node, const char *command) {
        openMenu(node);
        QQuickItem *item = menuItem(command);
        const bool enabled = item && item->isEnabled();
        closeMenu();
        return enabled;
    }
};

} // namespace

class TestImageActions : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The three commands are on a node's menu and are enabled only when it has an image.
    void theMenuOffersThemOnlyForANodeWithAnImage() {
        ActionApp r(kSinks);
        QVERIFY(r.app.open());
        for (const char *id : {"node.open-image", "node.copy-image", "node.save-image"})
            QVERIFY2(!r.menuItemEnabled("show", id), id);
        r.runWithPicture(png(1200, 800));
        for (const char *id : {"node.open-image", "node.copy-image", "node.save-image"}) {
            QVERIFY2(r.menuItemEnabled("show", id), id);
            QVERIFY2(!r.menuItemEnabled("show-size", id), id); // a number is no image
        }
        QCOMPARE(r.app.commands.entry("node.save-image").value("label").toString(), QStringLiteral("Save Image\u2026"));
        QVERIFY(r.app.warnings.isEmpty());
    }

    // Copy puts the full picture on the clipboard, and the file's own bytes under their MIME
    // type, which a Fugue paste then stores as they are.
    void copyPutsTheFullImageAndItsBytesOnTheClipboard() {
        ActionApp r(kSinks);
        QVERIFY(r.app.open());
        const QByteArray picture = png(1200, 800);
        r.runWithPicture(picture);
        QGuiApplication::clipboard()->clear();
        QSignalSpy copied(&r.actions, &fugue::ImageActions::copied);
        r.openMenu("show");
        QQuickItem *item = r.menuItem("node.copy-image");
        QVERIFY(item && item->isEnabled());
        QTest::mouseClick(r.app.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QTRY_COMPARE(copied.size(), 1);
        const QMimeData *data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data->hasImage());
        QCOMPARE(qvariant_cast<QImage>(data->imageData()).size(), QSize(1200, 800));
        QCOMPARE(data->data("image/png"), picture);
        QVERIFY(fugue::clipboardHasImage(*data));
        const auto bytes = fugue::clipboardImageBytes(*data);
        QVERIFY(bytes.ok());
        QCOMPARE(*bytes, picture);
        QVERIFY(r.messages.isEmpty());
    }

    // Save asks the window for a file, then writes the original bytes, with an extension when
    // the name has none; a folder that cannot be written is a notice.
    void saveWritesTheOriginalBytesAndFailuresAreNotices() {
        ActionApp r(kSinks);
        QVERIFY(r.app.open());
        const QByteArray picture = png(1200, 800);
        r.runWithPicture(picture);
        QSignalSpy requested(&r.actions, &fugue::ImageActions::saveRequested);
        QSignalSpy saved(&r.actions, &fugue::ImageActions::saved);
        r.app.model.select({"show"});
        QVERIFY(r.app.commands.trigger("node.save-image"));
        QCOMPARE(requested.size(), 1);
        QVERIFY2(requested.first().at(1).toString().endsWith(".png"), qPrintable(requested.first().at(1).toString()));
        r.actions.saveTo(QUrl::fromLocalFile(r.app.dir.filePath("out/cat")).toString());
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().toString(), r.app.dir.filePath("out/cat.png"));
        QFile file(r.app.dir.filePath("out/cat.png"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), picture);
        // Nothing but the file is left in the folder.
        QCOMPARE(QDir(r.app.dir.filePath("out")).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"cat.png"});

        QVERIFY(r.app.commands.trigger("node.save-image"));
        r.actions.saveTo("/proc/nowhere/cat.png");
        QTRY_COMPARE(r.messages.size(), 1);
        QVERIFY2(r.messages.first().startsWith("The image was not saved"), qPrintable(r.messages.first()));
        // Cancelling the dialog forgets the image: a later answer writes nothing.
        QVERIFY(r.app.commands.trigger("node.save-image"));
        r.actions.cancelSave();
        r.actions.saveTo(r.app.dir.filePath("never.png"));
        QTest::qWait(100);
        QVERIFY(!QFileInfo::exists(r.app.dir.filePath("never.png")));
    }

    // Open gives the viewer a file URL of the full picture, with the right extension.
    void openGivesTheViewerTheFullFile() {
        ActionApp r(kSinks);
        QVERIFY(r.app.open());
        const QByteArray picture = png(1200, 800);
        r.runWithPicture(picture);
        r.app.model.select({"show"});
        QVERIFY(r.app.commands.trigger("node.open-image"));
        QCOMPARE(r.opened.size(), 1);
        QVERIFY(r.opened.first().isLocalFile());
        const QString path = r.opened.first().toLocalFile();
        QVERIFY2(path.endsWith(".png"), qPrintable(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), picture);
        QVERIFY(!path.contains("thumbnails"));
    }

    // A Load Image with a file acts on that file before any run. A jpeg named .png is shown to
    // the viewer under its real extension; a viewer that fails is a notice.
    void aLoadImageWithAFileActsOnItsFile() {
        ActionApp r(kLoadImage);
        QVERIFY(r.app.open());
        QVERIFY(!r.app.commands.isEnabled("node.copy-image", {{"node", "src"}}));
        const QString file = r.app.dir.filePath("photo.png");
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(file, "JPEG"));
        QVERIFY(r.app.model.widgets()->setText("src", "file", file).isEmpty());
        QTRY_VERIFY(r.app.commands.isEnabled("node.copy-image", {{"node", "src"}}));
        QCOMPARE(r.runs->imageFile("src"), file);
        r.app.model.select({"src"});
        QVERIFY(r.app.commands.trigger("node.open-image"));
        QCOMPARE(r.opened.size(), 1);
        const QString shown = r.opened.first().toLocalFile();
        QVERIFY2(shown.endsWith(".jpg") && shown != file, qPrintable(shown));
        QFile a(file), b(shown);
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), a.readAll());
        QVERIFY(QFile::remove(file));
        QVERIFY(!r.app.commands.isEnabled("node.copy-image", {{"node", "src"}}));
        QVERIFY(r.messages.isEmpty());
    }
};

QTEST_MAIN(TestImageActions)
#include "test_canvas_image_actions.moc"
