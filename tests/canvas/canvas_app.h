#pragma once

// The app's window around a canvas model, as main() makes it, for tests that drive the real
// window (Main.qml, offscreen) with the mouse and keys.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include <functional>

#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/document.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "menu/menu_commands.h"
#include "media/media_library.h"
#include "node_type_reloads.h"
#include "panels/chrome_commands.h"
#include "panels/chrome_visibility.h"
#include "panels/side_panels.h"
#include "search/node_library.h"
#include "workflow/recent_files.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "search/node_search.h"
#include "settings/settings_model.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"
#include "../cpu_time.h"
#include "../window_grab.h"

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
// fallbacks) and Hyprland's rounding (negative: none, as without Hyprland). The canvas draws
// with that rounding too, set as the corner_radius setting (none: the setting's default).
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
    QDir(dir.path()).mkpath("config/fugue");
    QFile settings(dir.filePath("config/fugue/settings.json"));
    if (settings.open(QIODevice::WriteOnly))
        settings.write(QStringLiteral("{\"corner_radius\": %1}\n").arg(look.radius).toUtf8());
    settings.close();
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
    fugue::Registry registry;
    CanvasModel model{&registry};
    fugue::CommandRegistry commands;
    fugue::Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    // settings.json and providers.json in the test's own folder, never the owner's.
    // In a project root (FUGUE_HOME set, test_projects_window) the files are the project's own.
    fugue::SettingsModel settings{qEnvironmentVariableIsSet("FUGUE_HOME") ? QString() : dir.path() + "/config/fugue/settings.json",
                                  qEnvironmentVariableIsSet("FUGUE_HOME") ? QString() : dir.path() + "/config/fugue/providers.json"};
    // A test's own runs (test_canvas_run), set before open(); the window runs without them.
    fugue::CanvasRuns *runs = nullptr;
    fugue::RunBar *runBar = nullptr;     // the run toolbar's (test_canvas_run), likewise
    fugue::RunQueue *runQueue = nullptr;
    fugue::ImageActions *imageActions = nullptr; // likewise (test_canvas_image_actions)
    fugue::ProjectModel *projects = nullptr;     // the projects dialog's and the chip's (test_projects_window), likewise
    // The side panels (#56), with their state in the test's own folder.
    fugue::ChromeVisibility chrome{dir.path() + "/chrome.json"};
    fugue::SidePanels sidePanels{dir.path() + "/side-panels.json", &chrome};
    fugue::NodeLibrary nodeLibrary{&registry};
    fugue::RecentFiles recentFiles{dir.path() + "/recent-files.json"};
    // The media browser's results (#133): the run folders and thumbnails are the test's own.
    fugue::MediaLibrary mediaLibrary{dir.path() + "/runs", dir.path() + "/thumbs"};
    // The color picker's recent colors (#52), in the test's own folder, and layout coloring.
    fugue::ColorHistory colorHistory{dir.path() + "/recent-colors.json"};
    fugue::LayoutColors layoutColors{model};
    QQmlApplicationEngine engine;
    QStringList warnings;
    QList<QUrl> openedUrls; // what help.docs and help.report-issue opened
    QQuickWindow *window = nullptr;
    QQuickItem *canvas = nullptr;
    QQuickItem *world = nullptr;
    // Fills the registry again as main()'s does (#70): the built-ins, the test's types and the
    // packs of the test's own XDG data folder, which it watches. Wired to the window in open().
    std::unique_ptr<fugue::RegistryReloader> reloader;

    // `addTypes` adds node types of the test's own before the document opens, and again at each
    // reload; `look` is the theme and corner radius to draw with.
    explicit App(const fugue::Document &document, const std::function<void(fugue::Registry &)> &addTypes = {},
                 const Look &look = {})
        : hyprctl(prepareLook(dir, look)), theme(dir.path(), nullptr, hyprctl) {
        const auto fill = [addTypes](fugue::Registry &into) {
            fugue::nodes::addBuiltinNodeTypes(into);
            if (addTypes)
                addTypes(into);
            fugue::loadInstalledPacks(into); // the test's own (XDG) packs folder, mostly empty
        };
        fill(registry);
        reloader = std::make_unique<fugue::RegistryReloader>(&registry, fill);
        fugue::registerDefaultCommands(commands);
        fugue::registerMenuCommands(commands);
        fugue::registerFileCommands(commands, workflows);
        model.search()->registerCommands(commands);
        registerCanvasCommands(commands, model);
        fugue::bindMenuCommands(commands, model, [this](const QUrl &url) { openedUrls << url; });
        nodeLibrary.refresh(); // the registry is filled now
        sidePanels.registerCommands(commands);
        fugue::registerChromeCommands(commands, chrome);
        recentFiles.follow(&workflows);
        settings.attach(&translator, &model, &registry, &theme); // as main.cpp does
        settings.registerCommands(commands, [this](const QUrl &url) { openedUrls << url; });
        // Opened as a file is: one tab, nothing unsaved.
        workflows.openText(QString::fromUtf8(document.save()));
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
    }

    ~App() { QCoreApplication::removeTranslator(&translator); }

    bool open() {
        // As main() does: the English catalog gives counted strings their plural forms.
        QCoreApplication::installTranslator(&translator);
        const fugue::AppObjects objects{&translator, &theme, &commands, &model, &workflows, &settings, runs, imageActions, &colorHistory, &layoutColors, runBar, runQueue, &sidePanels, &nodeLibrary, &recentFiles, &chrome, &mediaLibrary, projects};
        fugue::followNodeTypeReloads(*reloader, objects);
        fugue::exposeToQml(engine, objects);
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
