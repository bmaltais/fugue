// fugue — one line on what it does.

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "search/node_search.h"
#include "commands/command_registry.h"
#include "cli/args.h"
#include "cli/cli.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "theme.h"
#include "workflow/workflows.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // `fugue <command>` is the command line (spec 13): no QGuiApplication, no
    // display, no QML. It must be decided before any application object exists.
    if (fugue::cli::wantsCli(argc, argv))
        return fugue::cli::run(argc, argv, {fugue::nodes::addBuiltinNodeTypes, fugue::nodes::addBuiltinKernels});

    QGuiApplication app(argc, argv);
    app.setApplicationName("fugue");
    app.setApplicationVersion("0.1.0");

    // Matches the window to fugue.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("fugue");
    app.setWindowIcon(QIcon::fromTheme("fugue", QIcon(":/fugue.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    // The command registry: the default keymap, then the user's changes to it (spec 12.5).
    fugue::CommandRegistry commands;
    fugue::registerDefaultCommands(commands);
    commands.loadKeymap(fugue::CommandRegistry::keymapPath());

    // The canvas reads node types from the same registry the CLI fills (built-ins now,
    // packs from M3) and starts on an empty workflow.
    fugue::Registry registry;
    fugue::nodes::addBuiltinNodeTypes(registry);
    CanvasModel canvasModel(&registry);
    // The open workflows: tabs, open, save, recovery, import (spec 11). It starts with one
    // empty workflow and has the previous session's recovery copies ready to offer.
    fugue::Workflows workflows(&canvasModel, &registry, &commands);
    fugue::registerFileCommands(commands, workflows);
    canvasModel.search()->registerCommands(commands);
    // The canvas is always there for now; Tab and Shift+A reach node.search through it.
    commands.setContextActive("canvas", true);
    // The canvas's edit, node and view commands; each edit is one step on its undo stack.
    registerCanvasCommands(commands, canvasModel);
    // The window runs its own runs (spec 9.8) on a thread of their own, queued with the CLI's.
    fugue::RunSetup runSetup;
    runSetup.addNodeTypes = fugue::nodes::addBuiltinNodeTypes;
    runSetup.addKernels = fugue::nodes::addBuiltinKernels;
    fugue::RunService runService(runSetup);
    fugue::CanvasRuns runs(&canvasModel, &workflows, &runService);
    runs.registerCommands(commands);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("canvasModel", &canvasModel);
    engine.rootContext()->setContextProperty("commands", &commands);
    engine.rootContext()->setContextProperty("workflows", &workflows);
    engine.rootContext()->setContextProperty("runs", &runs);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // `fugue path.fugue` opens that file (an image with a workflow in it works too). Qt has
    // already taken its own options (-platform, ...) off the arguments.
    QCommandLineParser parser;
    parser.addPositionalArgument("file", "A workflow to open.", "[file...]");
    parser.parse(app.arguments());
    workflows.openStartup(parser.positionalArguments());

    // `omarchy agent look app` runs the app offscreen with these set, to see
    // the window without a display: size it to OMARCHY_LOOK_SIZE when given,
    // press the keys in OMARCHY_LOOK_KEYS through the window's Shortcuts,
    // save it once it has settled, and quit.
    if (const QString shot = qEnvironmentVariable("OMARCHY_LOOK_SCREENSHOT"); !shot.isEmpty()) {
        const QStringList size = qEnvironmentVariable("OMARCHY_LOOK_SIZE").split('x');
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()); window && size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        const QStringList keys = qEnvironmentVariable("OMARCHY_LOOK_KEYS").split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < keys.size(); ++i) {
            QTimer::singleShot(800 + i * 300, &app, [&engine, key = QKeySequence(keys[i])] {
                for (QObject *shortcut : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                    if (qstrcmp(shortcut->metaObject()->className(), "QQuickShortcut") == 0
                        && shortcut->property("enabled").toBool()
                        && QKeySequence(shortcut->property("sequence").toString()) == key) {
                        QMetaObject::invokeMethod(shortcut, "activated");
                        break;
                    }
                }
            });
        }
        const int delay = qEnvironmentVariableIsSet("OMARCHY_LOOK_DELAY_MS") ? qEnvironmentVariableIntValue("OMARCHY_LOOK_DELAY_MS") : 1500;
        QTimer::singleShot(800 + keys.size() * 300 + delay, &app, [&engine, shot] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
                window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
