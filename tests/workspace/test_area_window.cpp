// The window's one area (#217 slice 0): the default window is item for item what it was before areas. Its body
// is an area tree of one Graph area whose view is the window's own canvas, in the place it always had; the area
// adds no item, renames nothing, moves nothing and writes no file.
//
// The item count and the item names of the default window are kept in data/default-window.txt, measured on main
// at 1ba1e815, before areas. A change that means to add to the default window updates that file and says why:
// the default window stays as simple as it is (epic #217, the product principle). FUGUE_AREA_WINDOW_OUT=<file>
// writes what the window has now, in that file's form.

#include <QDirIterator>
#include <QQuickStyle>

#include "../canvas/canvas_app.h"
#include "window_items.h"

using namespace canvas_test;
using namespace window_items;

namespace {

// The files of the test's folder, home and settings, but caches (Qt keeps compiled QML and shaders there).
QStringList keptFiles(const QString &testFolder) {
    const QString cache = qEnvironmentVariable("XDG_CACHE_HOME", QDir::homePath() + "/.cache");
    QStringList files;
    for (const QString &folder : {testFolder, QDir::homePath(), qEnvironmentVariable("XDG_CONFIG_HOME")}) {
        if (folder.isEmpty())
            continue;
        QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext())
            if (const QString path = it.next(); !path.startsWith(cache + '/'))
                files << path;
    }
    files.removeDuplicates();
    files.sort();
    return files;
}

const char *blank = R"({"fugue": 1, "graph": {"nodes": {}}})";

} // namespace

class TestAreaWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theDefaultWindowHasTheItemsItHadBeforeAreas() {
        App app(load(blank));
        QVERIFY(app.open());
        QTest::qWait(200); // what is made after the first frame (tooltips' styles, the menu bar's slide)
        const QString now = describe(app.window->contentItem());
        if (const QString out = qEnvironmentVariable("FUGUE_AREA_WINDOW_OUT"); !out.isEmpty()) {
            QFile file(out);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(now.toUtf8());
        }
        const QString before = expected();
        QVERIFY2(!before.isEmpty(), "tests/workspace/data/default-window.txt is missing");
        QVERIFY2(now == before, qPrintable("the default window changed; it has now:\n" + now));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void itsOneAreaIsTheGraphInThePlaceTheCanvasHad() {
        App app(load(blank));
        QVERIFY(app.open());
        QObject *areas = app.window->findChild<QObject *>("areas");
        QVERIFY(areas);
        QVERIFY(!qobject_cast<QQuickItem *>(areas)); // not an item: nothing for the scene graph to walk
        QCOMPARE(areas->property("leaf").toBool(), true);
        QCOMPARE(areas->property("areaId").toInt(), 1);
        QCOMPARE(areas->property("viewType").toString(), QString("graph"));
        QCOMPARE(areas->property("adopted").toBool(), true);
        QQuickItem *view = nullptr;
        QMetaObject::invokeMethod(areas, "viewOf", Q_RETURN_ARG(QQuickItem *, view), Q_ARG(int, 1));
        QCOMPARE(view, app.canvas);
        int made = -1;
        QMetaObject::invokeMethod(areas, "madeItems", Q_RETURN_ARG(int, made));
        QCOMPARE(made, 0);
        // The canvas is where it was: in the window's column, under the tabs, filling the rest.
        QVERIFY(QByteArray(app.canvas->parentItem()->metaObject()->className()).contains("ColumnLayout"));
        QCOMPARE(app.canvas->objectName(), QString("canvas"));
        QCOMPARE(app.canvas->x() + app.canvas->width(), app.canvas->parentItem()->width()); // the side strip before it
        QCOMPARE(app.canvas->y() + app.canvas->height(), app.canvas->parentItem()->height());
    }

    void openingTheWindowWritesNoFile() {
        App app(load(blank));
        const QStringList before = keptFiles(app.dir.path());
        QVERIFY(app.open());
        QTest::qWait(300); // past the debounce of anything kept between runs
        const QStringList after = keptFiles(app.dir.path());
        QCOMPARE(after, before);
    }
};

QTEST_MAIN(TestAreaWindow)
#include "test_area_window.moc"
