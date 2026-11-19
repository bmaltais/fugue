#pragma once

// The window's C++ objects as its QML reads them: `import Fugue`, then App.theme,
// App.commands, App.canvasModel, ... Typed, so qmllint checks every use (tests/qmllint).
// main() and every test window wire them the same way, with exposeToQml() before
// engine.load(). A test window may leave settings and runs out; QML reads them as null.

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "run/canvas_runs.h"
#include "settings/settings_model.h"
#include "theme.h"
#include "translator.h"
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

public:
    explicit QmlApp(const AppObjects &objects, QObject *parent = nullptr) : QObject(parent), m_objects(objects) {}

    Translator *translator() const { return m_objects.translator; }
    Theme *theme() const { return m_objects.theme; }
    CommandRegistry *commands() const { return m_objects.commands; }
    CanvasModel *canvasModel() const { return m_objects.canvasModel; }
    Workflows *workflows() const { return m_objects.workflows; }
    SettingsModel *settings() const { return m_objects.settings; }
    CanvasRuns *runs() const { return m_objects.runs; }

private:
    AppObjects m_objects;
};

// Makes `objects` the App singleton of `engine`, which owns it from here. Call it once per
// engine, before engine.load(); the objects must outlive the engine's QML.
void exposeToQml(QQmlEngine &engine, const AppObjects &objects);

} // namespace fugue
