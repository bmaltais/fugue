#pragma once

// The App view in the window (#385): what ties AppView to the open workflows, the runs and the commands, so main.cpp has one
// line for it and a test can make the same ties.
//
//  - The shown tab's workflow (Workflows, CanvasModel) is given to the view when the shown tab changes (Workflows::tabsChanged,
//    which comes after the canvas has the tab's document) and when the person comes back from the graph
//    (AppView::documentWanted); a tab that closes is forgotten.
//  - The window's events of every run reach the view (RunService::event); it takes the ones of its own runs.
//  - Run (Ctrl+Enter, also ahead of the queue) and Cancel (Ctrl+Alt+Enter) act on the app while the App view is shown and on
//    the graph otherwise: the graph is not on screen then, and running it as it stands would skip the form.

#include "apps/app_view.h"

class CanvasModel;

namespace fugue {

class CanvasRuns;
class CommandRegistry;
class RunService;
class Workflows;

// Call after CanvasRuns::registerCommands (the handlers of run.* are replaced). Everything must outlive the window.
void bindAppView(AppView &view, Workflows &workflows, CanvasModel &canvas, RunService &service, CommandRegistry &commands,
                 CanvasRuns &runs);

} // namespace fugue
