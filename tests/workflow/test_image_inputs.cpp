// Pasting and dropping images (spec 14.1, Appendix A "images in"): the pure parts of
// workflow/image_inputs.h on a temporary inputs folder, and the same on a canvas.

#include <QBuffer>
#include <QClipboard>
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
