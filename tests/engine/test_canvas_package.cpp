// A canvas shared as a .fuguepack (#249, epic #206): the head alone or with its history goes out, and comes in as a
// new canvas in another store without touching what is there. The package is untrusted input, so the second half of
// the file hands the importer bad ones. Seam: engine/canvas_package.h, with folders and a clock of the test's own.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_package.h"
#include "engine/canonical.h"
#include "engine/canvas_layer_ops.h"
#include "engine/canvas_tiles.h"
#include "engine/canvas_store.h"
#include "engine/zip_archive.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

PaintDocument sample(int size = 512) {
    PaintDocument doc(size, size);
    doc.addLayer(LayerKind::Paint, "Base");
    doc.brush.color = Rgba{10, 120, 200, 255};
    doc.fillRect(0, 0, size, size);
    doc.addLayer(LayerKind::Paint, "Top");
    doc.brush.color = Rgba{255, 255, 255, 255};
    doc.fillRect(10, 10, 20, 20);
    return doc;
}

QByteArray readFileBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// QVERIFY2 builds its message even when the check passes, so the first error is taken only when there is one.
QString firstMessage(const Errors &errors) { return errors.isEmpty() ? QString() : errors.first().message; }

QByteArray jsonOf(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Indented); }

} // namespace

class TestCanvasPackage : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_tick = 0;
    Clock clock() { return [this] { return QDateTime(QDate(2026, 11, 22), QTime(12, 0, 0), QTimeZone::utc()).addSecs(m_tick++); }; }
    CanvasStore storeAt(const QString &name) { return CanvasStore(m_dir.path() + "/" + name, clock()); }
    QString zipPath(const QString &name) { return m_dir.path() + "/" + name + ".fuguepack"; }

    // A canvas of three revisions: created, a layer added, the top layer hidden.
    QString threeRevisions(CanvasStore &store) {
        const Result<CanvasInfo> made = store.create("My canvas", sample(), "new");
        if (!(made.ok())) qFatal("threeRevisions: create");
        const QString id = made->id;
        const QString first = made->head;
        const Result<PaintDocument> loaded = store.load(id, first);
        if (!(loaded.ok())) qFatal("threeRevisions: load");
        PaintDocument second = loaded.value();
        second.addLayer(LayerKind::Paint, "Third");
        second.brush.color = Rgba{200, 0, 0, 255};
        second.fillRect(100, 100, 50, 50);
        const Result<Commit> added = store.commit(id, second, first, "test");
        if (!(added.ok() && added->move.moved)) qFatal("threeRevisions: commit second");
        PaintDocument third = second;
        third.setVisible(third.layer(0).id, false);
        const Result<Commit> last = store.commit(id, third, added->revision.id, "test");
        if (!(last.ok() && last->move.moved)) qFatal("threeRevisions: commit third");
        return id;
    }

    // The folder a package unpacks to, so a test can change one file and zip it again.
    QList<ZipEntry> entriesOf(const QString &zip, QString *folder = nullptr) {
        QTemporaryDir out;
        if (hasErrors(unpackZip(zip, out.path(), {qint64(1) << 28, qint64(1) << 30, 100000})))
            qFatal("entriesOf: unpack");
        QList<ZipEntry> entries;
        QDirIterator it(out.path(), QDir::Files, QDirIterator::Subdirectories);
        const QDir base(out.path());
        while (it.hasNext()) {
            it.next();
            entries << ZipEntry{base.relativeFilePath(it.filePath()), readFileBytes(it.filePath()), {}};
        }
        if (folder && !entries.isEmpty())
            *folder = entries.first().name.section('/', 0, 0);
        return entries;
    }
    static ZipEntry &entry(QList<ZipEntry> &entries, const QString &endsWith) {
        for (ZipEntry &e : entries)
            if (e.name.endsWith(endsWith))
                return e;
        qFatal("entry: none ends with %s", qPrintable(endsWith));
    }
    QString zipped(const QList<ZipEntry> &entries, const QString &name) {
        const QString path = zipPath(name);
        QFile::remove(path);
        if (hasErrors(writeZipFile(entries, path)))
            qFatal("zipped: write");
        return path;
    }
    // The canvases of a store, to see that a refused package left none.
    int canvasCount(CanvasStore &store) { return int(store.list().size()); }
    QStringList leftovers(const CanvasStore &store) { return QDir(store.root()).entryList({"import-*"}, QDir::Dirs | QDir::NoDotAndDotDot); }

private slots:
    // Each test starts with an empty folder: its stores and packages are its own.
    void init() {
        for (const QString &entry : QDir(m_dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
            if (QFileInfo(m_dir.filePath(entry)).isDir())
                QDir(m_dir.filePath(entry)).removeRecursively();
            else
                QFile::remove(m_dir.filePath(entry));
        m_tick = 0;
    }

    void historyRoundTripKeepsEveryRevision() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::History, zipPath("hist"))));
        QVERIFY(isCanvasPackage(zipPath("hist")));

        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipPath("hist"), to);
        QVERIFY2(got.ok(), qPrintable(firstMessage(got.errors())));
        QCOMPARE(got->revisions, 3);
        QCOMPARE(got->info.id, id); // nobody had that id here
        QVERIFY(!got->renamedId);
        QCOMPARE(got->info.name, QString("My canvas"));
        // The same head, the same history (ids are content hashes), the same pixels.
        QCOMPARE(got->info.head, from.info(id)->head);
        const Result<std::vector<Revision>> originalLog = from.log(id, QString(), 100);
        const Result<std::vector<Revision>> copyLog = to.log(id, QString(), 100);
        QVERIFY2(originalLog.ok() && copyLog.ok(), qPrintable(firstMessage(copyLog.errors())));
        const auto &original = originalLog.value();
        const auto &copy = copyLog.value();
        QCOMPARE(copy.size(), original.size());
        for (size_t i = 0; i < copy.size(); ++i)
            QCOMPARE(copy[i].id, original[i].id);
        QCOMPARE(CanvasStore::treeHashOf(to.load(id, got->info.head).value()).value(), CanvasStore::treeHashOf(from.load(id, got->info.head).value()).value());
        // Undo works on the imported history.
        const Result<HeadMove> undone = to.undo(id, got->info.head);
        QVERIFY2(undone.ok(), qPrintable(firstMessage(undone.errors())));
        QVERIFY(undone->moved);
    }

    void headOnlyIsOneRevisionWithTheLayers() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("head"))));
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipPath("head"), to);
        QVERIFY(got.ok());
        QCOMPARE(got->revisions, 1);
        QCOMPARE(to.log(id, QString(), 100)->size(), size_t(1));
        const Result<Revision> rev = to.revision(id, got->info.head);
        QVERIFY(rev->parent.isEmpty());
        QCOMPARE(rev->tree, from.revision(id, from.info(id)->head)->tree); // the layers, tiles and properties of the head
        QCOMPARE(to.tree(rev->tree)->layers.size(), size_t(3));
    }

    void aTakenIdGetsANewOneAndTheExistingCanvasIsUntouched() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("p"))));
        CanvasStore to = storeAt("b");
        const QString mine = to.create("Mine", sample(64), "new")->id;
        // A canvas of the opener with the very id of the shared one.
        QDir().mkpath(to.root() + "/canvases");
        QVERIFY(QDir().rename(to.root() + "/canvases/" + mine, to.root() + "/canvases/" + id));
        QFile record(to.root() + "/canvases/" + id + "/canvas.json");
        QVERIFY(record.open(QIODevice::ReadWrite));
        QJsonObject json = QJsonDocument::fromJson(record.readAll()).object();
        json["id"] = id;
        record.resize(0);
        record.write(jsonOf(json));
        record.close();
        const QString headBefore = to.info(id)->head;

        const Result<CanvasImported> got = importCanvasPackage(zipPath("p"), to);
        QVERIFY(got.ok());
        QVERIFY(got->renamedId);
        QCOMPARE(got->packagedId, id);
        QVERIFY(got->info.id != id);
        QVERIFY(isCanvasId(got->info.id));
        QCOMPARE(canvasCount(to), 2);
        QCOMPARE(to.info(id)->head, headBefore);
        QCOMPARE(to.info(id)->name, QString("Mine"));
        // Importing it again makes a third, never a replacement.
        const Result<CanvasImported> again = importCanvasPackage(zipPath("p"), to, "  Renamed  ");
        QVERIFY(again.ok());
        QCOMPARE(again->info.name, QString("Renamed"));
        QCOMPARE(canvasCount(to), 3);
    }

    void layerMasksTravel() {
        CanvasStore from = storeAt("a");
        PaintDocument doc = sample();
        const QString id = from.create("Masked", doc, "new")->id;
        MaskPaint mask(512, 512, 1);
        mask.fillRect(0, 0, 100, 100);
        const QString head = from.info(id)->head;
        QVERIFY(from.setLayerMask(id, head, 1, mask, "test")->move.moved);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("mask"))));
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipPath("mask"), to);
        QVERIFY(got.ok());
        const Tree tree = to.tree(to.revision(id, got->info.head)->tree).value();
        QVERIFY(tree.layers[1].hasMask);
        QVERIFY(!tree.layers[1].maskTiles.empty());
    }

    void nothingIsWrittenOverAnExistingFile() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("once"))));
        const Errors again = writeCanvasPackage(from, id, SharedPart::Head, zipPath("once"));
        QVERIFY(hasErrors(again));
        QCOMPARE(again.first().code, QString("permission_denied"));
    }

    void anUnknownCanvasIsRefused() {
        CanvasStore from = storeAt("a");
        const Errors e = writeCanvasPackage(from, "canvas-nope", SharedPart::Head, zipPath("none"));
        QCOMPARE(e.first().code, QString("canvas_not_found"));
        QVERIFY(!QFileInfo::exists(zipPath("none")));
    }

    // ---- untrusted packages ----

    void aWorkflowPackageIsNotACanvasPackage() {
        const QString path = zipped({ZipEntry{"w/workflow.fugue", "{}", {}}}, "wf");
        QVERIFY(!isCanvasPackage(path));
        CanvasStore to = storeAt("b");
        QCOMPARE(importCanvasPackage(path, to).errors().first().code, QString("invalid_document"));
    }

    void pathsOutsideTheFolderAreRefusedAndNothingIsWrittenThere() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        for (const QString &bad : {QString("../escaped.txt"), QString("/tmp/escaped.txt"), QString("canvas/../../escaped.txt"), QString("a\\b.txt")}) {
            QList<ZipEntry> entries = entriesOf(zipPath("good"));
            entries << ZipEntry{bad, "x", {}};
            const QString path = zipped(entries, "evil");
            CanvasStore to = storeAt("b");
            const Result<CanvasImported> got = importCanvasPackage(path, to);
            QVERIFY2(!got.ok(), qPrintable(bad));
            QCOMPARE(got.errors().first().code, QString("invalid_document"));
            QCOMPARE(canvasCount(to), 0);
            QVERIFY(leftovers(to).isEmpty());
            QVERIFY(!QFileInfo::exists(m_dir.path() + "/escaped.txt"));
            QVERIFY(!QFileInfo::exists(m_dir.path() + "/b/escaped.txt"));
        }
    }

    void aTreeThatIsNotItsNameIsCorrupt() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        for (ZipEntry &e : entries)
            if (e.name.contains("/trees/"))
                e.bytes = jsonOf(QJsonObject{{"format", 1}, {"width", 5}, {"height", 5}, {"layers", QJsonArray{}}});
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipped(entries, "badtree"), to);
        QVERIFY(!got.ok());
        QCOMPARE(got.errors().first().code, QString("canvas_corrupt"));
        QCOMPARE(canvasCount(to), 0);
    }

    void aTileThatIsNotItsNameIsCorruptAndLeavesNothing() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::History, zipPath("good"))));
        // Another tile's bytes under a tile's name: a real PNG of the right size, the wrong pixels.
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        int swapped = 0;
        for (ZipEntry &e : entries)
            if (e.name.contains("/tiles/") && swapped++ == 0) {
                // a PNG of the same size: re-encode a different color tile
                const MaskPaint tile(256, 256, std::vector<uint8_t>(256 * 256 * 4, 77), 4);
                e.bytes = tileToPng(tile).value();
            }
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipped(entries, "badtile"), to);
        QVERIFY(!got.ok());
        QCOMPARE(got.errors().first().code, QString("canvas_corrupt"));
        QCOMPARE(canvasCount(to), 0);
        QVERIFY(QDir(to.root() + "/canvases").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(leftovers(to).isEmpty());
    }

    void aTileOfAnotherSizeIsRefusedBeforeItIsDecoded() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        for (ZipEntry &e : entries)
            if (e.name.contains("/tiles/")) {
                MaskPaint big(1000, 1000, 4);
                e.bytes = tileToPng(big).value();
            }
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipped(entries, "badsize"), to);
        QVERIFY(!got.ok());
        QCOMPARE(got.errors().first().code, QString("canvas_corrupt"));
        QCOMPARE(canvasCount(to), 0);
    }

    void aMissingTileIsRefused() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        for (int i = 0; i < entries.size(); ++i)
            if (entries[i].name.contains("/tiles/")) {
                entries.removeAt(i);
                break;
            }
        CanvasStore to = storeAt("b");
        QVERIFY(!importCanvasPackage(zipped(entries, "missing"), to).ok());
        QCOMPARE(canvasCount(to), 0);
    }

    void aForgedRevisionIsRefused() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::History, zipPath("good"))));
        for (const QString &field : {QString("producer"), QString("time"), QString("parent")}) {
            QList<ZipEntry> entries = entriesOf(zipPath("good"));
            ZipEntry &manifest = entry(entries, "/canvas.json");
            QJsonObject json = QJsonDocument::fromJson(manifest.bytes).object();
            QJsonArray revisions = json["revisions"].toArray();
            QJsonObject last = revisions.last().toObject();
            last[field] = field == "parent" ? revisions.first().toObject()["id"].toString() : QString("forged");
            revisions.replace(revisions.size() - 1, last);
            json["revisions"] = revisions;
            manifest.bytes = jsonOf(json);
            CanvasStore to = storeAt("b");
            QVERIFY2(!importCanvasPackage(zipped(entries, "forged"), to).ok(), qPrintable(field));
            QCOMPARE(canvasCount(to), 0);
        }
    }

    void aManifestWithAHashThatIsAPathIsRefused() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        ZipEntry &manifest = entry(entries, "/canvas.json");
        QJsonObject json = QJsonDocument::fromJson(manifest.bytes).object();
        QJsonArray revisions = json["revisions"].toArray();
        QJsonObject only = revisions.first().toObject();
        only["tree"] = QString("../../../../etc/passwd");
        json["revisions"] = QJsonArray{only};
        manifest.bytes = jsonOf(json);
        CanvasStore to = storeAt("b");
        QVERIFY(!importCanvasPackage(zipped(entries, "path"), to).ok());
        QCOMPARE(canvasCount(to), 0);
    }

    void manifestLimitsAndShape() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::History, zipPath("good"))));
        const auto tamper = [&](const std::function<void(QJsonObject &)> &change) {
            QList<ZipEntry> entries = entriesOf(zipPath("good"));
            ZipEntry &manifest = entry(entries, "/canvas.json");
            QJsonObject json = QJsonDocument::fromJson(manifest.bytes).object();
            change(json);
            manifest.bytes = jsonOf(json);
            CanvasStore to = storeAt("t");
            const bool ok = importCanvasPackage(zipped(entries, "tamper"), to).ok();
            QDir(to.root()).removeRecursively();
            return ok;
        };
        QVERIFY(!tamper([](QJsonObject &j) { j["mode"] = "everything"; }));
        QVERIFY(!tamper([](QJsonObject &j) { j["format"] = 2; }));
        QVERIFY(!tamper([](QJsonObject &j) { j["kind"] = "workflow"; }));
        QVERIFY(!tamper([](QJsonObject &j) { j["mode"] = "head"; })); // three revisions in a head package
        QVERIFY(!tamper([](QJsonObject &j) { j["revisions"] = QJsonArray{}; }));
        QVERIFY(!tamper([](QJsonObject &j) { j["name"] = QString(); }));
        QVERIFY(!tamper([](QJsonObject &j) { j["name"] = QString(500, 'x'); }));
        QVERIFY(tamper([](QJsonObject &j) { j["id"] = "../../x"; })); // a bad id is not used: the canvas gets its own
    }

    void aBadIdNeverBecomesAFolder() {
        CanvasStore from = storeAt("a");
        const QString id = threeRevisions(from);
        QVERIFY(!hasErrors(writeCanvasPackage(from, id, SharedPart::Head, zipPath("good"))));
        QList<ZipEntry> entries = entriesOf(zipPath("good"));
        ZipEntry &manifest = entry(entries, "/canvas.json");
        QJsonObject json = QJsonDocument::fromJson(manifest.bytes).object();
        json["id"] = "../../escape";
        manifest.bytes = jsonOf(json);
        CanvasStore to = storeAt("b");
        const Result<CanvasImported> got = importCanvasPackage(zipped(entries, "idpath"), to);
        QVERIFY(got.ok());
        QVERIFY(got->renamedId);
        QVERIFY(got->info.id.startsWith("canvas-"));
        QVERIFY(!QFileInfo::exists(m_dir.path() + "/escape"));
    }

    void tooManyEntriesAndOversizedFilesAreRefused() {
        QList<ZipEntry> many;
        for (int i = 0; i < 5; ++i)
            many << ZipEntry{QString("c/f%1").arg(i), "x", {}};
        QTemporaryDir out2;
        QVERIFY(hasErrors(unpackZip(zipped(many, "many"), out2.path(), {10, 100, 3})));
        QTemporaryDir out3;
        QVERIFY(hasErrors(unpackZip(zipped({ZipEntry{"c/big", QByteArray(100, 'x'), {}}}, "big"), out3.path(), {10, 1000, 100})));
    }
};

QTEST_GUILESS_MAIN(TestCanvasPackage)
#include "test_canvas_package.moc"
