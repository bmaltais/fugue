#include "canvas/canvas_actions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

#include "canvas/canvas_model.h"
#include "canvas/mask_layer.h"
#include "canvas/region_pick.h"
#include "engine/atomic_file.h"
#include "engine/canvas_handle.h"
#include "engine/canvas_router.h"
#include "engine/canvas_store.h"
#include "engine/edge_fill.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "canvas/widget_edit.h"
#include "run/run_service.h"
#include "workflow/model_downloads.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

using canvas::cannotRunNow;

// The router's action as the bundled action that does it.
QString bundledFor(canvas::RouteAction action) {
    switch (action) {
    case canvas::RouteAction::Fill:
        return QStringLiteral("generative-fill");
    case canvas::RouteAction::Remove:
        return QStringLiteral("remove");
    case canvas::RouteAction::Refine:
        return QStringLiteral("refine");
    case canvas::RouteAction::Expand:
        return QStringLiteral("expand");
    case canvas::RouteAction::None:
        break;
    }
    return {};
}

std::optional<canvas::RouteRect> rectOf(const QVariantMap &map) {
    if (map.value("width").toInt() <= 0 || map.value("height").toInt() <= 0)
        return std::nullopt;
    return canvas::RouteRect{map.value("x").toInt(), map.value("y").toInt(), map.value("width").toInt(), map.value("height").toInt()};
}

} // namespace

CanvasActions::CanvasActions(RunService *service, const Registry *registry, Workflows *workflows, QObject *parent)
    : QObject(parent), m_service(service), m_registry(registry), m_workflows(workflows), m_roots{bundledRoot()},
      m_historyPath(stateDir() + QStringLiteral("/canvas-prompts.json")),
      m_settingsPath(stateDir() + QStringLiteral("/canvas-actions.json")) {
    m_form = new CanvasModel(registry, this);
    if (workflows)
        m_form->widgets()->setDownloads(workflows->downloads());
    connect(m_form, &CanvasModel::edited, this, &CanvasActions::syncForm);
    connect(m_service, &RunService::event, this, &CanvasActions::onEvent);
    if (m_workflows) {
        connect(m_workflows, &Workflows::templatesChanged, this, [this] { refresh(); });
        connect(m_workflows->downloads(), &ModelDownloads::changed, this, &CanvasActions::downloadsChanged);
        // The models may be here now.
        connect(m_workflows->downloads(), &ModelDownloads::finished, this, [this] { refresh(); });
    }
    m_liveClock.start();
    m_liveTimer.setSingleShot(true);
    connect(&m_liveTimer, &QTimer::timeout, this, &CanvasActions::liveStep);
    refresh();
}

void CanvasActions::setRoots(const QStringList &roots) {
    m_roots = roots;
    refresh();
}

void CanvasActions::setHistoryPath(const QString &path) {
    m_historyPath = path;
    emit historyChanged();
}

void CanvasActions::setSettingsPath(const QString &path) { m_settingsPath = path; }

int CanvasActions::runNumber() const {
    return busy() ? std::min(int(m_order.size()), int(m_order.size() - m_runs.size()) + 1) : 0;
}

bool CanvasActions::downloading() const {
    return m_workflows && m_workflows->downloads()->busy() && m_workflows->downloads()->what().startsWith(QLatin1String("action:"));
}

QString CanvasActions::downloadText() const {
    if (!downloading())
        return {};
    const ModelDownloads *downloads = m_workflows->downloads();
    return downloads->progressText().isEmpty() ? downloads->label() : tr("%1 · %2").arg(downloads->label(), downloads->progressText());
}

void CanvasActions::refresh() {
    // Where an action may come from, in order, the first of an id winning: the templates of the library (the person's own
    // first, a template of the library being how a person's own and a project's actions arrive, design 8), then the files
    // of the roots (the ones compiled into Fugue; they stay the fallback of a library that lacks them).
    QList<canvas::ActionSource> sources;
    if (m_workflows)
        for (const Template &t : m_workflows->templateList())
            sources.append({t.id, t.workflow, true, t.carriedPacks});
    for (const QString &root : std::as_const(m_roots))
        for (const QString &name : QDir(root).entryList({QStringLiteral("*.fugue")}, QDir::Files, QDir::Name))
            sources.append({QFileInfo(name).completeBaseName(), QDir(root).filePath(name), false, {}});
    m_entries = canvas::collectActions(sources, *m_registry);
    rebuildList();
}

QVariantMap CanvasActions::editWorkflow(const QString &id) {
    const Entry *e = entry(id.isEmpty() || id == QLatin1String("auto") ? QStringLiteral("generative-fill") : id);
    if (!e || !m_workflows)
        return refuse(tr("There is no such action."));
    // A template opens as templates do (its sample files taken in, its packs offered); a file compiled into Fugue opens
    // as a copy. Either is a tab with no file: Save asks where, and the original is never written.
    const QString name = e->action ? e->action->name : e->id;
    const bool opened = e->library ? m_workflows->openTemplateById(e->id) : m_workflows->openCopyOf(e->file, name);
    if (!opened)
        return refuse(m_workflows->notice());
    return {{"ok", true}, {"action", e->id}, {"name", name}};
}

void CanvasActions::rebuildList() {
    m_list.clear();
    const QJsonObject remembered = readSettings();
    for (const Entry &e : std::as_const(m_entries)) {
        QVariantMap row{{"id", e.id},
                        {"name", e.action ? e.action->name : e.id},
                        {"description", e.action ? e.action->description : QString()},
                        {"glyph", e.action && !e.action->glyph.isEmpty() ? e.action->glyph : QStringLiteral("sparkles")},
                        {"selection", e.action ? e.action->selection : QStringLiteral("required")},
                        {"needsPrompt", e.action ? e.action->needsPrompt() : false},
                        {"hasLayer", e.action ? e.action->input(QStringLiteral("layer")) != nullptr : false},
                        {"hasSeed", e.action ? e.action->input(QStringLiteral("seed")) != nullptr : false},
                        {"hasPrompt", e.action ? e.action->input(QStringLiteral("prompt")) != nullptr : false},
                        {"broken", !e.broken.isEmpty()},
                        {"reason", e.broken},
                        {"packMissing", !e.needPacks.isEmpty()},
                        {"packName", e.needPacks.isEmpty() ? QString() : e.needPacks.first().toMap().value("name").toString()},
                        {"packText", e.needPacks.isEmpty() ? QString() : tr("Needs the %1 pack").arg(e.needPacks.first().toMap().value("name").toString())},
                        {"packInstallable", !e.needPacks.isEmpty() && !e.needPacks.first().toMap().value("source").toString().isEmpty()},
                        {"missing", 0},
                        {"total", 0},
                        {"download", QString()},
                        {"size", QString()}};
        if (m_workflows) {
            // The models the action would run with: a model picked in the Advanced popover counts.
            const QVariantMap missing = m_workflows->missingModels(withRemembered(e, remembered.value(e.id).toObject().value("values").toObject()).models());
            row.insert("missing", missing.value("count"));
            row.insert("total", missing.value("total"));
            row.insert("download", missing.value("download"));
            row.insert("size", missing.value("size"));
        }
        m_list << row;
    }
    emit actionsChanged();
}

const CanvasActions::Entry *CanvasActions::entry(const QString &id) const {
    for (const Entry &e : m_entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

QString CanvasActions::plainWords(const Error &error, const QString &actionName) { return canvas::plainWords(error, actionName); }

void CanvasActions::setError(const QString &line, const QString &code, const QString &action) {
    m_error = line;
    m_errorCode = code;
    m_errorAction = action;
}

void CanvasActions::clearError() {
    if (m_error.isEmpty() && m_notice.isEmpty())
        return;
    setError({}, {});
    m_notice.clear();
    emit stateChanged();
}

QVariantMap CanvasActions::refuse(const QString &why) {
    if (m_errorCode.isEmpty())
        m_errorCode = QStringLiteral("refused");
    m_error = why;
    emit stateChanged();
    return {{"ok", false}, {"reason", why}};
}

QVariantMap CanvasActions::route(const QString &canvasId, const QString &revision, const QVariantMap &rect, bool hasPrompt) const {
    const Result<canvas::Shown> shown = canvas::shownOf(canvasId, revision);
    if (!shown)
        return {{"action", QString()}, {"reason", shown.error().message}};
    canvas::RouteInput in;
    in.canvasWidth = shown->width;
    in.canvasHeight = shown->height;
    in.selection = rectOf(rect);
    in.hasPrompt = hasPrompt;
    const canvas::Route routed = canvas::routeAction(in);
    const QString id = bundledFor(routed.action);
    return {{"action", entry(id) ? id : QString()},
            {"routed", QString::fromLatin1(canvas::routeActionName(routed.action))},
            {"reason", routed.reasons.empty() ? QString() : QString::fromStdString(routed.reasons.front())}};
}

QVariantMap CanvasActions::generate(const QVariantMap &request) {
    const bool live = request.value("live").toBool();
    if (!live && !m_liveRun.isEmpty())
        detachLive(); // a Generate keeps the preview: its run is not wanted any more
    const QVariantMap result = generateNow(request, live);
    if (live && !result.value("ok").toBool()) {
        m_link.ended(LiveLink::End::Failed, liveNow()); // a preview that cannot start switches live off; the error line says why
        emit liveStateChanged();
    }
    return result;
}

QVariantMap CanvasActions::generateNow(const QVariantMap &request, bool live) {
    if (live && (busy() || !m_liveRun.isEmpty()))
        return {{"ok", false}, {"reason", tr("Another run is going.")}};
    if (busy())
        return refuse(tr("Generate is already running: wait for it, or Cancel it."));
    setError({}, {});
    m_notice.clear();
    const QString canvasId = request.value("canvas").toString();
    if (canvasId.isEmpty())
        return refuse(tr("Open a canvas first."));
    const Result<canvas::Shown> shown = canvas::shownOf(canvasId, request.value("revision").toString());
    if (!shown)
        return refuse(shown.error().message);
    const QString prompt = request.value("prompt").toString().trimmed();
    const QString selection = request.value("selection").toString();
    std::optional<canvas::RouteRect> rect = rectOf(request.value("rect").toMap());

    canvas::RouteInput in;
    in.canvasWidth = shown->width;
    in.canvasHeight = shown->height;
    in.selection = rect;
    in.hasPrompt = !prompt.isEmpty();
    canvas::Route routed = canvas::routeAction(in);
    QString id = request.value("action").toString();
    if (id.isEmpty() || id == QLatin1String("auto")) {
        // Auto: the router's pick. Only Generative fill exists in this slice, so the others say what to do instead.
        switch (routed.action) {
        case canvas::RouteAction::None:
            return refuse(QString::fromStdString(routed.reasons.front()));
        case canvas::RouteAction::Remove:
            return refuse(tr("Type what to put in the selection, then Generate."));
        case canvas::RouteAction::Expand:
            return refuse(tr("The selection reaches past the canvas. Keep it inside the picture."));
        case canvas::RouteAction::Refine:
            if (!rect)
                return refuse(tr("Select an area first."));
            break;
        case canvas::RouteAction::Fill:
            break;
        }
        id = entry(bundledFor(routed.action)) ? bundledFor(routed.action) : QStringLiteral("generative-fill");
    }
    const Entry *chosen = entry(id);
    if (!chosen)
        return refuse(tr("There is no action %1.").arg(id));
    if (live && (chosen->action->selection == QLatin1String("bounds") || chosen->graph.writes.isEmpty()))
        return refuse(tr("%1 cannot preview: it works on the whole canvas.").arg(chosen->action->name));
    m_actionName = chosen->action ? chosen->action->name : id;
    if (!chosen->broken.isEmpty()) {
        m_errorCode = QStringLiteral("action_invalid");
        return refuse(chosen->broken);
    }
    // A pack it needs is not installed: the bar offers to install it (installPack asks the person); nothing runs.
    if (const std::optional<Error> why = canvas::notRunnable(*chosen)) {
        m_errorCode = why->code;
        m_errorAction = id;
        return refuse(why->message);
    }
    const canvas::Action &action = *chosen->action;
    // The Advanced values the person set (a model they picked is the one that must be here), a word only of a choice input.
    QJsonObject remembered = settingsOf(id).value("values").toObject();
    for (auto it = remembered.begin(); it != remembered.end();) {
        const canvas::ActionInput *input = action.input(it.key());
        const bool fits = input && inPopover(*input) &&
                          (!input->choiceValues || (it->isString() && input->choices->contains(it->toString())));
        it = fits ? it + 1 : remembered.erase(it);
    }
    canvas::ActRequest act;
    act.canvas = canvasId;
    act.revision = shown->revision;
    act.prompt = prompt;
    act.rect = rect;
    act.selection = selection;
    act.layer = request.value("layer").toString();
    act.currentLayer = request.value("currentLayer").toString();
    act.expand = request.value("expand").toMap();
    act.values = remembered;
    const QVariant fixedSeed = request.value("seed");
    if (fixedSeed.isValid() && !fixedSeed.isNull())
        act.seed = fixedSeed.toLongLong();
    else if (m_link.enabled())
        act.seed = m_liveSeed; // live: the preview and the Generate that keeps it are the same picture
    act.count = live ? 1 : request.contains("count") ? request.value("count").toInt() : variations(id);
    act.beforeChange = [&]() -> std::optional<Error> {
        if (m_workflows && m_workflows->missingModels(withRemembered(*chosen, remembered).models()).value("count").toInt() > 0)
            return makeError(QStringLiteral("model_missing"), tr("%1 needs models that are not on this machine. Download them first.").arg(m_actionName));
        return std::nullopt;
    };
    const Result<canvas::ActPlan> plan = canvas::planAct(*chosen, act);
    if (!plan) {
        const QString code = plan.error().code;
        if (code == QLatin1String("mask_empty") || code == QLatin1String("canvas_head_moved") || code == QLatin1String("model_missing"))
            m_errorCode = code;
        if (code == QLatin1String("model_missing"))
            m_errorAction = id;
        return refuse(plan.error().message);
    }
    if (live)
        return startLive(*chosen, *plan, canvasId);
    m_grown = {};
    m_growEnd.clear();
    if (plan->grew) {
        m_grown.canvas = plan->canvas;
        m_grown.grew = true;
        m_grown.growRevision = plan->growRevision;
    }
    QStringList started;
    for (const Document &bound : plan->runs) {
        RunRequest run;
        run.document = bound;
        run.workflow = chosen->file;
        const QString runId = m_service->start(run);
        started << runId;
        m_runs << runId;
        if (plan->rect) {
            // Where a step picture of this run goes: the Region's context box (the selection grown by padding, raised for the
            // feather, kept in the canvas), and the selection inside it.
            const Node &regionNode = bound.graph.nodes[chosen->graph.region];
            const auto literal = [&](const char *name) { return regionNode.inputs.value(QLatin1String(name)).literal().toInt(); };
            const canvas::RouteRect &sel = *plan->rect;
            const auto box = region::contextOf(sel.x, sel.y, sel.width, sel.height, literal("padding"), plan->width, plan->height, literal("feather"));
            const int left = std::max(box.x, sel.x), top = std::max(box.y, sel.y);
            const int right = std::min(box.x + box.width, sel.x + sel.width), bottom = std::min(box.y + box.height, sel.y + sel.height);
            const auto map = [](int x, int y, int w, int h) { return QVariantMap{{"x", x}, {"y", y}, {"width", w}, {"height", h}}; };
            if (right > left && bottom > top)
                m_liveRects.insert(runId, LiveRect{map(box.x, box.y, box.width, box.height), map(left, top, right - left, bottom - top)});
        }
    }
    m_order = started;
    m_canvas = canvasId;
    m_action = id;
    m_progress = -1;
    m_lastRun = started.last();
    if (!prompt.isEmpty() && action.input(QStringLiteral("prompt")))
        remember(id, prompt);
    emit stateChanged();
    emit progressChanged();
    return {{"ok", true}, {"run", started.first()}, {"runs", started}, {"action", id}};
}

Result<canvas::BatchAction> CanvasActions::batchAction(const QString &id) const {
    const Entry *chosen = entry(id);
    if (!chosen)
        return makeError(QStringLiteral("file_not_found"), tr("There is no action %1.").arg(id));
    if (!chosen->broken.isEmpty() || !chosen->action)
        return makeError(QStringLiteral("action_invalid"), chosen->broken);
    if (!chosen->needPacks.isEmpty())
        return makeError(QStringLiteral("pack_missing"), tr("%1 needs the %2 pack, which is not installed here.")
                                                             .arg(chosen->action->name, chosen->needPacks.first().toMap().value("name").toString()));
    if (chosen->action->input(QStringLiteral("layer")))
        return makeError(QStringLiteral("action_invalid"), tr("%1 works on a layer of a canvas, so it cannot run over a folder.").arg(chosen->action->name));
    if (chosen->action->selection == QLatin1String("bounds"))
        return makeError(QStringLiteral("action_invalid"), tr("%1 works on a crop frame beyond the canvas, so it cannot run over a folder.").arg(chosen->action->name));
    if (m_workflows && m_workflows->missingModels(chosen->document.models()).value("count").toInt() > 0)
        return makeError(QStringLiteral("model_missing"), tr("%1 needs models that are not on this machine. Download them first.").arg(chosen->action->name));
    return canvas::BatchAction{chosen->id, chosen->file, chosen->document, *chosen->action, chosen->graph};
}

void CanvasActions::cancel() {
    // The waiting ones first, so none of them starts when the active one ends.
    for (auto it = m_runs.crbegin(); it != m_runs.crend(); ++it)
        if (*it != m_active)
            m_service->cancel(*it);
    if (!m_active.isEmpty())
        m_service->cancel(m_active);
}

void CanvasActions::onEvent(const QString &run, const QJsonObject &event) {
    if (run == m_liveRun) {
        onLiveEvent(event);
        return;
    }
    if (!m_runs.contains(run))
        return;
    const QString kind = event.value("event").toString();
    const QString node = event.value("node").toString();
    if (kind == QLatin1String("run.started")) {
        m_active = run;
        m_progress = -1;
        emit stateChanged();
        emit progressChanged();
    } else if (kind == QLatin1String("node.running")) {
        m_progress = -1;
        emit progressChanged();
    } else if (kind == QLatin1String("node.progress")) {
        m_progress = std::clamp(event.value("value").toDouble(), 0.0, 1.0);
        emit progressChanged();
    } else if (kind == QLatin1String("node.preview")) {
        // The newest picture of any node of this run wins (in practice the Sampler's step frames, the only node that previews
        // while it runs). Nothing shows when the Sampler preview setting is Off: no event comes, the shimmer stays.
        const QString url = event.value("thumbnails").toObject().value("preview").toObject().value("url").toString();
        const auto rects = m_liveRects.constFind(run);
        if (!url.isEmpty() && rects != m_liveRects.cend() && run == m_active) {
            m_liveFrame = url;
            m_liveBox = rects->box;
            m_liveClip = rects->clip;
            m_liveFade = false;
            emit liveChanged();
        }
    } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
        const Entry *chosen = entry(m_action);
        if (chosen && chosen->graph.writes.contains(node)) {
            const QJsonObject meta = event.value("outputs").toObject().value("canvas").toObject().value("meta").toObject();
            m_writes[run] << WriteResult{kind == QLatin1String("node.cached"), meta.value("canvas").toString(), meta.value("revision").toString()};
            if (m_link.enabled() && !m_writes[run].last().revision.isEmpty()) {
                // The layer this Generate lands is the preview kept, not a change to preview again.
                m_ownRevisions.insert(m_writes[run].last().revision);
                if (m_noteRevision == m_writes[run].last().revision)
                    m_link.adopt(m_noteKey);
            }
        }
        if (chosen && chosen->graph.news.contains(node) && kind == QLatin1String("node.done")) {
            const QString made = event.value("outputs").toObject().value("canvas").toObject().value("meta").toObject().value("canvas").toString();
            if (!made.isEmpty())
                m_made[run] << made;
        }
    } else if (kind == QLatin1String("node.error")) {
        m_nodeErrors[run] << Error::fromJson(event.value("error").toObject());
    } else if (kind == QLatin1String("run.done")) {
        Errors errors = m_nodeErrors.take(run);
        for (const QJsonValue &value : event.value("errors").toArray())
            errors << Error::fromJson(value.toObject());
        endRun(run, event.value("status").toString(), errors);
    }
}

void CanvasActions::clearLive(bool fade) {
    if (m_liveFrame.isEmpty())
        return;
    m_liveFrame.clear();
    m_liveBox.clear();
    m_liveClip.clear();
    m_liveFade = fade;
    emit liveChanged();
}

void CanvasActions::endRun(const QString &run, const QString &status, const Errors &errors) {
    m_runs.removeAll(run);
    m_liveRects.remove(run);
    clearLive(status == QLatin1String("ok")); // the layer is already on the canvas: the frame fades; else it goes at once
    if (m_active == run) {
        m_active.clear();
        m_progress = -1;
    }
    const QList<WriteResult> writes = m_writes.take(run);
    const QStringList made = m_made.take(run);
    if (status == QLatin1String("ok")) {
        const QString source = m_canvas;
        for (const QString &id : made)
            emit canvasMade(source, id);
        for (const WriteResult &write : writes)
            if (write.cached && !write.revision.isEmpty())
                placeAgain(write);
    } else if (status != QLatin1String("cancelled")) {
        const auto hard = std::find_if(errors.cbegin(), errors.cend(), [](const Error &e) { return e.severity == Error::Severity::Error; });
        const Error first = hard != errors.cend() ? *hard : makeError(QStringLiteral("node_failed"), tr("%1 did not finish.").arg(m_actionName));
        // A missing model offers the download; nothing else does.
        setError(plainWords(first, m_actionName), first.code, cannotRunNow(first.code) ? m_action : QString());
        cancel(); // the rest of a Count N would fail the same way: one Generate ends as one
    }
    if (m_grown.grew && status != QLatin1String("ok") && m_growEnd.isEmpty())
        m_growEnd = status;
    if (m_grown.grew && m_runs.isEmpty()) {
        // The last run of this Generate ended: an Expand that did not write anything takes its empty strip back.
        if (!m_growEnd.isEmpty() && canvas::takeBackGrowth(m_grown))
            m_notice = canvas::growthTakenBack(m_actionName, m_growEnd == QLatin1String("cancelled"));
        m_grown = {};
        m_growEnd.clear();
    }
    emit stateChanged();
    emit progressChanged();
    emit finished(run, status);
}

void CanvasActions::placeAgain(const WriteResult &write) {
    const Result<canvas::PlacedAgain> placed = canvas::placeWriteAgain(write.canvas, write.revision);
    if (!placed)
        setError(plainWords(placed.error(), m_actionName), placed.error().code);
    else if (!placed->moved && !placed->layer.isEmpty())
        m_notice = tr("Same prompt, seed and selection as “%1”. Change the seed to get another.").arg(placed->layer);
}

// --- the Advanced popover and Count (slice 4) ---

QJsonObject CanvasActions::readSettings() const {
    QFile file(m_settingsPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("actions").toObject();
}

void CanvasActions::writeSettings(const QJsonObject &actions) {
    if (actions.isEmpty()) {
        QFile::remove(m_settingsPath);
        return;
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{"format", 1}, {"actions", actions}}).toJson(QJsonDocument::Indented);
    if (hasErrors(makeFolder(QFileInfo(m_settingsPath).absolutePath())))
        return;
    (void)replaceFile(m_settingsPath, bytes, QStringLiteral("the task bar's settings"));
}

QJsonObject CanvasActions::settingsOf(const QString &id) const { return readSettings().value(resolved(id)).toObject(); }

void CanvasActions::storeSettings(const QString &id, const QJsonObject &entry) {
    QJsonObject actions = readSettings();
    if (entry.value("values").toObject().isEmpty() && !entry.contains("count"))
        actions.remove(resolved(id));
    else
        actions.insert(resolved(id), entry);
    writeSettings(actions);
}

QString CanvasActions::resolved(const QString &id) const {
    return id.isEmpty() || id == QLatin1String("auto") ? QStringLiteral("generative-fill") : id;
}

bool CanvasActions::inPopover(const canvas::ActionInput &input) { return canvas::inPopover(input); }

Document CanvasActions::withRemembered(const Entry &entry, const QJsonObject &values) const { return canvas::withValues(entry, values); }

QVariantList CanvasActions::advanced(const QString &id) {
    const Entry *e = entry(resolved(id));
    QVariantList rows;
    if (!e || !e->action || !e->broken.isEmpty()) {
        m_formAction.clear();
        return rows;
    }
    const QJsonObject values = settingsOf(id).value("values").toObject();
    m_formAction = e->id;
    m_loadingForm = true;
    m_form->setDocument(withRemembered(*e, values));
    m_loadingForm = false;
    for (const canvas::ActionInput &input : e->action->inputs) {
        if (!inPopover(input) || input.targets.isEmpty())
            continue;
        const PortRef &target = input.targets.first();
        QVariantMap row{{"key", input.key()}, {"node", target.node}, {"input", target.port}, {"note", QString()}};
        if (input.choiceValues) {
            const QString word = values.value(input.key()).isString() && input.choices->contains(values.value(input.key()).toString())
                                     ? values.value(input.key()).toString() : input.defaultValue.toString();
            row.insert("kind", "choice");
            row.insert("label", input.label.isEmpty() ? input.key() : input.label);
            row.insert("choices", *input.choices);
            row.insert("value", word);
            row.insert("changed", word != input.defaultValue.toString());
        } else {
            const QVariantMap port = m_form->inputPort(target.node, target.port);
            if (m_form->widgets()->spec(target.node, target.port).isEmpty())
                continue; // nothing to edit it with
            row.insert("kind", "widget");
            row.insert("label", !input.label.isEmpty() ? input.label : port.value("label").toString());
            row.insert("changed", values.contains(input.key()));
        }
        rows << row;
    }
    return rows;
}

void CanvasActions::syncForm() {
    if (m_loadingForm || m_formAction.isEmpty())
        return;
    const Entry *e = entry(m_formAction);
    if (!e || !e->action)
        return;
    const Document now = m_form->document();
    QJsonObject values = readSettings().value(e->id).toObject().value("values").toObject();
    for (const canvas::ActionInput &input : e->action->inputs) {
        if (input.choiceValues || !inPopover(input) || input.targets.isEmpty())
            continue;
        const PortRef &target = input.targets.first();
        const auto node = now.graph.nodes.constFind(target.node);
        if (node == now.graph.nodes.constEnd())
            continue;
        const InputValue value = node->inputs.value(target.port);
        const InputValue original = input.defaultValue.isUndefined() ? e->document.graph.nodes.value(target.node).inputs.value(target.port)
                                                                     : InputValue::fromJson(input.defaultValue);
        if (value.isEmpty() || value.kind() == InputValue::Kind::Link || value.toJson() == original.toJson())
            values.remove(input.key()); // what the action has anyway: nothing to remember
        else
            values.insert(input.key(), value.toJson());
    }
    QJsonObject settings = settingsOf(e->id);
    settings.insert("values", values);
    if (values.isEmpty())
        settings.remove("values");
    storeSettings(e->id, settings);
    rebuildList(); // the models it would run with may be others now
    emit stateChanged();
}

QString CanvasActions::choiceOf(const QString &id, const QString &key) const {
    const Entry *e = entry(resolved(id));
    const canvas::ActionInput *input = e && e->action ? e->action->input(key) : nullptr;
    if (!input || !input->choiceValues)
        return {};
    const QJsonValue word = settingsOf(id).value("values").toObject().value(key);
    return word.isString() && input->choices->contains(word.toString()) ? word.toString() : input->defaultValue.toString();
}

bool CanvasActions::setChoice(const QString &id, const QString &key, const QString &word) {
    const Entry *e = entry(resolved(id));
    const canvas::ActionInput *input = e && e->action ? e->action->input(key) : nullptr;
    if (!input || !input->choiceValues || !input->choices->contains(word))
        return false;
    QJsonObject settings = settingsOf(id);
    QJsonObject values = settings.value("values").toObject();
    if (input->defaultValue.toString() == word)
        values.remove(key);
    else
        values.insert(key, word);
    if (values.isEmpty())
        settings.remove("values");
    else
        settings.insert("values", values);
    storeSettings(id, settings);
    emit stateChanged();
    return true;
}

bool CanvasActions::hasAdvancedChanges(const QString &id) const { return !settingsOf(id).value("values").toObject().isEmpty(); }

void CanvasActions::resetAdvanced(const QString &id) {
    QJsonObject settings = settingsOf(id);
    settings.remove("values");
    storeSettings(id, settings);
    if (m_formAction == resolved(id))
        advanced(id);
    emit stateChanged();
}

int CanvasActions::variations(const QString &id) const {
    const Entry *e = entry(resolved(id));
    if (!e || !e->action || !e->action->input(QStringLiteral("seed")))
        return 1;
    return std::clamp(settingsOf(id).value("count").toInt(1), 1, maxVariations);
}

void CanvasActions::setVariations(const QString &id, int count) {
    QJsonObject settings = settingsOf(id);
    count = std::clamp(count, 1, maxVariations);
    if (count == 1)
        settings.remove("count");
    else
        settings.insert("count", count);
    storeSettings(id, settings);
    emit stateChanged();
}

QJsonObject CanvasActions::readHistory() const {
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("actions").toObject();
}

QStringList CanvasActions::history(const QString &action) const {
    QStringList out;
    for (const QJsonValue &value : readHistory().value(action).toArray())
        out << value.toString();
    return out;
}

void CanvasActions::remember(const QString &action, const QString &prompt) {
    QJsonObject actions = readHistory();
    QJsonArray list{prompt};
    for (const QJsonValue &value : actions.value(action).toArray())
        if (value.toString() != prompt && list.size() < historyLimit)
            list.append(value);
    actions.insert(action, list);
    const QByteArray bytes = QJsonDocument(QJsonObject{{"format", 1}, {"actions", actions}}).toJson(QJsonDocument::Indented);
    if (hasErrors(makeFolder(QFileInfo(m_historyPath).absolutePath())))
        return;
    (void)replaceFile(m_historyPath, bytes, QStringLiteral("the prompt history"));
    emit historyChanged();
}

void CanvasActions::clearHistory() {
    QFile::remove(m_historyPath);
    emit historyChanged();
}

QVariantMap CanvasActions::selectionBounds(MaskLayer *selection) const {
    if (!selection || selection->isEmpty())
        return {};
    const mask::MaskPaint &painted = selection->paint();
    int left = painted.width(), top = painted.height(), right = -1, bottom = -1;
    for (int y = 0; y < painted.height(); ++y)
        for (int x = 0; x < painted.width(); ++x)
            if (painted.at(x, y) > 0) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    if (right < 0)
        return {};
    return {{"x", left}, {"y", top}, {"width", right - left + 1}, {"height", bottom - top + 1}};
}

QVariantMap CanvasActions::storeSelection(MaskLayer *selection) const {
    const auto no = [](const QString &why) { return QVariantMap{{"ok", false}, {"reason", why}}; };
    if (!selection)
        return no(tr("Nothing is selected."));
    QVariantMap bounds = selectionBounds(selection);
    if (bounds.isEmpty())
        return no(tr("The selection is empty. Draw one, then Generate."));
    const Result<QString> hash = canvas::CanvasStore::forSession().storeSelection(selection->paint());
    if (!hash)
        return no(hash.error().message);
    bounds.insert("ok", true);
    bounds.insert("hash", hash.value());
    return bounds;
}

bool CanvasActions::installPack(const QString &action) {
    const Entry *chosen = entry(action);
    if (!chosen || !m_workflows)
        return false;
    for (const QVariant &need : chosen->needPacks) {
        const QVariantMap pack = need.toMap();
        if (pack.value("source").toString().isEmpty())
            continue;
        m_workflows->requestPackInstall(pack.value("source").toString(), pack.value("ref").toString());
        return true;
    }
    return false;
}

void CanvasActions::download(const QString &action) {
    const Entry *chosen = entry(action);
    if (!chosen || !m_workflows)
        return;
    m_workflows->downloadModels(withRemembered(*chosen, settingsOf(action).value("values").toObject()).models(), QStringLiteral("action:") + action);
    emit downloadsChanged();
}

// --- the live link (#315, design 16) ---

void CanvasActions::setLive(bool on) {
    if (on == m_link.enabled())
        return;
    m_link.setEnabled(on, liveNow());
    if (on) {
        m_liveSeed = QRandomGenerator::global()->bounded(1, 2147483647);
        m_ownRevisions.clear();
    } else {
        detachLive();
        m_liveTimer.stop();
        clearLive(false);
    }
    emit liveStateChanged();
    m_liveTimer.start(0);
}

void CanvasActions::liveReroll() {
    if (!m_link.enabled())
        return;
    m_liveSeed = QRandomGenerator::global()->bounded(1, 2147483647);
    m_noteKey.append(QLatin1Char('+')); // the state differs, so the preview runs again
    detachLive();
    m_link.note(m_noteKey, true, liveNow());
    m_liveTimer.start(0);
}

void CanvasActions::liveNote(const QString &key, const QString &revision, bool ready) {
    if (!m_link.enabled())
        return;
    const QString seeded = key + QLatin1Char('|') + revision + QLatin1Char('|') + QString::number(m_liveSeed);
    m_noteKey = seeded;
    m_noteRevision = revision;
    if (m_ownRevisions.contains(revision))
        m_link.adopt(seeded);
    m_link.note(seeded, ready, liveNow());
    m_liveTimer.start(0);
}

void CanvasActions::liveStep() {
    const LiveLink::Step step = m_link.poll(liveNow());
    switch (step.what) {
    case LiveLink::Do::Cancel:
        if (!m_liveRun.isEmpty())
            m_service->cancel(m_liveRun);
        break;
    case LiveLink::Do::Start:
        emit liveDue();
        if (m_liveRun.isEmpty()) { // nobody started it (no bar shows this canvas): off, so it does not spin
            m_link.ended(LiveLink::End::Failed, liveNow());
            emit liveStateChanged();
        }
        break;
    case LiveLink::Do::Nothing:
        if (step.waitMs > 0)
            m_liveTimer.start(int(step.waitMs));
        break;
    }
}

// The run ends without a word: a Generate has taken over, or live went off.
void CanvasActions::detachLive() {
    if (m_liveRun.isEmpty())
        return;
    const QString run = m_liveRun;
    m_liveRun.clear();
    m_liveRects.remove(run);
    m_liveErrors.clear();
    m_service->cancel(run);
    m_link.ended(LiveLink::End::Cancelled, liveNow());
    emit liveStateChanged();
}

QVariantMap CanvasActions::startLive(const Entry &chosen, const canvas::ActPlan &plan, const QString &canvasId) {
    // The run stops where Canvas Write would take the picture: the node its `image` comes from.
    const Document &bound = plan.runs.first();
    QString target;
    for (const QString &write : chosen.graph.writes) {
        const InputValue image = bound.graph.nodes.value(write).inputs.value(QStringLiteral("image"));
        if (image.kind() == InputValue::Kind::Link) {
            target = image.from().node;
            break;
        }
    }
    if (target.isEmpty())
        return refuse(tr("%1 cannot preview: it makes no picture to show first.").arg(m_actionName));
    RunRequest run;
    run.document = bound;
    run.workflow = chosen.file;
    run.target = target;
    m_liveRun = m_service->start(run);
    m_liveTarget = target;
    m_liveErrors.clear();
    m_canvas = canvasId;
    if (plan.rect) {
        const Node &regionNode = bound.graph.nodes[chosen.graph.region];
        const auto literal = [&](const char *name) { return regionNode.inputs.value(QLatin1String(name)).literal().toInt(); };
        const canvas::RouteRect &sel = *plan.rect;
        const auto box = region::contextOf(sel.x, sel.y, sel.width, sel.height, literal("padding"), plan.width, plan.height, literal("feather"));
        const int left = std::max(box.x, sel.x), top = std::max(box.y, sel.y);
        const int right = std::min(box.x + box.width, sel.x + sel.width), bottom = std::min(box.y + box.height, sel.y + sel.height);
        const auto map = [](int x, int y, int w, int h) { return QVariantMap{{"x", x}, {"y", y}, {"width", w}, {"height", h}}; };
        if (right > left && bottom > top)
            m_liveRects.insert(m_liveRun, LiveRect{map(box.x, box.y, box.width, box.height), map(left, top, right - left, bottom - top)});
    }
    emit liveStateChanged();
    emit stateChanged();
    return {{"ok", true}, {"run", m_liveRun}, {"action", chosen.id}, {"live", true}};
}

void CanvasActions::onLiveEvent(const QJsonObject &event) {
    const QString kind = event.value("event").toString();
    const QString node = event.value("node").toString();
    const auto show = [&](const QString &url) {
        const auto rects = m_liveRects.constFind(m_liveRun);
        if (url.isEmpty() || rects == m_liveRects.cend())
            return;
        m_liveFrame = url;
        m_liveBox = rects->box;
        m_liveClip = rects->clip;
        m_liveFade = false;
        emit liveChanged();
    };
    if (kind == QLatin1String("node.preview")) {
        show(event.value("thumbnails").toObject().value("preview").toObject().value("url").toString());
    } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
        if (node == m_liveTarget) { // the finished picture replaces the step frame
            const QJsonObject thumbnails = event.value("thumbnails").toObject();
            const QJsonObject picture = thumbnails.contains("image") ? thumbnails.value("image").toObject()
                                        : thumbnails.isEmpty()       ? QJsonObject()
                                                                     : thumbnails.begin().value().toObject();
            show(picture.value("url").toString());
        }
    } else if (kind == QLatin1String("node.error")) {
        m_liveErrors << Error::fromJson(event.value("error").toObject());
    } else if (kind == QLatin1String("run.done")) {
        Errors errors = m_liveErrors;
        for (const QJsonValue &value : event.value("errors").toArray())
            errors << Error::fromJson(value.toObject());
        endLive(event.value("status").toString(), errors);
    }
}

void CanvasActions::endLive(const QString &status, const Errors &errors) {
    m_liveRects.remove(m_liveRun);
    m_liveRun.clear();
    m_liveErrors.clear();
    if (status == QLatin1String("ok")) {
        m_link.ended(LiveLink::End::Ok, liveNow());
    } else if (status == QLatin1String("cancelled")) {
        m_link.ended(LiveLink::End::Cancelled, liveNow());
    } else {
        const auto hard = std::find_if(errors.cbegin(), errors.cend(), [](const Error &e) { return e.severity == Error::Severity::Error; });
        const Error first = hard != errors.cend() ? *hard : makeError(QStringLiteral("node_failed"), tr("%1 did not finish.").arg(m_actionName));
        setError(plainWords(first, m_actionName), first.code, cannotRunNow(first.code) ? m_action : QString());
        m_link.ended(LiveLink::End::Failed, liveNow());
        clearLive(false);
    }
    emit liveStateChanged();
    emit stateChanged();
    m_liveTimer.start(0);
}

} // namespace fugue
