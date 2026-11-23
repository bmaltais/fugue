// The guides and grid of a canvas (#253): the sidecar file's validation on read, and the store keeping it beside the canvas
// without making a revision. Seam: the pure functions on bytes, and the store with a folder of the test's own.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_guides.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::canvas;

class TestCanvasGuides : public QObject {
    Q_OBJECT

    static QByteArray bytes(const char *json) { return QByteArray(json); }

private slots:
    void aSetRoundTripsSortedAndWithoutRepeats() {
        GuideSet set;
        set.grid = 32;
        set.guides = {{false, 40}, {true, 90}, {true, 10}, {true, 10}};
        const Result<GuideSet> back = guidesFromJson(guidesToJson(set));
        QVERIFY(back);
        QCOMPARE(back->grid, 32);
        QCOMPARE(back->guides.size(), size_t(3));
        QCOMPARE(back->guides[0], (Guide{true, 10}));
        QCOMPARE(back->guides[1], (Guide{true, 90}));
        QCOMPARE(back->guides[2], (Guide{false, 40}));
        QCOMPARE(guidesToJson(*back), guidesToJson(set)); // the same set is always the same file
    }

    void aMinimalFileReadsWithTheDefaults() {
        const Result<GuideSet> set = guidesFromJson(bytes(R"({"format": 1})"));
        QVERIFY(set);
        QVERIFY(set->guides.empty());
        QCOMPARE(set->grid, defaultGridSpacing);
    }

    void aBadFileIsRefusedInPlainWords() {
        const char *bad[] = {
            "not json",
            "[]",
            R"({})",                                                     // no format
            R"({"format": 2})",                                          // a format this version does not know
            R"({"format": "1"})",
            R"({"format": 1, "grid": 1})",                               // below the range
            R"({"format": 1, "grid": 99999})",
            R"({"format": 1, "grid": 2.5})",
            R"({"format": 1, "guides": {}})",
            R"({"format": 1, "guides": [{"axis": "z", "at": 3}]})",
            R"({"format": 1, "guides": [{"axis": "x"}]})",
            R"({"format": 1, "guides": [{"axis": "x", "at": 1.5}]})",
            R"({"format": 1, "guides": [{"axis": "x", "at": "7"}]})",
            R"({"format": 1, "guides": [{"axis": "x", "at": 99999999}]})",
            R"({"format": 1, "guides": [5]})",
        };
        for (const char *json : bad) {
            const Result<GuideSet> set = guidesFromJson(bytes(json));
            QVERIFY2(!set, json);
            QCOMPARE(set.errors().front().code, QString("canvas_corrupt"));
            QVERIFY2(set.errors().front().message.contains("guides"), json);
        }
    }

    void moreGuidesThanTheLimitAreRefused() {
        QByteArray json = R"({"format": 1, "guides": [)";
        for (int i = 0; i <= maxGuides; ++i)
            json += (i ? "," : "") + QByteArray(R"({"axis": "x", "at": )") + QByteArray::number(i) + "}";
        json += "]}";
        QVERIFY(!guidesFromJson(json));
    }

    void theStoreKeepsGuidesBesideTheCanvasNotInItsHistory() {
        QTemporaryDir dir;
        CanvasStore store(dir.path());
        mask::PaintDocument doc(64, 48);
        doc.addLayer(mask::LayerKind::Paint, "Base");
        const Result<CanvasInfo> made = store.create("c", doc, "test");
        QVERIFY(made);
        const QString head = made->head;
        // None yet: the defaults.
        Result<GuideSet> none = store.guides(made->id);
        QVERIFY(none && none->guides.empty());
        GuideSet set;
        set.grid = 16;
        set.guides = {{true, 20}, {false, 30}};
        QVERIFY(!hasErrors(store.setGuides(made->id, set)));
        const Result<GuideSet> read = store.guides(made->id);
        QVERIFY(read);
        QCOMPARE(read->grid, 16);
        QCOMPARE(read->guides.size(), size_t(2));
        // Not a revision: the head and the history are what they were.
        QCOMPARE(store.info(made->id)->head, head);
        QCOMPARE(store.log(made->id)->size(), size_t(1));
        QVERIFY(QFile::exists(dir.path() + "/canvases/" + made->id + "/guides.json"));
        // A copy of the canvas keeps them.
        const Result<CanvasInfo> copy = store.duplicate(made->id, "copy");
        QVERIFY(copy);
        QCOMPARE(store.guides(copy->id)->guides.size(), size_t(2));
        // An unknown canvas has none to give or take.
        QVERIFY(!store.guides("nope"));
        QVERIFY(hasErrors(store.setGuides("nope", set)));
    }

    void aDamagedFileIsAnErrorTheCallerCanShowNotACrash() {
        QTemporaryDir dir;
        CanvasStore store(dir.path());
        mask::PaintDocument doc(8, 8);
        doc.addLayer(mask::LayerKind::Paint, "Base");
        const Result<CanvasInfo> made = store.create("c", doc, "test");
        QVERIFY(made);
        QFile file(dir.path() + "/canvases/" + made->id + "/guides.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{ broken");
        file.close();
        const Result<GuideSet> read = store.guides(made->id);
        QVERIFY(!read);
        // Writing replaces it.
        QVERIFY(!hasErrors(store.setGuides(made->id, GuideSet{})));
        QVERIFY(store.guides(made->id));
    }
};

QTEST_GUILESS_MAIN(TestCanvasGuides)
#include "test_canvas_guides.moc"
