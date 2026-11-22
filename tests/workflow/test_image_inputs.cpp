// Pasting and dropping images (spec 14.1, Appendix A "images in"): the pure parts of
// workflow/image_inputs.h on a temporary inputs folder, and the same on a canvas.

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/embed.h"
#include "engine/paint_document.h"
#include "engine/paint_file.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "workflow/image_inputs.h"
#include "workflow/workflows.h"

using namespace fugue;

namespace {

QByteArray encoded(const QImage &image, const char *format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

QImage picture(QColor color = Qt::red) {
    QImage image(12, 8, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

// A small painted canvas as the bytes of its .ora file (#181).
QByteArray paintBytes() {
    mask::PaintDocument d(12, 8);
    d.addLayer(mask::LayerKind::Picture, "Picture");
    d.addLayer(mask::LayerKind::Paint, "Paint");
    d.brush.color = mask::Rgba{0, 0, 255, 255};
    d.fillRect(1, 1, 4, 4);
    return mask::paintToOra(d).value();
}

bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList names(const QString &folder) { return QDir(folder).entryList(QDir::Files, QDir::Name); }

// Each test has its own pictures folder, so its inputs folder is empty.
struct Pictures {
    QTemporaryDir dir;
    Pictures() { qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()); }
    QString inputs() const { return inputsDir(); }
};

} // namespace

class TestImageInputs : public QObject {
    Q_OBJECT

private slots:
    void sniffsTheFormatFromTheBytes() {
        QCOMPARE(sniffImage(encoded(picture(), "PNG"))->extension, QString("png"));
        QCOMPARE(sniffImage(encoded(picture(), "JPEG"))->extension, QString("jpg"));
        QCOMPARE(sniffImage(encoded(picture(), "BMP"))->extension, QString("bmp"));
        QCOMPARE(sniffImage(QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16))->mime, QString("image/webp"));
        QVERIFY(!sniffImage("GIF"));
        QVERIFY(!sniffImage("<svg></svg>"));
        QVERIFY(!sniffImage({}));
    }

    void storesTheBytesAsTheyAreNamedByContent() {
        Pictures p;
        const QByteArray jpeg = encoded(picture(), "JPEG");
        const Result<QString> stored = storeImage(jpeg, p.inputs(), FileRoots::forInputs());
        QVERIFY2(stored.ok(), qPrintable(stored.error().message));
        // The real format's extension, the bytes untouched, a name with the hash in it.
        QVERIFY(QRegularExpression("/pasted-\\d{8}-\\d{6}-[0-9a-f]{10}\\.jpg$").match(stored.value()).hasMatch());
        QCOMPARE(read(stored.value()), jpeg);
        QVERIFY(names(p.inputs()).size() == 1);
    }

    void aWrongLabelDoesNotChangeTheRealFormat() {
        Pictures p;
        QMimeData data;
        data.setData("image/png", encoded(picture(), "JPEG")); // labelled PNG, really JPEG
        const Result<QByteArray> bytes = clipboardImageBytes(data);
        QVERIFY(bytes.ok());
        const Result<QString> stored = storeImage(bytes.value(), p.inputs(), FileRoots::forInputs());
        QVERIFY(stored.value().endsWith(".jpg"));
    }

    void theSameBytesAreOneFileWhateverTheyAreCalled() {
        Pictures p;
        const QByteArray png = encoded(picture(), "PNG");
        const FileRoots roots = FileRoots::forInputs();
        const Result<QString> first = storeImage(png, p.inputs(), roots, "holiday photo.png");
        const Result<QString> second = storeImage(png, p.inputs(), roots, "other-name.png");
        const Result<QString> third = storeImage(png, p.inputs(), roots);
        QVERIFY(first.ok() && second.ok() && third.ok());
        QCOMPARE(second.value(), first.value());
        QCOMPARE(third.value(), first.value());
        QCOMPARE(names(p.inputs()).size(), 1);
        QVERIFY(QFileInfo(first.value()).fileName().startsWith("holiday_photo-"));
        // Other bytes are another file.
        QVERIFY(storeImage(encoded(picture(Qt::blue), "PNG"), p.inputs(), roots, "holiday photo.png").ok());
        QCOMPARE(names(p.inputs()).size(), 2);
    }

    // A file is reused only when its bytes are the image's, not by its name and size: a run's
    // Save Image (or anyone) may have planted a file named after the hash of a known image.
    // A symbolic link is never reused, and a planted file is never replaced.
    void aFileNamedByTheHashIsReusedOnlyWithTheSameBytes() {
        Pictures p;
        QTemporaryDir other;
        const QByteArray png = encoded(picture(), "PNG");
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex().left(10));
        QByteArray fake = png;
        fake[fake.size() - 5] = char(fake.at(fake.size() - 5) ^ 1); // same size, other bytes
        QVERIFY(QDir().mkpath(p.inputs()));
        const QString planted = p.inputs() + "/a-" + hash + ".png";
        QVERIFY(write(planted, fake));
        QVERIFY(write(p.inputs() + "/cat-" + hash + ".png", fake)); // the name the store would write
        QVERIFY(write(other.filePath("real.png"), png));
        QVERIFY(QFile::link(other.filePath("real.png"), p.inputs() + "/b-" + hash + ".png"));
        const FileRoots roots = FileRoots::forInputs();
        const Result<QString> stored = storeImage(png, p.inputs(), roots, "cat.png");
        QVERIFY2(stored.ok(), qPrintable(stored.error().message));
        QCOMPARE(read(stored.value()), png);
        QVERIFY(!QFileInfo(stored.value()).isSymLink());
        QCOMPARE(QFileInfo(stored.value()).absolutePath(), canonicalPath(p.inputs()));
        QCOMPARE(read(planted), fake);
        QCOMPARE(read(p.inputs() + "/cat-" + hash + ".png"), fake);
        // The real one is reused from now on, and a dropped file finds it too.
        QCOMPARE(storeImage(png, p.inputs(), roots).value(), stored.value());
        QCOMPARE(storeImageFile(other.filePath("real.png"), p.inputs(), roots).value(), stored.value());
    }

    void hostileNamesBecomePlainStems() {
        QCOMPARE(safeStem("../../etc/passwd.png"), QString("passwd"));
        QCOMPARE(safeStem("..\\..\\win\\evil.png"), QString("evil"));
        QCOMPARE(safeStem("a\nb\x01\x7f""c.png"), QString("a_b__c"));
        QCOMPARE(safeStem(".hidden.png"), QString("hidden"));
        QCOMPARE(safeStem("..."), QString("image"));
        QCOMPARE(safeStem(""), QString("image"));
        QCOMPARE(safeStem(QString(200, 'x') + ".png").size(), 48);

        Pictures p;
        const Result<QString> stored =
            storeImage(encoded(picture(), "PNG"), p.inputs(), FileRoots::forInputs(), "../../escape\n.png");
        QVERIFY(stored.ok());
        QCOMPARE(QFileInfo(stored.value()).absolutePath(), canonicalPath(p.inputs()));
        QVERIFY(!QFileInfo(p.dir.filePath("escape.png")).exists());
    }

    void refusesWhatIsNotAnImageOrIsAbsurd() {
        Pictures p;
        const FileRoots roots = FileRoots::forInputs();
        QVERIFY(!storeImage("hello", p.inputs(), roots).ok());
        QVERIFY(!storeImage({}, p.inputs(), roots).ok());
        // Right magic, damaged body.
        QVERIFY(!storeImage(QByteArray("\x89PNG\r\n\x1a\nnonsense", 16), p.inputs(), roots).ok());
        QVERIFY(!storeImage(QByteArray(maxImageBytes + 1, 'x'), p.inputs(), roots).ok());
        // A folder and a pipe are not files.
        QVERIFY(!storeImageFile(p.dir.path(), p.inputs(), roots).ok());
        QVERIFY(names(p.inputs()).isEmpty());
        // Outside the roots is permission_denied.
        const Result<QString> outside = storeImage(encoded(picture(), "PNG"), p.dir.filePath("elsewhere"), roots);
        QCOMPARE(outside.error().code, QString("permission_denied"));
    }

    void aDroppedFileIsCopiedByteForByte() {
        Pictures p;
        QTemporaryDir other;
        // A JPEG with trailing bytes a re-encode would lose.
        const QByteArray jpeg = encoded(picture(), "JPEG") + "EXTRA";
        QVERIFY(write(other.filePath("Camera 1.JPG"), jpeg));
        const Result<QString> stored = storeImageFile(other.filePath("Camera 1.JPG"), p.inputs(), FileRoots::forInputs());
        QVERIFY2(stored.ok(), qPrintable(stored.error().message));
        QCOMPARE(read(stored.value()), jpeg);
        QVERIFY(QFileInfo(stored.value()).fileName().startsWith("Camera_1-"));
        QVERIFY(stored.value().endsWith(".jpg"));
    }

    void theClipboardPrefersEncodedBytesAndOnlyThenPixels() {
        const QByteArray png = encoded(picture(), "PNG"), jpeg = encoded(picture(), "JPEG");
        QMimeData both;
        both.setData("image/jpeg", jpeg);
        both.setData("image/png", png);
        QCOMPARE(clipboardImageBytes(both).value(), png);
        QMimeData onlyJpeg;
        onlyJpeg.setData("image/jpeg", jpeg);
        QCOMPARE(clipboardImageBytes(onlyJpeg).value(), jpeg); // never re-encoded
        QMimeData pixels;
        pixels.setImageData(picture());
        const Result<QByteArray> fromPixels = clipboardImageBytes(pixels);
        QCOMPARE(sniffImage(fromPixels.value())->extension, QString("png")); // lossless
        QVERIFY(!clipboardImageBytes(QMimeData()).ok());
        QVERIFY(!clipboardHasImage(QMimeData()));
    }

    // Moving a workflow's inputs never points a node at a name it did not write: a symbolic
    // link (even one that leads nowhere) holds the name, so the copy takes another.
    void movingInputsNeverTakesANameALinkHolds() {
        Pictures p;
        QTemporaryDir place;
        const QByteArray png = encoded(picture(), "PNG");
        const Result<QString> stored = storeImage(png, p.inputs(), FileRoots::forInputs(), "cat.png");
        QVERIFY(stored.ok());
        const QString name = QFileInfo(stored.value()).fileName();
        QVERIFY(QDir().mkpath(place.filePath("inputs")));
        QVERIFY(QFile::link(place.filePath("nowhere.png"), place.filePath("inputs/" + name)));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%1"}}}}})")
                                               .arg(stored.value()).toUtf8()).value();
        const Errors errors = relocateInputs(document, registry, {}, place.filePath("flow.fugue"));
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString literal = document.graph.nodes.value("src").inputs.value("file").literal().toString();
        QVERIFY2(literal.startsWith("inputs/") && literal != "inputs/" + name, qPrintable(literal));
        QVERIFY(!QFileInfo(place.filePath(literal)).isSymLink());
        QCOMPARE(read(place.filePath(literal)), png);
        QVERIFY(QFileInfo(place.filePath("inputs/" + name)).isSymLink());
    }

    // A painted mask is an input file as a picture is (#174): saving the workflow to another folder
    // copies it into `inputs/` beside the file and the `mask` input names it there, relative.
    void aPaintedMaskIsCarriedIntoTheInputsFolderOnSave() {
        Pictures p;
        QTemporaryDir place;
        QImage gray(12, 8, QImage::Format_Grayscale8);
        gray.fill(255);
        const QByteArray png = encoded(picture(), "PNG");
        const QByteArray maskPng = encoded(gray, "PNG");
        const Result<QString> photo = storeImage(png, p.inputs(), FileRoots::forInputs(), "cat.png");
        const Result<QString> mask = storeImage(maskPng, p.inputs(), FileRoots::forInputs(), "cat-mask");
        QVERIFY(photo.ok() && mask.ok());
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "%1", "mask": "%2"}},
            "paint": {"type": "fugue/paint-mask", "version": "1.0.0", "inputs": {"image": {"from": "src.image"}, "mask": "%2"}}}}})")
                                               .arg(photo.value(), mask.value()).toUtf8()).value();
        const Errors errors = relocateInputs(document, registry, {}, place.filePath("flow.fugue"));
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString name = QFileInfo(mask.value()).fileName();
        QCOMPARE(document.graph.nodes.value("src").inputs.value("mask").literal().toString(), "inputs/" + name);
        QCOMPARE(document.graph.nodes.value("paint").inputs.value("mask").literal().toString(), "inputs/" + name);
        QCOMPARE(read(place.filePath("inputs/" + name)), maskPng);
        // The picture's file is untouched: the same bytes under the same name.
        QCOMPARE(read(photo.value()), png);
        QCOMPARE(read(place.filePath("inputs/" + QFileInfo(photo.value()).fileName())), png);
    }

    // Relative paths become full ones so a copy of the workflow elsewhere names the same files, the
    // mask's too.
    void aRelativeMaskPathBecomesFullWhenMadeAbsolute() {
        QTemporaryDir place;
        QVERIFY(QDir().mkpath(place.filePath("inputs")));
        const QString where = QDir(place.path()).canonicalPath();
        QVERIFY(write(where + "/inputs/m.png", encoded(picture(), "PNG")));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QByteArray(R"({"fugue": 1, "graph": {"nodes": {
            "paint": {"type": "fugue/paint-mask", "version": "1.0.0", "inputs": {"mask": "inputs/m.png"}}}}})")).value();
        makeInputsAbsolute(document, registry, where + "/flow.fugue");
        QCOMPARE(document.graph.nodes.value("paint").inputs.value("mask").literal().toString(), where + "/inputs/m.png");
    }

    // A template's painted mask is taken into the inputs folder with its picture, so the opened workflow
    // no longer leans on the template's folder.
    void aTemplatesMaskIsTakenIn() {
        Pictures p;
        QTemporaryDir folder;
        QVERIFY(QDir().mkpath(folder.filePath("inputs")));
        const QString root = QDir(folder.path()).canonicalPath();
        QImage gray(12, 8, QImage::Format_Grayscale8);
        gray.fill(255);
        const QByteArray maskPng = encoded(gray, "PNG");
        QVERIFY(write(root + "/inputs/shot.png", encoded(picture(), "PNG")));
        QVERIFY(write(root + "/inputs/shot-mask.png", maskPng));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-image", "version": "1.1.0",
                    "inputs": {"file": "%1/inputs/shot.png", "mask": "%1/inputs/shot-mask.png"}}}}})")
                                               .arg(root).toUtf8()).value();
        const Errors errors = takeInputsFrom(document, registry, folder.path());
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString mask = document.graph.nodes.value("src").inputs.value("mask").literal().toString();
        QVERIFY2(mask.startsWith(p.inputs() + "/") && mask.endsWith(".png"), qPrintable(mask));
        QCOMPARE(read(mask), maskPng);
        QVERIFY(document.graph.nodes.value("src").inputs.value("file").literal().toString().startsWith(p.inputs() + "/"));
    }

    // The same for a `paint` input (#181): its .ora file is carried into `inputs/` on save, made
    // absolute with the others, and taken in with a template; a file that is not a paint canvas is
    // not stored as one.
    void aPaintFileIsCarriedIntoTheInputsFolderOnSave() {
        Pictures p;
        QTemporaryDir place;
        const QByteArray ora = paintBytes();
        const Result<QString> stored = storeImage(ora, p.inputs(), FileRoots::forInputs(), "sketch");
        QVERIFY2(stored.ok(), qPrintable(stored.error().message));
        QVERIFY(stored.value().endsWith(".ora"));
        QCOMPARE(read(stored.value()), ora);
        // The same bytes again are the same file.
        QCOMPARE(storeImage(ora, p.inputs(), FileRoots::forInputs(), "other").value(), stored.value());
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "p": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"width": 12, "height": 8, "paint": "%1"}}}}})")
                                               .arg(stored.value()).toUtf8()).value();
        const Errors errors = relocateInputs(document, registry, {}, place.filePath("flow.fugue"));
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString name = QFileInfo(stored.value()).fileName();
        QCOMPARE(document.graph.nodes.value("p").inputs.value("paint").literal().toString(), "inputs/" + name);
        QCOMPARE(read(place.filePath("inputs/" + name)), ora);
    }

    void aRelativePaintPathBecomesFullWhenMadeAbsolute() {
        QTemporaryDir place;
        QVERIFY(QDir().mkpath(place.filePath("inputs")));
        const QString where = QDir(place.path()).canonicalPath();
        QVERIFY(write(where + "/inputs/p.ora", paintBytes()));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QByteArray(R"({"fugue": 1, "graph": {"nodes": {
            "p": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"paint": "inputs/p.ora"}}}}})")).value();
        makeInputsAbsolute(document, registry, where + "/flow.fugue");
        QCOMPARE(document.graph.nodes.value("p").inputs.value("paint").literal().toString(), where + "/inputs/p.ora");
    }

    void aTemplatesPaintFileIsTakenIn() {
        Pictures p;
        QTemporaryDir folder;
        QVERIFY(QDir().mkpath(folder.filePath("inputs")));
        const QString root = QDir(folder.path()).canonicalPath();
        const QByteArray ora = paintBytes();
        QVERIFY(write(root + "/inputs/sketch.ora", ora));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "p": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"paint": "%1/inputs/sketch.ora"}}}}})")
                                               .arg(root).toUtf8()).value();
        const Errors errors = takeInputsFrom(document, registry, folder.path());
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString paint = document.graph.nodes.value("p").inputs.value("paint").literal().toString();
        QVERIFY2(paint.startsWith(p.inputs() + "/") && paint.endsWith(".ora"), qPrintable(paint));
        QCOMPARE(read(paint), ora);
    }

    void aZipThatLooksLikeAPaintFileButIsNotOneIsNotStored() {
        Pictures p;
        QByteArray fake = paintBytes();
        fake.truncate(fake.size() / 2); // the header says paint, the zip is cut
        QVERIFY(!storeImage(fake, p.inputs(), FileRoots::forInputs(), "cut").ok());
        QVERIFY(names(p.inputs()).isEmpty());
    }

    // A template that ships a video (or a song) keeps it where it is: only pictures are taken in.
    void aTemplatesVideoIsNotTakenInAsAnImage() {
        Pictures p;
        QTemporaryDir folder;
        QVERIFY(QDir().mkpath(folder.filePath("inputs")));
        const QString video = QDir(folder.path()).canonicalPath() + "/inputs/clip.mp4";
        QVERIFY(write(video, QByteArray("\0\0\0\x18" "ftypmp42", 12) + QByteArray(100, 'x')));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Document document = Document::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-video", "version": "1.0.0", "inputs": {"file": "%1"}}}}})")
                                               .arg(video).toUtf8()).value();
        const Errors errors = takeInputsFrom(document, registry, folder.path());
        QVERIFY2(!hasErrors(errors), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(document.graph.nodes.value("src").inputs.value("file").literal().toString(), video);
        QVERIFY(names(p.inputs()).isEmpty());
    }

    // A file manager's copy also offers decoded pixels: the file, byte for byte, is taken
    // before them (Appendix A: encoded bytes, then files, then pixels).
    void aCopiedFileIsTakenBeforeDecodedPixels() {
        Pictures p;
        QTemporaryDir other;
        const QByteArray jpeg = encoded(picture(), "JPEG") + "EXTRA";
        QVERIFY(write(other.filePath("photo.jpg"), jpeg));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        ImageInputs inputs(&canvas, &registry);
        QMimeData data;
        data.setImageData(picture(Qt::blue));
        data.setUrls({QUrl::fromLocalFile(other.filePath("photo.jpg"))});
        QVERIFY(inputs.paste(data));
        const Nodes nodes = canvas.document().graph.nodes;
        QCOMPARE(nodes.size(), 1);
        const QString file = nodes.cbegin()->inputs.value("file").literal().toString();
        QVERIFY2(file.endsWith(".jpg"), qPrintable(file));
        QCOMPARE(read(file), jpeg);
    }

    // The picker's thumbnails are file URLs made from the path: #, ? and % are the name.
    void recentInputsCarryAFileUrlOfTheirPath() {
        Pictures p;
        QVERIFY(QDir().mkpath(p.inputs()));
        const QString path = p.inputs() + "/c#1 %41?.png";
        QVERIFY(write(path, encoded(picture(), "PNG")));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        ImageInputs inputs(&canvas, &registry);
        const QVariantList recent = inputs.recent();
        QCOMPARE(recent.size(), 1);
        QCOMPARE(recent.first().toMap().value("url").toUrl().toLocalFile(), path);
    }

    void recentInputsAreNewestFirstAndCapped() {
        Pictures p;
        const FileRoots roots = FileRoots::forInputs();
        for (int i = 0; i < 60; ++i) {
            const Result<QString> stored = storeImage(encoded(picture(QColor(i, 0, 0)), "PNG"), p.inputs(), roots, "n");
            QVERIFY(stored.ok());
            QFile file(stored.value());
            QVERIFY(file.open(QIODevice::ReadWrite));
            file.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + i), QFileDevice::FileModificationTime);
        }
        const QList<RecentInput> recent = recentInputs(48);
        QCOMPARE(recent.size(), 48);
        QVERIFY(recent.first().modified > recent.last().modified);
        QCOMPARE(recent.first().modified, qint64(1700000059) * 1000);
    }
};

QTEST_MAIN(TestImageInputs)
#include "test_image_inputs.moc"
