// KitDialogButtons (src/kit/KitDialogButtons.qml, docs/ui-ergonomics.md 2026-11-27): a dialog's one row of actions in a Question.
// The row puts its buttons in GNOME's order whatever the file's order (extras at the start, Cancel, the destructive answer, the acting
// answer at the end), Left and Right move the focus as the buttons are seen (right to left too), Enter presses the focused button,
// Enter with no button focused presses the default, and Escape presses Cancel.

#include <QGuiApplication>
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

using namespace fugue;

namespace {

const char *windowQml = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue
import "qrc:/kit"
import "qrc:/canvas"

Window {
    width: 560
    height: 320
    visible: true
    id: root
    property string answered: ""
    property bool withField: false
    Question {
        id: ask
        objectName: "ask"
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Label { text: "Close all tabs anyway?" }
            Item { objectName: "plain"; implicitHeight: 1; activeFocusOnTab: true } // takes the focus and no key
            ThemedTextField { objectName: "field"; visible: withField; Layout.fillWidth: true }
            KitDialogButtons {
                objectName: "answers"
                // In the file out of order on purpose: the row puts them in place.
                ThemedButton { objectName: "save"; text: "Save"; highlighted: true; onClicked: root.answered = "save" }
                ThemedButton { objectName: "discard"; dialogRole: "destructive"; danger: true; text: "Discard"; onClicked: root.answered = "discard" }
                ThemedButton { objectName: "cancel"; dialogRole: "cancel"; text: "Cancel"; onClicked: { root.answered = "cancel"; ask.close() } }
                ThemedButton { objectName: "extra"; dialogRole: "extra"; text: "Read more"; onClicked: root.answered = "extra" }
            }
        }
    }
}
)";

} // namespace

class TestKitDialogButtons : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    Translator translator{dir.filePath("language")};
    Theme theme{dir.path()};
    Registry registry;
    CanvasModel model{&registry};
    CommandRegistry commands;
    Workflows workflows{&model, &registry, &commands, {dir.filePath("recovery"), dir.filePath("trust.json")}};
    QQmlEngine engine;
    QQuickWindow *window = nullptr;
    QObject *ask = nullptr;
    QStringList warnings;

    QQuickItem *item(const char *name) { return window->findChild<QQuickItem *>(name); }
    static qreal x(QQuickItem *item) { return item->mapToScene(QPointF(0, 0)).x(); }
    QString focused() const { return window->activeFocusItem() ? window->activeFocusItem()->objectName() : QString(); }
    void open() {
        window->setProperty("answered", QString());
        QMetaObject::invokeMethod(ask, "open");
        QTRY_VERIFY(ask->property("opened").toBool());
        QTRY_VERIFY(item("save")->width() > 0);
    }
    void key(Qt::Key k) { QTest::keyClick(window, k); }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        exposeToQml(engine, {&translator, &theme, &commands, &model, &workflows});
        connect(&engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        QQmlComponent component(&engine);
        component.setData(windowQml, QUrl("qrc:/test_kit_dialog_buttons.qml"));
        window = qobject_cast<QQuickWindow *>(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        ask = window->findChild<QObject *>("ask");
        QVERIFY(ask);
    }
    void cleanupTestCase() {
        delete window;
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // GNOME's order at the bottom right, the destructive answer its own chunk; the arrows walk it as seen, Enter presses the focused.
    void theRowOrdersItsButtonsAndTheArrowsWalkThem() {
        open();
        QQuickItem *extra = item("extra"), *cancel = item("cancel"), *discard = item("discard"), *save = item("save");
        QVERIFY(x(extra) < x(cancel) && x(cancel) < x(discard) && x(discard) < x(save));
        QQuickItem *row = item("answers");
        const qreal gap = row->property("spacing").toReal();
        QVERIFY(x(discard) - (x(cancel) + cancel->width()) >= 2 * gap - 0.5); // twice the gap around the destructive answer
        QVERIFY(x(save) - (x(discard) + discard->width()) >= 2 * gap - 0.5);
        QCOMPARE(qRound(x(save) + save->width()), qRound(x(row) + row->width())); // the acting answer ends the row
        QVERIFY(x(cancel) - (x(extra) + extra->width()) > 2 * gap); // the extra stands apart

        save->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(focused(), QString("save"));
        key(Qt::Key_Left);
        QCOMPARE(focused(), QString("discard"));
        QVERIFY(discard->property("ring").toBool()); // the keyboard's focus ring
        key(Qt::Key_Left);
        key(Qt::Key_Left);
        key(Qt::Key_Left); // stops at the start
        QCOMPARE(focused(), QString("extra"));
        key(Qt::Key_Right);
        QCOMPARE(focused(), QString("cancel"));
        key(Qt::Key_Tab); // Tab follows the order on screen
        QCOMPARE(focused(), QString("discard"));
        key(Qt::Key_Return);
        QCOMPARE(window->property("answered").toString(), QString("discard"));
        key(Qt::Key_Escape);
        QCOMPARE(window->property("answered").toString(), QString("cancel"));
        QTRY_VERIFY(!ask->property("visible").toBool());
    }

    // Enter where no button has the focus presses the default (the primary); in a text field it is the field's; Escape is Cancel.
    void enterPressesTheDefaultAndEscapeCancels() {
        open();
        item("plain")->forceActiveFocus();
        QTRY_COMPARE(focused(), QString("plain"));
        key(Qt::Key_Return);
        QCOMPARE(window->property("answered").toString(), QString("save"));

        window->setProperty("withField", true);
        window->setProperty("answered", QString());
        item("field")->forceActiveFocus();
        QTRY_COMPARE(focused(), QString("field"));
        key(Qt::Key_Return);
        QCOMPARE(window->property("answered").toString(), QString());
        key(Qt::Key_Escape);
        QCOMPARE(window->property("answered").toString(), QString("cancel"));
        QTRY_VERIFY(!ask->property("visible").toBool());
        window->setProperty("withField", false);
    }

    // Right to left the row mirrors (the acting answer at the left end) and Left moves the focus to the button on its left.
    void rightToLeftMirrorsTheOrderAndTheArrows() {
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        open();
        QQuickItem *extra = item("extra"), *cancel = item("cancel"), *discard = item("discard"), *save = item("save");
        QTRY_VERIFY(x(save) < x(discard) && x(discard) < x(cancel) && x(cancel) < x(extra));
        cancel->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Left);
        QCOMPARE(focused(), QString("discard"));
        key(Qt::Key_Right);
        key(Qt::Key_Right);
        QCOMPARE(focused(), QString("extra"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }
};

QTEST_MAIN(TestKitDialogButtons)
#include "test_kit_dialog_buttons.moc"
