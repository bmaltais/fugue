// A workflow inside an image file (spec 11.7): a PNG iTXt chunk with the keyword
// `fugue`, a WebP XMP chunk. The tests read the chunk out of the written bytes with
// parsers of their own rather than through readEmbeddedWorkflow(), because a library
// may write a tEXt chunk where iTXt was meant and a reader that accepts both would hide it.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <vips/vips8>

#include "engine/embed.h"
#include "engine/file_hashes.h"
#include "engine/media.h"

using namespace fugue;

namespace {

struct Chunk {
    QByteArray type;
    QByteArray data;
};

// The WebP bytes webpWithWorkflow() makes, written to `path`.
Errors writeWebp(const vips::VImage &image, const QString &path, const QByteArray &workflow) {
    const Result<QByteArray> bytes = webpWithWorkflow(image, workflow);
    if (!bytes)
        return bytes.errors();
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(*bytes) != bytes->size())
        return {makeError("permission_denied", "cannot write " + path)};
    return {};
}

QList<Chunk> pngChunks(const QByteArray &bytes) {
    QList<Chunk> chunks;
    qsizetype at = 8;
    while (at + 12 <= bytes.size()) {
        const quint32 length = qFromBigEndian<quint32>(bytes.constData() + at);
        chunks.append({bytes.mid(at + 4, 4), bytes.mid(at + 8, length)});
        at += 12 + length;
    }
    return chunks;
}

// Every RIFF chunk of a WebP file, by FourCC.
QList<Chunk> webpChunks(const QByteArray &bytes) {
    QList<Chunk> chunks;
    qsizetype at = 12;
    while (at + 8 <= bytes.size()) {
        const quint32 length = qFromLittleEndian<quint32>(bytes.constData() + at + 4);
        chunks.append({bytes.mid(at, 4), bytes.mid(at + 8, length)});
        at += 8 + length + (length & 1);
    }
    return chunks;
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray crcOf(const QByteArray &typeAndData) {
    quint32 crc = 0xffffffffu;
    for (const char byte : typeAndData) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    crc = ~crc;
    QByteArray out(4, 0);
    qToBigEndian(crc, out.data());
    return out;
}

} // namespace

class TestEmbed : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<FileHashes> m_hashes;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    static vips::VImage pixels(int width, int height, int bands = 3) {
        return (vips::VImage::black(width, height, vips::VImage::option()->set("bands", bands)) + 80).cast(VIPS_FORMAT_UCHAR);
    }
    static MediaHandle image(int width, int height, int bands = 3) {
        return MediaHandle::fromImage(pixels(width, height, bands)).value();
    }

    static QByteArray workflowText() {
        return "{\"fugue\":1,\"graph\":{\"nodes\":{\"t\":{\"type\":\"fugue/text\",\"version\":\"1.0.0\","
               "\"inputs\":{\"text\":\"a <b> & \\\"c\\\" \\u00e9\"}}}}}";
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        m_hashes = std::make_unique<FileHashes>(path("hashes.json"));
    }

    void aPngGetsAnITXtChunkKeyedFugue() {
        const MediaHandle png = image(5, 4).saved(path("src"), "a", *m_hashes).value();
        const QByteArray original = readAll(png.path());
        const Result<QByteArray> embedded = pngWithWorkflow(original, workflowText());
        QVERIFY2(embedded.ok(), qPrintable(embedded.error().message));

        const QList<Chunk> chunks = pngChunks(embedded.value());
        QCOMPARE(chunks.first().type, QByteArray("IHDR"));
        QList<Chunk> text;
        for (const Chunk &chunk : chunks)
            if (chunk.type == "iTXt" || chunk.type == "tEXt" || chunk.type == "zTXt")
                text.append(chunk);
        QCOMPARE(text.size(), 1);
        QCOMPARE(text.first().type, QByteArray("iTXt"));
        // keyword NUL, compression flag, compression method, language NUL, translated keyword NUL, text
        const QByteArray &data = text.first().data;
        QVERIFY(data.startsWith(QByteArray("fugue\0\0\0\0\0", 10)));
        QCOMPARE(data.mid(10), workflowText());
        // It comes before the pixels, and the CRC is right.
        qsizetype itxt = -1, idat = -1;
        for (qsizetype i = 0; i < chunks.size(); ++i) {
            if (chunks.at(i).type == "iTXt")
                itxt = i;
            if (chunks.at(i).type == "IDAT" && idat < 0)
                idat = i;
        }
        QVERIFY(itxt < idat);
        const qsizetype start = embedded.value().indexOf("iTXt") + 4 + data.size();
        QCOMPARE(embedded.value().mid(start, 4), crcOf("iTXt" + data));
        // Pixels are untouched: libvips decodes both to the same image.
        QFile out(path("embedded.png"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(embedded.value());
        out.close();
        const vips::VImage a = vips::VImage::new_from_file(png.path().toUtf8().constData());
        const vips::VImage b = vips::VImage::new_from_file(path("embedded.png").toUtf8().constData());
        QCOMPARE(a.width(), b.width());
        QCOMPARE((a - b).abs().max(), 0.0);
    }

    void embeddingTwiceKeepsOneChunk() {
        const MediaHandle png = image(3, 3).saved(path("src"), "twice", *m_hashes).value();
        const QByteArray once = pngWithWorkflow(readAll(png.path()), "{\"a\":1}").value();
        const QByteArray twice = pngWithWorkflow(once, "{\"b\":2}").value();
        int count = 0;
        for (const Chunk &chunk : pngChunks(twice))
            if (chunk.type == "iTXt") {
                ++count;
                QCOMPARE(chunk.data.mid(10), QByteArray("{\"b\":2}"));
            }
        QCOMPARE(count, 1);
    }

    void aFileThatIsNotAPngIsRefused() {
        QCOMPARE(pngWithWorkflow("not a png", "{}").error().code, QString("media_unreadable"));
    }

    void aWebpGetsAnXmpChunkHoldingTheWorkflow() {
        const Errors errors = writeWebp(pixels(6, 5), path("out.webp"), workflowText());
        QVERIFY2(!hasErrors(errors), qPrintable(errors.value(0).message));
        const QByteArray bytes = readAll(path("out.webp"));
        QVERIFY(bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP");
        QByteArray xmp;
        for (const Chunk &chunk : webpChunks(bytes))
            if (chunk.type == "XMP ")
                xmp = chunk.data;
        QVERIFY2(!xmp.isEmpty(), "no XMP chunk in the WebP file");
        const QRegularExpression tag("<fugue:workflow>(.*)</fugue:workflow>", QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = tag.match(QString::fromUtf8(xmp));
        QVERIFY2(match.hasMatch(), xmp.constData());
        QString text = match.captured(1);
        text.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&amp;", "&");
        QCOMPARE(text.toUtf8(), workflowText());
        // The image is still readable and the right size.
        const vips::VImage decoded = vips::VImage::new_from_file(path("out.webp").toUtf8().constData());
        QCOMPARE(decoded.width(), 6);
        QCOMPARE(decoded.height(), 5);
    }

    void aWebpWithoutAWorkflowHasNoXmp() {
        QVERIFY(!hasErrors(writeWebp(pixels(4, 4), path("plain.webp"), {})));
        for (const Chunk &chunk : webpChunks(readAll(path("plain.webp"))))
            QVERIFY(chunk.type != "XMP ");
    }

    void aWebpWithAlphaKeepsItAndItsWorkflow() {
        QVERIFY(!hasErrors(writeWebp(pixels(4, 4, 4), path("alpha.webp"), "{\"k\":1}")));
        const vips::VImage decoded = vips::VImage::new_from_file(path("alpha.webp").toUtf8().constData());
        QCOMPARE(decoded.bands(), 4);
        QCOMPARE(readEmbeddedWorkflow(path("alpha.webp")).value().value("k").toInt(), 1);
    }

    void readEmbeddedWorkflowReadsBothFormats() {
        const MediaHandle png = image(3, 3).saved(path("src"), "rd", *m_hashes).value();
        QFile out(path("rd.png"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(pngWithWorkflow(readAll(png.path()), workflowText()).value());
        out.close();
        QVERIFY(!hasErrors(writeWebp(pixels(3, 3), path("rd.webp"), workflowText())));
        const QJsonObject expected = QJsonDocument::fromJson(workflowText()).object();
        QCOMPARE(readEmbeddedWorkflow(path("rd.png")).value(), expected);
        QCOMPARE(readEmbeddedWorkflow(path("rd.webp")).value(), expected);
    }

    void readEmbeddedWorkflowSaysWhenThereIsNone() {
        const MediaHandle png = image(3, 3).saved(path("src"), "none", *m_hashes).value();
        const Result<QJsonObject> read = readEmbeddedWorkflow(png.path());
        QVERIFY(!read.ok());
        QCOMPARE(read.error().code, QString("invalid_document"));
        QCOMPARE(readEmbeddedWorkflow(path("missing.png")).error().code, QString("file_not_found"));
    }
};

QTEST_GUILESS_MAIN(TestEmbed)
#include "test_embed.moc"
