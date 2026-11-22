// A paint canvas as one OpenRaster file (#181, spec 14.4): the same layers give the same bytes,
// a document survives a round trip, the picture's pixels are never stored, and a hostile file is
// refused cleanly.
//
// Seam: engine/paint_file.h (and the PaintDocument it saves). Zips for the hostile cases are made
// here with libarchive, in memory; nothing is written outside the test's own folder.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <archive.h>
#include <archive_entry.h>

#include "engine/mask_paint.h"
#include "engine/paint_document.h"
#include "engine/paint_file.h"
#include "engine/vips.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

struct Entry {
    QByteArray name;
    QByteArray bytes;
};

QByteArray zipOf(const QList<Entry> &entries, bool deflate = false) {
    QByteArray out;
    archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_set_bytes_in_last_block(a, 1);
    if (deflate)
        archive_write_zip_set_compression_deflate(a);
    else
        archive_write_zip_set_compression_store(a);
    archive_write_open(
        a, &out, nullptr,
        [](archive *, void *data, const void *buffer, size_t length) -> la_ssize_t {
            static_cast<QByteArray *>(data)->append(static_cast<const char *>(buffer), qsizetype(length));
            return la_ssize_t(length);
        },
        nullptr);
    for (const Entry &entry : entries) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, entry.name.constData());
        const bool folder = entry.name.endsWith('/'); // a folder entry has no bytes
        archive_entry_set_filetype(e, folder ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(e, folder ? 0755 : 0644);
        archive_entry_set_size(e, folder ? 0 : entry.bytes.size());
        archive_write_header(a, e);
        archive_write_data(a, entry.bytes.constData(), size_t(entry.bytes.size()));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

const QByteArray mime = "image/openraster";

QByteArray stack(int w, int h, const QByteArray &layers) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><image version=\"0.0.3\" w=\"" + QByteArray::number(w) + "\" h=\"" +
           QByteArray::number(h) + "\"><stack>" + layers + "</stack></image>";
}

QByteArray layerTag(const QByteArray &src, const QByteArray &extra = {}) {
    return "<layer name=\"L\" src=\"" + src + "\" x=\"0\" y=\"0\" opacity=\"1\" visibility=\"visible\" " + extra + "/>";
}

QByteArray png(int w, int h, int channels = 4) {
    MaskPaint m(w, h, channels);
    return m.toPng().value();
}

// A PNG header claiming a size, with nothing behind it that decodes.
QByteArray pngClaiming(quint32 w, quint32 h) {
    QByteArray b("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
    for (quint32 v : {w, h})
        for (int shift = 24; shift >= 0; shift -= 8)
            b.append(char((v >> shift) & 0xff));
    b.append(QByteArray("\x08\x06\0\0\0\0\0\0\0", 9));
    return b;
}

QByteArray validZip() {
    return zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"data/layer-0.png", png(4, 4)}});
}

void stroke(PaintDocument &d, double x, double y) {
    d.brush.size = 6;
    d.brush.color = Rgba{200, 30, 60, 255};
    QVERIFY(d.beginStroke());
    d.strokeTo({x, y});
    d.strokeTo({x + 10, y + 5});
    d.endStroke();
}

PaintDocument sample() {
    PaintDocument d(40, 30);
    MaskPaint picture(8, 8, 4);
    picture.fill(Rgba{10, 200, 30, 255});
    d.addLayer(LayerKind::Picture, "Picture", picture);
    d.addLayer(LayerKind::Paint, "Sketch \"one\" <&> \xc3\xa9");
    stroke(d, 5, 5);
    d.addLayer(LayerKind::Mask, "Mask");
    d.mode = Mode::Paint;
    stroke(d, 20, 15);
    const int hidden = d.addLayer(LayerKind::Paint, "Hidden");
    stroke(d, 2, 20);
    d.setVisible(hidden, false);
    d.setOpacity(d.layer(1).id, 0.35);
    return d;
}

} // namespace

class TestPaintFile : public QObject {
    Q_OBJECT

    static bool failsWith(const Result<PaintDocument> &result) {
        return !result.ok() && result.error().code == QLatin1String("media_unreadable");
    }

private slots:
    void initTestCase() { QVERIFY(initVips()); }

    // ---- the round trip ----------------------------------------------------------------------

    void aDocumentComesBackAsItWas() {
        const PaintDocument d = sample();
        const Result<QByteArray> bytes = paintToOra(d);
        QVERIFY(bytes.ok());
        const Result<PaintDocument> back = paintFromOra(bytes.value());
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back->width(), 40);
        QCOMPARE(back->height(), 30);
        QCOMPARE(back->layerCount(), d.layerCount());
        for (int i = 0; i < d.layerCount(); ++i) {
            const LayerInfo a = d.layer(i), b = back->layer(i);
            QCOMPARE(b.name, a.name);
            QCOMPARE(int(b.kind), int(a.kind));
            QCOMPARE(b.visible, a.visible);
            QCOMPARE(b.opacity, a.opacity);
            if (a.kind != LayerKind::Picture)
                QCOMPARE(back->pixels(b.id)->pixels(), d.pixels(a.id)->pixels());
        }
        QCOMPARE(back->flattenMask().pixels(), d.flattenMask().pixels());
        QVERIFY(!back->canUndo());
        QCOMPARE(back->layer(back->layerCount() - 1).id, back->activeLayer());
    }

    void anEmptyDocumentAndAOneLayerOneRoundTrip() {
        PaintDocument none(7, 5);
        const Result<PaintDocument> back = paintFromOra(paintToOra(none).value());
        QVERIFY(back.ok());
        QCOMPARE(back->layerCount(), 0);
        QCOMPARE(back->width(), 7);
    }

    void aFileOnDiskRoundTrips() {
        QTemporaryDir dir;
        const QString path = dir.filePath("canvas.ora");
        const PaintDocument d = sample();
        QVERIFY(!hasErrors(writePaintFile(d, path)));
        const Result<PaintDocument> back = readPaintFile(path);
        QVERIFY(back.ok());
        QCOMPARE(back->layerCount(), d.layerCount());
        QCOMPARE(readPaintFile(dir.filePath("gone.ora")).error().code, QStringLiteral("file_not_found"));
    }

    // ---- the same layers, the same bytes -------------------------------------------------------

    void theSameLayersGiveTheSameBytes() {
        const QByteArray one = paintToOra(sample()).value();
        QTest::qSleep(1100); // a clock in the file would show
        const QByteArray two = paintToOra(sample()).value();
        QCOMPARE(one, two);
        // Read and written again: the same bytes (ids and history are not in the file).
        QCOMPARE(paintToOra(paintFromOra(one).value()).value(), one);
    }

    void theFileIsAnOpenRasterZip() {
        const QByteArray bytes = paintToOra(sample()).value();
        QVERIFY(looksLikeOra(bytes));
        QCOMPARE(bytes.left(4), QByteArray("PK\x03\x04", 4));
        QCOMPARE(bytes.mid(8, 2), QByteArray("\0\0", 2)); // the first entry is stored
        QVERIFY(!looksLikeOra(png(4, 4)));
        QVERIFY(!looksLikeOra(QByteArray()));
    }

    void thePicturesPixelsAreNotInTheFile() {
        PaintDocument a(20, 20), b(20, 20);
        MaskPaint red(64, 64, 4), blue(3, 3, 4);
        red.fill(Rgba{255, 0, 0, 255});
        blue.fill(Rgba{0, 0, 255, 255});
        a.addLayer(LayerKind::Picture, "Picture", red);
        b.addLayer(LayerKind::Picture, "Picture", blue);
        QCOMPARE(paintToOra(a).value(), paintToOra(b).value());
        const PaintDocument back = paintFromOra(paintToOra(a).value()).value();
        QCOMPARE(back.layer(0).kind, LayerKind::Picture);
        QVERIFY(back.pixels(back.layer(0).id)->isEmpty()); // a placeholder: nothing in it
    }

    void aLayerChangeIsADifferentFile() {
        PaintDocument d = sample();
        const QByteArray before = paintToOra(d).value();
        stroke(d, 1, 1);
        QVERIFY(paintToOra(d).value() != before);
    }

    void theCanvasSizeAndLayerCountAreCappedOnSave() {
        const Result<QByteArray> big = paintToOra(PaintDocument(20000, 10));
        QVERIFY(!big.ok());
        QCOMPARE(big.error().code, QStringLiteral("value_out_of_range"));
    }

    // ---- hostile files -------------------------------------------------------------------------

    void aValidHandMadeZipReads() {
        const Result<PaintDocument> r = paintFromOra(validZip());
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QCOMPARE(r->layerCount(), 1);
    }

    void folderEntriesOfAKnownNameAreSkippedAndOthersRefused() {
        // Zip tools add `data/`; it unpacks nothing, so a file with it reads.
        const QByteArray withFolder = zipOf({{"mimetype", mime}, {"data/", {}}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"data/layer-0.png", png(4, 4)}});
        const Result<PaintDocument> r = paintFromOra(withFolder);
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QCOMPARE(r->layerCount(), 1);
        // A folder of any other name, or one that climbs, is still refused.
        for (const char *name : {"other/", "data/sub/", "../", "/abs/"}) {
            QVERIFY2(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {name, {}}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"data/layer-0.png", png(4, 4)}}))), name);
        }
        // A folder entry first is not the mimetype.
        QVERIFY(failsWith(paintFromOra(zipOf({{"data/", {}}, {"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"data/layer-0.png", png(4, 4)}}))));
    }

    void aPlainPictureIsAOneLayerCanvas() {
        MaskPaint m(30, 20, 4);
        m.fill(Rgba{10, 20, 30, 255});
        const QByteArray bytes = m.toPng().value();
        QVERIFY(looksLikePicture(bytes));
        QVERIFY(!looksLikePicture("PK\x03\x04 text"));
        const Result<PaintDocument> r = paintFromPicture(bytes, "photo");
        QVERIFY2(r.ok(), qPrintable(r.error().message));
        QCOMPARE(r->width(), 30);
        QCOMPARE(r->height(), 20);
        QCOMPARE(r->layerCount(), 1);
        QCOMPARE(r->layer(0).name, QString("photo"));
        QCOMPARE(r->layer(0).kind, LayerKind::Paint);
        QCOMPARE(r->pixels(r->layer(0).id)->rgbaAt(3, 3).g, uint8_t(20));
    }

    void readCanvasFileTakesAnOraOrAPictureAndNamesTheKindsOtherwise() {
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &bytes) {
            QFile f(dir.filePath(name));
            [&] { QVERIFY(f.open(QIODevice::WriteOnly)); }();
            f.write(bytes);
            return dir.filePath(name);
        };
        QVERIFY(readCanvasFile(write("a.png", png(8, 6))).ok());
        QCOMPARE(readCanvasFile(write("a.png", png(8, 6)))->layer(0).name, QString("a"));
        QVERIFY(readCanvasFile(write("b.ora", validZip())).ok());
        const Result<PaintDocument> text = readCanvasFile(write("c.txt", "hello"));
        QVERIFY(failsWith(text));
        QVERIFY(text.error().message.contains("PNG, JPEG or WebP"));
        QVERIFY(failsWith(readCanvasFile(write("d.png", "\x89PNG\r\n\x1a\n broken"))));
        QCOMPARE(readCanvasFile(dir.filePath("missing.png")).error().code, QString("file_not_found"));
    }

    void notAZipOrEmptyOrCut() {
        QVERIFY(failsWith(paintFromOra(QByteArray())));
        QVERIFY(failsWith(paintFromOra("this is not a zip file at all, just text")));
        QVERIFY(failsWith(paintFromOra(png(4, 4))));
        const QByteArray good = validZip();
        QVERIFY(failsWith(paintFromOra(good.left(good.size() / 2))));
        QVERIFY(failsWith(paintFromOra(good.left(good.size() - 5))));
        QVERIFY(failsWith(paintFromOra(good.left(40))));
    }

    void theMimetypeMustBeFirstAndRight() {
        QVERIFY(failsWith(paintFromOra(zipOf({{"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"mimetype", mime}}))));
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", "image/png"}, {"stack.xml", stack(4, 4, "")}}))));
    }

    void aMissingStackIsRefused() {
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"data/layer-0.png", png(4, 4)}}))));
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", "<image w=\"4\" h=\"4\"/>"}}))));
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", "not xml <<<"}}))));
    }

    void aMissingLayerPictureIsRefused() {
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}}))));
    }

    void pathTraversalAndOddNamesAreRefused() {
        for (const char *name : {"../evil.png", "data/../../evil.png", "/etc/passwd", "..\\evil.png", "data//x.png", "C:evil"}) {
            const QByteArray zip = zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))},
                                          {"data/layer-0.png", png(4, 4)}, {name, "x"}});
            QVERIFY2(failsWith(paintFromOra(zip)), name);
        }
        // A layer that names a file outside data/.
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("../x.png"))}, {"../x.png", png(4, 4)}}))));
        // An entry no paint file has (skipping it would still unpack it).
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, "")}, {"readme.txt", "hi"}}))));
    }

    void hugeDimensionsAreRefusedBeforeAnythingIsMade() {
        for (const auto &size : {std::pair{100000, 100000}, std::pair{16384, 16384}, std::pair{20000, 1}, std::pair{0, 4}, std::pair{-3, 4}}) {
            const Result<PaintDocument> r = paintFromOra(
                zipOf({{"mimetype", mime}, {"stack.xml", stack(size.first, size.second, layerTag("data/layer-0.png"))}, {"data/layer-0.png", png(4, 4)}}));
            QVERIFY2(failsWith(r), qPrintable(QString::number(size.first)));
        }
    }

    void aLayerPictureThatClaimsMoreThanTheCanvasIsRefusedBeforeItDecodes() {
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))},
                                              {"data/layer-0.png", pngClaiming(60000, 60000)}}))));
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))},
                                              {"data/layer-0.png", png(5, 4)}}))));
    }

    void tooManyLayersOrTooMuchPixelMemoryIsRefused() {
        QByteArray tags;
        QList<Entry> entries{{"mimetype", mime}};
        for (int i = 0; i < paintFileMaxLayers + 1; ++i)
            tags += layerTag("data/layer-" + QByteArray::number(i) + ".png", "fugue:kind=\"picture\" xmlns:fugue=\"urn:fugue:ora\" ");
        entries << Entry{"stack.xml", stack(4, 4, tags)};
        QVERIFY(failsWith(paintFromOra(zipOf(entries))));
        // 8192 x 8192 and five full layers would be 1.25 GB decoded: refused from the stack alone.
        QByteArray five;
        for (int i = 0; i < 5; ++i)
            five += layerTag("data/layer-" + QByteArray::number(i) + ".png");
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(8192, 8192, five)}}))));
    }

    void aZipBombIsStoppedAtTheCap() {
        // 300 MB of zeros packs to a few hundred KB; the entry cap stops reading at 256 MB.
        const QByteArray zeros(300 << 20, '\0');
        const QByteArray bomb = zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))}, {"data/layer-0.png", zeros}}, true);
        QVERIFY(bomb.size() < (2 << 20));
        QVERIFY(failsWith(paintFromOra(bomb)));
        // The same in stack.xml, whose cap is 1 MB.
        const QByteArray xmlBomb = zipOf({{"mimetype", mime}, {"stack.xml", QByteArray(5 << 20, ' ')}}, true);
        QVERIFY(failsWith(paintFromOra(xmlBomb)));
    }

    void aDoctypeGroupOffsetOrBadOpacityIsRefused() {
        const QByteArray tag = layerTag("data/layer-0.png");
        const auto zipWith = [&](const QByteArray &xml) {
            return zipOf({{"mimetype", mime}, {"stack.xml", xml}, {"data/layer-0.png", png(4, 4)}});
        };
        QVERIFY(failsWith(paintFromOra(zipWith("<!DOCTYPE image [<!ENTITY a \"aaaa\">]>" + stack(4, 4, tag)))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, "<stack>" + tag + "</stack>")))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, "<layer name=\"L\" src=\"data/layer-0.png\" x=\"3\" y=\"0\"/>")))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, "<layer name=\"L\" src=\"data/layer-0.png\" opacity=\"7\"/>")))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, "<layer name=\"L\" src=\"data/layer-0.png\" opacity=\"nan\"/>")))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, tag + "<text/>")))));
        QVERIFY(failsWith(paintFromOra(zipWith(stack(4, 4, layerTag("data/layer-0.png", "xmlns:fugue=\"urn:fugue:ora\" fugue:kind=\"banana\""))))));
    }

    void aLayerPictureThatIsNotAPictureIsRefused() {
        QVERIFY(failsWith(paintFromOra(zipOf({{"mimetype", mime}, {"stack.xml", stack(4, 4, layerTag("data/layer-0.png"))},
                                              {"data/layer-0.png", "plain text, 40 bytes long, not a picture....."}}))));
    }

    void aHugeFileOnDiskIsRefusedBeforeItIsRead() {
        QTemporaryDir dir;
        QFile file(dir.filePath("big.ora"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(paintFileMaxBytes + 1)); // sparse
        file.close();
        QVERIFY(failsWith(readPaintFile(file.fileName())));
        QCOMPARE(readPaintFile(dir.path()).error().code, QStringLiteral("file_not_found")); // a folder is not a file
    }

    // ---- the document's new calls ----------------------------------------------------------------

    void scaledToKeepsTheLayersAndRescalesTheirPixels() {
        const PaintDocument d = sample();
        const PaintDocument big = d.scaledTo(80, 60);
        QCOMPARE(big.width(), 80);
        QCOMPARE(big.layerCount(), d.layerCount());
        QCOMPARE(big.layer(1).name, d.layer(1).name);
        QCOMPARE(big.layer(1).opacity, d.layer(1).opacity);
        QCOMPARE(big.pixels(big.layer(1).id)->width(), 80);
        QCOMPARE(big.pixels(big.layer(2).id)->channels(), 1);
        QVERIFY(!big.canUndo());
        QVERIFY(!big.flatten().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPaintFile)
#include "test_paint_file.moc"
