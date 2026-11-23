// A picture placed on a canvas as a new layer (#236, epic #206, spec 14.5): CanvasStore::pasteLayer keeps the picture whole as the
// layer's original (engine/canvas_layer_source.h) and shows it centered, cut at the canvas's edge; ONE revision, a name "Pasted N",
// the original stored in the tree (versioned, hash-stable for a tree without one), shared by a package and a duplicate, and read
// as untrusted (sizes, keys, numbers).
//
// Seam: engine/canvas_transform.h (pure) and CanvasStore with a folder and a clock of the test's own.

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_diff.h"
#include "engine/canvas_package.h"
#include "engine/canvas_store.h"
#include "engine/canvas_transform.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// A picture whose pixel (x, y) is (x, y, 7, 255), so a placement is checked pixel by pixel.
MaskPaint ramp(int w, int h) {
    std::vector<uint8_t> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (uint8_t v : {uint8_t(x % 256), uint8_t(y % 256), uint8_t(7), uint8_t(255)})
                px.push_back(v);
    return MaskPaint(w, h, px, 4);
}

Rgba pixelAt(const MaskPaint &m, int x, int y) {
    const size_t at = (size_t(y) * m.width() + x) * 4;
    return {m.pixels()[at], m.pixels()[at + 1], m.pixels()[at + 2], m.pixels()[at + 3]};
}

} // namespace

class TestCanvasPaste : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_tick = 0;
    Clock clock() { return [this] { return QDateTime(QDate(2026, 11, 23), QTime(9, 0, 0), QTimeZone::utc()).addSecs(m_tick++); }; }
    CanvasStore store() { return CanvasStore(m_dir.path(), clock()); }
    CanvasInfo made(CanvasStore &s, int w = 100, int h = 80) {
        PaintDocument d(w, h);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(w, h, {10, 20, 30, 255}));
        return s.create("c", d, "test").value();
    }

private slots:
    void placementCentersOnWholePixels() {
        const LayerTransform even = centeredPlacement(10, 6, 50, 40);
        QCOMPARE(even.cx, 50.0);
        QCOMPARE(even.cy, 40.0);
        const LayerTransform odd = centeredPlacement(11, 7, 50, 40); // the corner is whole: 50 - 5.5 rounds to 45 (left), 40 - 3.5 to 37 (top)
        QCOMPARE(odd.cx - 11 / 2.0, 45.0);
        QCOMPARE(odd.cy - 7 / 2.0, 37.0);
        QVERIFY(odd.isPlacement(11, 7));
    }

    void renderCutsAtTheCanvasEdgeAndKeepsPixels() {
        const MaskPaint picture = ramp(20, 10);
        // Centered on (5, 5): the picture's left 5 columns and top 0 rows... its corner lands at (-5, 0).
        const LayerTransform t = centeredPlacement(20, 10, 5, 5);
        const MaskPaint shown = renderPlaced(picture, t, 30, 20).value();
        QCOMPARE(shown.width(), 30);
        QCOMPARE(pixelAt(shown, 0, 0).r, uint8_t(5)); // picture column 5 is at canvas column 0
        QCOMPARE(pixelAt(shown, 14, 9).g, uint8_t(9));
        QCOMPARE(pixelAt(shown, 15, 0).a, uint8_t(0)); // right of the picture
        QCOMPARE(pixelAt(shown, 0, 10).a, uint8_t(0)); // below it
        // Wholly off the canvas: nothing shown, no error.
        const MaskPaint away = renderPlaced(picture, centeredPlacement(20, 10, 500, 500), 30, 20).value();
        QVERIFY(std::all_of(away.pixels().begin(), away.pixels().end(), [](uint8_t v) { return v == 0; }));
        // A scaling placement is not drawn here (the transform does that).
        LayerTransform scaled = t;
        scaled.width = 40;
        QVERIFY(!renderPlaced(picture, scaled, 30, 20));
    }

    void pastingIsOneRevisionOfANamedLayerAboveTheOthers() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Commit first = s.pasteLayer(c.id, c.head, ramp(30, 20), 50, 40, -1, {}, "canvas-view:paste").value();
        QVERIFY(first.move.moved);
        QCOMPARE(s.info(c.id)->head, first.revision.id);
        QCOMPARE(first.revision.parent, c.head);
        QCOMPARE(first.revision.producer, QStringLiteral("canvas-view:paste"));
        const Tree t = s.loadTree(c.id, first.revision.id).value();
        QCOMPARE(t.layers.size(), size_t(2));
        QCOMPARE(t.layers[1].name, QStringLiteral("Pasted 1"));
        QVERIFY(t.layers[1].source.has_value());
        QCOMPARE(t.layers[1].source->width, 30);
        // The shown pixels are the picture, centered: its corner at (35, 30).
        const PaintDocument doc = s.load(c.id, first.revision.id).value();
        const MaskPaint *pixels = doc.pixels(doc.layer(1).id);
        QCOMPARE(pixelAt(*pixels, 35, 30).r, uint8_t(0));
        QCOMPARE(pixelAt(*pixels, 64, 49).g, uint8_t(19));
        QCOMPARE(pixelAt(*pixels, 34, 30).a, uint8_t(0));
        // The second one is Pasted 2, and goes right above layer 0 when asked.
        const Commit second = s.pasteLayer(c.id, first.revision.id, solid(4, 4, {1, 2, 3, 255}), 10, 10, 0, {}, "canvas-view:paste").value();
        const Tree t2 = s.loadTree(c.id, second.revision.id).value();
        QCOMPARE(t2.layers[1].name, QStringLiteral("Pasted 2"));
        QCOMPARE(t2.layers[2].name, QStringLiteral("Pasted 1"));
        // Undo takes it back to the revision before.
        QVERIFY(s.undo(c.id, second.revision.id).value().moved);
        QCOMPARE(s.info(c.id)->head, first.revision.id);
    }

    void aPictureBiggerThanTheCanvasKeepsItsOffCanvasPart() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 40, 30);
        const MaskPaint big = ramp(200, 150);
        const Commit pasted = s.pasteLayer(c.id, c.head, big, 20, 15, -1, "Photo", "canvas-view:paste").value();
        const Tree t = s.loadTree(c.id, pasted.revision.id).value();
        QCOMPARE(t.layers[1].name, QStringLiteral("Photo"));
        QCOMPARE(t.layers[1].source->width, 200);
        // The layer shows the middle 40 x 30 of the picture; the original is whole in the store.
        const PaintDocument doc = s.load(c.id, pasted.revision.id).value();
        QCOMPARE(pixelAt(*doc.pixels(doc.layer(1).id), 0, 0).r, uint8_t(80));
        const MaskPaint original = assembleLayer(200, 150, 4, t.layers[1].source->tiles, [&](const TileKey &key, const QString &hash) {
                                       const IRect r = tileRect(key, 200, 150);
                                       return s.tilePixels(hash, r.width, r.height, 4);
                                   }).value();
        QCOMPARE(pixelAt(original, 199, 149).r, uint8_t(199));
        QCOMPARE(pixelAt(original, 0, 149).g, uint8_t(149));
    }

    void tooBigOrWrongKindIsRefusedWithPlainWords() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        QCOMPARE(s.pasteLayer(c.id, c.head, MaskPaint(10, 10, 1), 0, 0, -1, {}, "p").error().code, QStringLiteral("input_invalid"));
        QVERIFY(!placeableProblem(16384, 16384).isEmpty());
        QVERIFY(placeableProblem(8192, 8192).isEmpty());
        QVERIFY(placeableProblem(0, 5).contains("empty"));
        QCOMPARE(s.pasteLayer(c.id, c.head, solid(4, 4, {}), 0, 0, 7, {}, "p").error().code, QStringLiteral("input_invalid")); // no layer 7
        QCOMPARE(s.info(c.id)->head, c.head);
    }

    void aStaleHeadWritesNothing() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Commit one = s.pasteLayer(c.id, c.head, solid(4, 4, {9, 9, 9, 255}), 10, 10, -1, {}, "p").value();
        const Commit late = s.pasteLayer(c.id, c.head, solid(4, 4, {9, 9, 9, 255}), 10, 10, -1, {}, "p").value();
        QVERIFY(!late.move.moved);
        QCOMPARE(late.move.head, one.revision.id);
    }

    void aTreeWithoutAnOriginalKeepsItsHash() {
        // The source is written only when a layer has one: a tree of before has the same JSON, so the same hash.
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Tree plain = s.loadTree(c.id, c.head).value();
        QVERIFY(!QJsonDocument(treeToJson(plain)).toJson().contains("source"));
    }

    void theOriginalRoundTripsThroughJsonAndIsChecked() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Commit pasted = s.pasteLayer(c.id, c.head, ramp(30, 20), 50, 40, -1, {}, "p").value();
        const Tree t = s.loadTree(c.id, pasted.revision.id).value();
        const QJsonObject json = treeToJson(t);
        QCOMPARE(treeToJson(treeFromJson(json).value()), json);
        const auto withSource = [&](const std::function<void(QJsonObject &)> &change) {
            QJsonObject tree = json;
            QJsonArray layers = tree.value("layers").toArray();
            QJsonObject layer = layers.at(1).toObject();
            QJsonObject source = layer.value("source").toObject();
            change(source);
            layer.insert("source", source);
            layers.replace(1, layer);
            tree.insert("layers", layers);
            return treeFromJson(tree);
        };
        QVERIFY(withSource([](QJsonObject &) {}));
        QVERIFY(!withSource([](QJsonObject &o) { o.insert("width", 0); }));
        QVERIFY(!withSource([](QJsonObject &o) { o.insert("width", 99999); }));
        QVERIFY(!withSource([](QJsonObject &o) { o.insert("tiles", QJsonObject{{"9,9", "sha256:" + QString(64, 'a')}}); }));
        QVERIFY(!withSource([](QJsonObject &o) { o.insert("rendered", "nope"); }));
        QVERIFY(!withSource([](QJsonObject &o) {
            QJsonObject t = o.value("transform").toObject();
            t.insert("filter", "sinc");
            o.insert("transform", t);
        }));
        QVERIFY(!withSource([](QJsonObject &o) {
            QJsonObject t = o.value("transform").toObject();
            t.insert("width", 1e9);
            o.insert("transform", t);
        }));
    }

    void theOriginalTravelsWithADuplicateAndAPackage() {
        CanvasStore s = store();
        const CanvasInfo c = made(s);
        const Commit pasted = s.pasteLayer(c.id, c.head, ramp(300, 20), 50, 40, -1, {}, "p").value(); // two tiles across
        const CanvasInfo copy = s.duplicate(c.id, "copy").value();
        const Tree t = s.loadTree(copy.id, s.info(copy.id)->head).value();
        QVERIFY(t.layers[1].source.has_value());
        QCOMPARE(t.layers[1].source->tiles.size(), size_t(2));
        // A package carries the original too: the other store has the same tree and can read the original's tiles.
        QVERIFY(!hasErrors(writeCanvasPackage(s, c.id, SharedPart::Head, m_dir.filePath("p.zip"))));
        CanvasStore elsewhere(m_dir.filePath("elsewhere"), clock());
        const CanvasImported got = importCanvasPackage(m_dir.filePath("p.zip"), elsewhere).value();
        const Tree theirs = elsewhere.loadTree(got.info.id, got.info.head).value();
        QCOMPARE(theirs.layers[1].source->tiles, t.layers[1].source->tiles);
        const TileKey key = theirs.layers[1].source->tiles.begin()->first;
        const IRect r = tileRect(key, 300, 20);
        QVERIFY(elsewhere.tilePixels(theirs.layers[1].source->tiles.begin()->second, r.width, r.height, 4).ok());
        // The diff says the original moved when only it changes.
        Tree moved = s.loadTree(c.id, pasted.revision.id).value();
        Tree other = moved;
        other.layers[1].source->transform.cx += 5;
        const TreeDiff diff = diffTrees(moved, other);
        QCOMPARE(diff.layers.size(), size_t(1));
        QVERIFY(diff.layers[0].properties.contains("placement"));
    }

    void anExportShowsTheShownPixelsNotTheOriginal() {
        CanvasStore s = store();
        const CanvasInfo c = made(s, 40, 30);
        const Commit pasted = s.pasteLayer(c.id, c.head, solid(100, 100, {200, 0, 0, 255}), 20, 15, -1, {}, "p").value();
        const PaintDocument doc = s.load(c.id, pasted.revision.id).value();
        const MaskPaint flat = doc.flatten();
        QCOMPARE(flat.width(), 40); // the canvas, not the picture
        QCOMPARE(pixelAt(flat, 39, 29).r, uint8_t(200));
    }
};

QTEST_GUILESS_MAIN(TestCanvasPaste)
#include "test_canvas_paste.moc"
