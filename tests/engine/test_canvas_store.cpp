// The canvas revision store (#208, epic #206, spec 14.5): tiles addressed by raw pixels, a tree hash
// that is pixels and layer properties only, a head moved by compare-and-swap, and an .ora round trip.
// Seam: engine/canvas_store.h with a folder and a clock of the test's own.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>

#include "engine/canvas_diff.h"
#include "engine/canvas_store.h"
#include "engine/hash.h"
#include "engine/paint_file.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

// A 1024 px document whose base layer has a different color in each of its 16 tiles, so no two tiles
// share a blob, and a second layer of one small stroke.
PaintDocument sample(int size = 1024) {
    PaintDocument doc(size, size);
    doc.addLayer(LayerKind::Paint, "Base");
    for (int ty = 0; ty < size / 256; ++ty) {
        for (int tx = 0; tx < size / 256; ++tx) {
            doc.brush.color = Rgba{uint8_t(10 + tx * 40), uint8_t(10 + ty * 40), uint8_t(200 - tx * 10), 255};
            doc.fillRect(tx * 256, ty * 256, 256, 256);
        }
    }
    doc.addLayer(LayerKind::Paint, "Top");
    doc.brush.color = Rgba{255, 255, 255, 255};
    doc.fillRect(10, 10, 20, 20);
    return doc;
}

int blobCount(const QString &root) {
    int count = 0;
    QDirIterator it(root + "/blobs", {"*.png"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QByteArray oraOf(const PaintDocument &doc) { return paintToOra(doc).value(); }

} // namespace

class TestCanvasStore : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_tick = 0;
    Clock clock() { return [this] { return QDateTime(QDate(2026, 11, 22), QTime(12, 0, 0), QTimeZone::utc()).addSecs(m_tick++); }; }
    CanvasStore store() { return CanvasStore(m_dir.path(), clock()); }

    // A canvas with a head and a document that adds one layer on top of it.
    struct Fixture {
        QString id;
        QString head;
        PaintDocument base;
    };
    Fixture canvasWithBase(CanvasStore &s) {
        Fixture f{{}, {}, sample(512)};
        const Result<CanvasInfo> made = s.create("fill", f.base, "test");
        f.id = made->id;
        f.head = made->head;
        return f;
    }
    static PaintDocument withFill(PaintDocument doc, const QString &name, int x) {
        doc.addLayer(LayerKind::Paint, name);
        doc.brush.color = Rgba{200, 10, 10, 255};
        doc.fillRect(x, 100, 60, 60);
        return doc;
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(m_dir.path()));
        m_tick = 0;
    }
    void cleanup() { QDir(m_dir.path()).removeRecursively(); }

    void samePixelsGiveTheSameTreeHashHoweverTheyWereMade() {
        // One document painted bottom layer first, another in a different order, with other ids.
        PaintDocument a = sample();
        PaintDocument b(1024, 1024);
        b.addLayer(LayerKind::Paint, "Scratch");
        b.removeLayer(b.layer(0).id);
        b.addLayer(LayerKind::Paint, "Base");
        for (int ty = 3; ty >= 0; --ty)
            for (int tx = 3; tx >= 0; --tx) {
                b.brush.color = Rgba{uint8_t(10 + tx * 40), uint8_t(10 + ty * 40), uint8_t(200 - tx * 10), 255};
                b.fillRect(tx * 256, ty * 256, 256, 256);
            }
        b.addLayer(LayerKind::Paint, "Top");
        b.brush.color = Rgba{255, 255, 255, 255};
        b.fillRect(10, 10, 20, 20);
        const Result<QString> ha = CanvasStore::treeHashOf(a), hb = CanvasStore::treeHashOf(b);
        QVERIFY(ha && hb);
        QCOMPARE(ha.value(), hb.value());
        QVERIFY(isSha256(ha.value()));
        // A layer property is part of the tree, a history step or the clock is not.
        b.setOpacity(b.layer(1).id, 0.5);
        QVERIFY(CanvasStore::treeHashOf(b).value() != ha.value());
        b.setOpacity(b.layer(1).id, 1);
        b.setName(b.layer(1).id, "Renamed");
        QVERIFY(CanvasStore::treeHashOf(b).value() != ha.value());
    }

    void twoCommitsOfOneTreeAreTwoRevisionsWithOneTreeHash() {
        CanvasStore s = store();
        const PaintDocument doc = sample(512);
        const Result<CanvasInfo> made = s.create("one", doc, "test");
        QVERIFY(made);
        const Result<Commit> again = s.commit(made->id, doc, made->head, "test");
        QVERIFY(again);
        QVERIFY(again->move.moved);
        QVERIFY(again->revision.id != made->head); // another time, another record
        QCOMPARE(again->revision.tree, s.revision(made->id, made->head)->tree);
        QCOMPARE(again->revision.parent, made->head);
        const int blobs = blobCount(m_dir.path());
        QVERIFY(s.commit(made->id, doc, again->revision.id, "test"));
        QCOMPARE(blobCount(m_dir.path()), blobs); // the same pixels store nothing new
    }

    void aFiveHundredTwelvePixelEditAddsOnlyTheTouchedTiles() {
        CanvasStore s = store();
        PaintDocument doc = sample();
        const Result<CanvasInfo> made = s.create("edit", doc, "test");
        QVERIFY(made);
        const int before = blobCount(m_dir.path());
        QCOMPARE(before, 16 + 1 + 0); // 16 distinct base tiles, the top layer's one tile; the empty ones are not stored
        doc.setActiveLayer(doc.layer(0).id);
        doc.brush.color = Rgba{1, 2, 3, 255};
        doc.fillRect(256, 256, 512, 512); // exactly four tiles
        const Result<Commit> next = s.commit(made->id, doc, made->head, "edit");
        QVERIFY(next);
        QCOMPARE(blobCount(m_dir.path()), before + 1); // the four tiles are one color: one new blob
        const Tree a = s.tree(s.revision(made->id, made->head)->tree).value();
        const Tree b = s.tree(next->revision.tree).value();
        int changed = 0;
        for (const auto &[key, hash] : b.layers[0].tiles)
            changed += a.layers[0].tiles.at(key) != hash;
        QCOMPARE(changed, 4);
        QCOMPARE(b.layers[1].tiles, a.layers[1].tiles);
        // Painting different pixels in each of the four tiles adds four blobs.
        for (int i = 0; i < 4; ++i) {
            doc.brush.color = Rgba{uint8_t(50 + i), 9, 9, 255};
            doc.fillRect(256 + (i % 2) * 256, 256 + (i / 2) * 256, 256, 256);
        }
        QVERIFY(s.commit(made->id, doc, next->revision.id, "edit"));
        QCOMPARE(blobCount(m_dir.path()), before + 1 + 4);
    }

    void aStaleHeadMoveIsReportedAndLeavesTheHeadAlone() {
        CanvasStore s = store();
        PaintDocument doc = sample(256);
        const Result<CanvasInfo> made = s.create("cas", doc, "test");
        const QString first = made->head;
        doc.fillRect(0, 0, 30, 30);
        const Result<Commit> winner = s.commit(made->id, doc, first, "a");
        QVERIFY(winner && winner->move.moved);
        doc.fillRect(0, 0, 40, 40);
        const Result<Commit> loser = s.commit(made->id, doc, first, "b"); // saw the old head
        QVERIFY(loser);
        QVERIFY(!loser->move.moved);
        QCOMPARE(loser->move.head, winner->revision.id);
        QCOMPARE(s.info(made->id)->head, winner->revision.id);
        // The losing revision is kept, a candidate for rebase or keep-both, and can be loaded.
        QVERIFY(s.load(made->id, loser->revision.id));
        // The caller retries on top of the new head.
        QVERIFY(s.moveHead(made->id, winner->revision.id, loser->revision.id)->moved);
    }

    void oneOfManyRacingWritersWins() {
        CanvasStore s = store();
        const PaintDocument doc = sample(256);
        const Result<CanvasInfo> made = s.create("race", doc, "test");
        std::vector<QString> revisions;
        for (int i = 0; i < 8; ++i) {
            PaintDocument other = doc;
            other.brush.color = Rgba{uint8_t(i * 9), 1, 1, 255};
            other.fillRect(0, 0, 10 + i, 10);
            revisions.push_back(s.addRevision(made->id, other, made->head, "racer").value().id);
        }
        std::atomic<int> winners{0};
        std::vector<std::thread> threads;
        for (const QString &revision : revisions)
            threads.emplace_back([&, revision] {
                CanvasStore mine(m_dir.path());
                const Result<HeadMove> move = mine.moveHead(made->id, made->head, revision);
                if (move && move->moved)
                    ++winners;
            });
        for (std::thread &t : threads)
            t.join();
        QCOMPARE(winners.load(), 1);
        QVERIFY(std::find(revisions.begin(), revisions.end(), s.info(made->id)->head) != revisions.end());
    }

    void anUnknownRevisionCannotBecomeTheHead() {
        CanvasStore s = store();
        const Result<CanvasInfo> made = s.create("x", sample(256), "test");
        const Result<HeadMove> move = s.moveHead(made->id, made->head, "sha256:" + QString(64, 'c'));
        QVERIFY(!move);
        QCOMPARE(move.error().code, QString("canvas_not_found"));
    }

    void importAndExportRoundTripToTheSameBytes() {
        CanvasStore s = store();
        PaintDocument doc = sample();
        doc.addLayer(LayerKind::Mask, "Selection");
        doc.fillRect(300, 300, 100, 100);
        doc.addLayer(LayerKind::Picture, "Under");
        doc.setOpacity(doc.layer(1).id, 0.5);
        doc.setVisible(doc.layer(3).id, false);
        const QByteArray ora = oraOf(doc);
        const QString file = m_dir.filePath("in.ora");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(ora);
        f.close();
        const Result<PaintDocument> read = readPaintFile(file);
        QVERIFY(read);
        const Result<CanvasInfo> made = s.create("rt", read.value(), "import");
        QVERIFY(made);
        const Result<PaintDocument> back = s.load(made->id, made->head);
        QVERIFY(back);
        QCOMPARE(oraOf(back.value()), ora);
        QCOMPARE(CanvasStore::treeHashOf(back.value()).value(), CanvasStore::treeHashOf(doc).value());
        QCOMPARE(back->layerCount(), 4);
        QCOMPARE(back->layer(1).opacity, 0.5);
        QVERIFY(!back->layer(3).visible);
    }

    void aNonSquareDocumentWithEdgeTilesRoundTrips() {
        CanvasStore s = store();
        PaintDocument doc(300, 270);
        doc.addLayer(LayerKind::Paint, "P");
        doc.brush.color = Rgba{9, 8, 7, 200};
        doc.fillRect(250, 250, 50, 20);
        const Result<CanvasInfo> made = s.create("edge", doc, "test");
        const Result<PaintDocument> back = s.load(made->id, made->head);
        QVERIFY(back);
        QCOMPARE(back->flatten().pixels(), doc.flatten().pixels());
    }

    void logWalksTheParentsAndRefsResolve() {
        CanvasStore s = store();
        PaintDocument doc = sample(256);
        const Result<CanvasInfo> made = s.create("log", doc, "new");
        QString head = made->head;
        for (int i = 1; i <= 3; ++i) {
            doc.fillRect(0, 0, 10 * i, 10);
            head = s.commit(made->id, doc, head, QString("edit %1").arg(i))->revision.id;
        }
        const auto all = s.log(made->id).value();
        QCOMPARE(all.size(), size_t(4));
        QCOMPARE(all[0].id, head);
        QCOMPARE(all[0].producer, QString("edit 3"));
        QCOMPARE(all[3].id, made->head);
        QVERIFY(all[3].parent.isEmpty());
        QVERIFY(all[0].time > all[3].time);
        QCOMPARE(s.log(made->id, {}, 2).value().size(), size_t(2));
        QCOMPARE(s.log(made->id, all[2].id).value().size(), size_t(2));
        QCOMPARE(s.resolve(made->id, "head").value(), head);
        QCOMPARE(s.resolve(made->id, head.left(7 + 10)).value(), head);
        QCOMPARE(s.resolve(made->id, head.mid(7, 12)).value(), head);
        QVERIFY(!s.resolve(made->id, "abc"));
        QVERIFY(!s.resolve(made->id, "sha256:" + QString(64, 'd')));
        QVERIFY(!s.resolve("canvas-nope", "head"));
        QVERIFY(!s.resolve("../x", "head"));
    }

    void aDamagedStoreIsCanvasCorruptNotWrongPixels() {
        CanvasStore s = store();
        const Result<CanvasInfo> made = s.create("bad", sample(256), "test");
        QDirIterator it(m_dir.path() + "/blobs", {"*.png"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        QFile blob(it.next());
        QVERIFY(blob.open(QIODevice::WriteOnly | QIODevice::Truncate));
        blob.write("not a png");
        blob.close();
        const Result<PaintDocument> loaded = s.load(made->id, made->head);
        QVERIFY(!loaded);
        QCOMPARE(loaded.error().code, QString("canvas_corrupt"));
    }

    void aTamperedManifestIsRefused() {
        CanvasStore s = store();
        const Result<CanvasInfo> made = s.create("bad", sample(256), "test");
        const QString file = m_dir.path() + "/canvases/" + made->id + "/commits/" + made->head.mid(7) + ".json";
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QByteArray text = f.readAll();
        text.replace("\"test\"", "\"evil\"");
        f.seek(0);
        f.write(text);
        f.close();
        QCOMPARE(s.revision(made->id, made->head).error().code, QString("canvas_corrupt"));
    }

    // ---- candidates: accept, drop, diff (#210) ----

    void aRevisionNobodyPointsAtIsACandidateUntilResolved() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        QVERIFY(s.candidates(f.id)->empty()); // the head's own history is not a candidate
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        const Revision b = s.addRevision(f.id, withFill(f.base, "B", 300), f.head, "write").value();
        const auto listed = s.candidates(f.id).value();
        QCOMPARE(listed.size(), size_t(2));
        QCOMPARE(listed[0].revision.id, b.id); // newest first
        QVERIFY(listed[0].onHead && listed[1].onHead);
        QCOMPARE(s.drop(f.id, a.id).value().id, a.id);
        QCOMPARE(s.candidates(f.id)->size(), size_t(1));
        QCOMPARE(s.candidates(f.id, true)->size(), size_t(2));
        QVERIFY(s.candidates(f.id, true).value()[1].dropped);
        QVERIFY(s.drop(f.id, a.id)); // dropping twice is fine
        // Nothing is deleted: a dropped revision still loads.
        QVERIFY(s.load(f.id, a.id));
    }

    void acceptingACandidateMadeFromTheHeadMovesTheHead() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        const Result<Accepted> done = s.accept(f.id, a.id);
        QVERIFY(done);
        QVERIFY(done->moved);
        QVERIFY(!done->rebased);
        QCOMPARE(done->head.id, a.id);
        QCOMPARE(s.info(f.id)->head, a.id);
        QVERIFY(s.candidates(f.id)->empty());
        // It is history now: not a candidate again, so neither accepted nor dropped.
        QCOMPARE(s.accept(f.id, a.id).error().code, QString("canvas_not_candidate"));
        QCOMPARE(s.drop(f.id, a.id).error().code, QString("canvas_not_candidate"));
        QCOMPARE(s.drop(f.id, f.head).error().code, QString("canvas_not_candidate"));
    }

    void aCandidateMadeFromAnOlderRevisionIsPlacedOnTheHead() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        const Revision b = s.addRevision(f.id, withFill(f.base, "B", 300), f.head, "write").value();
        QVERIFY(s.accept(f.id, a.id)->moved); // the head is on a; b was made from the base
        QVERIFY(!s.candidates(f.id)->at(0).onHead);
        const Result<Accepted> done = s.accept(f.id, b.id);
        QVERIFY(done);
        QVERIFY(done->moved && done->rebased);
        QCOMPARE(s.info(f.id)->head, done->head.id);
        QCOMPARE(done->head.parent, a.id);
        const PaintDocument now = s.load(f.id, done->head.id).value();
        QCOMPARE(now.layerCount(), 4); // Base, Top, A, B
        QCOMPARE(now.layer(2).name, QString("A"));
        QCOMPARE(now.layer(3).name, QString("B"));
        QVERIFY(s.candidates(f.id)->empty()); // b is resolved though it is not in the history
        QCOMPARE(s.accept(f.id, b.id).error().code, QString("canvas_not_candidate"));
        // The placed layer has b's pixels.
        const PaintDocument original = s.load(f.id, b.id).value();
        QCOMPARE(now.pixels(now.layer(3).id)->pixels(), original.pixels(original.layer(2).id)->pixels());
    }

    void aCandidateThatChangedAnExistingLayerCannotBePlacedOnAMovedHead() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        PaintDocument edited = sample(512); // not a copy of f.base: a copy shares its pixels
        edited.setActiveLayer(edited.layer(0).id);
        edited.brush.color = Rgba{1, 2, 3, 255};
        edited.fillRect(0, 0, 40, 40);
        const Revision edit = s.addRevision(f.id, edited, f.head, "edit").value();
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        QVERIFY(s.accept(f.id, a.id)->moved);
        const Result<Accepted> refused = s.accept(f.id, edit.id);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, QString("canvas_conflict"));
        QCOMPARE(s.info(f.id)->head, a.id); // nothing changed
        QCOMPARE(s.candidates(f.id)->size(), size_t(1));
        QVERIFY(s.drop(f.id, edit.id));
    }

    void aDroppedCandidateCanStillBeAccepted() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        QVERIFY(s.drop(f.id, a.id));
        QVERIFY(s.accept(f.id, a.id)->moved);
        QCOMPARE(s.info(f.id)->head, a.id);
        QVERIFY(s.candidates(f.id, true)->empty());
    }

    void theStoreListsItsCanvasesNewestFirst() {
        CanvasStore s = store();
        QVERIFY(s.list().empty());
        const QString first = s.create("one", sample(256), "test")->id;
        const QString second = s.create("two", sample(256), "test")->id;
        const auto all = s.list();
        QCOMPARE(all.size(), size_t(2));
        QCOMPARE(all[0].id, second); // the clock ticks per call
        QCOMPARE(all[1].id, first);
        QCOMPARE(all[0].name, QString("two"));
        QVERIFY(!all[0].head.isEmpty());
    }

    void aCanvasWithoutThatRevisionIsCanvasNotFound() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        const QString none = "sha256:" + QString(64, 'e');
        QCOMPARE(s.accept(f.id, none).error().code, QString("canvas_not_found"));
        QCOMPARE(s.drop(f.id, none).error().code, QString("canvas_not_found"));
        QCOMPARE(s.candidates("canvas-nope").error().code, QString("canvas_not_found"));
    }

    void diffNamesTheLayersAndTilesThatDiffer() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        const Revision a = s.addRevision(f.id, withFill(f.base, "A", 100), f.head, "write").value();
        const TreeDiff diff = diffTrees(s.tree(s.revision(f.id, f.head)->tree).value(), s.tree(a.tree).value());
        QVERIFY(!diff.identical());
        QCOMPARE(diff.layers.size(), size_t(1));
        QCOMPARE(diff.layers[0].name, QString("A"));
        QCOMPARE(diff.layers[0].change, LayerChange::Added);
        QCOMPARE(diff.layers[0].tilesChanged, 1);
        QCOMPARE(diff.bounds(), (mask::IRect{0, 0, 256, 256}));
        // The same two ways round: removed. Identical trees: nothing.
        const TreeDiff back = diffTrees(s.tree(a.tree).value(), s.tree(s.revision(f.id, f.head)->tree).value());
        QCOMPARE(back.layers[0].change, LayerChange::Removed);
        QVERIFY(diffTrees(s.tree(a.tree).value(), s.tree(a.tree).value()).identical());
    }

    void diffSeesAnEditedLayerAndAChangedPropertyAndASizeChange() {
        CanvasStore s = store();
        const Fixture f = canvasWithBase(s);
        PaintDocument edited = sample(512); // not a copy of f.base: a copy shares its pixels
        edited.setActiveLayer(edited.layer(0).id);
        edited.brush.color = Rgba{1, 2, 3, 255};
        edited.fillRect(256, 256, 10, 10);
        edited.setOpacity(edited.layer(1).id, 0.5);
        const Tree a = treeOf(f.base).value(), b = treeOf(edited).value();
        const TreeDiff diff = diffTrees(a, b);
        QCOMPARE(diff.layers.size(), size_t(2));
        QCOMPARE(diff.layers[0].name, QString("Base"));
        QCOMPARE(diff.layers[0].tilesChanged, 1);
        QCOMPARE(diff.layers[0].bounds, (mask::IRect{256, 256, 256, 256}));
        QCOMPARE(diff.layers[1].properties, QStringList{"opacity"});
        QCOMPARE(diff.layers[1].tilesChanged, 0);
        QVERIFY(!diff.sizeChanged);
        QVERIFY(diffTrees(a, treeOf(sample(256)).value()).sizeChanged);
    }

    void objectsAreWrittenOnceAndNothingIsLeftHalfWritten() {
        CanvasStore s = store();
        s.create("x", sample(256), "test");
        QDirIterator it(m_dir.path(), {"*.part", ".*.part"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        QVERIFY(!it.hasNext());
    }
};

QTEST_GUILESS_MAIN(TestCanvasStore)
#include "test_canvas_store.moc"
