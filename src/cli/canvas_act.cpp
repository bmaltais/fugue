// `fugue canvas act <canvas> <action> ...` and `fugue canvas actions` (#231 slice 10): a canvas action, the ones the Canvas view's
// task bar offers, run from the command line the way the bar's Generate runs it. The binding (the selection, Expand's growth of the
// canvas, the layer role, the seeds of Count N, the hidden variations) is engine/canvas_act.h's planAct, the same code the bar calls
// (canvas/canvas_actions.h); this reads the arguments, queues each run the way `fugue run` does, waits for them with --wait, and says
// which layers (or which new canvas) came of it.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

#include "cli/canvas_run.h"
#include "cli/commands.h"
#include "cli/runs.h"
#include "engine/canvas_act.h"
#include "engine/canvas_store.h"
#include "engine/file_hashes.h"
#include "engine/mask_paint.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/queue.h"
#include "engine/registry.h"
#include "engine/templates.h"

namespace fugue::cli {

namespace {

using canvas::ActionEntry;

Error usageProblem(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

// The actions of this machine: a file named on the command line first, then the template library, then the ones compiled into Fugue.
QList<ActionEntry> allActions(Context &context, const QString &file = {}) {
    QList<canvas::ActionSource> sources;
    if (!file.isEmpty())
        sources.append({QFileInfo(file).completeBaseName(), QFileInfo(file).absoluteFilePath(), false, {}});
    for (const Template &t : TemplateLibrary(TemplateLibrary::standardRoots(), &context.registry).list())
        sources.append({t.id, t.workflow, true, t.carriedPacks});
    const QString root = QStringLiteral(":/fugue/actions");
    for (const QString &name : QDir(root).entryList({QStringLiteral("*.fugue")}, QDir::Files, QDir::Name))
        sources.append({QFileInfo(name).completeBaseName(), QDir(root).filePath(name), false, {}});
    return canvas::collectActions(sources, context.registry);
}

const ActionEntry *find(const QList<ActionEntry> &entries, const QString &id) {
    for (const ActionEntry &e : entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

QList<MissingModel> missingModels(const Document &document, Context &context) {
    FileHashes hashes(FileHashes::defaultPath());
    return ModelLibrary::current(&context.registry).missing(document.models(), hashes);
}

// The resource type a block input's targets take (the handle each holds in the action's workflow, else the port's declared type), or
// nothing when it is not a model input.
std::optional<PortType> resourceTypeOf(const ActionEntry &e, const canvas::ActionInput &input, const Registry &registry) {
    for (const PortRef &target : input.targets) {
        const auto node = e.document.graph.nodes.constFind(target.node);
        if (node == e.document.graph.nodes.cend())
            continue;
        const InputValue held = node->inputs.value(target.port);
        if (held.kind() == InputValue::Kind::Resource)
            if (const std::optional<PortType> type = PortType::parse(held.handle().value(QStringLiteral("resource")).toString()))
                return type;
        if (const NodeType *type = registry.nodeType(node->type))
            if (const InputSpec *spec = type->input(target.port); spec && registry.portKind(spec->type.type) == PortKind::Resource)
                return spec->type.type;
    }
    return std::nullopt;
}

// A model picked by its file name, as the task bar's picker does: the model files of `type` (the model folders, as `fugue model list`
// shows them) whose name is `word` or whose file name is, hashed once (the hash is remembered, so the next run is quick). The handle
// the file's content names, or a plain error that lists the names there are.
Result<QJsonObject> modelByName(const PortType &type, const QString &word, Context &context) {
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary library = ModelLibrary::current(&context.registry);
    const QList<ModelFile> files = library.list(type);
    const auto same = [&](const ModelFile &file, Qt::CaseSensitivity sensitivity) {
        return file.name.compare(word, sensitivity) == 0 || QFileInfo(file.path).fileName().compare(word, sensitivity) == 0;
    };
    QList<ModelFile> found;
    for (const Qt::CaseSensitivity sensitivity : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
        for (const ModelFile &file : files)
            if (same(file, sensitivity))
                found << file;
        if (!found.isEmpty())
            break;
    }
    if (found.size() > 1) {
        Error error = usageProblem(QStringLiteral("%1 names %2 model files; give the name with its folder.").arg(word).arg(found.size()));
        for (const ModelFile &file : std::as_const(found))
            error.valid.append(file.name);
        return Result<QJsonObject>(error);
    }
    if (found.isEmpty()) {
        Error error = usageProblem(QStringLiteral("There is no %1 model file named \"%2\" in the model folders.").arg(type.toString(), word),
                                   QStringLiteral("`fugue model list --type %1` lists them; a full resource handle also works.").arg(type.toString()));
        for (const ModelFile &file : files)
            error.valid.append(file.name);
        return Result<QJsonObject>(error);
    }
    const QString hash = hashes.hash(found.first().path);
    if (hash.isEmpty())
        return Result<QJsonObject>(makeError(QStringLiteral("media_unreadable"), QStringLiteral("%1 could not be read.").arg(found.first().path)));
    hashes.save();
    return diskHandle(type.toString(), found.first().name, hash);
}

QString nameOf(const ActionEntry &e) { return e.action ? e.action->name : e.id; }

// The inputs of the block, as a list and as the words of --help for each.
QJsonArray inputsJson(const ActionEntry &e) {
    QJsonArray out;
    if (!e.action)
        return out;
    for (const canvas::ActionInput &input : e.action->inputs) {
        QJsonObject row{{"key", input.key()}, {"label", input.label.isEmpty() ? input.key() : input.label}, {"advanced", canvas::inPopover(input)}};
        if (input.choices)
            row.insert("choices", QJsonArray::fromStringList(*input.choices));
        if (!input.defaultValue.isUndefined())
            row.insert("default", input.defaultValue);
        out.append(row);
    }
    return out;
}

QString download(const ActionEntry &e) {
    return QStringLiteral("`fugue model get %1` fetches them").arg(e.id);
}

} // namespace

Reply canvasActions(const Command &, Context &context) {
    const QList<ActionEntry> entries = allActions(context);
    QJsonArray listed;
    QStringList lines;
    for (const ActionEntry &e : entries) {
        const QList<MissingModel> absent = missingModels(e.document, context);
        const int total = int(e.document.models().size());
        const QString pack = e.needPacks.isEmpty() ? QString() : e.needPacks.first().toMap().value("name").toString();
        QStringList needs;
        if (total > 0)
            needs << (absent.isEmpty() ? QStringLiteral("%1 model(s), here").arg(total) : QStringLiteral("%1 of %2 model(s) missing").arg(absent.size()).arg(total));
        if (!pack.isEmpty())
            needs << QStringLiteral("the %1 pack (not installed)").arg(pack);
        const QString selection = e.action ? e.action->selection : QStringLiteral("required");
        QJsonObject row{{"id", e.id},
                        {"name", nameOf(e)},
                        {"description", e.action ? e.action->description : QString()},
                        {"selection", selection},
                        {"needs_prompt", e.action && e.action->needsPrompt()},
                        {"has_seed", e.action && e.action->input(QStringLiteral("seed")) != nullptr},
                        {"has_layer", e.action && e.action->input(QStringLiteral("layer")) != nullptr},
                        {"needs", QJsonObject{{"models", total}, {"models_missing", int(absent.size())}, {"pack", pack}}},
                        {"broken", !e.broken.isEmpty()},
                        {"inputs", inputsJson(e)}};
        if (!e.broken.isEmpty())
            row.insert("reason", e.broken);
        listed.append(row);
        QStringList keys;
        for (const QJsonValue &input : inputsJson(e))
            keys << input.toObject().value("key").toString();
        lines << QStringLiteral("%1  %2  selection: %3%4  needs: %5  inputs: %6")
                     .arg(e.id, nameOf(e), selection, e.action && e.action->needsPrompt() ? QStringLiteral(", prompt") : QString(),
                          needs.isEmpty() ? QStringLiteral("nothing") : needs.join(QStringLiteral(", ")), keys.join(QStringLiteral(", ")))
                     .append(e.broken.isEmpty() ? QString() : QStringLiteral("  BROKEN: ") + e.broken);
    }
    return Reply::success(QJsonObject{{"actions", listed}}, lines.join('\n'));
}

namespace {

// The words that name a layer: its position (0 is the bottom) or its exact name. The name, or why not.
Result<QString> layerName(const canvas::Tree &tree, const QString &word) {
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,4}$"));
    if (digits.match(word).hasMatch()) {
        const int at = word.toInt();
        if (at >= int(tree.layers.size()))
            return Result<QString>(usageProblem(QStringLiteral("The canvas has %1 layer(s): there is no layer at position %2.").arg(tree.layers.size()).arg(at),
                                                QStringLiteral("`fugue canvas layer list <canvas>` lists them, 0 the bottom.")));
        return tree.layers[size_t(at)].name;
    }
    int found = 0;
    for (const canvas::LayerRecord &layer : tree.layers)
        found += layer.name == word;
    if (found != 1)
        return Result<QString>(usageProblem(found == 0 ? QStringLiteral("No layer is named \"%1\".").arg(word)
                                                       : QStringLiteral("%1 layers are named \"%2\"; give the one you mean by its position.").arg(found).arg(word),
                                            QStringLiteral("`fugue canvas layer list <canvas>` lists the layers with their positions, 0 the bottom.")));
    return word;
}

// A value as a JSON literal when it parses as one (a number, true, "text"), else the plain text: as `fugue run --set`.
QJsonValue literalOf(const QString &raw) {
    const QJsonDocument parsed = QJsonDocument::fromJson("[" + raw.toUtf8() + "]");
    return parsed.isArray() && parsed.array().size() == 1 ? parsed.array().at(0) : QJsonValue(raw);
}

Result<qint64> wholeNumber(const Command &command, const QString &name, qint64 least, qint64 most) {
    bool ok = false;
    const qint64 value = command.value(name).toLongLong(&ok);
    if (!ok || value < least || value > most)
        return Result<qint64>(usageProblem(QStringLiteral("--%1 is a whole number from %2 to %3.").arg(name).arg(least).arg(most), command.value(name)));
    return value;
}

void cancelBound(const RunFolder &folder, Context &context) {
    QueueEntry entry;
    const Queue queue(context.queueDir);
    cancelRun(folder, queue.find(folder.id(), &entry) ? std::optional<QueueEntry>(entry) : std::nullopt, context);
}

} // namespace

Reply canvasAct(const Command &command, Context &context) {
    if (command.has("list") || command.positionals.isEmpty()) {
        if (!command.positionals.isEmpty() || (command.options.size() > (command.has("list") ? 1 : 0)))
            return Reply::failure(usageProblem(QStringLiteral("--list takes no canvas or action."), QStringLiteral("`fugue canvas act --list`, or `fugue canvas actions`.")));
        return canvasActions(command, context);
    }
    if (command.positionals.size() != 2)
        return Reply::failure(usageProblem(QStringLiteral("`fugue canvas act` takes a canvas and an action."), QStringLiteral("`fugue canvas act --list` shows the actions.")));
    const QString canvasId = command.positionals.at(0).section(QLatin1Char('@'), 0, 0);
    const QString revisionWord = command.positionals.at(0).contains(QLatin1Char('@')) ? command.positionals.at(0).section(QLatin1Char('@'), 1) : QStringLiteral("head");
    const QString actionId = command.positionals.at(1);

    const QList<ActionEntry> entries = allActions(context, QFileInfo(actionId).isFile() ? actionId : QString());
    const ActionEntry *chosen = find(entries, QFileInfo(actionId).isFile() ? QFileInfo(actionId).completeBaseName() : actionId);
    if (!chosen) {
        Error error = makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no action %1.").arg(actionId),
                                QStringLiteral("`fugue canvas actions` lists the actions; give one's id (or a workflow file with an `action` block)."));
        for (const ActionEntry &e : entries)
            error.valid.append(e.id);
        return Reply::failure(error);
    }
    if (const std::optional<Error> why = canvas::notRunnable(*chosen)) {
        Error error = *why;
        if (error.code == QLatin1String("pack_missing") && chosen->needPacks.first().toMap().value("source").toString().isEmpty() == false)
            error.hint = QStringLiteral("`fugue pack add %1` installs it.").arg(chosen->needPacks.first().toMap().value("source").toString());
        return Reply::failure(error);
    }
    const canvas::Action &action = *chosen->action;

    // The canvas and the revision that is shown.
    const Result<canvas::Shown> shown = canvas::shownOf(canvasId, revisionWord);
    if (!shown)
        return Reply::failure(shown.errors());
    canvas::CanvasStore store = canvas::CanvasStore::forSession();
    const Result<canvas::Revision> shownRevision = store.revision(canvasId, shown->revision);
    if (!shownRevision)
        return Reply::failure(shownRevision.errors());
    const Result<canvas::Tree> tree = store.tree(shownRevision->tree);
    if (!tree)
        return Reply::failure(tree.errors());

    canvas::ActRequest request;
    request.canvas = canvasId;
    request.revision = shown->revision;
    request.prompt = command.value("prompt");
    if (command.has("seed")) {
        const Result<qint64> seed = wholeNumber(command, QStringLiteral("seed"), 0, Q_INT64_C(4294967295));
        if (!seed)
            return Reply::failure(seed.errors());
        request.seed = seed.value();
    }
    if (command.has("count")) {
        const Result<qint64> count = wholeNumber(command, QStringLiteral("count"), 1, canvas::maxActVariations);
        if (!count)
            return Reply::failure(count.errors());
        if (count.value() > 1 && !action.input(QStringLiteral("seed")))
            return Reply::failure(usageProblem(QStringLiteral("%1 has no seed, so --count %2 would make the same picture %2 times.").arg(action.name).arg(count.value()),
                                               QStringLiteral("Leave --count out.")));
        request.count = int(count.value());
    }
    if (command.has("layer")) {
        const Result<QString> name = layerName(tree.value(), command.value("layer"));
        if (!name)
            return Reply::failure(name.errors());
        (action.input(QStringLiteral("layer")) ? request.currentLayer : request.layer) = name.value();
    }
    const bool takesSelection = action.selection == QLatin1String("required") || action.selection == QLatin1String("optional");
    if (command.has("rect") && command.has("selection"))
        return Reply::failure(usageProblem(QStringLiteral("Give --rect or --selection, not both.")));
    QStringList notes;
    if (command.has("rect")) {
        static const QRegularExpression pattern(QStringLiteral("^(-?[0-9]{1,6}),(-?[0-9]{1,6}),([0-9]{1,6}),([0-9]{1,6})$"));
        const QRegularExpressionMatch match = pattern.match(command.value("rect"));
        if (!match.hasMatch() || match.captured(3).toInt() <= 0 || match.captured(4).toInt() <= 0)
            return Reply::failure(usageProblem(QStringLiteral("--rect is x,y,width,height in canvas pixels, such as 100,80,512,512."), command.value("rect")));
        request.rect = canvas::RouteRect{match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt(), match.captured(4).toInt()};
    }
    if (command.has("selection")) {
        QFile file(command.value("selection"));
        if (!file.open(QIODevice::ReadOnly))
            return Reply::failure(makeError(QStringLiteral("file_not_found"), QStringLiteral("The selection file %1 cannot be read.").arg(command.value("selection"))));
        const Result<mask::MaskPaint> painted = mask::MaskPaint::fromPng(file.readAll());
        if (!painted)
            return Reply::failure(painted.errors());
        if (painted->width() != shown->width || painted->height() != shown->height)
            return Reply::failure(usageProblem(QStringLiteral("The selection is %1 x %2 but the canvas is %3 x %4.").arg(painted->width()).arg(painted->height()).arg(shown->width).arg(shown->height),
                                               QStringLiteral("Make the mask the size of the canvas: white where selected.")));
        int left = painted->width(), top = painted->height(), right = -1, bottom = -1;
        for (int y = 0; y < painted->height(); ++y)
            for (int x = 0; x < painted->width(); ++x)
                if (painted->at(x, y) > 0) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
        if (right < 0)
            return Reply::failure(makeError(QStringLiteral("mask_empty"), QStringLiteral("The selection is empty. Draw one, then Generate."), QStringLiteral("White is selected: the file has no white.")));
        const Result<QString> hash = store.storeSelection(painted.value());
        if (!hash)
            return Reply::failure(hash.errors());
        request.selection = hash.value();
        request.rect = canvas::RouteRect{left, top, right - left + 1, bottom - top + 1};
    }
    if ((command.has("rect") || command.has("selection")) && !takesSelection)
        notes << QStringLiteral("%1 works on the whole picture or the canvas bounds: --rect and --selection are left out.").arg(action.name);
    const bool bounds = action.selection == QLatin1String("bounds");
    if (command.has("expand") || command.has("percent") || command.has("pixels")) {
        if (!bounds)
            return Reply::failure(usageProblem(QStringLiteral("--expand, --percent and --pixels are for an action that grows the canvas, such as expand; %1 does not.").arg(action.name)));
        if (command.has("percent") && command.has("pixels"))
            return Reply::failure(usageProblem(QStringLiteral("Give --percent or --pixels, not both.")));
        request.expand.insert("side", command.value("expand"));
        if (command.has("percent")) {
            const QString percent = command.value("percent");
            if (percent != QLatin1String("25") && percent != QLatin1String("50"))
                return Reply::failure(usageProblem(QStringLiteral("--percent is 25 or 50."), percent));
            request.expand.insert("percent", percent.toInt());
        }
        if (command.has("pixels")) {
            const Result<qint64> pixels = wholeNumber(command, QStringLiteral("pixels"), 1, 16384);
            if (!pixels)
                return Reply::failure(pixels.errors());
            request.expand.insert("pixels", int(pixels.value()));
        }
    }
    // --set <input>=<value> (an input of the action's block: `fugue canvas actions` lists them) or <node>.<input>=<value> (a node's
    // input in the run's copy of the workflow, as `fugue run --set`).
    for (const QString &set : command.options.value("set")) {
        const int equals = set.indexOf(QLatin1Char('='));
        if (equals < 1)
            return Reply::failure(usageProblem(QStringLiteral("--set takes <input>=<value> or <node>.<input>=<value>, not \"%1\".").arg(set)));
        const QString key = set.left(equals), raw = set.mid(equals + 1);
        const int dot = key.indexOf(QLatin1Char('.'));
        if (dot > 0) {
            request.nodeSets.append({key.left(dot), key.mid(dot + 1), literalOf(raw)});
            continue;
        }
        const canvas::ActionInput *input = action.input(key);
        if (!input || !canvas::inPopover(*input)) {
            Error error = usageProblem(QStringLiteral("%1 has no input \"%2\" to set.").arg(action.name, key),
                                       QStringLiteral("The prompt, seed and layer have their own options (--prompt, --seed, --layer)."));
            for (const canvas::ActionInput &each : action.inputs)
                if (canvas::inPopover(each))
                    error.valid.append(each.key());
            return Reply::failure(error);
        }
        if (input->choices && input->choiceValues) {
            const auto word = std::find_if(input->choices->cbegin(), input->choices->cend(), [&](const QString &c) { return c.compare(raw, Qt::CaseInsensitive) == 0; });
            if (word == input->choices->cend()) {
                Error error = usageProblem(QStringLiteral("\"%1\" is not one of the choices of %2.").arg(raw, key));
                for (const QString &c : *input->choices)
                    error.valid.append(c);
                return Reply::failure(error);
            }
            request.values.insert(key, *word);
        } else if (const std::optional<PortType> model = resourceTypeOf(*chosen, *input, context.registry);
                   model && !literalOf(raw).isObject()) {
            // A model input: its file name (the way the bar's picker names it), unless a whole resource handle was given.
            const Result<QJsonObject> handle = modelByName(*model, raw, context);
            if (!handle)
                return Reply::failure(handle.errors());
            request.values.insert(key, handle.value());
        } else {
            request.values.insert(key, literalOf(raw));
        }
    }

    // The models it would run with must be here before anything is changed (Expand grows the canvas first).
    request.beforeChange = [&]() -> std::optional<Error> {
        const QList<MissingModel> absent = missingModels(canvas::withValues(*chosen, request.values), context);
        if (absent.isEmpty())
            return std::nullopt;
        QStringList names;
        for (const MissingModel &model : absent)
            names << model.handle.value(QStringLiteral("name")).toString();
        Error error = makeError(QStringLiteral("model_missing"),
                                QStringLiteral("%1 needs %2 model(s) that are not on this machine: %3.").arg(action.name).arg(absent.size()).arg(names.join(QStringLiteral(", "))),
                                download(*chosen) + QStringLiteral(" (or put the files in the models folder)."));
        return error;
    };

    const Result<canvas::ActPlan> plan = canvas::planAct(*chosen, request);
    if (!plan)
        return Reply::failure(plan.errors());

    for (const QString &note : std::as_const(notes))
        context.log(note);
    // Queue every run (they wait for the one GPU in order); the canvas is untouched until a Canvas Write lands.
    QList<RunFolder> runs;
    for (const Document &bound : plan->runs) {
        const Result<RunFolder> folder = startBoundRun(bound, chosen->file, context);
        if (!folder) {
            for (const RunFolder &started : std::as_const(runs))
                cancelBound(started, context);
            return Reply::failure(folder.errors());
        }
        runs.append(folder.value());
    }
    QJsonArray runJson;
    for (int n = 0; n < runs.size(); ++n) {
        QJsonObject row{{"run", runs.at(n).id()}};
        if (const canvas::ActionInput *seed = action.input(QStringLiteral("seed")); seed && !seed->targets.isEmpty()) {
            const PortRef &target = seed->targets.first();
            row.insert("seed", plan->runs.at(n).graph.nodes.value(target.node).inputs.value(target.port).literal());
        }
        runJson.append(row);
    }
    QJsonObject data{{"canvas", canvasId}, {"action", chosen->id}, {"previous", shown->revision}, {"runs", runJson}};
    if (plan->grew) {
        data.insert("expanded", QJsonObject{{"revision", plan->growRevision}, {"width", plan->width}, {"height", plan->height}});
    }
    if (!command.has("wait")) {
        data.insert("status", QStringLiteral("queued"));
        QStringList ids;
        for (const RunFolder &run : std::as_const(runs))
            ids << run.id();
        return Reply::success(data, QStringLiteral("Queued %1 run(s) of %2 on %3: %4. `fugue runs status <run>` follows one; --wait blocks until they end.").arg(runs.size()).arg(action.name, canvasId, ids.join(QStringLiteral(", "))));
    }

    installStopHandlers();
    struct Written {
        QString canvas, revision;
        bool cached = false;
    };
    QList<Written> writes;
    QStringList made;
    QString status = QStringLiteral("ok");
    Error failure;
    int finished = 0;
    for (int n = 0; n < runs.size(); ++n) {
        int lastPercent = -10;
        QString lastNode;
        const std::function<void(const QJsonObject &)> onEvent = [&](const QJsonObject &event) {
            const QString kind = event.value("event").toString(), node = event.value("node").toString();
            const QString prefix = runs.size() > 1 ? QStringLiteral("[%1 of %2] ").arg(n + 1).arg(runs.size()) : QString();
            if (kind == QLatin1String("node.running")) {
                lastPercent = -10;
                context.log(prefix + node + QStringLiteral(" ..."));
            } else if (kind == QLatin1String("node.progress")) {
                const int percent = int(event.value("value").toDouble() * 100);
                if (node != lastNode || percent >= lastPercent + 10) {
                    lastNode = node;
                    lastPercent = percent;
                    context.log(prefix + node + QStringLiteral(" %1%").arg(percent));
                }
            } else if (kind == QLatin1String("node.preview")) {
                context.log(prefix + node + QStringLiteral(" preview frame"));
            } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
                const QJsonObject meta = event.value("outputs").toObject().value("canvas").toObject().value("meta").toObject();
                if (chosen->graph.writes.contains(node))
                    writes.append({meta.value("canvas").toString(), meta.value("revision").toString(), kind == QLatin1String("node.cached")});
                if (chosen->graph.news.contains(node) && kind == QLatin1String("node.done") && !meta.value("canvas").toString().isEmpty())
                    made << meta.value("canvas").toString();
                context.log(prefix + node + (kind == QLatin1String("node.cached") ? QStringLiteral(" (cached)") : QStringLiteral(" done")));
            }
        };
        const canvas::BatchRunResult result = waitForRun(runs.at(n), context, onEvent);
        if (result.status == QLatin1String("ok")) {
            ++finished;
            continue;
        }
        status = result.status;
        const Error raw = makeError(result.code.isEmpty() ? QStringLiteral("node_failed") : result.code, result.message);
        failure = makeError(result.status == QLatin1String("cancelled") ? QStringLiteral("cancelled") : raw.code,
                            result.status == QLatin1String("cancelled") ? QStringLiteral("Cancelled.") : canvas::plainWords(raw, action.name),
                            raw.message != canvas::plainWords(raw, action.name) ? raw.message : QString());
        for (int rest = n + 1; rest < runs.size(); ++rest) // one Generate ends as one
            cancelBound(runs.at(rest), context);
        break;
    }
    // A Write that came from the cache lands nothing: its revision goes on the head again (design decision 5).
    QString notice;
    for (const Written &write : std::as_const(writes))
        if (write.cached && !write.revision.isEmpty()) {
            const Result<canvas::PlacedAgain> placed = canvas::placeWriteAgain(write.canvas, write.revision);
            if (!placed)
                notice = canvas::plainWords(placed.error(), action.name);
            else if (!placed->moved && !placed->layer.isEmpty())
                notice = QStringLiteral("Same prompt, seed and selection as \"%1\". Change the seed to get another.").arg(placed->layer);
        }

    const Result<canvas::CanvasInfo> info = store.info(canvasId);
    QJsonArray layers;
    QString head = shown->revision;
    if (info && !info->head.isEmpty()) {
        head = info->head;
        const Result<canvas::Revision> after = store.revision(canvasId, head);
        const Result<canvas::Tree> afterTree = after ? store.tree(after->tree) : Result<canvas::Tree>(after.errors());
        if (afterTree) {
            QStringList before;
            for (const canvas::LayerRecord &layer : tree->layers)
                before << layer.name;
            for (const canvas::LayerRecord &layer : afterTree->layers)
                if (!before.removeOne(layer.name))
                    layers.append(QJsonObject{{"name", layer.name}, {"visible", layer.visible}});
        }
    }
    data.insert("status", status);
    data.insert("head", head);
    data.insert("layers", layers);
    if (!made.isEmpty())
        data.insert("canvases", QJsonArray::fromStringList(made));
    if (!notice.isEmpty())
        data.insert("notice", notice);
    if (status != QLatin1String("ok")) {
        failure.message += QStringLiteral(" (%1 of %2 run(s) finished.)").arg(finished).arg(runs.size());
        return Reply::failure(failure);
    }
    QStringList names;
    for (const QJsonValue &layer : layers)
        names << QStringLiteral("\"%1\"").arg(layer.toObject().value("name").toString());
    QString text = made.isEmpty() ? QStringLiteral("%1 on %2: head is now %3.").arg(action.name, canvasId, head)
                                  : QStringLiteral("%1 made canvas %2 (%3 is as it was).").arg(action.name, made.join(QStringLiteral(", ")), canvasId);
    if (!names.isEmpty())
        text += QStringLiteral(" New layer(s): %1.").arg(names.join(QStringLiteral(", ")));
    if (!notice.isEmpty())
        text += QLatin1Char(' ') + notice;
    return Reply::success(data, text);
}

} // namespace fugue::cli
