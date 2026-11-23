// Pasting and dropping images in the real window (Main.qml, offscreen): Ctrl+V with a QImage and
// with encoded PNG and JPEG data, replacing the image of a selected Load Image, undo, a drop on
// the canvas and on a node, the Paste button, and a saved workflow that reloads with its images
// (spec 14.1, Appendix A).

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQuickStyle>
#include <QUndoStack>

#include "canvas_app.h"
#include "engine/embed.h"
#include "engine/file_roots.h"
#include "engine/paths.h"
#include "workflow/image_inputs.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

const char *oneLoadImage = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "src": {"type": "fugue/load-image", "version": "1.0.0"},
    "txt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "hi"}}}},
  "layout": {"nodes": {"src": {"pos": [100, 100]}, "txt": {"pos": [500, 100]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

// A group of a Load Image and a Resize to Megapixels that exposes the Load Image's file on its face.
const char *groupWithAnImage = R"({
  "fugue": 1,
  "groups": {"local/pic": {
    "version": "1.0.0", "name": "Picture",
    "inputs": [{"name": "picture", "targets": ["load.file"]}],
    "outputs": [{"name": "image", "from": "size.image"}],
    "nodes": {"load": {"type": "fugue/load-image", "version": "1.0.0"},
              "size": {"type": "fugue/resize-megapixels", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}}}}},
  "graph": {"nodes": {
    "grp": {"type": "local/pic", "version": "1.0.0"},
    "txt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "hi"}}}},
  "layout": {"nodes": {"grp": {"pos": [100, 100]}, "txt": {"pos": [500, 100]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

QByteArray encoded(const QImage &image, const char *format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

QImage picture(QColor color) {
    QImage image(16, 10, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString fileOf(const App &app, const QString &node) {
    const InputValue value = app.model.document().graph.nodes.value(node).inputs.value("file");
    return value.kind() == InputValue::Kind::Literal ? value.literal().toString() : QString();
}

QStringList loadImages(const App &app) {
    QStringList ids;
    const Nodes nodes = app.model.document().graph.nodes;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        if (it->type == "fugue/load-image")
            ids.append(it.key());
    return ids;
}

QStringList inputFiles() { return QDir(inputsDir()).entryList(QDir::Files, QDir::Name); }

void putOnClipboard(QMimeData *data) { QGuiApplication::clipboard()->setMimeData(data); }

// Load Image's file widget (InputWidget.qml) in the window.
QQuickItem *fileWidget(const App &app, const QString &node) {
    QList<QQuickItem *> todo{app.window->contentItem()};
    while (!todo.isEmpty()) {
        QQuickItem *at = todo.takeFirst();
        if (at->objectName() == "inputWidget" && at->property("nodeId").toString() == node
            && at->property("name").toString() == "file")
            return at;
        todo << at->childItems();
    }
    return nullptr;
}

QRectF sceneRect(const QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

QString rectText(const QRectF &r) { return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); }

} // namespace

class TestCanvasImages : public QObject {
    Q_OBJECT

    QTemporaryDir pictures;

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        qputenv("XDG_PICTURES_DIR", pictures.path().toUtf8());
    }
    void init() { QDir(inputsDir()).removeRecursively(); }

    void pasteOfAQImageMakesALoadImageAtThePointerAndUndoRemovesIt() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        auto *data = new QMimeData;
        data->setImageData(picture(Qt::red));
        putOnClipboard(data);
        QTest::mouseMove(app.window, app.screen(300, 400));
        app.key(Qt::Key_V, Qt::ControlModifier);

        const QStringList images = loadImages(app);
        QCOMPARE(images.size(), 2);
        const QString added = images.first() == "src" ? images.last() : images.first();
        QCOMPARE(app.model.position(added), QPointF(300, 400));
        QCOMPARE(app.model.selectedIds(), QStringList{added});
        // Decoded pixels only: a lossless PNG named by content, in the inputs folder.
        const QString file = fileOf(app, added);
        QVERIFY(file.startsWith(inputsDir() + "/pasted-"));
        QVERIFY(file.endsWith(".png"));
        QVERIFY(QImage(file).size() == QSize(16, 10));
        QVERIFY(app.workflows.unsaved());

        // The node and its image are one undo step.
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(loadImages(app), QStringList{"src"});
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void encodedPngAndJpegAreStoredAsTheyCame() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        const QByteArray png = encoded(picture(Qt::green), "PNG"), jpeg = encoded(picture(Qt::blue), "JPEG");
        for (const auto &[type, bytes, extension] : {std::tuple{"image/png", png, ".png"}, {"image/jpeg", jpeg, ".jpg"}}) {
            app.model.select({}, "replace");
            auto *data = new QMimeData;
            data->setData(type, bytes);
            putOnClipboard(data);
            const int before = loadImages(app).size();
            app.key(Qt::Key_V, Qt::ControlModifier);
            QCOMPARE(loadImages(app).size(), before + 1);
            const QString file = fileOf(app, app.model.selectedIds().value(0));
            QVERIFY(file.endsWith(extension));
            QCOMPARE(read(file), bytes);
        }
    }

    void pasteWithALoadImageSelectedReplacesItsImageInOneStepAndTwiceIsOneFile() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(150, 105)); // src's header
        QCOMPARE(app.model.selectedIds(), QStringList{"src"});
        auto *data = new QMimeData;
        data->setData("image/png", encoded(picture(Qt::red), "PNG"));
        putOnClipboard(data);
        const int steps = app.model.undoStack()->count();
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(loadImages(app), QStringList{"src"});
        QCOMPARE(app.model.undoStack()->count(), steps + 1);
        const QString first = fileOf(app, "src");
        QVERIFY(!first.isEmpty());

        // The same bytes again: the same file, no second one.
        auto *again = new QMimeData;
        again->setData("image/png", encoded(picture(Qt::red), "PNG"));
        putOnClipboard(again);
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(fileOf(app, "src"), first);
        QCOMPARE(inputFiles().size(), 1);

        app.key(Qt::Key_Z, Qt::ControlModifier);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(fileOf(app, "src"), QString());
    }

    // A group whose face exposes a Load Image's file takes the picture on its own face: no new Load Image node.
    void pasteWithAGroupThatExposesAnImageFillsItsFaceInput() {
        App app(load(groupWithAnImage));
        QVERIFY(app.open());
        QCOMPARE(app.model.widgets()->canPasteImage("grp", "picture"), true);
        QVERIFY(!app.model.widgets()->canPasteImage("grp", "file"));
        app.model.select({QStringLiteral("grp")});
        auto *data = new QMimeData;
        data->setData("image/png", encoded(picture(Qt::red), "PNG"));
        putOnClipboard(data);
        const int steps = app.model.undoStack()->count();
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app.model.document().graph.nodes.size(), 2); // none made
        const InputValue value = app.model.document().graph.nodes.value("grp").inputs.value("picture");
        QCOMPARE(value.kind(), InputValue::Kind::Literal);
        QVERIFY(value.literal().toString().endsWith(".png"));
        QCOMPARE(app.model.undoStack()->count(), steps + 1);
        // A drop on the group does the same with another picture.
        QVERIFY(app.model.widgets()->useInput("grp", value.literal().toString()));
    }

    void workflowDataOnTheClipboardIsPastedBeforeAnImage() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        auto *data = new QMimeData;
        data->setData(canvasClipboardType, QByteArray(R"({"fugue": 1, "graph": {"nodes": {
            "t": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}}}}})"));
        data->setImageData(picture(Qt::red));
        putOnClipboard(data);
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app.model.document().graph.nodes.size(), 3);
        QCOMPARE(loadImages(app), QStringList{"src"});
        QVERIFY(inputFiles().isEmpty());
    }

    void nothingUsableOnTheClipboardChangesNothing() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        auto *data = new QMimeData;
        data->setText("just words");
        putOnClipboard(data);
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app.model.document().graph.nodes.size(), 2);
        QVERIFY(app.workflows.notice().isEmpty());
        // A "PNG" that is not one is refused with a message on screen.
        auto *bad = new QMimeData;
        bad->setData("image/png", "not really");
        putOnClipboard(bad);
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app.model.document().graph.nodes.size(), 2);
        QVERIFY2(app.workflows.notice().contains("Could not paste the image"), qPrintable(app.workflows.notice()));
    }

    void aDropOnTheCanvasMakesANodeAndOnANodeReplaces() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        QTemporaryDir other;
        const QByteArray jpeg = encoded(picture(Qt::blue), "JPEG");
        QVERIFY(write(other.filePath("Camera 1.jpg"), jpeg));
        QVERIFY(write(other.filePath("notes.txt"), "text"));

        app.workflows.dropUrls({QUrl::fromLocalFile(other.filePath("Camera 1.jpg"))}, 300, 400);
        QCOMPARE(loadImages(app).size(), 2);
        const QString added = app.model.selectedIds().value(0);
        QCOMPARE(app.model.position(added), QPointF(300, 400));
        QCOMPARE(read(fileOf(app, added)), jpeg); // byte for byte
        QVERIFY(fileOf(app, added).contains("/Camera_1-"));

        // On a Load Image: its image is replaced, one undo step, and no new node.
        const int steps = app.model.undoStack()->count();
        const QByteArray png = encoded(picture(Qt::green), "PNG");
        QVERIFY(write(other.filePath("Green.png"), png));
        app.workflows.dropUrls({QUrl::fromLocalFile(other.filePath("Green.png"))}, 150, 105);
        QCOMPARE(loadImages(app).size(), 2);
        QCOMPARE(read(fileOf(app, "src")), png);
        QCOMPARE(app.model.undoStack()->count(), steps + 1);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(fileOf(app, "src"), QString());

        // Not an image: a notice, and the canvas is as it was.
        app.workflows.dropUrls({QUrl::fromLocalFile(other.filePath("notes.txt"))}, 300, 400);
        QVERIFY(app.workflows.notice().contains("notes.txt") || app.workflows.count() > 1);
    }

    void aPictureWithAWorkflowInItOpensAsAWorkflowExceptOnALoadImage() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        QTemporaryDir other;
        const Result<QByteArray> png = pngWithWorkflow(
            encoded(picture(Qt::red), "PNG"),
            R"({"fugue": 1, "graph": {"nodes": {"t": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}}}}})");
        QVERIFY(png.ok());
        QVERIFY(write(other.filePath("out.png"), png.value()));

        app.workflows.dropUrls({QUrl::fromLocalFile(other.filePath("out.png"))}, 300, 400);
        QCOMPARE(app.workflows.count(), 2); // opened as a workflow
        app.workflows.setCurrent(0);
        app.workflows.dropUrls({QUrl::fromLocalFile(other.filePath("out.png"))}, 150, 105); // on src
        QCOMPARE(app.workflows.count(), 2);
        QCOMPARE(read(fileOf(app, "src")), png.value()); // the picture, workflow and all
    }

    void thePasteButtonTakesTheClipboardIntoTheNode() {
        App app(load(oneLoadImage));
        QVERIFY(app.open());
        QVERIFY(app.model.widgets()->canPasteImage("src", "file"));
        QVERIFY(!app.model.widgets()->canPasteImage("txt", "text"));
        auto *data = new QMimeData;
        data->setImageData(picture(Qt::yellow));
        putOnClipboard(data);
        QVERIFY(app.model.widgets()->pasteImage("src"));
        QVERIFY(fileOf(app, "src").endsWith(".png"));
        // Recent inputs lists it and picking it sets the file again.
        const QVariantList recent = app.model.widgets()->recentInputs();
        QCOMPARE(recent.size(), 1);
        QVERIFY(app.model.widgets()->useInput("src", recent.first().toMap().value("path").toString()));
        // A path outside the inputs folder is not taken here.
        QVERIFY(!app.model.widgets()->useInput("src", pictures.filePath("elsewhere.png")));
        // An empty clipboard says so.
        putOnClipboard(new QMimeData);
        QVERIFY(!app.model.widgets()->pasteImage("src"));
        QVERIFY(app.workflows.notice().contains("no image"));
    }

    // A board of Load Image's Paste button and its picker's recent inputs, cheap like the other
    // boards: the geometry is checked on every run (the button inside its node and the window,
    // the menu inside the window with a row per recent image); the pictures are written only
    // with FUGUE_INPUT_SHOTS set to a folder, e.g.
    //   FUGUE_INPUT_SHOTS=/tmp/shots QT_QUICK_BACKEND=rhi build/test_canvas_images pasteAndRecentInputsBoard
    void pasteAndRecentInputsBoard_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("radius");
        QTest::newRow("default") << QString() << -1;
        QTest::newRow("ethereal r0") << "ethereal" << 0;
        QTest::newRow("flexoki-light r8") << "flexoki-light" << 8;
    }
    void pasteAndRecentInputsBoard() {
        QFETCH(QString, theme);
        QFETCH(int, radius);
        if (!theme.isEmpty() && !QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        App app(load(oneLoadImage), {}, canvas_test::Look{theme, radius});
        if (radius >= 0)
            QTRY_COMPARE(app.theme.radius(), radius);
        QVERIFY(app.open());
        app.window->resize(900, 640);
        QTRY_COMPARE(app.window->contentItem()->width(), 900.0);
        for (const QColor color : {QColor(Qt::red), QColor(Qt::darkGreen), QColor(Qt::blue)}) {
            auto *data = new QMimeData;
            data->setData("image/png", encoded(picture(color), "PNG"));
            putOnClipboard(data);
            QVERIFY(app.model.widgets()->pasteImage("src"));
        }
        const QString out = qEnvironmentVariable("FUGUE_INPUT_SHOTS");
        const QString name = QStringLiteral("%1/inputs-%2").arg(out, theme.isEmpty() ? QStringLiteral("default") : theme);

        QQuickItem *widget = fileWidget(app, "src");
        QVERIFY(widget);
        QQuickItem *paste = canvas_test::item(widget, "pasteImageButton");
        QQuickItem *card = canvas_test::item(app.window->contentItem(), "node-src");
        QVERIFY(paste && paste->isVisible() && card);
        const QRectF window(0, 0, app.window->width(), app.window->height());
        QVERIFY2(sceneRect(widget).adjusted(-0.5, -0.5, 0.5, 0.5).contains(sceneRect(paste)), qPrintable(rectText(sceneRect(paste))));
        QVERIFY2(sceneRect(card).contains(sceneRect(paste)) && window.contains(sceneRect(paste)), qPrintable(rectText(sceneRect(paste))));
        QVERIFY(paste->width() > 0 && paste->height() > 0);
        if (!out.isEmpty())
            QVERIFY(test_support::grab(app.window).save(name + "-paste.png"));

        // The picker glyph is just left of the Paste button.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, widget->mapToScene(QPointF(paste->x() - 6, widget->height() / 2)).toPoint());
        const auto openMenu = [widget]() -> QObject * {
            auto *pickers = widget->property("pickers").value<QObject *>();
            QObject *found = pickers ? pickers->findChild<QObject *>("widgetMenu") : nullptr;
            return found && found->property("opened").toBool() ? found : nullptr;
        };
        QTRY_VERIFY(openMenu());
        QObject *menu = openMenu();
        const QVariantList entries = menu->property("entries").toList();
        QCOMPARE(entries.size(), 4); // Browse files…, then the three images
        QVERIFY(entries.at(1).toMap().contains("thumbnail"));
        auto *parent = menu->property("parent").value<QQuickItem *>();
        const QRectF rect(parent->mapToScene(QPointF(menu->property("x").toReal(), menu->property("y").toReal())),
                          QSizeF(menu->property("width").toReal(), menu->property("height").toReal()));
        QVERIFY2(window.contains(rect), qPrintable(rectText(rect)));
        // Under the widget (or above it, where there is no room below), as wide as it at least.
        const QRectF well = sceneRect(widget);
        QVERIFY2(rect.top() >= well.bottom() - 1 || rect.bottom() <= well.top() + 1,
                 qPrintable(rectText(rect) + " for the widget at " + rectText(well)));
        QVERIFY2(qAbs(rect.left() - well.left()) <= 1, qPrintable(rectText(rect) + " for the widget at " + rectText(well)));
        QVERIFY(rect.height() > 3 * menu->property("thumbHeight").toReal()); // the thumbnail rows show
        QTest::qWait(200); // the thumbnails decode
        if (!out.isEmpty())
            QVERIFY(test_support::grab(app.window).save(name + "-recent.png"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!openMenu());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void savingCopiesTheInputsNextToTheWorkflowAndItReloads() {
        QTemporaryDir place;
        QString name;
        {
            App app(load(oneLoadImage));
            QVERIFY(app.open());
            auto *data = new QMimeData;
            data->setData("image/png", encoded(picture(Qt::cyan), "PNG"));
            putOnClipboard(data);
            app.model.select({"src"}, "replace");
            app.key(Qt::Key_V, Qt::ControlModifier);
            const QString stored = fileOf(app, "src");
            QVERIFY(stored.startsWith(inputsDir()));
            name = QFileInfo(stored).fileName();

            QVERIFY(app.workflows.saveAs(place.filePath("flow.fugue")));
            // Copied beside the workflow, the node repointed by a path relative to the workflow's
            // folder (so the folder can move), the original kept.
            QCOMPARE(fileOf(app, "src"), "inputs/" + name);
            QCOMPARE(read(place.filePath("inputs/" + name)), read(stored));
            QVERIFY(QFileInfo(stored).isFile());
            QVERIFY(!app.workflows.unsaved());
        }
        // A fresh start opens the file with the image.
        App app(load(oneLoadImage));
        QVERIFY(app.workflows.openPath(place.filePath("flow.fugue")));
        QCOMPARE(fileOf(app, "src"), "inputs/" + name);
        QVERIFY(QImage(place.filePath("inputs/" + name)).size() == QSize(16, 10));
    }

    // Save As elsewhere takes the images beside the workflow along, still relative; a path
    // into the new `inputs` folder written in full (by an older save) becomes relative; any
    // other relative path, which would lead nowhere from the new folder, becomes the full path
    // it led to.
    void saveAsElsewhereTakesTheWorkflowsImagesAlong() {
        QTemporaryDir place;
        QVERIFY(QDir().mkpath(place.filePath("a/inputs")) && QDir().mkpath(place.filePath("b/inputs")));
        QVERIFY(write(place.filePath("a/inputs/one.png"), encoded(picture(Qt::red), "PNG")));
        QVERIFY(write(place.filePath("a/two.png"), encoded(picture(Qt::green), "PNG")));
        QVERIFY(write(place.filePath("b/inputs/three.png"), encoded(picture(Qt::blue), "PNG")));
        const QString flow = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "one": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "inputs/one.png"}},
            "two": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "two.png"}},
            "three": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%1"}}}}})")
                                 .arg(place.filePath("b/inputs/three.png"));
        QVERIFY(write(place.filePath("a/flow.fugue"), flow.toUtf8()));
        App app(load(oneLoadImage));
        QVERIFY(app.workflows.openPath(place.filePath("a/flow.fugue")));
        QVERIFY(app.workflows.saveAs(place.filePath("b/flow.fugue")));
        QCOMPARE(fileOf(app, "one"), QString("inputs/one.png"));
        QCOMPARE(read(place.filePath("b/inputs/one.png")), read(place.filePath("a/inputs/one.png")));
        QCOMPARE(fileOf(app, "two"), canonicalPath(place.filePath("a/two.png")));
        QCOMPARE(fileOf(app, "three"), QString("inputs/three.png"));
        // Saved again in the same place, nothing changes.
        const Document before = app.model.document();
        QVERIFY(app.workflows.save());
        QCOMPARE(app.model.document().save(), before.save());
    }
};

QTEST_MAIN(TestCanvasImages)
#include "test_canvas_images.moc"
