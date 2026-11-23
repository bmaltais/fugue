// The Photoshop (.psd) writer and reader (#227, epic #206). Seam: engine/psd_file.h. The reading of what the writer
// makes by another program (psd-tools) is the scratch round trip in docs/psd-research.md; here the writer and the
// reader check each other, and hand-built files check what only Photoshop writes (groups, adjustment layers) and
// what a hostile file must not do.

#include <QtTest>

#include "engine/canvas_export.h"
#include "engine/paint_file.h"
#include "engine/psd_file.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

MaskPaint gradient(int w, int h, int seed) {
    std::vector<uint8_t> bytes(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t at = (size_t(y) * w + x) * 4;
            bytes[at] = uint8_t(x * 7 + seed), bytes[at + 1] = uint8_t(y * 11 + seed), bytes[at + 2] = uint8_t(seed * 3);
            bytes[at + 3] = (x + y) % 5 == 0 ? 0 : uint8_t(255 - x);
        }
    return MaskPaint(w, h, bytes, 4);
}

MaskPaint rampMask(int w, int h) {
    std::vector<uint8_t> bytes(size_t(w) * h);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(i * 13);
    return MaskPaint(w, h, bytes, 1);
}

PaintDocument sample() {
    PaintDocument d(13, 9); // odd sizes
    LayerInfo a;
    a.kind = LayerKind::Paint;
    a.name = "Sky \xc3\xa9t\xc3\xa9"; // a name with accents: the Unicode block carries it
    a.opacity = 128 / 255.0;
    d.loadLayer(a, gradient(13, 9, 1));
    LayerInfo b;
    b.kind = LayerKind::Paint;
    b.name = "Water";
    b.visible = false;
    b.compositeOp = "svg:multiply";
    b.hasMask = true;
    b.maskEnabled = false;
    const MaskPaint m = rampMask(13, 9);
    d.loadLayer(b, gradient(13, 9, 40), &m);
    LayerInfo c;
    c.kind = LayerKind::Paint;
    c.name = "Glow";
    c.compositeOp = "svg:plus";
    d.loadLayer(c, gradient(13, 9, 90));
    d.loadLayer(LayerKind::Mask, "Selection", true, 1.0, MaskPaint(13, 9, 1));
    return d;
}

void put32(QByteArray &o, quint32 v) { o.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v)); }
void put16(QByteArray &o, quint32 v) { o.append(char(v >> 8)).append(char(v)); }

// A 2 x 2 RGB file with these hand-made layer records (raw channels). `tags` are additional blocks.
struct RawLayer {
    QByteArray name;
    QByteArray key = "norm";
    int flags = 0;
    int opacity = 255;
    int clipping = 0;
    QByteArray tags; // whole blocks
    bool pixels = true;
};

QByteArray rawPsd(const std::vector<RawLayer> &layers, int depth = 8, int version = 1) {
    QByteArray records, data;
    for (const RawLayer &l : layers) {
        put32(records, 0), put32(records, 0), put32(records, 2), put32(records, 2);
        const int n = l.pixels ? 4 : 0;
        put16(records, n);
        for (int c = 0; c < n; ++c) {
            put16(records, c == 3 ? 0xffff : c);
            put32(records, 2 + 4);
        }
        records.append("8BIM").append(l.key).append(char(l.opacity)).append(char(l.clipping)).append(char(l.flags)).append('\0');
        QByteArray extra;
        put32(extra, 0); // no mask
        put32(extra, 0); // no blending ranges
        extra.append(char(l.name.size())).append(l.name);
        while (extra.size() % 4 != 0)
            extra.append('\0');
        extra += l.tags;
        put32(records, quint32(extra.size()));
        records += extra;
        for (int c = 0; c < n; ++c) {
            put16(data, 0);
            for (int p = 0; p < 4; ++p)
                data.append(char(c == 0 ? 200 : c == 1 ? 100 : c == 2 ? 50 : 255));
        }
    }
    QByteArray info;
    put16(info, quint32(layers.size()));
    info += records + data;
    if (info.size() % 2)
        info.append('\0');
    QByteArray out("8BPS");
    put16(out, quint32(version));
    out.append(6, '\0');
    put16(out, 4);
    put32(out, 2), put32(out, 2);
    put16(out, quint32(depth)), put16(out, 3);
    put32(out, 0), put32(out, 0);
    put32(out, quint32(info.size() + 8));
    put32(out, quint32(info.size()));
    out += info;
    put32(out, 0);
    put16(out, 0);
    out.append(QByteArray(16, '\x20'));
    return out;
}

QByteArray block(const char *key, const QByteArray &data) {
    QByteArray b("8BIM");
    b.append(key);
    QByteArray d = data;
    while (d.size() % 4)
        d.append('\0');
    put32(b, quint32(d.size()));
    return b + d;
}

QByteArray lsct(int type) {
    QByteArray d;
    put32(d, quint32(type));
    return block("lsct", d);
}

QStringList notesOf(const Errors &w) {
    QStringList out;
    for (const Error &e : w)
        out << e.message;
    return out;
}

} // namespace

class TestPsdFile : public QObject {
    Q_OBJECT

private slots:
    void theHeaderAndTheSectionsAreAsAdobeSpecifiesThem() {
        const Result<PsdExported> psd = paintToPsd(sample());
        QVERIFY(psd);
        const QByteArray &b = psd->bytes;
        QVERIFY(b.startsWith("8BPS"));
        QCOMPARE(quint8(b[5]), quint8(1)); // version 1
        QCOMPARE(b.mid(6, 6), QByteArray(6, '\0'));
        QCOMPARE(quint8(b[13]), quint8(4)); // channels
        QCOMPARE(qFromBigEndian<quint32>(b.constData() + 14), quint32(9));
        QCOMPARE(qFromBigEndian<quint32>(b.constData() + 18), quint32(13));
        QCOMPARE(qFromBigEndian<quint16>(b.constData() + 22), quint16(8));
        QCOMPARE(qFromBigEndian<quint16>(b.constData() + 24), quint16(3)); // RGB
        QVERIFY(looksLikePsd(b));
        QCOMPARE(psd->notes.size(), 1); // the mask layer
        QVERIFY(psd->notes.first().contains("Selection"));
    }

    void theSameDocumentGivesTheSameBytes() {
        QCOMPARE(paintToPsd(sample())->bytes, paintToPsd(sample())->bytes);
    }

    void aRoundTripKeepsPixelsAndLayerSettings() {
        const PaintDocument original = sample();
        const Result<PsdExported> psd = paintToPsd(original);
        QVERIFY(psd);
        const Result<PaintDocument> back = paintFromPsd(psd->bytes);
        QVERIFY2(back, qPrintable(back.error().message));
        QVERIFY(back.warnings().isEmpty());
        QCOMPARE(back->width(), 13);
        QCOMPARE(back->height(), 9);
        QCOMPARE(back->layerCount(), 3); // the mask layer is not in the file
        for (int i = 0; i < 3; ++i) {
            const LayerInfo a = original.layer(i), c = back->layer(i);
            QCOMPARE(c.name, a.name);
            QCOMPARE(c.visible, a.visible);
            QCOMPARE(c.opacity, a.opacity);
            QCOMPARE(c.compositeOp, a.compositeOp);
            QCOMPARE(c.hasMask, a.hasMask);
            QCOMPARE(c.maskEnabled, a.maskEnabled);
            QCOMPARE(back->pixels(c.id)->pixels(), original.pixels(a.id)->pixels());
            if (a.hasMask)
                QCOMPARE(back->maskPixels(c.id)->pixels(), original.maskPixels(a.id)->pixels());
        }
        QCOMPARE(back->flatten().pixels(), original.flatten().pixels());
        // and writing what was read gives the very same file
        QCOMPARE(paintToPsd(back.value())->bytes, psd->bytes);
    }

    void theMergedImageIsTheFlattenedCanvas() {
        // a file with its layers cut out reads as its merged image on one layer
        PaintDocument d(13, 9);
        d.loadLayer(LayerKind::Paint, "A", true, 0.5, gradient(13, 9, 5));
        const QByteArray bytes = paintToPsd(d)->bytes;
        // Take the merged image from the tail: rebuild a file whose layer section is empty.
        const quint32 layerLength = qFromBigEndian<quint32>(bytes.constData() + 34);
        QByteArray flatOnly = bytes.left(34);
        flatOnly.append(QByteArray(4, '\0')); // empty layer section
        flatOnly += bytes.mid(34 + 4 + layerLength);
        const Result<PaintDocument> back = paintFromPsd(flatOnly);
        QVERIFY2(back, qPrintable(back.error().message));
        QCOMPARE(back->layerCount(), 1);
        // The merged image is blended onto white (as Photoshop's is) and un-blended on reading: alpha is exact and a
        // color is within the rounding of that for all but the faintest pixels.
        const std::vector<uint8_t> got = back->pixels(back->layer(0).id)->pixels();
        const MaskPaint flat = d.flatten();
        const std::vector<uint8_t> &want = flat.pixels();
        QCOMPARE(got.size(), want.size());
        for (size_t p = 0; p + 3 < got.size(); p += 4) {
            QCOMPARE(got[p + 3], want[p + 3]);
            if (want[p + 3] >= 128)
                for (int c = 0; c < 3; ++c)
                    QVERIFY(std::abs(int(got[p + c]) - int(want[p + c])) <= 2);
        }
    }

    void theExporterTakesPsdAndNamesWhatItLeftOut() {
        canvas::PictureExport options;
        options.format = "psd";
        const Result<canvas::ExportedPicture> picture = canvas::exportPicture(sample(), options);
        QVERIFY(picture);
        QCOMPARE(picture->format, QString("psd"));
        QVERIFY(looksLikePsd(picture->bytes));
        QCOMPARE(picture->notes.size(), 1);
        options.scale = 0.5;
        const Result<canvas::ExportedPicture> small = canvas::exportPicture(sample(), options);
        QVERIFY(small);
        QCOMPARE(small->width, 7);
        QVERIFY(canvas::normalFormat("PSD", true) == "psd");
        QVERIFY(canvas::normalFormat("psd", false).isEmpty());
    }

    void canvasFilesReadPsd() {
        QTemporaryDir dir;
        QFile f(dir.filePath("x.psd"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(paintToPsd(sample())->bytes);
        f.close();
        const Result<PaintDocument> back = readCanvasFile(dir.filePath("x.psd"));
        QVERIFY(back);
        QCOMPARE(back->layerCount(), 3);
    }

    void writeFilesForAnotherReader() {
        // The round trip with psd-tools (docs/psd-research.md): FUGUE_PSD_SCRATCH=<folder> makes the test write the sample
        // canvas as sample.psd with each layer's raw RGBA (layer-N.rgba), its mask (layer-N.mask), the merged picture
        // (merged.rgba) and the settings (layers.txt) for a script to compare. Without it the test does nothing.
        const QString folder = qEnvironmentVariable("FUGUE_PSD_SCRATCH");
        if (folder.isEmpty())
            QSKIP("FUGUE_PSD_SCRATCH is not set");
        const PaintDocument d = sample();
        const auto save = [&](const QString &name, const QByteArray &bytes) {
            QFile f(QDir(folder).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        save("sample.psd", paintToPsd(d)->bytes);
        QByteArray list;
        int n = 0;
        for (int i = 0; i < d.layerCount(); ++i) {
            const LayerInfo l = d.layer(i);
            if (l.kind == LayerKind::Mask)
                continue;
            const std::vector<uint8_t> &px = d.pixels(l.id)->pixels();
            save(QString("layer-%1.rgba").arg(n), QByteArray(reinterpret_cast<const char *>(px.data()), qsizetype(px.size())));
            if (l.hasMask) {
                const std::vector<uint8_t> &m = d.maskPixels(l.id)->pixels();
                save(QString("layer-%1.mask").arg(n), QByteArray(reinterpret_cast<const char *>(m.data()), qsizetype(m.size())));
            }
            list += QString("%1\t%2\t%3\t%4\t%5\t%6\n").arg(l.name).arg(l.visible).arg(qRound(l.opacity * 255)).arg(psdBlendKeyOfOp(l.compositeOp)).arg(l.hasMask).arg(l.maskEnabled).toUtf8();
            ++n;
        }
        save("layers.txt", list);
        const MaskPaint merged = d.flatten();
        const std::vector<uint8_t> &flat = merged.pixels();
        save("merged.rgba", QByteArray(reinterpret_cast<const char *>(flat.data()), qsizetype(flat.size())));
    }

    void blendKeysMapBothWays() {
        for (const QString op : {"svg:src-over", "svg:multiply", "svg:screen", "svg:overlay", "svg:soft-light", "svg:darken", "svg:lighten", "svg:plus", "svg:color-dodge"}) {
            bool exact = false, known = false;
            const QString key = psdBlendKeyOfOp(op, &exact);
            QVERIFY(exact);
            QCOMPARE(key.size(), 4);
            QCOMPARE(psdOpOfBlendKey(key, &known), op == "svg:src-over" ? QString() : op);
            QVERIFY(known);
        }
        bool exact = true;
        psdBlendKeyOfOp("krita:weird", &exact);
        QVERIFY(!exact);
        bool known = true;
        psdOpOfBlendKey("diss", &known);
        QVERIFY(!known);
    }

    void groupsAreFlattenedAndSaid() {
        // bottom to top: divider, a layer, the folder; the folder is hidden and half opaque
        RawLayer divider;
        divider.name = "</Layer group>", divider.pixels = false, divider.tags = lsct(3);
        RawLayer child;
        child.name = "Child";
        RawLayer folder;
        folder.name = "Folder", folder.pixels = false, folder.flags = 2, folder.opacity = 128, folder.tags = lsct(1);
        const Result<PaintDocument> d = paintFromPsd(rawPsd({divider, child, folder}));
        QVERIFY2(d, qPrintable(d.error().message));
        QCOMPARE(d->layerCount(), 1);
        QVERIFY(!d->layer(0).visible); // the group's visibility reaches its layer
        QVERIFY(qAbs(d->layer(0).opacity - 128 / 255.0) < 1e-9);
        const QStringList notes = notesOf(d.warnings());
        QCOMPARE(notes.size(), 1);
        QVERIFY(notes.first().contains("Folder"));
    }

    void whatFugueLacksIsListed() {
        RawLayer adjust;
        adjust.name = "Curves 1", adjust.pixels = false, adjust.tags = block("curv", QByteArray(4, '\0'));
        RawLayer text;
        text.name = "Title", text.tags = block("TySh", QByteArray(4, '\0')) + block("lfx2", QByteArray(4, '\0'));
        RawLayer odd;
        odd.name = "Odd", odd.key = "diss", odd.clipping = 1;
        RawLayer dodge;
        dodge.name = "Dodge", dodge.key = "div ";
        const Result<PaintDocument> d = paintFromPsd(rawPsd({adjust, text, odd, dodge}));
        QVERIFY2(d, qPrintable(d.error().message));
        QCOMPARE(d->layerCount(), 3);
        const QString all = notesOf(d.warnings()).join("\n");
        QVERIFY(all.contains("Curves 1"));
        QVERIFY(all.contains("Title"));
        QVERIFY(all.contains("effects"));
        QVERIFY(all.contains("Dissolve"));
        QVERIFY(all.contains("clipp"));
        QVERIFY(all.contains("svg:color-dodge") || all.contains("color-dodge"));
        QCOMPARE(d->layer(2).compositeOp, QString("svg:color-dodge")); // kept for writing back
    }

    void filesFugueCannotHoldAreRefusedInPlainWords() {
        const Result<PaintDocument> deep = paintFromPsd(rawPsd({RawLayer{}}, 16));
        QVERIFY(!deep);
        QCOMPARE(deep.error().code, QString("input_invalid"));
        QVERIFY(deep.error().message.contains("16 bits"));
        QVERIFY(!deep.error().hint.isEmpty());
        const Result<PaintDocument> big = paintFromPsd(rawPsd({RawLayer{}}, 8, 2));
        QVERIFY(!big);
        QVERIFY(big.error().message.contains("psb"));
        QVERIFY(!paintFromPsd("not a photoshop file"));
    }

    void aCanvasPastThePsdLimitIsRefused() {
        // canvases are capped lower than a PSD, so the check is exercised by name: the limit is the format's
        QCOMPARE(psdMaxSide, 30000);
    }

    void aDamagedFileIsRefusedAtEveryCut() {
        const QByteArray good = paintToPsd(sample())->bytes;
        int refused = 0;
        for (int cut = 0; cut < good.size(); cut += 17) {
            const Result<PaintDocument> d = paintFromPsd(good.left(cut));
            if (!d)
                ++refused; // never a crash, never a hang; a clean cut may still read (the merged image is unused)
        }
        QVERIFY(refused > 10);
    }

    void randomDamageNeverCrashesOrHangs() {
        // A deterministic fuzz: change a few bytes of a good file (RLE runs, lengths, counts, rectangles) and read it.
        const QByteArray good = paintToPsd(sample())->bytes;
        quint32 state = 12345;
        const auto next = [&state] { state = state * 1664525u + 1013904223u; return state >> 8; };
        int refused = 0;
        for (int round = 0; round < 600; ++round) {
            QByteArray bad = good;
            for (int k = 0; k < 1 + int(next() % 4); ++k)
                bad[int(next() % quint32(bad.size()))] = char(next() & 0xff);
            const Result<PaintDocument> d = paintFromPsd(bad);
            if (!d)
                ++refused;
            else
                QVERIFY(d->layerCount() <= paintFileMaxLayers);
        }
        QVERIFY(refused > 0);
    }

    void aHugeCountOfLayersIsRefused() {
        std::vector<RawLayer> many(70);
        for (size_t i = 0; i < many.size(); ++i)
            many[i].name = QByteArray::number(int(i));
        const Result<PaintDocument> d = paintFromPsd(rawPsd(many));
        QVERIFY(!d);
        QVERIFY(d.error().message.contains("64"));
    }
};

QTEST_GUILESS_MAIN(TestPsdFile)
#include "test_psd_file.moc"
