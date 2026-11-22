// The New canvas dialog's model (#232, epic #206, spec 14.5): presets, the typed size and its limits in plain words,
// backgrounds, the clipboard's picture (its size pre-filled, as Photoshop does), a picture file, unique names, the
// settings remembered, and Paste as new canvas. Seam: canvas/new_canvas_model.h with a store and a settings file of
// the test's own; the clipboard is the process's (QClipboard works offscreen). No window.

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/new_canvas_model.h"
#include "engine/canvas_store.h"

using namespace fugue;

namespace {

struct Fixture {
    QTemporaryDir dir;
    NewCanvasModel model;
    Fixture() {
        model.setStoreRoot(dir.filePath("store"));
        model.setStateFile(dir.filePath("new-canvas.json"));
        QGuiApplication::clipboard()->clear();
        model.reset();
    }
    canvas::CanvasStore store() const { return canvas::CanvasStore(dir.filePath("store")); }
    // The first (only) canvas of the store as a document.
    mask::PaintDocument made(int index = 0) const {
        const std::vector<canvas::CanvasInfo> all = store().list();
        return store().load(all.at(size_t(index)).id, all.at(size_t(index)).head).value();
    }
    static mask::Rgba pixel(const mask::PaintDocument &document, int layer, int x, int y) {
        return document.pixels(document.layer(layer).id)->rgbaAt(x, y);
    }
};

void putOnClipboard(const QImage &image) {
    QGuiApplication::clipboard()->setImage(image);
    QTRY_VERIFY(QGuiApplication::clipboard()->mimeData()->hasImage());
}

} // namespace

class TestNewCanvasModel : public QObject {
    Q_OBJECT

private slots:
    void opensOnABlankSquareNamedUntitled1() {
        Fixture f;
        QCOMPARE(f.model.source(), QString("blank"));
        QCOMPARE(f.model.name(), QString("Untitled 1"));
        QCOMPARE(f.model.widthText(), QString("1024"));
        QCOMPARE(f.model.heightText(), QString("1024"));
        QCOMPARE(f.model.preset(), QString("square"));
        QCOMPARE(f.model.background(), QString("transparent"));
        QVERIFY(f.model.canCreate());
        QVERIFY(f.model.summary().contains("1024 × 1024 px · 1.05 megapixels"));
        QVERIFY(f.model.summary().contains("At most 16384 px a side and 67 megapixels."));
    }

    void presetsAreSquareThenTheShapesThenCustom() {
        Fixture f;
        QStringList ids;
        for (const QVariant &preset : f.model.presets())
            ids << preset.toMap().value("id").toString();
        QCOMPARE(ids, (QStringList{"square", "landscape", "wide", "portrait", "hd", "4k", "custom"}));
        const QVariantMap hd = f.model.presets().at(4).toMap();
        QCOMPARE(hd.value("label").toString(), QString("HD"));
        QCOMPARE(hd.value("width").toInt(), 1920);
        QCOMPARE(hd.value("height").toInt(), 1080);
        f.model.choosePreset("4k");
        QCOMPARE(f.model.widthText(), QString("3840"));
        QCOMPARE(f.model.heightText(), QString("2160"));
        QCOMPARE(f.model.preset(), QString("4k"));
        f.model.choosePreset("portrait");
        QCOMPARE(f.model.width(), 768);
        QCOMPARE(f.model.height(), 1024);
    }

    void aTypedSizeIsCustomUnlessItIsAPreset() {
        Fixture f;
        f.model.setWidthText("1000");
        QCOMPARE(f.model.preset(), QString("custom"));
        f.model.setWidthText("1920");
        f.model.setHeightText("1080");
        QCOMPARE(f.model.preset(), QString("hd"));
        f.model.choosePreset("custom"); // Custom keeps the numbers
        QCOMPARE(f.model.widthText(), QString("1920"));
        QCOMPARE(f.model.preset(), QString("hd"));
    }

    void aBlankCanvasIsMadeAtTheSizeWithTheNameAndOneEmptyLayer() {
        Fixture f;
        QSignalSpy created(&f.model, &NewCanvasModel::created);
        f.model.setName("  Poster ");
        f.model.setWidthText("640");
        f.model.setHeightText("480");
        QCOMPARE(f.model.create(), QString());
        QCOMPARE(created.size(), 1);
        QCOMPARE(created.at(0).at(0).toString(), f.model.createdId());
        const std::vector<canvas::CanvasInfo> all = f.store().list();
        QCOMPARE(int(all.size()), 1);
        QCOMPARE(all[0].name, QString("Poster"));
        const mask::PaintDocument doc = f.made();
        QCOMPARE(doc.width(), 640);
        QCOMPARE(doc.height(), 480);
        QCOMPARE(doc.layerCount(), 1);
        QCOMPARE(Fixture::pixel(doc, 0, 3, 3).a, uint8_t(0));
    }

    void backgroundsFillTheFirstLayer() {
        Fixture f;
        f.model.setWidthText("70");
        f.model.setHeightText("70");
        f.model.setBackground("white");
        QCOMPARE(f.model.create(), QString());
        f.model.setBackground("black");
        QCOMPARE(f.model.create(), QString());
        f.model.setBackground("colour");
        f.model.setColour("#336699");
        QCOMPARE(f.model.create(), QString());
        QCOMPARE(int(f.store().list().size()), 3);
        int white = 0, black = 0, colour = 0;
        for (int i = 0; i < 3; ++i) {
            const mask::PaintDocument doc = f.made(i);
            QCOMPARE(doc.layer(0).name, QString("Background"));
            const mask::Rgba at = Fixture::pixel(doc, 0, 69, 69);
            white += at == mask::Rgba{255, 255, 255, 255};
            black += at == mask::Rgba{0, 0, 0, 255};
            colour += at == mask::Rgba{0x33, 0x66, 0x99, 255};
        }
        QCOMPARE(white, 1);
        QCOMPARE(black, 1);
        QCOMPARE(colour, 1);
    }

    void aBadColourOrSizeIsRefusedInPlainWordsAndNothingIsMade() {
        Fixture f;
        f.model.setBackground("colour");
        f.model.setColour("blue-ish");
        QVERIFY(!f.model.canCreate());
        QVERIFY(f.model.problem().contains("hex code such as #336699"));
        f.model.setColour("#abc");
        QVERIFY(f.model.canCreate());
        f.model.setWidthText("20000");
        QVERIFY(!f.model.canCreate());
        QVERIFY2(f.model.problem().contains("at most 16384 px a side"), qPrintable(f.model.problem()));
        QCOMPARE(f.model.create(), f.model.problem());
        QCOMPARE(f.model.error(), f.model.problem());
        f.model.setWidthText("9000");
        f.model.setHeightText("9000"); // each side fits, the megapixels do not
        QVERIFY(f.model.problem().contains("67 megapixels"));
        f.model.setWidthText("");
        QVERIFY(f.model.problem().contains("whole numbers"));
        f.model.setWidthText("12x");
        QVERIFY(!f.model.canCreate());
        QVERIFY(f.store().list().empty());
    }

    void aNameIsNeverOneTheRootAlreadyHas() {
        Fixture f;
        QCOMPARE(f.model.create(), QString());
        f.model.reset();
        QCOMPARE(f.model.name(), QString("Untitled 2"));
        f.model.setName("Untitled 5");
        QCOMPARE(f.model.create(), QString());
        f.model.reset();
        QCOMPARE(f.model.name(), QString("Untitled 2")); // the first free number, not the next after the largest
        f.model.setName("   "); // a blank name is the next free one
        QCOMPARE(f.model.create(), QString());
        QStringList names;
        for (const canvas::CanvasInfo &info : f.store().list())
            names << info.name;
        names.sort();
        QCOMPARE(names, (QStringList{"Untitled 1", "Untitled 2", "Untitled 5"}));
    }

    void theLastSettingsComeBackAndOnlyAfterACanvasWasMade() {
        Fixture f;
        f.model.choosePreset("hd");
        f.model.setBackground("white");
        { // not made: nothing is remembered
            NewCanvasModel other;
            other.setStoreRoot(f.dir.filePath("store"));
            other.setStateFile(f.dir.filePath("new-canvas.json"));
            other.reset();
            QCOMPARE(other.preset(), QString("square"));
        }
        QCOMPARE(f.model.create(), QString());
        NewCanvasModel again;
        again.setStoreRoot(f.dir.filePath("store"));
        again.setStateFile(f.dir.filePath("new-canvas.json"));
        again.reset();
        QCOMPARE(again.preset(), QString("hd"));
        QCOMPARE(again.background(), QString("white"));
        // A custom size is remembered as numbers; a colour with its value.
        f.model.setWidthText("777");
        f.model.setHeightText("333");
        f.model.setBackground("colour");
        f.model.setColour("#102030");
        QCOMPARE(f.model.create(), QString());
        again.reset();
        QCOMPARE(again.widthText(), QString("777"));
        QCOMPARE(again.heightText(), QString("333"));
        QCOMPARE(again.preset(), QString("custom"));
        QCOMPARE(again.background(), QString("colour"));
        QCOMPARE(again.colour(), QString("#102030"));
    }

    void anUnreadableSettingsFileIsIgnored() {
        Fixture f;
        QFile file(f.dir.filePath("new-canvas.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not json");
        file.close();
        f.model.reset();
        QCOMPARE(f.model.preset(), QString("square"));
        QCOMPARE(f.model.background(), QString("transparent"));
    }

    void thePictureOnTheClipboardIsOfferedAndItsSizePreFilled() {
        Fixture f;
        QVERIFY(!f.model.clipboardAvailable());
        QCOMPARE(f.model.source(), QString("blank"));
        QImage image(300, 120, QImage::Format_ARGB32);
        image.fill(QColor(255, 0, 0, 255));
        image.setPixelColor(1, 0, QColor(0, 255, 0, 255));
        image.setPixelColor(2, 0, QColor(0, 0, 0, 0));
        putOnClipboard(image);
        f.model.reset();
        QVERIFY(f.model.clipboardAvailable());
        QCOMPARE(f.model.clipboardSize(), QString("300 × 120"));
        QCOMPARE(f.model.source(), QString("clipboard"));
        QCOMPARE(f.model.widthText(), QString("300"));
        QCOMPARE(f.model.heightText(), QString("120"));
        QCOMPARE(f.model.preset(), QString()); // the presets are for a blank canvas
        QVERIFY(f.model.summary().contains("300 × 120 px · 0.036 megapixels"));
        QCOMPARE(f.model.create(), QString());
        const mask::PaintDocument doc = f.made();
        QCOMPARE(doc.width(), 300);
        QCOMPARE(doc.height(), 120);
        QCOMPARE(doc.layerCount(), 1);
        QCOMPARE(Fixture::pixel(doc, 0, 0, 0), (mask::Rgba{255, 0, 0, 255}));
        QCOMPARE(Fixture::pixel(doc, 0, 1, 0), (mask::Rgba{0, 255, 0, 255}));
        QCOMPARE(Fixture::pixel(doc, 0, 2, 0).a, uint8_t(0));
        // Back to a blank canvas takes the typed size again, not the picture's.
        f.model.reset();
        f.model.setSource("blank");
        QCOMPARE(f.model.widthText(), QString("1024"));
    }

    void semiTransparentPixelsStayStraightNotPremultiplied() {
        Fixture f;
        QImage image(4, 4, QImage::Format_ARGB32);
        image.fill(QColor(255, 0, 0, 128));
        putOnClipboard(image);
        f.model.reset();
        QCOMPARE(f.model.create(), QString());
        const mask::Rgba at = Fixture::pixel(f.made(), 0, 1, 1);
        QVERIFY2(at.a >= 127 && at.a <= 129 && at.r >= 254 && at.g == 0, qPrintable(QString("%1 %2 %3 %4").arg(at.r).arg(at.g).arg(at.b).arg(at.a)));
    }

    void withNoPictureTheClipboardSourceSaysSoAndMakesNothing() {
        Fixture f;
        f.model.setSource("clipboard");
        QVERIFY(!f.model.canCreate());
        QCOMPARE(f.model.problem(), QString("The clipboard has no picture."));
        QCOMPARE(f.model.create(), QString("The clipboard has no picture."));
        QVERIFY(f.store().list().empty());
        // A picture that went from the clipboard while the dialog was open returns the source to blank.
        putOnClipboard(QImage(8, 8, QImage::Format_RGBA8888));
        f.model.reset();
        QCOMPARE(f.model.source(), QString("clipboard"));
        QGuiApplication::clipboard()->clear();
        f.model.refreshClipboard();
        QCOMPARE(f.model.source(), QString("blank"));
    }

    void aClipboardPictureOverTheCapIsRefusedWithItsSize() {
        Fixture f;
        putOnClipboard(QImage(17000, 2, QImage::Format_RGBA8888));
        f.model.reset();
        QVERIFY(!f.model.canCreate());
        QVERIFY2(f.model.problem().contains("at most 16384 px a side"), qPrintable(f.model.problem()));
        QVERIFY(f.store().list().empty());
    }

    void aPictureFileGivesItsSizeAndItsNameUntilTheNameIsTyped() {
        Fixture f;
        QImage image(300, 120, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        QVERIFY(image.save(f.dir.filePath("harbour.png")));
        QVERIFY(!f.model.canCreate() || f.model.source() == "blank");
        f.model.setFile(QUrl::fromLocalFile(f.dir.filePath("harbour.png")).toString());
        QCOMPARE(f.model.source(), QString("file"));
        QCOMPARE(f.model.fileLine(), QString("harbour.png · 300 × 120"));
        QCOMPARE(f.model.name(), QString("harbour"));
        QCOMPARE(f.model.widthText(), QString("300"));
        QCOMPARE(f.model.create(), QString());
        QCOMPARE(f.store().list().at(0).name, QString("harbour"));
        QCOMPARE(f.made().height(), 120);
        // A typed name wins over the file's.
        f.model.reset();
        f.model.setName("Mine");
        f.model.setFile(f.dir.filePath("harbour.png"));
        QCOMPARE(f.model.name(), QString("Mine"));
        // A file that is no picture is refused by the engine, in the dialog.
        QFile text(f.dir.filePath("notes.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("hello");
        text.close();
        f.model.setFile(f.dir.filePath("notes.txt"));
        QVERIFY(f.model.create().contains("not a file a canvas can be made from"));
        QCOMPARE(int(f.store().list().size()), 1);
    }

    void pasteAsNewMakesTheClipboardsPictureAtOnceNamedPasted1() {
        Fixture f;
        QSignalSpy created(&f.model, &NewCanvasModel::created);
        QVERIFY(!f.model.pasteAsNew().isEmpty()); // nothing on the clipboard
        QCOMPARE(created.size(), 0);
        QImage image(50, 40, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        putOnClipboard(image);
        QCOMPARE(f.model.pasteAsNew(), QString());
        QCOMPARE(created.size(), 1);
        QCOMPARE(f.store().list().at(0).name, QString("Pasted 1"));
        QCOMPARE(f.made().width(), 50);
        QCOMPARE(f.model.pasteAsNew(), QString());
        QCOMPARE(f.model.createdId() == created.at(1).at(0).toString(), true);
        QStringList names;
        for (const canvas::CanvasInfo &info : f.store().list())
            names << info.name;
        names.sort();
        QCOMPARE(names, (QStringList{"Pasted 1", "Pasted 2"}));
    }
};

QTEST_MAIN(TestNewCanvasModel)
#include "test_new_canvas_model.moc"
