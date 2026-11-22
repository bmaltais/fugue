// fugue — one line on what it does.

#include <QCommandLineParser>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "search/node_search.h"
#include "commands/command_registry.h"
#include "cli/args.h"
#include "cli/cli.h"
#include "menu/color_history.h"
#include "menu/image_actions.h"
#include "menu/layout_colors.h"
#include "menu/menu_commands.h"
#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/trust_store.h"
#include "node_type_reloads.h"
#include "engine/fugue_home.h"
#include "engine/project.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "run/canvas_runs.h"
#include "panels/chrome_commands.h"
#include "panels/chrome_visibility.h"
#include "panels/side_panels.h"
#include "run/run_bar.h"
#include "search/node_library.h"
#include "workflow/recent_files.h"
#include "run/run_queue.h"
#include "run/run_service.h"
#include "engine/settings.h"
#include "settings/settings_model.h"
#include "theme.h"
#include "workflow/workflows.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // FUGUE_HOME or `--home <dir>`: one root for everything Fugue writes (spec 1.4.1). Before Qt and
    // every other call, so each QStandardPaths lookup and each child process sees the redirected folders.
    if (QString homeError; !fugue::applyFugueHome(argc, argv, &homeError)) {
        std::fprintf(stderr, "fugue: %s\n", qPrintable(homeError));
        return 2;
    }
    // `fugue <command>` is the command line (spec 13): no QGuiApplication, no
    // display, no QML. It must be decided before any application object exists.
    if (fugue::cli::wantsCli(argc, argv))
        return fugue::cli::run(argc, argv,
                               {[](fugue::Registry &registry) {
                                    fugue::nodes::addBuiltinNodeTypes(registry);
                                    fugue::loadInstalledPacks(registry);
                                },
                                fugue::nodes::addBuiltinKernels});

    // The window only: with nothing selected it opens the project it last ran in (never the command line, so
    // scripts get the same session every time), and remembers where it is running.
    if (QString lastError; !fugue::applyLastUsedProject(&lastError))
        std::fprintf(stderr, "fugue: %s\n", qPrintable(lastError));
    fugue::recordLastUsedProject();

    QGuiApplication app(argc, argv);
    app.setApplicationName("fugue");
    app.setApplicationVersion("0.1.0");
    // Qt Multimedia's FFmpeg backend prints ffmpeg's whole stream dump to stderr for every file a
    // player opens (the player on a video or song node, #116), which drowns the real messages.
    // Started from a terminal or a launcher that already holds an application ID, Qt's try to
    // register the app with the desktop portal fails with "Connection already associated with an
    // application ID" on every start; nothing is lost (file dialogs and the rest work), so that
    // category stays quiet too. QT_LOGGING_RULES still beats this.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg.mediadataholder.info=false\n"
                                                    "qt.qpa.services.warning=false"));

    // Matches the window to fugue.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The window's own icon follows the
    // Omarchy theme (set below, once the theme is read): the dark art on a dark theme.
    app.setDesktopFileName("fugue");

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    const auto setIcon = [&theme, &app] {
        const bool light = theme.light();
        app.setWindowIcon(QIcon::fromTheme(light ? "fugue" : "fugue-dark",
                                           QIcon(light ? ":/fugue-light.png" : ":/fugue-dark.png")));
    };
    setIcon();
    QObject::connect(&theme, &Theme::changed, &app, setIcon);
    // The command registry: the default keymap, then the user's changes to it (spec 12.5).
    fugue::CommandRegistry commands;
    fugue::registerDefaultCommands(commands);
    fugue::registerMenuCommands(commands);
    commands.loadKeymap(fugue::effectiveKeymapPath());

    // The canvas reads node types from the same registry the CLI fills (the built-ins, then the
    // packs) and starts on an empty workflow. The run thread fills its own the same way.
    const auto addNodeTypes = [](fugue::Registry &registry) {
        fugue::nodes::addBuiltinNodeTypes(registry);
        fugue::loadInstalledPacks(registry);
    };
    fugue::Registry registry;
    addNodeTypes(registry);
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
    // The menus' commands that need the canvas, and when each can run (#50).
    fugue::bindMenuCommands(commands, canvasModel);
    // settings.json and providers.json (#51): the language, what new nodes start with and the
    // corner radius apply live from here; app.settings opens the panel.
    fugue::SettingsModel appSettings;
    appSettings.attach(&translator, &canvasModel, &registry, &theme);
    appSettings.registerCommands(commands, [](const QUrl &url) { QDesktopServices::openUrl(url); });
    // The cache is kept within its limits at start and while idle, never during a run (10.6, #141).
    appSettings.cache()->startAutomatic();
    // The window runs its own runs (spec 9.8) on a thread of their own, queued with the CLI's.
    fugue::RunSetup runSetup;
    runSetup.addNodeTypes = addNodeTypes;
    runSetup.addKernels = fugue::nodes::addBuiltinKernels;
    fugue::RunService runService(runSetup);
    fugue::CanvasRuns runs(&canvasModel, &workflows, &runService, &registry);
    runs.registerCommands(commands);
    // The run toolbar's state and the queue panel's list (#60): the same queue the CLI's runs are in.
    // Which bars and panels are shown (#129): each registers with this, and the Bars and panels menu, focus
    // mode and the file that remembers it are its.
    fugue::ChromeVisibility chrome(fugue::stateDir() + "/chrome.json");
    fugue::RunBar runBar(&runs, fugue::stateDir() + "/run-bar.json", &chrome);
    runBar.registerCommands(commands);
    fugue::RunQueue runQueue({}, &runService);
    // Open, Copy and Save Image on a node's menu (#57); failures go to the window banner.
    fugue::ImageActions imageActions(commands, canvasModel, runs);
    QObject::connect(&imageActions, &fugue::ImageActions::message, &workflows, &fugue::Workflows::notify);

    // The color picker's recent colors (#52), kept in the state folder, and node and Frame coloring with it.
    fugue::ColorHistory colorHistory(fugue::TrustStore::defaultPath().section('/', 0, -2) + "/recent-colors.json");
    fugue::LayoutColors layoutColors(canvasModel);

    // The side panels (#56): which is open and how wide, the node library's rows, the files lately open.
    fugue::SidePanels sidePanels(fugue::stateDir() + "/side-panels.json", &chrome);
    sidePanels.registerCommands(commands);
    fugue::registerChromeCommands(commands, chrome);
    fugue::NodeLibrary nodeLibrary(&registry);
    fugue::RecentFiles recentFiles(fugue::stateDir() + "/recent-files.json");
    recentFiles.follow(&workflows);
    fugue::MediaLibrary mediaLibrary;

    // Projects (spec 1.4.2): the dialog's model; the window title and the chip follow it, and a switch starts
    // another process and closes this window through the quit command (which asks about unsaved work).
    fugue::ProjectModel projects;
    projects.setWindow([&commands] { return commands.unsavedWork(); }, [&commands] { commands.requestQuit(); });
    projects.registerCommands(commands);
    projects.bindTitle(workflows);

    // The window's areas (#217, spec 12.16): the layout kept in the root's state folder, read before the window is
    // made so it opens as it was left; a layout that cannot be read is said on the notice line.
    fugue::Workspace workspace(fugue::stateDir() + "/workspaces.json");
    QObject::connect(&workspace, &fugue::Workspace::message, &workflows, &fugue::Workflows::notify);
    workspace.registerCommands(commands);
    workspace.restore();

    const fugue::AppObjects objects{&translator, &theme,   &commands, &canvasModel,  &workflows,
                                    &appSettings, &runs, &imageActions, &colorHistory, &layoutColors,
                                    &runBar,      &runQueue,    &sidePanels,   &nodeLibrary,  &recentFiles,  &chrome,
                                    &mediaLibrary, &projects, &workspace};
    // The registry is filled again when the packs or the trust change, so what is installed is
    // usable with no restart (#70). Made after what it refreshes, so it goes first.
    fugue::RegistryReloader reloader(&registry, addNodeTypes);
    reloader.setTrustFile(fugue::TrustStore::defaultPath());
    fugue::followNodeTypeReloads(reloader, objects);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    fugue::exposeToQml(engine, objects);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // `fugue path.fugue` opens that file (an image with a workflow in it works too). Qt has
    // already taken its own options (-platform, ...) off the arguments.
    QCommandLineParser parser;
    parser.addPositionalArgument("file", "A workflow to open.", "[file...]");
    parser.parse(app.arguments());
    // A file named opens just that file and leaves the kept session alone; with none, the tabs the
    // last session had open come back (settings.json's restore_tabs, live), and are kept from here on.
    workflows.setSessionEnabled(appSettings.restoreTabs());
    QObject::connect(&appSettings, &fugue::SettingsModel::changed, &workflows,
                     [&workflows, &appSettings] { workflows.setSessionEnabled(appSettings.restoreTabs()); });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &workflows, &fugue::Workflows::flushSession);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &workspace, &fugue::Workspace::flush);
    if (parser.positionalArguments().isEmpty())
        workflows.restoreSession();
    else
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
