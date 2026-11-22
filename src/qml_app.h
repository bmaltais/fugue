#pragma once

// The window's C++ objects as its QML reads them: `import Fugue`, then App.theme,
// App.commands, App.canvasModel, ... Typed, so qmllint checks every use (tests/qmllint).
// main() and every test window wire them the same way, with exposeToQml() before
// engine.load(). A test window may leave settings, runs and imageActions out; QML reads them as null.

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_history.h" // a QML type (CanvasHistory): the module registers it from here
#include "canvas/canvas_model.h"
#include "canvas/crop_qml.h" // a QML singleton (CropGeometry): the module registers it from here
#include "canvas/mask_layer.h" // a QML item (MaskLayer): the module registers it from here
#include "canvas/paint_layer.h" // a QML item (PaintLayer): likewise
#include "canvas/float_place_qml.h" // a QML singleton (FloatPlace): the module registers it from here
#include "commands/command_registry.h"
#include "menu/color_history.h"
#include "menu/image_actions.h"
#include "media/media_library.h"
#include "menu/layout_colors.h"
#include "panels/chrome_visibility.h"
#include "panels/side_panels.h"
#include "run/canvas_runs.h"
#include "run/run_bar.h"
#include "run/run_queue.h"
#include "search/node_library.h"
#include "settings/project_model.h"
#include "settings/settings_model.h"
#include "theme.h"
#include "translator.h"
#include "workflow/recent_files.h"
#include "workflow/workflows.h"

class QQmlEngine;

namespace fugue {

struct AppObjects {
    Translator *translator = nullptr;
    Theme *theme = nullptr;
    CommandRegistry *commands = nullptr;
    CanvasModel *canvasModel = nullptr;
    Workflows *workflows = nullptr;
    SettingsModel *settings = nullptr; // optional
    CanvasRuns *runs = nullptr;        // optional
    ImageActions *imageActions = nullptr; // optional
    ColorHistory *colorHistory = nullptr; // optional
    LayoutColors *layoutColors = nullptr; // optional
    RunBar *runBar = nullptr;             // optional: the run toolbar and queue panel (#60)
    RunQueue *runQueue = nullptr;         // optional, with runBar
    SidePanels *sidePanels = nullptr;     // optional: the side panels (#56)
    NodeLibrary *nodeLibrary = nullptr;   // optional, with sidePanels
    RecentFiles *recentFiles = nullptr;   // optional, with sidePanels
    ChromeVisibility *chrome = nullptr;   // optional: which bars and panels are shown (#129)
    MediaLibrary *mediaLibrary = nullptr; // optional: the media browser's results and thumbnails (#133)
    ProjectModel *projects = nullptr;     // optional: the projects dialog and the project chip (spec 1.4.2)
};

class QmlApp : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON
    QML_UNCREATABLE("made by exposeToQml()")
    Q_PROPERTY(Translator *translator READ translator CONSTANT)
    Q_PROPERTY(Theme *theme READ theme CONSTANT)
    Q_PROPERTY(fugue::CommandRegistry *commands READ commands CONSTANT)
    Q_PROPERTY(CanvasModel *canvasModel READ canvasModel CONSTANT)
    Q_PROPERTY(fugue::Workflows *workflows READ workflows CONSTANT)
    Q_PROPERTY(fugue::SettingsModel *settings READ settings CONSTANT)
    Q_PROPERTY(fugue::CanvasRuns *runs READ runs CONSTANT)
    Q_PROPERTY(fugue::ImageActions *imageActions READ imageActions CONSTANT)
    Q_PROPERTY(fugue::ColorHistory *colorHistory READ colorHistory CONSTANT)
    Q_PROPERTY(fugue::LayoutColors *layoutColors READ layoutColors CONSTANT)
    Q_PROPERTY(fugue::RunBar *runBar READ runBar CONSTANT)
    Q_PROPERTY(fugue::RunQueue *runQueue READ runQueue CONSTANT)
    Q_PROPERTY(fugue::SidePanels *sidePanels READ sidePanels CONSTANT)
    Q_PROPERTY(fugue::NodeLibrary *nodeLibrary READ nodeLibrary CONSTANT)
    Q_PROPERTY(fugue::RecentFiles *recentFiles READ recentFiles CONSTANT)
    Q_PROPERTY(fugue::ChromeVisibility *chrome READ chrome CONSTANT)
    Q_PROPERTY(fugue::MediaLibrary *mediaLibrary READ mediaLibrary CONSTANT)
    Q_PROPERTY(fugue::ProjectModel *projects READ projects CONSTANT)

public:
    explicit QmlApp(const AppObjects &objects, QObject *parent = nullptr) : QObject(parent), m_objects(objects) {}

    Translator *translator() const { return m_objects.translator; }
    Theme *theme() const { return m_objects.theme; }
    CommandRegistry *commands() const { return m_objects.commands; }
    CanvasModel *canvasModel() const { return m_objects.canvasModel; }
    Workflows *workflows() const { return m_objects.workflows; }
    SettingsModel *settings() const { return m_objects.settings; }
    CanvasRuns *runs() const { return m_objects.runs; }
    ImageActions *imageActions() const { return m_objects.imageActions; }
    ColorHistory *colorHistory() const { return m_objects.colorHistory; }
    LayoutColors *layoutColors() const { return m_objects.layoutColors; }
    RunBar *runBar() const { return m_objects.runBar; }
    RunQueue *runQueue() const { return m_objects.runQueue; }
    SidePanels *sidePanels() const { return m_objects.sidePanels; }
    NodeLibrary *nodeLibrary() const { return m_objects.nodeLibrary; }
    RecentFiles *recentFiles() const { return m_objects.recentFiles; }
    ChromeVisibility *chrome() const { return m_objects.chrome; }
    MediaLibrary *mediaLibrary() const { return m_objects.mediaLibrary; }
    ProjectModel *projects() const { return m_objects.projects; }

private:
    AppObjects m_objects;
};

// Makes `objects` the App singleton of `engine`, which owns it from here. Call it once per
// engine, before engine.load(); the objects must outlive the engine's QML.
void exposeToQml(QQmlEngine &engine, const AppObjects &objects);

} // namespace fugue
