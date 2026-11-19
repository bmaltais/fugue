#pragma once

// The app's window around a canvas model, as main() makes it, for tests that drive the real
// window (Main.qml, offscreen) with the mouse and keys.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
#include "menu/menu_commands.h"
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

// An Omarchy theme for the window: a shipped theme's palette by name (empty: Theme's
// fallbacks) and Hyprland's rounding (negative: none, as without Hyprland).
struct Look {
    QString theme;
    int radius = -1;
};

// Writes the theme's folder into `dir`; the fake hyprctl that reports the rounding, or empty.
inline QString prepareLook(const QTemporaryDir &dir, const Look &look) {
    if (!look.theme.isEmpty()) {
        const QString from = QStringLiteral("/usr/share/omarchy/themes/") + look.theme;
        QDir(dir.path()).mkpath("theme");
        QFile::copy(from + "/colors.toml", dir.filePath("theme/colors.toml"));
        if (QFile::exists(from + "/light.mode"))
            QFile::copy(from + "/light.mode", dir.filePath("theme/light.mode"));
    }
    if (look.radius < 0)
        return {};
    QFile script(dir.filePath("hyprctl"));
    if (!script.open(QIODevice::WriteOnly))
        return {};
    script.write(QStringLiteral("#!/bin/sh\necho '{\"int\": %1}'\n").arg(look.radius).toUtf8());
    script.close();
    script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return script.fileName();
}

struct App {
    QTemporaryDir dir;
    QString hyprctl;
    Translator translator{dir.path() + "/language"};
    Theme theme;
    Backend backend;
    fugue::Registry registry;
    CanvasModel model{&registry};
    fugue::CommandRegistry commands;
    fugue::Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    QQmlApplicationEngine engine;
    QStringList warnings;
    QList<QUrl> openedUrls; // what help.docs and help.report-issue opened
    QQuickWindow *window = nullptr;
    QQuickItem *canvas = nullptr;
    QQuickItem *world = nullptr;

    // `addTypes` adds node types of the test's own before the document opens; `look` is the
    // theme and corner radius to draw with.
    explicit App(const fugue::Document &document, const std::function<void(fugue::Registry &)> &addTypes = {},
                 const Look &look = {})
        : hyprctl(prepareLook(dir, look)), theme(dir.path(), nullptr, hyprctl) {
        fugue::nodes::addBuiltinNodeTypes(registry);
        if (addTypes)
            addTypes(registry);
        fugue::registerDefaultCommands(commands);
        fugue::registerMenuCommands(commands);
        fugue::registerFileCommands(commands, workflows);
        model.search()->registerCommands(commands);
        registerCanvasCommands(commands, model);
        fugue::bindMenuCommands(commands, model, [this](const QUrl &url) { openedUrls << url; });
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
    // A point of the canvas (its own coordinates) in window coordinates: the menu bar is above it.
    QPoint at(qreal x, qreal y) const { return canvas->mapToScene(QPointF(x, y)).toPoint(); }
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
