#include "apps/app_window.h"

#include <algorithm>
#include <functional>
#include <memory>

#include "apps/app_launcher.h"
#include "apps/app_shelf.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/providers.h"
#include "engine/remote_placement.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "workflow/readiness_sheet.h"
#include "workflow/workflows.h"

namespace fugue {

void bindAppView(AppView &view, Workflows &workflows, CanvasModel &canvas, RunService &service, CommandRegistry &commands,
                 CanvasRuns &runs, std::function<QString(const QString &typeId)> capabilityOf) {
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
    // The app runs on the paired helper when its models are not here, that host answers, and it can take every node that needs them
    // (#395, #491): "Runs on <host>" says so. Whether the host has the model files cannot be asked (describe lists no model of a
    // helper that takes its files in the job), so the rows of the models stay, marked as the host's if it has them (#493). A host
    // that cannot take a node leaves the rows and adds the node and why (`blocked`).
    const auto blocked = std::make_shared<Errors>();
    view.setPlacementSource([&workflows, &canvas, registry = view.registry(), needs, blocked, capabilityOf = std::move(capabilityOf)](std::function<void(const QString &)> done) {
        const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
        probePairedHelpers(instances ? instances.value() : QList<ProviderInstance>(),
                           [&workflows, &canvas, registry, needs, blocked, capabilityOf, instances = instances ? instances.value() : QList<ProviderInstance>(),
                            done = std::move(done)](const QMap<QString, Description> &answered) {
            blocked->clear();
            QString host; // the first helper, in the user's order, that answered
            for (const ProviderInstance &instance : instances)
                if (host.isEmpty() && answered.contains(instance.name))
                    host = instance.name;
            if (!host.isEmpty()) {
                RemotePlacement placement;
                placement.instances = instances;
                placement.capabilityOf = capabilityOf;
                placement.registry = registry;
                for (auto it = answered.cbegin(); it != answered.cend(); ++it) { // what each said just now, not a file read back
                    QStringList capabilities;
                    for (const CapabilityOffer &offer : it->capabilities)
                        capabilities << offer.capability;
                    placement.hostCapabilities.insert(it.key(), capabilities);
                }
                *blocked = remoteRunProblems(canvas.document(), workflows.readiness()->currentPlan(), placement);
            }
            if (!blocked->isEmpty())
                needs->refreshSoon(); // the answer is "not there", which changes nothing the view signals: the panel names the nodes now
            done(blocked->isEmpty() ? host : QString());
        });
    });
    needs->setPlanSource([&view, &workflows, blocked] {
        if (!view.isApp() || !workflows.missingSomething()) {
            view.askPlacement(false);
            return ReadinessPlan();
        }
        ReadinessPlan plan = workflows.readiness()->currentPlan();
        const auto isDownloadProblem = [](const Error &problem) {
            return problem.code == QLatin1String("download_failed") || problem.code == QLatin1String("model_missing");
        };
        view.askPlacement(!plan.models.isEmpty() || std::any_of(plan.problems.cbegin(), plan.problems.cend(), isDownloadProblem));
        if (!view.runsOn().isEmpty()) {
            for (ReadinessModel &model : plan.models)
                model.onHost = view.runsOn();
        } else {
            plan.problems += *blocked;
        }
        return plan;
    });
    QObject::connect(&view, &AppView::runsOnChanged, needs, &NeedsModel::refreshSoon);
    // A node routed to a service off this machine (#404) is said on the same "Runs on" line.
    QObject::connect(&runs, &CanvasRuns::runsOnChanged, &view, [&view, &runs] { view.setOffMachine(runs.runsOn()); });
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

    // Make app and Edit app (#393): the block the builder wrote is in the form now. Apply and open shows the App view; plain Apply
    // leaves the window where it was (a first block would otherwise open the tab in its defaultView, taking the graph away).
    QObject::connect(canvas.appBuilder(), &AppBuilder::applied, &view, [&view](bool openAppView) {
        const bool wasShown = view.shown();
        emit view.documentWanted();
        if (openAppView)
            view.showApp();
        else if (!wasShown)
            view.openInGraph();
    });

    // The App | Graph switch is always there (#397): the App view's other view for an app; for a plain graph, the App segment is the
    // builder (Make app), and bindViewSwitch adds the shelf's two states.
    commands.setHandler(QStringLiteral("view.toggle-app"), [&view, &commands] {
        if (view.isApp())
            view.toggle();
        else
            commands.trigger(QStringLiteral("app.make"));
    });
    commands.setEnabledWhen(QStringLiteral("view.toggle-app"), [] { return true; });
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

void bindViewSwitch(AppShelf &shelf, AppView &view, CommandRegistry &commands) {
    const QString id = QStringLiteral("view.toggle-app");
    commands.setHandler(id, [&shelf, &view, graphOrBuilder = commands.handlerOf(id)] {
        if (view.isApp() || (!shelf.atHome() && !shelf.landing()))
            graphOrBuilder();
        else if (shelf.atHome())
            shelf.dismiss(); // the shelf's Graph: Start blank
        else
            shelf.showAgain(); // the empty graph's App: back to the shelf
    });
    commands.setCheckedWhen(id, [&shelf, &view] { return view.shown() || shelf.atHome(); });
    QObject::connect(&shelf, &AppShelf::homeChanged, &commands, &CommandRegistry::changed);
    // App inputs (the App view's panel) only while the App view itself is shown: view.toggle-app is also checked on the shelf now.
    commands.setEnabledWhen(QStringLiteral("view.toggle-app-inputs"), [&view] { return view.shown(); });
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
