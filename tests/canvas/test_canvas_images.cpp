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

    void savingCopiesTheInputsNextToTheWorkflowAndItReloads() {
        QTemporaryDir place;
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

            QVERIFY(app.workflows.saveAs(place.filePath("flow.fugue")));
            // Copied beside the workflow, the node repointed, the original kept.
            const QString beside = fileOf(app, "src");
            QCOMPARE(QFileInfo(beside).absolutePath(), place.filePath("inputs"));
            QCOMPARE(read(beside), read(stored));
            QVERIFY(QFileInfo(stored).isFile());
            QVERIFY(!app.workflows.unsaved());
        }
        // A fresh start opens the file with the image.
        App app(load(oneLoadImage));
        QVERIFY(app.workflows.openPath(place.filePath("flow.fugue")));
        const QString file = fileOf(app, "src");
        QCOMPARE(QFileInfo(file).absolutePath(), place.filePath("inputs"));
        QVERIFY(QImage(file).size() == QSize(16, 10));
    }
};

QTEST_MAIN(TestCanvasImages)
#include "test_canvas_images.moc"
