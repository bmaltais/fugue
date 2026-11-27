// The Question dialogs in the real window (#235): each answer is the window's one button (ViewButton, D4), Cancel or the safe
// answer first and the primary last at the right (D9), a destructive answer in the danger tint (D8), and Enter and Escape keep
// working. With FUGUE_SHOT_DIR set a test saves the quit question and others, dark and light, beside Settings, as pictures.

#include <QJsonObject>
#include <QQuickStyle>
#include <QtTest>

#include "canvas_app.h"
#include "../window_grab.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::Look;
using canvas_test::load;

namespace {

const char *emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

QQuickItem *visibleItem(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visibleItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *find(QQuickWindow *window, const QString &name) {
    QQuickItem *root = window->contentItem();
    while (root->parentItem())
        root = root->parentItem();
    return visibleItem(root, name);
}

QObject *popup(App &app, const char *name) {
    QObject *found = app.window->findChild<QObject *>(name);
    if (!found)
        qFatal("no popup %s", name);
    return found;
}

bool opened(App &app, const char *name) { return popup(app, name)->property("opened").toBool(); }

QRectF sceneRect(QQuickItem *item) { return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height())); }

// The question's answer buttons by object name, left to right.
QList<QQuickItem *> row(App &app, const QStringList &names) {
    QList<QQuickItem *> items;
    for (const QString &name : names) {
        QQuickItem *item = find(app.window, name);
        if (!item)
            qFatal("no visible %s", qPrintable(name));
        items << item;
    }
    std::sort(items.begin(), items.end(), [](QQuickItem *a, QQuickItem *b) { return sceneRect(a).x() < sceneRect(b).x(); });
    return items;
}

} // namespace

class TestQuestionDialogs : public QObject {
    Q_OBJECT

    void shot(App &app, const QString &name) {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        QTest::qWait(300);
        QVERIFY(test_support::grab(app.window).save(QString::fromLocal8Bit(dir) + "/" + name + ".png"));
    }

    void shotsFor(const QString &theme, const QString &tag) {
        App app(load(emptyDoc), {}, Look{theme, 8});
        QVERIFY(app.open());
        app.window->resize(900, 560);
        QObject *quit = popup(app, "quitAsk");
        QMetaObject::invokeMethod(quit, "open");
        QTRY_VERIFY(opened(app, "quitAsk"));
        shot(app, "question-quit-" + tag);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!quit->property("visible").toBool());

        QObject *close = popup(app, "closeAsk");
        close->setProperty("title", "fox-pass.fugue");
        QMetaObject::invokeMethod(close, "open");
        QTRY_VERIFY(opened(app, "closeAsk"));
        shot(app, "question-close-tab-" + tag);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!close->property("visible").toBool());

        if (QObject *group = app.window->findChild<QObject *>("deleteGroupQuestion")) {
            QMetaObject::invokeMethod(group, "ask", Q_ARG(QVariant, "local/fox"), Q_ARG(QVariant, "Fox pass"));
            QTRY_VERIFY(group->property("opened").toBool());
            shot(app, "question-delete-group-" + tag);
            app.key(Qt::Key_Escape);
            QTRY_VERIFY(!group->property("visible").toBool());
        }

        app.commands.trigger("app.settings");
        QTRY_VERIFY(opened(app, "settingsPanel"));
        shot(app, "question-reference-settings-" + tag);
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Quit: Keep working (the safe answer, highlighted) then Quit (danger tint, last); Escape keeps working, Enter on the focused
    // safe answer too, a click on Quit quits.
    void theQuitQuestionBuildsItsAnswersFromTheSharedButton() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QObject *ask = popup(app, "quitAsk");
        QMetaObject::invokeMethod(ask, "open");
        QTRY_VERIFY(opened(app, "quitAsk"));
        const QList<QQuickItem *> answers = row(app, {"quitKeep", "quitAnyway"});
        QCOMPARE(answers[0]->objectName(), QString("quitKeep"));
        QVERIFY(answers[0]->property("primary").toBool());   // the safe answer is the highlighted one
        QVERIFY(!answers[1]->property("primary").toBool());
        QVERIFY(answers[1]->property("danger").toBool());
        for (QQuickItem *answer : answers) {
            QVERIFY(answer->metaObject()->indexOfProperty("bordered") >= 0); // a ViewButton, not a Material Button
            QVERIFY(sceneRect(answer).height() >= 24);
        }
        QVERIFY(sceneRect(answers[0]).right() < sceneRect(answers[1]).left());
        QVERIFY(answers[0]->hasActiveFocus()); // Enter keeps working, never quits (D8)
        app.key(Qt::Key_Return);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QMetaObject::invokeMethod(ask, "open");
        QTRY_VERIFY(opened(app, "quitAsk"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Close a tab with unsaved changes, GNOME's order (KitDialogButtons): Cancel, Discard (danger tint, its own chunk), Save (the
    // primary, last at the right).
    void theCloseTabQuestionTintsDiscardAndEndsOnSave() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        QObject *ask = popup(app, "closeAsk");
        ask->setProperty("title", "fox-pass.fugue");
        QMetaObject::invokeMethod(ask, "open");
        QTRY_VERIFY(opened(app, "closeAsk"));
        const QList<QQuickItem *> answers = row(app, {"closeDiscard", "closeCancel", "closeSave"});
        QCOMPARE(answers[0]->objectName(), QString("closeCancel"));
        QCOMPARE(answers[1]->objectName(), QString("closeDiscard"));
        QCOMPARE(answers[2]->objectName(), QString("closeSave"));
        QVERIFY(answers[1]->property("danger").toBool());
        QVERIFY(!answers[0]->property("primary").toBool() && !answers[0]->property("danger").toBool());
        QVERIFY(answers[2]->property("primary").toBool());
        const qreal gap = answers[0]->parentItem()->property("spacing").toReal(); // the row's gap
        QVERIFY(sceneRect(answers[1]).left() - sceneRect(answers[0]).right() >= 2 * gap - 0.5);
        QVERIFY(sceneRect(answers[2]).left() - sceneRect(answers[1]).right() >= 2 * gap - 0.5);
        QVERIFY(answers[2]->hasActiveFocus()); // Enter takes the primary (D5)
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_question_dialogs savesThePictures (nothing without the variable).
    void savesThePictures() {
        if (qgetenv("FUGUE_SHOT_DIR").isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the question pictures");
        shotsFor("catppuccin", "dark");
        shotsFor("catppuccin-latte", "light");
    }
};

QTEST_MAIN(TestQuestionDialogs)
#include "test_question_dialogs.moc"
