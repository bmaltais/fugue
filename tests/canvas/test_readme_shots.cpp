// Not checks: the README's pictures (#94, docs/recording-screenshots.md). Each test skips unless
// FUGUE_SHOT_DIR names a folder, then saves the real window (Main.qml, offscreen) there as a PNG.
// Nothing here uses the owner's files: the window, its settings, templates folder and results are
// the test's own, the templates are the bundled ones. FUGUE_SHOT_TEMPLATE names another template
// for hero.png.

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQuickStyle>
#include <QtTest>

#include "canvas_app.h"
#include "../readme_shot.h"

using namespace canvas_test;

namespace {

QString shotDir() { return qEnvironmentVariable("FUGUE_SHOT_DIR"); }

QByteArray template_(const QString &name) {
    QFile file(QFINDTESTDATA("../../templates/" + name + "/workflow.fugue"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The window as the README shows it: a wide one, no trust or notice banner, the graph fitted.
void prepare(App &app, const QSize &size = {1600, 1000}) {
    app.window->resize(size);
    app.workflows.dismissPendingTrust();
    app.workflows.dismissNotice();
    QTest::qWait(300); // the canvas takes its new size before it fits
    app.model.select({});
    app.model.requestFit();
    QTest::qWait(800);
}

bool saveGraph(App &app, const QString &name) {
    return readme_shot::graphOnly(app.window, app.canvas).save(shotDir() + "/" + name);
}

} // namespace

class TestReadmeShots : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); } // as main() does

    void init() {
        if (shotDir().isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the README screenshots");
        qputenv("FUGUE_TEMPLATES", QFINDTESTDATA("../../templates").toUtf8());
    }

    // The bundled text-to-image template on the canvas, in the dark look and in a light one.
    void hero_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("file");
        QTest::newRow("dark") << QString() << "hero.png";
        QTest::newRow("light") << "catppuccin-latte" << "hero-light.png";
    }
    void hero() {
        QFETCH(QString, theme);
        QFETCH(QString, file);
        const QString name = qEnvironmentVariable("FUGUE_SHOT_TEMPLATE", "text-to-image");
        App app(load(template_(name)), {}, Look{theme, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(saveGraph(app, file));
    }

    // File > Browse templates: the dialog over the canvas.
    void templatesDialog() {
        App app(load(template_("text-to-image")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(app.commands.run("file.templates"));
        QTest::qWait(1500); // the cards' previews load
        // The dialog and a margin of the dimmed canvas. Its footnote names this test's own
        // temporary folder, so that line is painted over with the sidebar's color.
        QImage shot = test_support::grab(app.window).convertToFormat(QImage::Format_ARGB32);
        const QRect dialog(320, 180, 960, 640); // centered, as Popup sizes it in a 1600 x 1000 window
        QPainter(&shot).fillRect(QRect(dialog.left() + 1, dialog.bottom() - 60, 207, 58), shot.pixelColor(dialog.left() + 60, dialog.bottom() - 100));
        QVERIFY(shot.copy(dialog.adjusted(-40, -40, 40, 40)).save(shotDir() + "/templates.png"));
    }

    // The node library panel (N) beside a workflow.
    void nodeLibrary() {
        App app(load(template_("z-image-turbo")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        app.key(Qt::Key_N);
        QTest::qWait(300);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, QPoint(40, 202)); // Image
        QTest::mouseClick(app.window, Qt::LeftButton, {}, QPoint(40, 174)); // Generate
        QTest::qWait(300);
        QTest::mouseMove(app.window, QPoint(900, 900)); // off the list
        app.model.requestFit(); // the canvas beside the panel
        QTest::qWait(800);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 1600, 760).save(shotDir() + "/node-library.png"));
    }

    // The prompt encoder, sampler and decoder made into one group node.
    void group() {
        App app(load(template_("z-image-turbo")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        app.model.select({"encode", "sample", "decode"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        app.key(Qt::Key_Escape); // ends the name edit the new group starts
        app.model.select({});
        app.model.requestFit();
        QTest::qWait(800);
        QVERIFY(saveGraph(app, "group.png"));
    }
};

QTEST_MAIN(TestReadmeShots)
#include "test_readme_shots.moc"
