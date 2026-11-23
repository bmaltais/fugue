#include "canvas/canvas_actions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

#include "canvas/mask_layer.h"
#include "canvas/region_pick.h"
#include "engine/atomic_file.h"
#include "engine/canvas_handle.h"
#include "engine/canvas_router.h"
#include "engine/canvas_store.h"
#include "engine/edge_fill.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "run/run_service.h"
#include "workflow/model_downloads.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

// The errors of 8.5: nothing can serve the node now (as CanvasRuns names them).
bool cannotRunNow(const QString &code) {
    static const QStringList codes{"capability_unavailable", "model_not_running", "model_missing", "provider_unreachable",
                                   "unknown_provider"};
    return codes.contains(code);
}

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

// The rectangle held inside the canvas (a Generate never asks for an Expand it cannot do in this slice).
canvas::RouteRect clamped(const canvas::RouteRect &r, int width, int height) {
    const int left = std::clamp(r.x, 0, width), top = std::clamp(r.y, 0, height);
    const int right = std::clamp(r.x + r.width, 0, width), bottom = std::clamp(r.y + r.height, 0, height);
    return {left, top, right - left, bottom - top};
}

// How far an Expand grows the canvas: `side` (left, right, top or bottom) by `percent` of the picture's side along that
// direction, in whole multiples of 16, at least 16; or by `pixels` when the request gives them. The rectangle of the
// grown canvas in the old canvas's pixels. Empty, with `why`, for a side that is not one of the four.
std::optional<canvas::RouteRect> expandedBounds(const QVariantMap &expand, int width, int height, QString *why) {
    const QString side = expand.value("side").toString();
    const bool horizontal = side == QLatin1String("left") || side == QLatin1String("right");
    if (!horizontal && side != QLatin1String("top") && side != QLatin1String("bottom")) {
        *why = CanvasActions::tr("Pick the side to grow: left, right, top or bottom.");
        return std::nullopt;
    }
    int grow = expand.value("pixels").toInt();
    if (grow <= 0)
        grow = std::max(16, int(std::lround(double(horizontal ? width : height) * expand.value("percent", 25).toDouble() / 100.0 / 16.0)) * 16);
    canvas::RouteRect to{0, 0, width, height};
    if (side == QLatin1String("left")) {
        to.x = -grow;
        to.width += grow;
    } else if (side == QLatin1String("right")) {
        to.width += grow;
    } else if (side == QLatin1String("top")) {
        to.y = -grow;
        to.height += grow;
    } else {
        to.height += grow;
    }
    return to;
}

// The size of a canvas at a revision, and that revision in full.
struct Shown {
    QString revision;
    int width = 0, height = 0;
};
Result<Shown> shownOf(const QString &canvasId, const QString &ref) {
    const canvas::CanvasStore store = canvas::CanvasStore::forSession();
    const Result<QString> full = store.resolve(canvasId, ref.isEmpty() ? QStringLiteral("head") : ref);
    if (!full)
        return full.errors();
    const Result<canvas::Revision> revision = store.revision(canvasId, full.value());
    if (!revision)
        return revision.errors();
    const Result<canvas::Tree> tree = store.tree(revision->tree);
    if (!tree)
        return tree.errors();
    return Shown{full.value(), tree->width, tree->height};
}

} // namespace

CanvasActions::CanvasActions(RunService *service, const Registry *registry, Workflows *workflows, QObject *parent)
    : QObject(parent), m_service(service), m_registry(registry), m_workflows(workflows), m_roots{bundledRoot()},
      m_historyPath(stateDir() + QStringLiteral("/canvas-prompts.json")) {
    connect(m_service, &RunService::event, this, &CanvasActions::onEvent);
    if (m_workflows) {
        connect(m_workflows, &Workflows::templatesChanged, this, [this] { refresh(); });
        connect(m_workflows->downloads(), &ModelDownloads::changed, this, &CanvasActions::downloadsChanged);
        // The models may be here now.
        connect(m_workflows->downloads(), &ModelDownloads::finished, this, [this] { refresh(); });
    }
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
    m_entries.clear();
    // Where an action may come from, in order, the first of an id winning: the templates of the library (the person's own
    // first, a template of the library being how a person's own and a project's actions arrive, design 8), then the files
    // of the roots (the ones compiled into Fugue; they stay the fallback of a library that lacks them).
    struct Source {
        QString id, file, title;
        bool library;
    };
    QList<Source> sources;
    if (m_workflows)
        for (const Template &t : m_workflows->templateList())
            sources.append({t.id, t.workflow, t.title, true});
    for (const QString &root : std::as_const(m_roots))
        for (const QString &name : QDir(root).entryList({QStringLiteral("*.fugue")}, QDir::Files, QDir::Name))
            sources.append({QFileInfo(name).completeBaseName(), QDir(root).filePath(name), {}, false});
    QSet<QString> seen;
    for (const Source &source : std::as_const(sources)) {
        if (seen.contains(source.id))
            continue;
        QFile file(source.file);
        const Result<Document> loaded = file.open(QIODevice::ReadOnly) ? Document::load(file.readAll())
                                                                      : Result<Document>(makeError("file_not_found", source.file));
        if (!loaded)
            continue; // not a workflow: not an action either
        Entry entry;
        entry.id = source.id;
        entry.file = source.file;
        entry.library = source.library;
        entry.document = loaded.value();
        const Result<std::optional<canvas::Action>> action = canvas::actionOf(entry.document);
        if (action && !action->has_value())
            continue; // a plain workflow: it never hides an action of the same id
        seen.insert(entry.id);
        if (!action) {
            entry.broken = action.error().code == QLatin1String("unsupported_format_version") ? tr("Needs a newer Fugue.") : action.error().message;
        } else {
            entry.action = **action;
            const Result<canvas::ActionGraph> graph = canvas::checkAction(**action, entry.document, *m_registry);
            if (graph)
                entry.graph = graph.value();
            else
                entry.broken = graph.error().message;
        }
        m_entries << entry;
    }
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
    for (const Entry &e : std::as_const(m_entries)) {
        QVariantMap row{{"id", e.id},
                        {"name", e.action ? e.action->name : e.id},
                        {"description", e.action ? e.action->description : QString()},
                        {"glyph", e.action && !e.action->glyph.isEmpty() ? e.action->glyph : QStringLiteral("sparkles")},
                        {"selection", e.action ? e.action->selection : QStringLiteral("required")},
                        {"needsPrompt", e.action ? e.action->needsPrompt() : false},
                        {"hasPrompt", e.action ? e.action->input(QStringLiteral("prompt")) != nullptr : false},
                        {"broken", !e.broken.isEmpty()},
                        {"reason", e.broken},
                        {"missing", 0},
                        {"total", 0},
                        {"download", QString()},
                        {"size", QString()}};
        if (m_workflows) {
            const QVariantMap missing = m_workflows->missingModels(e.document.models());
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

QString CanvasActions::plainWords(const Error &error, const QString &actionName) {
    if (error.code == QLatin1String("mask_empty"))
        return tr("The selection is empty. Draw one, then Generate.");
    if (cannotRunNow(error.code))
        return tr("No model on this machine can run %1.").arg(actionName);
    if (error.code == QLatin1String("out_of_memory"))
        return tr("Not enough GPU memory. Try a smaller selection.");
    if (error.code == QLatin1String("stitch_mismatch"))
        return tr("The action's workflow returned a picture of another shape.");
    if (error.code == QLatin1String("canvas_conflict") || error.code == QLatin1String("canvas_head_moved"))
        return tr("The canvas changed size while this ran, so nothing was written. Run again.");
    return error.message;
}

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
    const Result<Shown> shown = shownOf(canvasId, revision);
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
    if (busy())
        return refuse(tr("Generate is already running: wait for it, or Cancel it."));
    setError({}, {});
    m_notice.clear();
    const QString canvasId = request.value("canvas").toString();
    if (canvasId.isEmpty())
        return refuse(tr("Open a canvas first."));
    const Result<Shown> shown = shownOf(canvasId, request.value("revision").toString());
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
    m_actionName = chosen->action ? chosen->action->name : id;
    if (!chosen->broken.isEmpty()) {
        m_errorCode = QStringLiteral("action_invalid");
        return refuse(chosen->broken);
    }
    const canvas::Action &action = *chosen->action;
    // An action on the canvas bounds (Expand) ignores the selection: it grows the canvas by the side and the amount asked for.
    const bool bounds = action.selection == QLatin1String("bounds");
    std::optional<canvas::RouteRect> grown;
    if (bounds) {
        QString why;
        grown = expandedBounds(request.value("expand").toMap(), shown->width, shown->height, &why);
        if (!grown)
            return refuse(why);
        rect.reset();
    }
    if (rect) {
        rect = clamped(*rect, shown->width, shown->height);
        in.selection = rect;
        routed = canvas::routeAction(in);
    }
    if (action.selection == QLatin1String("required") && !rect && selection.isEmpty() && !bounds)
        return refuse(tr("Select an area first."));
    if (rect && routed.action == canvas::RouteAction::None) {
        m_errorCode = QStringLiteral("mask_empty");
        return refuse(tr("The selection is empty. Draw one, then Generate."));
    }
    if (action.needsPrompt() && prompt.isEmpty())
        return refuse(tr("Type what to put in the selection, then Generate."));
    if (m_workflows) {
        const QVariantMap missing = m_workflows->missingModels(chosen->document.models());
        if (missing.value("count").toInt() > 0) {
            m_errorCode = QStringLiteral("model_missing");
            m_errorAction = id;
            return refuse(tr("%1 needs models that are not on this machine. Download them first.").arg(m_actionName));
        }
    }

    canvas::ActionBinding binding;
    binding.canvas = canvasId;
    binding.revision = shown->revision;
    binding.rect = rect;
    binding.selection = bounds ? QString() : selection;
    binding.layer = request.value("layer").toString().trimmed();
    if (routed.action != canvas::RouteAction::None) {
        binding.padding = routed.padding;
        binding.scale = routed.scale;
    }
    // Remove continues its surroundings, so it wants the router's Remove margin even when a prompt routed the pick elsewhere.
    if (id == QLatin1String("remove") && !bounds)
        binding.padding = std::max(binding.padding.value_or(0), canvas::RouteTuning{}.removeMinPadding);
    if (bounds) {
        // The canvas grows first (one revision, undone in one step, with the new area pre-filled as a layer), then the
        // action's Region reads the grown canvas: the new strip plus the overlap, with the router's context beside it.
        canvas::RouteInput grow;
        grow.canvasWidth = shown->width;
        grow.canvasHeight = shown->height;
        grow.resize = grown;
        grow.hasPrompt = !prompt.isEmpty();
        const canvas::Route expanded = canvas::routeAction(grow);
        const Result<canvas::Commit> done = canvas::CanvasStore::forSession().resize(
            canvasId, shown->revision, mask::IRect{grown->x, grown->y, grown->width, grown->height}, edgefill::Mode::EdgeBlur,
            edgefill::defaultBlur, QStringLiteral("action:%1").arg(id));
        if (!done)
            return refuse(done.error().message);
        if (!done->move.moved) {
            m_errorCode = QStringLiteral("canvas_head_moved");
            return refuse(tr("%1 works on the latest version of the canvas.").arg(m_actionName));
        }
        binding.revision = done->revision.id;
        binding.rect = expanded.fill;
        binding.padding = expanded.padding;
        binding.scale = expanded.scale;
    }
    if (action.input(QStringLiteral("prompt")))
        binding.values.insert(QStringLiteral("prompt"), prompt);
    if (action.input(QStringLiteral("seed"))) {
        const QVariant seed = request.value("seed");
        binding.values.insert(QStringLiteral("seed"), seed.isValid() && !seed.isNull() ? QJsonValue(seed.toLongLong())
                                                                                       : QJsonValue(qint64(QRandomGenerator::global()->generate())));
    }
    const Result<Document> bound = canvas::bindAction(chosen->document, action, chosen->graph, binding);
    if (!bound)
        return refuse(bound.error().message);

    RunRequest run;
    run.document = bound.value();
    run.workflow = chosen->file;
    const QString started = m_service->start(run);
    m_runs << started;
    if (binding.rect) {
        // Where a step picture of this run goes: the Region's context box (the selection grown by padding, raised for the
        // feather, kept in the canvas), and the selection inside it.
        const Node &regionNode = bound.value().graph.nodes[chosen->graph.region];
        const auto literal = [&](const char *name) { return regionNode.inputs.value(QLatin1String(name)).literal().toInt(); };
        const int width = bounds && grown ? grown->width : shown->width, height = bounds && grown ? grown->height : shown->height;
        const canvas::RouteRect &sel = *binding.rect;
        const auto box = region::contextOf(sel.x, sel.y, sel.width, sel.height, literal("padding"), width, height, literal("feather"));
        const int left = std::max(box.x, sel.x), top = std::max(box.y, sel.y);
        const int right = std::min(box.x + box.width, sel.x + sel.width), bottom = std::min(box.y + box.height, sel.y + sel.height);
        const auto map = [](int x, int y, int w, int h) { return QVariantMap{{"x", x}, {"y", y}, {"width", w}, {"height", h}}; };
        if (right > left && bottom > top)
            m_liveRects.insert(started, LiveRect{map(box.x, box.y, box.width, box.height), map(left, top, right - left, bottom - top)});
    }
    m_canvas = canvasId;
    m_action = id;
    m_progress = -1;
    m_lastRun = started;
    if (!prompt.isEmpty() && action.input(QStringLiteral("prompt")))
        remember(id, prompt);
    emit stateChanged();
    emit progressChanged();
    return {{"ok", true}, {"run", started}, {"action", id}};
}

Result<canvas::BatchAction> CanvasActions::batchAction(const QString &id) const {
    const Entry *chosen = entry(id);
    if (!chosen)
        return makeError(QStringLiteral("file_not_found"), tr("There is no action %1.").arg(id));
    if (!chosen->broken.isEmpty() || !chosen->action)
        return makeError(QStringLiteral("action_invalid"), chosen->broken);
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
    if (status == QLatin1String("ok")) {
        for (const WriteResult &write : writes)
            if (write.cached && !write.revision.isEmpty())
                placeAgain(write);
    } else if (status != QLatin1String("cancelled")) {
        const auto hard = std::find_if(errors.cbegin(), errors.cend(), [](const Error &e) { return e.severity == Error::Severity::Error; });
        const Error first = hard != errors.cend() ? *hard : makeError(QStringLiteral("node_failed"), tr("%1 did not finish.").arg(m_actionName));
        // A missing model offers the download; nothing else does.
        setError(plainWords(first, m_actionName), first.code, cannotRunNow(first.code) ? m_action : QString());
    }
    emit stateChanged();
    emit progressChanged();
    emit finished(run, status);
}

void CanvasActions::placeAgain(const WriteResult &write) {
    canvas::CanvasStore store = canvas::CanvasStore::forSession();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const Result<canvas::Accepted> placed = store.accept(write.canvas, write.revision);
        if (placed && placed->moved)
            return;
        if (placed)
            continue; // the head moved meanwhile: try again on the new one
        const QString code = placed.error().code;
        if (code == QLatin1String("canvas_not_candidate")) {
            // Still on the head: the cache was right, and nothing new can come of the same prompt, seed and selection.
            QString layer;
            if (const Result<canvas::Revision> revision = store.revision(write.canvas, write.revision))
                if (const Result<canvas::Tree> tree = store.tree(revision->tree); tree && !tree->layers.empty())
                    layer = tree->layers.back().name;
            m_notice = tr("Same prompt, seed and selection as “%1”. Change the seed to get another.").arg(layer);
        } else {
            setError(plainWords(placed.error(), m_actionName), code);
        }
        return;
    }
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

void CanvasActions::download(const QString &action) {
    const Entry *chosen = entry(action);
    if (!chosen || !m_workflows)
        return;
    m_workflows->downloadModels(chosen->document.models(), QStringLiteral("action:") + action);
    emit downloadsChanged();
}

} // namespace fugue
