#include "apps/app_window.h"

#include <memory>

#include "apps/app_launcher.h"
#include "apps/app_shelf.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "workflow/readiness_sheet.h"
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
        // A template's folder is set a moment after its tab is made: that is a change too (the app's takes are found by it).
        const QString token = id + QLatin1Char('|') + tab.value("template").toString();
        if (!force && token == *shown)
            return;
        *shown = token;
        view.show(id, canvas.document(), tab.value("path").toString(), tab.value("template").toString());
    };
    QObject::connect(&workflows, &Workflows::tabsChanged, &view, [give] { give(false); });
    QObject::connect(&view, &AppView::documentWanted, &view, [give] { give(true); });
    QObject::connect(&workflows, &Workflows::tabClosed, &view, [&view, shown](const QString &id) {
        if (shown->startsWith(id + QLatin1Char('|')))
            shown->clear();
        view.forget(id);
    });
    QObject::connect(&view, &AppView::notice, &workflows, &Workflows::notify);
    QObject::connect(&service, &RunService::event, &view, &AppView::onEvent);
    view.setHistory(service.runsRoot(), [&service](const QString &run) { service.replay(run); });
    view.setDownloads(workflows.downloads()); // a model field's Download shares the window's one download line

    // The Needs panel (#469): the shown app's missing models and packs, from the Get ready sheet's own plan, looked at only while the
    // shown workflow is an app that lacks something; its actions go through the paths the window already has.
    NeedsModel *needs = view.needs();
    needs->setPlanSource([&view, &workflows] {
        return view.isApp() && workflows.missingSomething() ? workflows.readiness()->currentPlan() : ReadinessPlan();
    });
    needs->setLocator([&canvas](const QVariantMap &item) { return NeedsModel::nodeFor(canvas.document(), item); });
    QObject::connect(&workflows, &Workflows::noticeChanged, needs, &NeedsModel::refreshSoon);
    QObject::connect(needs, &NeedsModel::getReadyRequested, &workflows, [&workflows](bool startNow) {
        ReadinessSheet *sheet = workflows.readiness();
        sheet->show();
        if (startNow)
            sheet->start(); // only models: the click is the yes
    });
    QObject::connect(needs, &NeedsModel::downloadRequested, &workflows,
                     [&workflows](const QJsonObject &model) { workflows.downloadModels(QJsonArray{model}); });
    QObject::connect(needs, &NeedsModel::showNodeRequested, &view, [&view, &canvas](const QString &node) {
        view.openInGraph();
        canvas.select({node});
        const QRectF at = canvas.nodeRect(node);
        if (!at.isEmpty())
            canvas.moveView(at.center().x(), at.center().y(), canvas.view().value(QStringLiteral("zoom"), 1.0).toDouble());
    });

    commands.setHandler(QStringLiteral("view.toggle-app"), [&view] { view.toggle(); });
    commands.setEnabledWhen(QStringLiteral("view.toggle-app"), [&view] { return view.isApp(); });
    commands.setCheckedWhen(QStringLiteral("view.toggle-app"), [&view] { return view.shown(); });

    const auto run = [&view, &runs](bool first) {
        if (view.shown())
            view.run();
        else
            runs.start(first);
    };
    commands.setHandler(QStringLiteral("run.start"), [run] { run(false); });
    commands.setHandler(QStringLiteral("run.start-first"), [run] { run(true); });
    // Ctrl+V and Edit > Paste: a picture on the clipboard goes into the app's picture input while the App view is shown; any
    // other paste (and the graph's) is the canvas's. A focused text field keeps Ctrl+V for text (the registry's own rule).
    commands.setHandler(QStringLiteral("edit.paste"), [&view, graph = commands.handlerOf(QStringLiteral("edit.paste"))] {
        if (view.shown())
            view.pasteToApp();
        else if (graph)
            graph();
    });
    commands.setHandler(QStringLiteral("run.cancel"), [&view, &runs] {
        if (view.shown())
            view.cancel();
        else
            runs.cancel();
    });
    give(true);
}

void bindOwnAppWindow(AppShelf &shelf, AppView &view, AppLauncher &launcher,
                      std::function<std::optional<AppRef>(const QString &id)> resolve, std::function<void(const AppRef &)> retitled) {
    QObject::connect(&shelf, &AppShelf::chosen, &view,
                     [&view, &launcher, resolve = std::move(resolve), retitled = std::move(retitled)](const QString &id) {
                         view.showApp(); // the chosen tab is shown now: a block that opens as the graph is the App view here
                         if (const std::optional<AppRef> app = resolve(id)) {
                             launcher.setApp(*app);
                             if (retitled)
                                 retitled(*app);
                         }
                     });
}

} // namespace fugue
