#pragma once

// The app's window around a canvas model, as main() makes it, for tests that drive the real
// window (Main.qml, offscreen) with the mouse and keys.

#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include <functional>

#include "backend.h"
#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "search/node_search.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"

namespace canvas_test {

inline QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

inline fugue::Document load(const QByteArray &json) {
    const fugue::Result<fugue::Document> loaded = fugue::Document::load(json);
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

struct App {
    QTemporaryDir dir;
    Translator translator{dir.path() + "/language"};
    Theme theme{dir.path()};
    Backend backend;
    fugue::Registry registry;
    CanvasModel model{&registry};
    fugue::CommandRegistry commands;
    fugue::Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QQuickItem *canvas = nullptr;
    QQuickItem *world = nullptr;

    // `addTypes` adds node types of the test's own before the document opens.
    explicit App(const fugue::Document &document, const std::function<void(fugue::Registry &)> &addTypes = {}) {
        fugue::nodes::addBuiltinNodeTypes(registry);
        if (addTypes)
            addTypes(registry);
        fugue::registerDefaultCommands(commands);
        fugue::registerFileCommands(commands, workflows);
        model.search()->registerCommands(commands);
        registerCanvasCommands(commands, model);
        // Opened as a file is: one tab, nothing unsaved.
        workflows.openText(QString::fromUtf8(document.save()));
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("canvasModel", &model);
        engine.rootContext()->setContextProperty("commands", &commands);
        engine.rootContext()->setContextProperty("workflows", &workflows);
    }

    bool open() {
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window || !QTest::qWaitForWindowExposed(window))
            return false;
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window))
            return false;
        canvas = item(window->contentItem(), "canvas");
        world = item(window->contentItem(), "world");
        return canvas && world;
    }

    qreal zoom() const { return world->scale(); }
    QPointF pan() const { return world->position(); }
    // A world point in window coordinates.
    QPoint screen(qreal x, qreal y) const {
        return canvas->mapToScene(QPointF(world->x() + x * zoom(), world->y() + y * zoom())).toPoint();
    }

    void drag(Qt::MouseButton button, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mousePress(window, button, modifiers, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(window, from + (to - from) * i / 8);
        QTest::mouseRelease(window, button, modifiers, to);
    }

    void key(int key, Qt::KeyboardModifiers modifiers = {}) {
        QTest::keyClick(window, Qt::Key(key), modifiers);
    }
};

} // namespace canvas_test
