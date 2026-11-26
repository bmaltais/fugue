#include "apps/app_window.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>

#include <QPointer>

#include "apps/app_launcher.h"
#include "apps/app_shelf.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/providers.h"
#include "engine/paired_placement.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"
#include "workflow/readiness_sheet.h"
#include "workflow/workflows.h"

namespace fugue {

void bindAppView(AppView &view, Workflows &workflows, CanvasModel &canvas, RunService &service, CommandRegistry &commands,
                 CanvasRuns &runs, PairedRequirementOf requirementOf, PairedProbe probeSource) {
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
    // Where the app runs when its models are not here (#395, #491, #581): the paired verdict of engine/paired_placement.h, the CLI's
    // too. It is made again, asynchronously, whenever the plan or the document changes (a recent answer of the hosts is reused), and
    // the line and the panel read the last one made. "Runs on <hosts>" when every node that needs a model has a host that answered; a
    // silent host is said ("<host> is not answering") and the model rows stay; a node no host can take is named with why (`blocked`).
    // Whether the host has the model files cannot be asked (describe lists no model of a helper that takes its files in the job), so
    // the rows of the models stay, marked as the host's if it has them (#493).
    struct Placement {
        QByteArray key; // the plan and document the verdict is for; empty before any
        PairedVerdict verdict;
    };
    const auto placement = std::make_shared<Placement>();
    const auto probe = keptPairedProbe(probeSource ? std::move(probeSource) : livePairedProbe(), std::chrono::seconds(30));
    const auto decide = [&canvas, registry = view.registry(), needs = QPointer<NeedsModel>(needs), placement, probe,
                         requirementOf = std::move(requirementOf)](const ReadinessPlan &plan) {
        const Document document = canvas.document();
        PairedPlacementRequest request;
        if (const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath()); instances)
            request.instances = instances.value();
        // Asked again only when what the verdict depends on changed (not the disk space the plan reads live); the last verdict stays
        // shown until the new one arrives.
        const QByteArray key = pairedPlacementKey(document, plan, request.instances);
        if (key == placement->key)
            return;
        placement->key = key;
        request.requirementOf = requirementOf;
        request.registry = registry;
        decidePairedPlacement(document, plan, request, probe, [placement, needs, key](const PairedVerdict &verdict) {
            if (placement->key != key)
                return; // the plan or the document changed while the hosts were asked
            const bool same = verdict.hostOf == placement->verdict.hostOf && verdict.silent == placement->verdict.silent
                              && verdict.applies == placement->verdict.applies && verdict.blocked.size() == placement->verdict.blocked.size()
                              && std::equal(verdict.blocked.cbegin(), verdict.blocked.cend(), placement->verdict.blocked.cbegin(),
                                            [](const Error &a, const Error &b) { return a.node == b.node && a.message == b.message; });
            placement->verdict = verdict;
            if (!same && needs)
                needs->refreshSoon(); // the answer may be "not there" or "not answering", which changes nothing the view signals
        });
    };
    needs->setPlanSource([&view, &workflows, placement, decide] {
        if (!view.isApp() || !workflows.missingSomething()) {
            placement->key.clear();
            placement->verdict = {};
            view.setPairedHosts({}, {});
            return ReadinessPlan();
        }
        const ReadinessPlan plan = workflows.readiness()->currentPlan();
        decide(plan);
        const PairedVerdict &verdict = placement->verdict;
        view.setPairedHosts(verdict.runsOn() ? verdict.hosts().join(QStringLiteral(", ")) : QString(), verdict.silent.join(QStringLiteral(", ")));
        return planUnder(plan, verdict);
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
