// The media browser in the real window (Main.qml, offscreen, #133): the workflows panel's Results
// view lists what the current workflow's runs made, in a grid, a filmstrip and a single view, by
// the pointer and the keys, and does an action on the current item.

#include <QImage>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
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

    static void click(App &app, QQuickItem *item) { QTest::mouseClick(app.window, Qt::LeftButton, {}, center(item)); }
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
        click(app, named(app, "workflowsView-results"));
        QTRY_VERIFY(named(app, "workflowsResults"));
        QVERIFY(named(app, "workflowsResults")->property("items").toList().isEmpty());
        QVERIFY(named(app, "workflowsResults")->property("emptyText").toString().contains("Save"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestMediaWindow)
#include "test_media_window.moc"
