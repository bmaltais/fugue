#include "apps/app_window.h"

#include <memory>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "workflow/workflows.h"

namespace fugue {

void bindAppView(AppView &view, Workflows &workflows, CanvasModel &canvas, RunService &service, CommandRegistry &commands,
                 CanvasRuns &runs) {
    const auto shown = std::make_shared<QString>(); // the tab given to the view last, so a title change does not give it again
    const auto give = [&view, &workflows, &canvas, shown](bool force) {
        const QVariantList tabs = workflows.tabs();
        const int index = workflows.current();
        if (index < 0 || index >= tabs.size())
            return;
        const QVariantMap tab = tabs.at(index).toMap();
        const QString id = tab.value("id").toString();
        if (!force && id == *shown)
            return;
        *shown = id;
        view.show(id, canvas.document(), tab.value("path").toString());
    };
    QObject::connect(&workflows, &Workflows::tabsChanged, &view, [give] { give(false); });
    QObject::connect(&view, &AppView::documentWanted, &view, [give] { give(true); });
    QObject::connect(&workflows, &Workflows::tabClosed, &view, [&view, shown](const QString &id) {
        if (id == *shown)
            shown->clear();
        view.forget(id);
    });
    QObject::connect(&view, &AppView::notice, &workflows, &Workflows::notify);
    QObject::connect(&service, &RunService::event, &view, &AppView::onEvent);

    const auto run = [&view, &runs](bool first) {
        if (view.shown())
            view.run();
        else
            runs.start(first);
    };
    commands.setHandler(QStringLiteral("run.start"), [run] { run(false); });
    commands.setHandler(QStringLiteral("run.start-first"), [run] { run(true); });
    commands.setHandler(QStringLiteral("run.cancel"), [&view, &runs] {
        if (view.shown())
            view.cancel();
        else
            runs.cancel();
    });
    give(true);
}

} // namespace fugue
