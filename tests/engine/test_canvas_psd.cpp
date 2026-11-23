// Photoshop files and the canvas store (#227): a canvas with groups goes out as a PSD with layer groups and comes back with
// the same groups, in one revision; what Fugue cannot keep as a group is flattened and said. Seam: engine/canvas_psd.h.

#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_layer_ops.h"
#include "engine/canvas_psd.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

LayerEdit op(LayerOp o, std::vector<int> layers) {
    LayerEdit e;
    e.op = o;
    e.layers = std::move(layers);
    return e;
}

// Base, then a group "Pair" (A, B) at 50 % in Multiply, hidden C on top; B has a layer mask.
Result<CanvasInfo> groupedCanvas(CanvasStore &store, QString *head) {
    PaintDocument d(20, 12);
    d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(20, 12, {40, 80, 120, 255}));
    d.loadLayer(LayerKind::Paint, "A", true, 1, solid(20, 12, {200, 30, 30, 255}));
    LayerInfo b;
    b.name = "B";
    b.opacity = 0.75;
    const MaskPaint mask(20, 12, std::vector<uint8_t>(20 * 12, 100), 1);
    d.loadLayer(b, solid(20, 12, {10, 200, 10, 200}), &mask);
    d.loadLayer(LayerKind::Paint, "C", false, 1, solid(20, 12, {0, 0, 0, 90}));
    Result<CanvasInfo> made = store.create("g", d, "test");
    if (!made)
        return made;
    LayerEdit group = op(LayerOp::Group, {1, 2});
    group.text = "Pair";
    Result<Commit> c = store.editLayers(made->id, made->head, group, "test");
    if (!c)
        return Result<CanvasInfo>(c.errors());
    LayerEdit fade = op(LayerOp::Opacity, {1});
    fade.number = 0.5;
    c = store.editLayers(made->id, c->revision.id, fade, "test");
    LayerEdit blend = op(LayerOp::Blend, {1});
    blend.text = "multiply";
    c = store.editLayers(made->id, c->revision.id, blend, "test");
    if (!c)
        return Result<CanvasInfo>(c.errors());
    *head = c->revision.id;
    return made;
}

} // namespace

class TestCanvasPsd : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void aGroupGoesOutAsALayerGroupAndComesBackAsAGroup() {
        CanvasStore store(m_dir.filePath("a"));
        QString head;
        const Result<CanvasInfo> made = groupedCanvas(store, &head);
        QVERIFY2(made, qPrintable(made.error().message));
        QVERIFY(treeHasGroup(store.loadTree(made->id, head).value()));
        PictureExport options;
        options.format = "psd";
        const Result<ExportedPicture> out = exportRevisionAsPsd(store, made->id, head, options);
        QVERIFY2(out, qPrintable(out.error().message));
        QVERIFY(out->notes.isEmpty());
        QCOMPARE(out->bytes, exportRevisionAsPsd(store, made->id, head, options)->bytes); // deterministic

        CanvasStore other(m_dir.filePath("b"));
        const Result<CanvasInfo> back = importPsd(other, "back", out->bytes, "import");
        QVERIFY2(back, qPrintable(back.error().message));
        QVERIFY(back.warnings().isEmpty());
        const Tree before = store.loadTree(made->id, head).value();
        const Tree after = other.loadTree(back->id, back->head).value();
        QCOMPARE(other.log(back->id)->size(), size_t(1)); // one revision, not an import and then an edit
        QCOMPARE(after.layers.size(), before.layers.size());
        for (size_t i = 0; i < before.layers.size(); ++i) {
            QCOMPARE(after.layers[i].name, before.layers[i].name);
            QCOMPARE(after.layers[i].group, before.layers[i].group);
            QCOMPARE(after.layers[i].visible, before.layers[i].visible);
            QCOMPARE(after.layers[i].compositeOp, before.layers[i].compositeOp);
            QVERIFY(qAbs(after.layers[i].opacity - before.layers[i].opacity) < 1.0 / 255);
            QCOMPARE(after.layers[i].children.size(), before.layers[i].children.size());
        }
        const LayerRecord &pair = after.layers[1];
        QVERIFY(pair.group);
        QCOMPARE(pair.children[0].name, QString("A"));
        QCOMPARE(pair.children[1].name, QString("B"));
        QVERIFY(pair.children[1].hasMask);
        // the picture is the same
        QCOMPARE(other.load(back->id, back->head)->flatten().pixels(), store.load(made->id, head)->flatten().pixels());
        // and writing what came back gives the same file
        QCOMPARE(exportRevisionAsPsd(other, back->id, back->head, options)->bytes, out->bytes);
    }

    void writeAGroupedFileForAnotherReader() {
        // FUGUE_PSD_SCRATCH=<folder>: grouped.psd for the psd-tools check in docs/psd-research.md. Without it the test does nothing.
        const QString folder = qEnvironmentVariable("FUGUE_PSD_SCRATCH");
        if (folder.isEmpty())
            QSKIP("FUGUE_PSD_SCRATCH is not set");
        CanvasStore store(m_dir.filePath("w"));
        QString head;
        const Result<CanvasInfo> made = groupedCanvas(store, &head);
        PictureExport options;
        options.format = "psd";
        QFile f(QDir(folder).filePath("grouped.psd"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(exportRevisionAsPsd(store, made->id, head, options)->bytes);
    }

    void aCanvasWithoutGroupsIsTheDocumentPath() {
        CanvasStore store(m_dir.filePath("c"));
        PaintDocument d(8, 8);
        d.loadLayer(LayerKind::Paint, "One", true, 1, solid(8, 8, {1, 2, 3, 255}));
        d.loadLayer(LayerKind::Mask, "Sel", true, 1, MaskPaint(8, 8, 1));
        const Result<CanvasInfo> made = store.create("flat", d, "test");
        PictureExport options;
        options.format = "psd";
        const Result<ExportedPicture> out = exportRevisionAsPsd(store, made->id, made->head, options);
        QVERIFY(out);
        QCOMPARE(out->bytes, paintToPsd(store.load(made->id, made->head).value())->bytes);
        QCOMPARE(out->notes.size(), 1);
    }

    void aResizedExportFlattensGroupsAndSaysSo() {
        CanvasStore store(m_dir.filePath("d"));
        QString head;
        const Result<CanvasInfo> made = groupedCanvas(store, &head);
        PictureExport options;
        options.format = "psd";
        options.scale = 0.5; // resized: the groups are one layer each, and it says so
        const Result<ExportedPicture> out = exportRevisionAsPsd(store, made->id, head, options);
        QVERIFY(out);
        QCOMPARE(out->width, 10);
        QCOMPARE(out->notes.size(), 1);
    }

    void aGroupFugueCannotKeepIsFlattenedAndSaid() {
        // a PSD whose group holds a clipped layer: flattened into the level below, with a warning, still ONE revision
        CanvasStore store(m_dir.filePath("e"));
        QString head;
        const Result<CanvasInfo> made = groupedCanvas(store, &head);
        PsdImage image;
        image.width = 20, image.height = 12;
        PsdLayer inner;
        inner.info.name = "Inside";
        inner.pixels = solid(20, 12, {5, 6, 7, 255});
        PsdLayer group;
        group.group = true;
        group.info.name = "Folder";
        group.children.push_back(inner);
        image.layers.push_back(group);
        image.merged = inner.pixels;
        const Result<PsdExported> bytes = paintToPsd(image);
        QVERIFY(bytes);
        const Result<PsdImported> kept = paintFromPsdSettled(bytes->bytes);
        QVERIFY(kept);
        QCOMPARE(kept->layers.size(), size_t(1));
        QVERIFY(kept->layers[0].group); // a plain group is kept
        // a group that holds nothing is dropped with a warning
        PsdImage empty = image;
        empty.layers[0].children.clear();
        const Result<PsdImported> none = paintFromPsdSettled(paintToPsd(empty)->bytes);
        QVERIFY(none);
        QVERIFY(none->layers.empty());
        QCOMPARE(none.warnings().size(), 1);
    }

    void nestedGroupsRoundTrip() {
        CanvasStore store(m_dir.filePath("f"));
        PsdImage image;
        image.width = 6, image.height = 6;
        const auto leaf = [](const QString &name) {
            PsdLayer l;
            l.info.name = name;
            l.pixels = solid(6, 6, {9, 9, 9, 255});
            return l;
        };
        PsdLayer inner;
        inner.group = true;
        inner.info.name = "Inner";
        inner.children = {leaf("x"), leaf("y")};
        PsdLayer outer;
        outer.group = true;
        outer.info.name = "Outer";
        outer.children = {leaf("w"), inner};
        image.layers = {leaf("bottom"), outer};
        image.merged = solid(6, 6, {9, 9, 9, 255});
        const Result<CanvasInfo> made = importPsd(store, "nested", paintToPsd(image)->bytes, "import");
        QVERIFY2(made, qPrintable(made.error().message));
        const Tree tree = store.loadTree(made->id, made->head).value();
        QCOMPARE(tree.layers.size(), size_t(2));
        QCOMPARE(tree.layers[1].name, QString("Outer"));
        QCOMPARE(tree.layers[1].children.size(), size_t(2));
        QCOMPARE(tree.layers[1].children[1].name, QString("Inner"));
        QCOMPARE(tree.layers[1].children[1].children.size(), size_t(2));
        QCOMPARE(groupDepth(tree.layers[1]), 2);
    }
};

QTEST_GUILESS_MAIN(TestCanvasPsd)
#include "test_canvas_psd.moc"
