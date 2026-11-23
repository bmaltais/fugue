// The media browser in the real window (Main.qml, offscreen, #133): the workflows panel's Results
// view lists what the current workflow's runs made, in a grid, a filmstrip and a single view, by
// the pointer and the keys, and does an action on the current item.

#include <QBuffer>
#include <QImage>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "engine/embed.h"
#include "engine/run_folder.h"

using namespace canvas_test;

namespace {

const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}}}
})";

QPoint center(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

} // namespace

class TestMediaWindow : public QObject {
    Q_OBJECT

    // A Flow places its buttons when the window is polished: grabbing it first makes the click land on the laid-out one.
    static void click(App &app, QQuickItem *item) {
        app.window->grabWindow();
        QTest::mouseClick(app.window, Qt::LeftButton, {}, center(item));
    }
    static QQuickItem *named(App &app, const QString &name) { return item(app.window->contentItem(), name); }

    // A run of `workflow` that made `count` pictures, named <prefix>0.png...
    static void makeRun(App &app, const QString &id, const QString &workflow, const QString &prefix, int count) {
        const fugue::RunFolder folder = fugue::RunFolder::create(app.dir.filePath("runs"), id).value();
        QJsonObject outputs;
        for (int i = 0; i < count; ++i) {
            const QString path = app.dir.filePath(prefix + QString::number(i) + ".png");
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(Qt::darkMagenta);
            QVERIFY(image.save(path));
            outputs.insert("f" + QString::number(i), QJsonObject{{"type", "image"}, {"path", path}});
        }
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", workflow}}},
                                   {"nodes", QJsonObject{{"save", QJsonObject{{"state", "cached"}, {"outputs", outputs}}}}}}));
    }

private slots:
    void theResultsViewBrowsesTheWorkflowsOutputs() {
        App app(load(small));
        QVERIFY(app.open());
        const QString path = app.dir.filePath("mine.fugue");
        QVERIFY(load(small).saveFile(path).isEmpty());
        QVERIFY(app.workflows.openPath(path));
        makeRun(app, "20261121-100000-000-aaaa", path, "one", 1);
        makeRun(app, "20261121-110000-000-aaaa", path, "two", 2);
        makeRun(app, "20261121-120000-000-bbbb", "/somewhere/else.fugue", "other", 1); // not this workflow's
        QUrl opened;
        app.mediaLibrary.setOpenUrl([&opened](const QUrl &url) { opened = url; });

        app.commands.run("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsViewSwitch"));
        QTRY_VERIFY(named(app, "workflowsView-results")->x() > 0); // the Flow has laid its buttons out
        // Nothing of the media browser exists until the results are asked for.
        QVERIFY(!named(app, "workflowsResults"));
        click(app, named(app, "workflowsView-results"));
        QTRY_VERIFY(named(app, "workflowsResults"));
        QQuickItem *browser = named(app, "workflowsResults");
        QCOMPARE(browser->property("items").toList().size(), 3);
        QCOMPARE(browser->property("currentIndex").toInt(), 0);
        QCOMPARE(browser->property("mode").toString(), QString("grid"));
        // Newest run first: its two pictures, then the older one.
        QCOMPARE(browser->property("items").toList()[0].toMap().value("title").toString(), QString("two0.png"));
        QTRY_VERIFY(named(app, "mediaCell-2"));

        // Keys step along the grid; a click on a cell chooses it.
        QTest::keyClick(app.window, Qt::Key_Right);
        QCOMPARE(browser->property("currentIndex").toInt(), 1);
        click(app, named(app, "mediaCell-2"));
        QCOMPARE(browser->property("currentIndex").toInt(), 2);
        
        QTest::keyClick(app.window, Qt::Key_Home);
        QCOMPARE(browser->property("currentIndex").toInt(), 0);
        QTest::keyClick(app.window, Qt::Key_End);
        QCOMPARE(browser->property("currentIndex").toInt(), 2);
        QTest::keyClick(app.window, Qt::Key_Right); // the end stays the end
        QCOMPARE(browser->property("currentIndex").toInt(), 2);

        // The filmstrip shows the current one large, with a strip of cells.
        click(app, named(app, "mediaMode-filmstrip"));
        QCOMPARE(browser->property("mode").toString(), QString("filmstrip"));
        QTRY_VERIFY(named(app, "mediaStripCell-0"));
        QVERIFY(named(app, "mediaLarge")->isVisible());
        QTest::keyClick(app.window, Qt::Key_Left);
        QCOMPARE(browser->property("currentIndex").toInt(), 1);
        if (qEnvironmentVariableIsSet("FUGUE_MEDIA_GRAB")) { // a look at it, by hand: the path to write
            QTest::qWait(500);
            test_support::grab(app.window).save(qEnvironmentVariable("FUGUE_MEDIA_GRAB"));
        }
        // The details of the current item are given by the host.
        QTRY_VERIFY(!browser->property("details").toList().isEmpty());

        // Enter goes to the single view, Escape comes back to the mode it came from.
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(browser->property("mode").toString(), QString("single"));
        QTest::keyClick(app.window, Qt::Key_Escape);
        QCOMPARE(browser->property("mode").toString(), QString("filmstrip"));

        // An action: Open hands the file of the current item to the desktop.
        click(app, named(app, "mediaAction-open"));
        QCOMPARE(opened, QUrl::fromLocalFile(app.dir.filePath("two1.png")));

        // Back to the files: the browser goes with the view.
        click(app, named(app, "workflowsView-files"));
        QTRY_VERIFY(!named(app, "workflowsResults"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Favorites, zoom, compare, send to Load Image, open the workflow, delete (to a trash of the test's own)
    // and what the browser remembers (#133), by the buttons and keys.
    void theBrowserMarksZoomsComparesSendsAndDeletes() {
        App app(load(small));
        QVERIFY(app.open());
        const QString path = app.dir.filePath("acts.fugue");
        QVERIFY(load(small).saveFile(path).isEmpty());
        QVERIFY(app.workflows.openPath(path));
        makeRun(app, "20261126-100000-000-aaaa", path, "pic", 3);
        // The last picture was made by a workflow: it can be opened again.
        QVERIFY(QFile::remove(app.dir.filePath("pic2.png")));
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::darkYellow);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        const QByteArray embedded = fugue::pngWithWorkflow(png, R"({"fugue": 1, "graph": {"nodes": {"t": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "again"}}}}})").value();
        QFile with(app.dir.filePath("pic2.png"));
        QVERIFY(with.open(QIODevice::WriteOnly) && with.write(embedded) == embedded.size());
        with.close();
        QDir().mkpath(app.dir.filePath("trash"));
        app.mediaLibrary.setTrash([&app](const QString &file) {
            return QFile::rename(file, app.dir.filePath("trash/") + QFileInfo(file).fileName());
        });

        app.commands.run("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsViewSwitch"));
        QTRY_VERIFY(named(app, "workflowsView-results")->x() > 0); // the Flow has laid its buttons out
        click(app, named(app, "workflowsView-results"));
        QTRY_VERIFY(named(app, "mediaCell-2"));
        QQuickItem *browser = named(app, "workflowsResults");
        const QString shotDir = qEnvironmentVariable("FUGUE_SHOT_DIR");
        auto shot = [&](const char *name) {
            if (shotDir.isEmpty())
                return;
            QTest::qWait(400);
            test_support::grab(app.window).save(shotDir + "/media-" + name + ".png");
        };

        // F marks the current one; the star shows and the library remembers it.
        QTest::keyClick(app.window, Qt::Key_F);
        QTRY_VERIFY(app.mediaLibrary.isFavorite(app.dir.filePath("pic0.png")));
        QTRY_VERIFY(browser->property("items").toList()[0].toMap().value("favorite").toBool());
        QTRY_VERIFY(named(app, "favoriteMark"));
        shot("grid-favorite");
        // The Favorites button narrows the list to it.
        click(app, named(app, "workflowsResultsFavorites"));
        QTRY_COMPARE(browser->property("items").toList().size(), 1);
        click(app, named(app, "workflowsResultsFavorites"));
        QTRY_COMPARE(browser->property("items").toList().size(), 3);

        // Single: + zooms the picture, 0 fits it again.
        click(app, named(app, "mediaMode-single"));
        QTRY_VERIFY(named(app, "mediaViewer"));
        QQuickItem *viewer = named(app, "mediaViewer");
        QCOMPARE(viewer->property("zoom").toDouble(), 1.0);
        QTest::keyClick(app.window, Qt::Key_Plus);
        QTRY_VERIFY(viewer->property("zoom").toDouble() > 1.0);
        shot("single-zoomed");
        QTest::keyClick(app.window, Qt::Key_0);
        QTRY_COMPARE(viewer->property("zoom").toDouble(), 1.0);

        // C pins this picture; the next one chosen is compared with it.
        QTest::keyClick(app.window, Qt::Key_C);
        QCOMPARE(browser->property("compareIndex").toInt(), 0);
        QVERIFY(!browser->property("comparing").toBool());
        QTest::keyClick(app.window, Qt::Key_Right);
        QTRY_VERIFY(browser->property("comparing").toBool());
        QTRY_COMPARE(named(app, "mediaViewer")->property("mode").toString(), QString("slider"));
        shot("compare");
        QTest::keyClick(app.window, Qt::Key_C);
        QVERIFY(!browser->property("comparing").toBool());

        // The picture with a workflow in it: Open workflow makes a tab of it; Send puts it in a Load Image node.
        QTest::keyClick(app.window, Qt::Key_End);
        QCOMPARE(browser->property("currentIndex").toInt(), 2);
        QTRY_VERIFY(named(app, "mediaAction-workflow")->isEnabled()); // the details of the new current item came
        const int tabs = app.workflows.count();
        click(app, named(app, "mediaAction-workflow"));
        QCOMPARE(app.workflows.count(), tabs + 1);
        app.workflows.setCurrent(tabs - 1);
        QTRY_VERIFY(named(app, "mediaAction-send")->isEnabled());
        const int before = app.model.document().graph.nodes.size();
        click(app, named(app, "mediaAction-send"));
        QTRY_COMPARE(app.model.document().graph.nodes.size(), before + 1);

        // Move to trash asks first; Cancel leaves the file, the question again and Yes moves it.
        const QString doomed = browser->property("currentItem").toMap().value("title").toString();
        QVERIFY(!doomed.isEmpty());
        QTest::keyClick(app.window, Qt::Key_Delete);
        QTRY_VERIFY(named(app, "mediaConfirm")->isVisible());
        shot("confirm-delete");
        click(app, named(app, "mediaConfirmNo"));
        QVERIFY(QFileInfo::exists(app.dir.filePath(doomed)));
        QTest::keyClick(app.window, Qt::Key_Delete);
        QTRY_VERIFY(named(app, "mediaConfirm")->isVisible());
        click(app, named(app, "mediaConfirmYes"));
        QTRY_VERIFY(!QFileInfo::exists(app.dir.filePath(doomed)));
        QVERIFY(QFileInfo::exists(app.dir.filePath("trash/" + doomed)));
        QTRY_COMPARE(browser->property("items").toList().size(), 2);

        // The mode and the view are kept for the next window.
        QCOMPARE(app.mediaLibrary.resultsMode(), QString("single"));
        QCOMPARE(app.mediaLibrary.resultsView(), QString("results"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A video's cell shows a frame of it, and a file ffmpeg cannot read keeps its glyph and name.
    void aVideoCellShowsAFrameAndABrokenOneTheGlyph() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        App app(load(small));
        QVERIFY(app.open());
        const QString path = app.dir.filePath("vid.fugue");
        QVERIFY(load(small).saveFile(path).isEmpty());
        QVERIFY(app.workflows.openPath(path));
        const QString clip = app.dir.filePath("good.mp4");
        if (ffmpeg.isEmpty() || QProcess::execute(ffmpeg, {"-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                                          "testsrc=s=160x90:d=2:r=10", clip}) != 0)
            QSKIP("ffmpeg is not installed (or cannot make the test clip): video cells not tested");
        QFile bad(app.dir.filePath("bad.mp4"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a video");
        bad.close();
        const fugue::RunFolder folder = fugue::RunFolder::create(app.dir.filePath("runs"), "20261125-100000-000-vvvv").value();
        const QJsonObject outputs{{"a", QJsonObject{{"type", "video"}, {"path", clip}}},
                                  {"b", QJsonObject{{"type", "video"}, {"path", bad.fileName()}}}};
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", path}}},
                                   {"nodes", QJsonObject{{"save", QJsonObject{{"state", "cached"}, {"outputs", outputs}}}}}}));
        app.commands.run("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsViewSwitch"));
        QTRY_VERIFY(named(app, "workflowsView-results")->x() > 0); // the Flow has laid its buttons out
        click(app, named(app, "workflowsView-results"));
        QTRY_COMPARE(named(app, "workflowsResults")->property("items").toList().size(), 2);
        // The cell's picture is its Image child: ready for the clip, an error for the broken file.
        auto status = [&](const QString &cell) {
            QQuickItem *cellItem = named(app, cell);
            if (!cellItem)
                return -1;
            for (QQuickItem *child : cellItem->childItems())
                if (child->inherits("QQuickImage"))
                    return child->property("status").toInt();
            return -1;
        };
        const int ready = 1, error = 3; // QQuickImageBase::Ready, Error
        QTRY_VERIFY(named(app, "mediaCell-1"));
        QTRY_VERIFY2(status("mediaCell-0") == ready && status("mediaCell-1") == error,
                     qPrintable(QString("%1 %2").arg(status("mediaCell-0")).arg(status("mediaCell-1"))));
        if (qEnvironmentVariableIsSet("FUGUE_MEDIA_GRAB")) {
            QTest::qWait(300);
            test_support::grab(app.window).save(qEnvironmentVariable("FUGUE_MEDIA_GRAB"));
        }
    }

    void aWorkflowThatWasNeverSavedHasNoResultsAndSaysSo() {
        App app(load(small));
        QVERIFY(app.open());
        app.commands.run("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsViewSwitch"));
        QTRY_VERIFY(named(app, "workflowsView-results")->x() > 0); // the Flow has laid its buttons out
        click(app, named(app, "workflowsView-results"));
        QTRY_VERIFY(named(app, "workflowsResults"));
        QVERIFY(named(app, "workflowsResults")->property("items").toList().isEmpty());
        QVERIFY(named(app, "workflowsResults")->property("emptyText").toString().contains("Save"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestMediaWindow)
#include "test_media_window.moc"
