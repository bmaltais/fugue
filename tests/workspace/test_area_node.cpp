// The area tree drawn (#217, workspace/AreaNode.qml), offscreen: a split tree puts each view at its rectangle in
// the host, an adopted view is moved but never made or destroyed, a type nobody registers is a placeholder, a
// change of sizes moves the same views, a change of shape makes them again, and right to left mirrors a row.
// The window itself never shows a split in slice 0; this is where the split is tested.

#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/registry.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"
#include "workspace/view_registry.h"

using namespace fugue;

namespace {

QVariantMap leaf(int id, const QString &view) {
    return {{"type", "leaf"}, {"area", id}, {"view", view}, {"state", QVariantMap{}}};
}
QVariantMap branch(const QString &axis, const QList<QPair<double, QVariantMap>> &children) {
    QVariantList list;
    for (const auto &child : children)
        list << QVariantMap{{"size", child.first}, {"node", child.second}};
    return {{"type", "branch"}, {"axis", axis}, {"children", list}};
}
// The Graph (area 1, adopted) at the start, and a column of a test view (2) over a type nobody registers (3).
QVariantMap split(double graph = 0.6) {
    return branch("row", {{graph, leaf(1, "graph")},
                          {1 - graph, branch("column", {{0.5, leaf(2, "test")}, {0.5, leaf(3, "storyboard")}})}});
}

const char *windowQml = R"(
import QtQuick
import Fugue
import "qrc:/workspace"

Window {
    id: window
    width: 800
    height: 600
    visible: true
    required property ViewRegistry registry
    property var tree: ({})
    property bool rtl: false
    Item {
        id: host
        objectName: "host"
        anchors.fill: parent
        // What the window's canvas is to its area: an item already there, adopted.
        Rectangle { id: graph; objectName: "graph" }
    }
    AreaNode {
        id: root
        objectName: "root"
        node: window.tree
        host: host
        area: Qt.rect(0, 0, host.width, host.height)
        registry: window.registry
        views: ({"1": graph})
        rightToLeft: window.rtl
    }
}
)";

QRectF geometry(const QQuickItem *item) { return QRectF(item->x(), item->y(), item->width(), item->height()); }

} // namespace

class TestAreaNode : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    Translator translator{dir.path() + "/language"};
    Theme theme{dir.path()};
    Registry registry;
    CanvasModel model{&registry};
    CommandRegistry commands;
    Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    ViewRegistry views;
    QQmlEngine engine;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QObject *root = nullptr;
    QQuickItem *host = nullptr;
    QQuickItem *graph = nullptr;

    QQuickItem *viewOf(int id) {
        QQuickItem *view = nullptr;
        QMetaObject::invokeMethod(root, "viewOf", Q_RETURN_ARG(QQuickItem *, view), Q_ARG(int, id));
        return view;
    }
    int madeItems() {
        int count = -1;
        QMetaObject::invokeMethod(root, "madeItems", Q_RETURN_ARG(int, count));
        return count;
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QFile view(dir.filePath("TestView.qml"));
        QVERIFY(view.open(QIODevice::WriteOnly));
        view.write("import QtQuick\nRectangle { objectName: \"testView\"; color: \"#c04040\" }\n");
        view.close();
        views.add({"test", "Test", "box", QUrl::fromLocalFile(view.fileName()), QSizeF(100, 50),
                   ViewRegistry::Multiplicity::Many});
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        exposeToQml(engine, {&translator, &theme, &commands, &model, &workflows});
        QQmlComponent component(&engine);
        component.setData(windowQml, QUrl("qrc:/test_area_node.qml"));
        QObject *made = component.createWithInitialProperties({{"registry", QVariant::fromValue(&views)}});
        QVERIFY2(made, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(made);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        root = window->findChild<QObject *>("root");
        host = window->findChild<QQuickItem *>("host");
        graph = window->findChild<QQuickItem *>("graph");
        QVERIFY(root && host && graph);
    }

    void cleanupTestCase() { delete window; }

    void aSplitTreePutsEachViewAtItsRectangle() {
        window->setProperty("tree", split());
        QCOMPARE(root->property("leaf").toBool(), false);
        QCOMPARE(viewOf(1), graph);
        QCOMPARE(geometry(graph), QRectF(0, 0, 480, 600));
        QQuickItem *test = viewOf(2);
        QVERIFY(test);
        QCOMPARE(test->objectName(), QString("testView"));
        QCOMPARE(test->parentItem(), host);
        QCOMPARE(geometry(test), QRectF(480, 0, 320, 300));
        QQuickItem *unknown = viewOf(3);
        QVERIFY(unknown);
        QCOMPARE(unknown->objectName(), QString("areaPlaceholder"));
        QCOMPARE(unknown->property("viewType").toString(), QString("storyboard"));
        QCOMPARE(geometry(unknown), QRectF(480, 300, 320, 300));
        // The areas are no items of their own: the host holds the views and nothing else.
        QCOMPARE(host->childItems().size(), 3);
        QCOMPARE(madeItems(), 2);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void aWindowResizeScalesEveryArea() {
        window->setProperty("tree", split());
        QQuickItem *test = viewOf(2);
        window->setWidth(1000);
        QTRY_COMPARE(geometry(graph), QRectF(0, 0, 600, 600));
        QCOMPARE(viewOf(2), test);
        QCOMPARE(geometry(test), QRectF(600, 0, 400, 300));
        window->setWidth(800);
        QTRY_COMPARE(geometry(graph), QRectF(0, 0, 480, 600));
    }

    void rightToLeftMirrorsTheRow() {
        window->setProperty("tree", split());
        window->setProperty("rtl", true);
        QCOMPARE(geometry(graph), QRectF(320, 0, 480, 600));
        QCOMPARE(geometry(viewOf(2)), QRectF(0, 0, 320, 300));
        QCOMPARE(geometry(viewOf(3)), QRectF(0, 300, 320, 300)); // a column is never mirrored
        window->setProperty("rtl", false);
        QCOMPARE(geometry(graph), QRectF(0, 0, 480, 600));
    }

    void newSizesMoveTheSameViews() {
        window->setProperty("tree", split());
        const QPointer<QQuickItem> test = viewOf(2), unknown = viewOf(3);
        window->setProperty("tree", split(0.25));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(test && unknown);
        QCOMPARE(viewOf(2), test.data());
        QCOMPARE(geometry(graph), QRectF(0, 0, 200, 600));
        QCOMPARE(geometry(test), QRectF(200, 0, 600, 300));
    }

    void anotherShapeMakesTheViewsAgainAndKeepsTheAdoptedOne() {
        window->setProperty("tree", split());
        const QPointer<QQuickItem> test = viewOf(2), unknown = viewOf(3);
        window->setProperty("tree", leaf(1, "graph"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!test && !unknown);
        QCOMPARE(root->property("leaf").toBool(), true);
        QCOMPARE(root->property("viewType").toString(), QString("graph"));
        QCOMPARE(viewOf(1), graph);
        QCOMPARE(geometry(graph), QRectF(0, 0, 800, 600));
        QCOMPARE(host->childItems().size(), 1);
        QCOMPARE(madeItems(), 0);
        // And back: new views, the same Graph.
        window->setProperty("tree", split());
        QVERIFY(viewOf(2) && viewOf(2) != test.data());
        QCOMPARE(viewOf(1), graph);
        QCOMPARE(host->childItems().size(), 3);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void aNodeThatIsNoTreeDrawsNothing() {
        window->setProperty("tree", split());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("AreaNode: not an area tree node"));
        window->setProperty("tree", QVariantMap{{"type", "blob"}});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(madeItems(), 0);
        QCOMPARE(host->childItems().size(), 1); // the adopted Graph stays where it is
        window->setProperty("tree", leaf(1, "graph"));
    }
};

QTEST_MAIN(TestAreaNode)
#include "test_area_node.moc"
