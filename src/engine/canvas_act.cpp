#include "engine/canvas_act.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "engine/canvas_router.h"
#include "engine/canvas_store.h"
#include "engine/edge_fill.h"
#include "engine/registry.h"

namespace fugue::canvas {

namespace {

Error refused(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("act_refused"), message, hint); }

// The rectangle held inside the canvas.
RouteRect clamped(const RouteRect &r, int width, int height) {
    const int left = std::clamp(r.x, 0, width), top = std::clamp(r.y, 0, height);
    const int right = std::clamp(r.x + r.width, 0, width), bottom = std::clamp(r.y + r.height, 0, height);
    return {left, top, right - left, bottom - top};
}

} // namespace

QVariantList packNeeds(const Document &document, const QList<CarriedPack> &carried, const Registry &registry) {
    QVariantList needs;
    for (const QString &pack : packsNeeded(document, registry)) {
        const auto folder = std::find_if(carried.cbegin(), carried.cend(), [&](const CarriedPack &c) { return c.id == pack; });
        const QJsonObject recorded = document.requirements.value(QStringLiteral("packs")).toObject().value(pack).toObject();
        QString source, ref, name = recorded.value(QStringLiteral("name")).toString();
        if (folder != carried.cend()) {
            source = folder->folder;
            if (!folder->name.isEmpty())
                name = folder->name;
        } else if (registry.packVersion(pack).isEmpty()) {
            source = recorded.value(QStringLiteral("source")).toString();
            ref = recorded.value(QStringLiteral("ref")).toString();
        }
        needs << QVariantMap{{"id", pack}, {"name", packTitle(pack, name)}, {"source", source}, {"ref", ref}};
    }
    return needs;
}

QList<ActionEntry> collectActions(const QList<ActionSource> &sources, const Registry &registry) {
    QList<ActionEntry> entries;
    QSet<QString> seen;
    for (const ActionSource &source : sources) {
        if (seen.contains(source.id))
            continue;
        QFile file(source.file);
        const Result<Document> loaded = file.open(QIODevice::ReadOnly) ? Document::load(file.readAll())
                                                                      : Result<Document>(makeError("file_not_found", source.file));
        if (!loaded)
            continue; // not a workflow: not an action either
        ActionEntry entry;
        entry.id = source.id;
        entry.file = source.file;
        entry.library = source.library;
        entry.document = loaded.value();
        entry.carried = source.carried;
        entry.needPacks = packNeeds(entry.document, source.carried, registry);
        const Result<std::optional<Action>> action = actionOf(entry.document);
        if (action && !action->has_value())
            continue; // a plain workflow: it never hides an action of the same id
        seen.insert(entry.id);
        if (!action) {
            entry.broken = action.error().code == QLatin1String("unsupported_format_version") ? QCoreApplication::translate("CanvasActions", "Needs a newer Fugue.") : action.error().message;
        } else {
            entry.action = **action;
            // The inputs that point into a node of a pack that is not installed wait for the pack: the action is listed with the
            // offer to install it, not as broken.
            Action visible = **action;
            if (!entry.needPacks.isEmpty())
                visible.inputs.removeIf([&](const ActionInput &input) {
                    return std::any_of(input.targets.cbegin(), input.targets.cend(), [&](const PortRef &target) {
                        const auto node = entry.document.graph.nodes.constFind(target.node);
                        return node != entry.document.graph.nodes.cend() && !entry.document.groups.contains(node->type) &&
                               !registry.nodeType(node->type);
                    });
                });
            const Result<ActionGraph> graph = checkAction(visible, entry.document, registry);
            if (graph)
                entry.graph = graph.value();
            else
                entry.broken = graph.error().message;
        }
        entries << entry;
    }
    return entries;
}

std::optional<Error> notRunnable(const ActionEntry &entry) {
    if (!entry.broken.isEmpty() || !entry.action)
        return makeError(QStringLiteral("action_invalid"), entry.broken);
    if (!entry.needPacks.isEmpty()) {
        const QString pack = entry.needPacks.first().toMap().value("name").toString();
        return makeError(QStringLiteral("pack_missing"), QCoreApplication::translate("CanvasActions", "%1 needs the %2 pack, which is not installed here. Install it first.").arg(entry.action->name, pack));
    }
    return std::nullopt;
}

bool inPopover(const ActionInput &input) {
    return input.role != QLatin1String("prompt") && input.role != QLatin1String("seed") && input.role != QLatin1String("layer");
}

Document withValues(const ActionEntry &entry, const QJsonObject &values) {
    Document document = entry.document;
    if (!entry.action)
        return document;
    for (const ActionInput &input : entry.action->inputs) {
        // What a run would use: the given value, else the block's default (else the file's own, which stays).
        const QJsonValue value = values.contains(input.key()) ? values.value(input.key()) : input.defaultValue;
        if (input.choiceValues || !inPopover(input) || value.isUndefined())
            continue;
        for (const PortRef &target : input.targets)
            if (document.graph.nodes.contains(target.node))
                document.graph.nodes[target.node].inputs.insert(target.port, InputValue::fromJson(value));
    }
    return document;
}

bool cannotRunNow(const QString &code) {
    static const QStringList codes{"capability_unavailable", "model_not_running", "model_missing", "provider_unreachable",
                                   "unknown_provider"};
    return codes.contains(code);
}

QString plainWords(const Error &error, const QString &actionName) {
    if (error.code == QLatin1String("mask_empty"))
        return QCoreApplication::translate("CanvasActions", "The selection is empty. Draw one, then Generate.");
    if (cannotRunNow(error.code))
        return QCoreApplication::translate("CanvasActions", "No model on this machine can run %1.").arg(actionName);
    if (error.code == QLatin1String("out_of_memory"))
        return QCoreApplication::translate("CanvasActions", "Not enough GPU memory. Try a smaller selection.");
    if (error.code == QLatin1String("input_invalid") && error.message.contains(QLatin1String(" is locked: ")))
        return QCoreApplication::translate("CanvasActions", "The layer is locked, so %1 cannot change its mask. Unlock it in the Layers panel, then try again.").arg(actionName);
    if (error.code == QLatin1String("stitch_mismatch"))
        return QCoreApplication::translate("CanvasActions", "The action's workflow returned a picture of another shape.");
    if (error.code == QLatin1String("canvas_conflict") || error.code == QLatin1String("canvas_head_moved"))
        return QCoreApplication::translate("CanvasActions", "The canvas changed size while this ran, so nothing was written. Run again.");
    return error.message;
}

Result<PlacedAgain> placeWriteAgain(const QString &canvas, const QString &revision) {
    CanvasStore store = CanvasStore::forSession();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const Result<Accepted> placed = store.accept(canvas, revision);
        if (placed && placed->moved)
            return PlacedAgain{true, {}};
        if (placed)
            continue; // the head moved meanwhile: try again on the new one
        if (placed.error().code == QLatin1String("canvas_not_candidate")) {
            // Still on the head: the cache was right, and nothing new can come of the same prompt, seed and selection.
            QString layer;
            if (const Result<Revision> found = store.revision(canvas, revision))
                if (const Result<Tree> tree = store.tree(found->tree); tree && !tree->layers.empty())
                    layer = tree->layers.back().name;
            return PlacedAgain{false, layer};
        }
        return placed.errors();
    }
    return PlacedAgain{false, {}};
}

Result<Shown> shownOf(const QString &canvasId, const QString &ref) {
    const CanvasStore store = CanvasStore::forSession();
    const Result<QString> full = store.resolve(canvasId, ref.isEmpty() ? QStringLiteral("head") : ref);
    if (!full)
        return full.errors();
    const Result<Revision> revision = store.revision(canvasId, full.value());
    if (!revision)
        return revision.errors();
    const Result<Tree> tree = store.tree(revision->tree);
    if (!tree)
        return tree.errors();
    return Shown{full.value(), tree->width, tree->height};
}

Result<RouteRect> expandedBounds(const QVariantMap &expand, int width, int height) {
    const QString side = expand.value("side").toString();
    const bool horizontal = side == QLatin1String("left") || side == QLatin1String("right");
    if (!horizontal && side != QLatin1String("top") && side != QLatin1String("bottom"))
        return refused(QCoreApplication::translate("CanvasActions", "Pick the side to grow: left, right, top or bottom."), QStringLiteral("Give --expand left|right|top|bottom."));
    int grow = expand.value("pixels").toInt();
    if (grow <= 0)
        grow = std::max(16, int(std::lround(double(horizontal ? width : height) * expand.value("percent", 25).toDouble() / 100.0 / 16.0)) * 16);
    RouteRect to{0, 0, width, height};
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

Result<ActPlan> planAct(const ActionEntry &entry, const ActRequest &request) {
    if (const std::optional<Error> why = notRunnable(entry))
        return *why;
    const Action &action = *entry.action;
    const QString id = entry.id;
    const Result<Shown> shown = shownOf(request.canvas, request.revision);
    if (!shown)
        return shown.errors();
    const QString prompt = request.prompt.trimmed();
    std::optional<RouteRect> rect = request.rect;

    RouteInput in;
    in.canvasWidth = shown->width;
    in.canvasHeight = shown->height;
    in.selection = rect;
    in.hasPrompt = !prompt.isEmpty();
    Route routed = routeAction(in);

    // An action on the canvas bounds (Expand) ignores the selection: it grows the canvas by the side and the amount asked for.
    const bool bounds = action.selection == QLatin1String("bounds");
    // `none` (Upscale) takes the whole picture: a rectangle or a painted selection is left out, not read.
    const bool whole = action.selection == QLatin1String("none");
    if (whole)
        rect.reset();
    std::optional<RouteRect> grown;
    if (bounds) {
        const Result<RouteRect> to = expandedBounds(request.expand, shown->width, shown->height);
        if (!to)
            return to.errors();
        grown = to.value();
        rect.reset();
    }
    if (rect) {
        rect = clamped(*rect, shown->width, shown->height);
        in.selection = rect;
        routed = routeAction(in);
    }
    if (action.selection == QLatin1String("required") && !rect && request.selection.isEmpty() && !bounds)
        return refused(QCoreApplication::translate("CanvasActions", "Select an area first."), QStringLiteral("Give --rect x,y,w,h or --selection <mask.png>."));
    if (rect && routed.action == RouteAction::None)
        return makeError(QStringLiteral("mask_empty"), QCoreApplication::translate("CanvasActions", "The selection is empty. Draw one, then Generate."));
    if (action.needsPrompt() && prompt.isEmpty())
        return refused(QCoreApplication::translate("CanvasActions", "Type what to put in the selection, then Generate."), QStringLiteral("Give --prompt <text>."));
    if (request.beforeChange)
        if (const std::optional<Error> why = request.beforeChange())
            return *why;

    ActionBinding binding;
    binding.canvas = request.canvas;
    binding.revision = shown->revision;
    binding.rect = rect;
    binding.selection = bounds || whole ? QString() : request.selection;
    binding.layer = request.layer.trimmed();
    // An action with a layer role works on the current layer (Remove background: its mask): that layer's name goes into every target
    // of the role (Canvas Region's Layer, Canvas Write's Onto), and Region keeps its own padding and scale: the layer whole, as it is.
    const bool onLayer = action.input(QStringLiteral("layer")) != nullptr;
    if (onLayer) {
        const QString current = request.currentLayer.trimmed();
        if (current.isEmpty())
            return refused(QCoreApplication::translate("CanvasActions", "Select a layer first: click one in the Layers panel."), QStringLiteral("Give --layer <name|position>."));
        binding.layer.clear();
        binding.values.insert(QStringLiteral("layer"), current);
    }
    if (routed.action != RouteAction::None && !onLayer) {
        binding.padding = routed.padding;
        binding.scale = routed.scale;
    }
    // Remove continues its surroundings, so it wants the router's Remove margin even when a prompt routed the pick elsewhere.
    if (id == QLatin1String("remove") && !bounds)
        binding.padding = std::max(binding.padding.value_or(0), RouteTuning{}.removeMinPadding);
    ActPlan plan;
    plan.canvas = request.canvas;
    plan.width = shown->width;
    plan.height = shown->height;
    if (bounds) {
        // The canvas grows first (one revision, undone in one step, with the new area pre-filled as a layer), then the
        // action's Region reads the grown canvas: the new strip plus the overlap, with the router's context beside it.
        RouteInput grow;
        grow.canvasWidth = shown->width;
        grow.canvasHeight = shown->height;
        grow.resize = grown;
        grow.hasPrompt = !prompt.isEmpty();
        const Route expanded = routeAction(grow);
        const Result<Commit> done = CanvasStore::forSession().resize(
            request.canvas, shown->revision, mask::IRect{grown->x, grown->y, grown->width, grown->height}, edgefill::Mode::EdgeBlur,
            edgefill::defaultBlur, QStringLiteral("action:%1").arg(id));
        if (!done)
            return done.errors();
        if (!done->move.moved)
            return makeError(QStringLiteral("canvas_head_moved"), QCoreApplication::translate("CanvasActions", "%1 works on the latest version of the canvas.").arg(action.name));
        binding.revision = done->revision.id;
        binding.rect = expanded.fill;
        binding.padding = expanded.padding;
        binding.scale = expanded.scale;
        plan.grew = true;
        plan.growRevision = done->revision.id;
        plan.width = grown->width;
        plan.height = grown->height;
    }
    plan.revision = binding.revision;
    plan.rect = binding.rect;
    if (action.input(QStringLiteral("prompt")))
        binding.values.insert(QStringLiteral("prompt"), prompt);
    for (auto it = request.values.begin(); it != request.values.end(); ++it)
        binding.values.insert(it.key(), it.value());
    // Count N: N runs of the same head and selection (an action with no seed role would repeat itself: one).
    const int total = action.input(QStringLiteral("seed")) ? std::clamp(request.count, 1, maxActVariations) : 1;
    for (int n = 0; n < total; ++n) {
        if (action.input(QStringLiteral("seed")))
            binding.values.insert(QStringLiteral("seed"), request.seed ? QJsonValue(*request.seed + n)
                                                                       : QJsonValue(qint64(QRandomGenerator::global()->generate())));
        binding.suffix = total > 1 ? QCoreApplication::translate("CanvasActions", "(%1 of %2)").arg(n + 1).arg(total) : QString();
        binding.hidden = n > 0;
        Result<Document> bound = bindAction(entry.document, action, entry.graph, binding);
        if (!bound)
            return bound.errors();
        for (const NodeSet &set : request.nodeSets) {
            const auto node = bound->graph.nodes.find(set.node);
            if (node == bound->graph.nodes.end())
                return refused(QStringLiteral("%1 has no node %2.").arg(action.name, set.node));
            node->inputs.insert(set.input, InputValue::fromJson(set.value));
        }
        plan.runs << bound.value();
    }
    return plan;
}

} // namespace fugue::canvas
