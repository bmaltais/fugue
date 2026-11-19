#include "cli/commands.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>

#include "cli/compile_step.h"
#include "cli/process.h"
#include "engine/queue.h"
#include "cli/runs.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/json.h"
#include "engine/run_folder.h"

namespace fugue::cli {

namespace {

QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// ---- nodes ----

QString humanProblem(const Error &error) {
    return QStringLiteral("%1 %2%3: %4")
        .arg(error.severity == Error::Severity::Warning ? "warning" : "error", error.code,
             error.node.isEmpty() ? QString() : " (" + error.node + ")", error.message);
}

QJsonObject nodeSummary(const NodeType &type) {
    QJsonObject summary{{"id", type.id}, {"version", type.version.toString()}, {"name", type.name},
                        {"category", type.category}, {"description", type.description}, {"sink", type.sink}};
    return summary;
}

Reply nodeList(const Command &command, Context &context) {
    std::optional<PortType> filter;
    if (command.has("type")) {
        filter = PortType::parse(command.value("type"));
        if (!filter) {
            Error error = makeError("usage", QStringLiteral("\"%1\" is not a port type.").arg(command.value("type")),
                                    "Use a name such as image, text or a pack's <namespace>/<name>.");
            return Reply::failure(error);
        }
    }
    QJsonArray nodes;
    QStringList lines;
    for (const NodeType *type : context.registry.nodeTypes()) {
        if (filter) {
            bool uses = false;
            for (const InputSpec &input : type->inputs)
                uses = uses || input.type.type == *filter;
            for (const OutputSpec &output : type->outputs)
                uses = uses || output.type.type == *filter;
            if (!uses)
                continue;
        }
        nodes.append(nodeSummary(*type));
        lines << QStringLiteral("%1  %2  %3").arg(type->id, type->version.toString(), type->name);
    }
    return Reply::success(QJsonObject{{"nodes", nodes}}, lines.join('\n'));
}

Reply nodeDescribe(const Command &command, Context &context) {
    QJsonArray nodes;
    Errors errors;
    for (const QString &id : command.positionals) {
        const NodeType *type = context.registry.nodeType(id);
        if (!type) {
            Error error = makeError("unknown_node_type", QStringLiteral("There is no node type %1.").arg(id),
                                    "`fugue node list` shows the installed types.");
            errors << error;
        } else {
            nodes.append(type->toJson());
        }
    }
    if (!errors.isEmpty())
        return Reply::failure(errors);
    return Reply::success(QJsonObject{{"nodes", nodes}}, QString::fromUtf8(writeJson(nodes)));
}

// ---- workflows ----

Reply loadFailed(const Errors &errors) {
    Errors list = errors;
    if (list.isEmpty())
        list << makeError("internal", "The document could not be loaded.");
    return Reply::failure(list);
}

Reply validate(const Command &command, Context &context) {
    Errors errors;
    const std::optional<Document> document = Document::loadFile(command.positionals.at(0), errors);
    if (!document)
        return loadFailed(errors);
    const Validation validation = validateDocument(*document, context);
    if (hasErrors(validation.errors))
        return Reply::failure(validation.errors);
    // The state of every node (10.3), for people too.
    QStringList lines{QStringLiteral("%1 is valid.").arg(command.positionals.at(0))};
    for (auto it = validation.nodes.begin(); it != validation.nodes.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        const QString reason = node.value("reason").toString();
        lines << QStringLiteral("  %1  %2%3").arg(it.key(), node.value("state").toString(),
                                                  reason.isEmpty() ? QString() : " (" + reason + ")");
    }
    Reply reply = Reply::success(QJsonObject{{"valid", true}, {"nodes", validation.nodes}}, lines.join('\n'));
    reply.errors = validation.errors; // warnings only
    return reply;
}

Reply compile(const Command &command, Context &context) {
    Errors errors;
    const std::optional<Document> document = Document::loadFile(command.positionals.at(0), errors);
    if (!document)
        return loadFailed(errors);
    const Compilation compilation = compileDocument(*document, context.registry);
    if (!compilation.ok())
        return Reply::failure(compilation.errors);
    const QJsonObject compiled = compilation.graph->toJson();
    Reply reply = Reply::success(compiled, QString::fromUtf8(writeJson(compiled)));
    reply.errors = compilation.errors; // warnings only
    return reply;
}

Reply workflowNew(const Command &command, Context &) {
    const QString path = command.positionals.at(0);
    if (QFileInfo::exists(path))
        return Reply::failure(makeError("usage", QStringLiteral("%1 already exists.").arg(path),
                                        "Choose a new file name; fugue does not overwrite."));
    Errors errors;
    if (!Document().saveFile(path, errors))
        return loadFailed(errors);
    return Reply::success(QJsonObject{{"file", path}}, QStringLiteral("Wrote %1.").arg(path));
}

Reply workflowDescribe(const Command &command, Context &context) {
    Errors errors;
    const std::optional<Document> document = Document::loadFile(command.positionals.at(0), errors);
    if (!document)
        return loadFailed(errors);
    QJsonArray nodes;
    QJsonArray links;
    QStringList lines;
    const Nodes &all = document->graph.nodes;
    for (auto it = all.cbegin(); it != all.cend(); ++it) {
        const NodeType *type = context.registry.nodeType(it->type);
        QJsonObject node{{"id", it.key()}, {"type", it->type}, {"mode", it->mode == NodeMode::Mute ? "mute"
                                                                      : it->mode == NodeMode::Bypass ? "bypass" : "normal"}};
        // A group id is not a node type, so only a missing type outside groups counts.
        node.insert("known", type != nullptr || document->group(it->type) != nullptr);
        nodes.append(node);
        lines << QStringLiteral("node %1: %2").arg(it.key(), it->type);
        for (auto input = it->inputs.cbegin(); input != it->inputs.cend(); ++input) {
            if (input->kind() == InputValue::Kind::Link) {
                links.append(QJsonObject{{"from", input->from().toString()},
                                         {"to", it.key() + "." + input.key()}});
                lines << QStringLiteral("link %1 -> %2.%3").arg(input->from().toString(), it.key(), input.key());
            }
        }
    }
    // What the compiler finds: sinks when it compiles, and every problem that keeps it from
    // compiling. `missing` is the part about things that are not there.
    const Compilation compilation = compileDocument(*document, context.registry);
    QJsonArray sinks;
    QJsonArray missing;
    const QJsonArray problems = toJson(compilation.errors);
    if (compilation.ok())
        for (const QString &sink : compilation.graph->sinks())
            sinks.append(sink);
    static const QSet<QString> missingCodes{"missing_input", "unknown_node_type", "unknown_link_source", "unknown_output",
                                            "unknown_input"};
    for (const Error &error : compilation.errors) {
        lines << humanProblem(error);
        if (missingCodes.contains(error.code))
            missing.append(error.toJson());
    }
    for (const QJsonValue &sink : sinks)
        lines << QStringLiteral("sink %1").arg(sink.toString());
    return Reply::success(QJsonObject{{"nodes", nodes}, {"links", links}, {"sinks", sinks}, {"missing", missing},
                                      {"problems", problems}},
                          lines.join('\n'));
}

// ---- runs ----

Result<RunFolder> openRun(const Command &command, Context &context) {
    return RunFolder::open(context.runsRoot, command.positionals.at(0));
}

// A run whose worker died is ended before it is shown, so it does not read as running
// for ever.
void endDeadRuns(const Context &context) {
    Lock lock(context.lockPath);
    endDeadRuns(context, lock);
}

Reply runsList(const Command &, Context &context) {
    endDeadRuns(context);
    QJsonArray runs;
    QStringList lines;
    QMap<QString, int> positions; // 0 is the run that is going, then the waiting ones in order
    const QList<QueueEntry> queued = Queue(context.queueDir).entries();
    for (int i = 0; i < queued.size(); ++i)
        positions.insert(queued.at(i).run, i);
    const QStringList ids = QDir(context.runsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString &id : ids) {
        const Result<RunFolder> folder = RunFolder::open(context.runsRoot, id);
        if (!folder)
            continue;
        const QJsonObject state = folder->state();
        QJsonObject run{{"id", id}, {"status", runStatus(*folder, folder->events())}};
        for (const char *key : {"workflow", "started", "ended"})
            if (state.contains(key))
                run.insert(key, state.value(key));
        if (positions.contains(id) && !isTerminal(run.value("status").toString()))
            run.insert("position", positions.value(id));
        runs.append(run);
        lines << QStringLiteral("%1  %2").arg(id, run.value("status").toString());
    }
    return Reply::success(QJsonObject{{"runs", runs}}, lines.join('\n'));
}

// Per-node state from the events (9.4).
QJsonObject foldNodes(const QList<QJsonObject> &events) {
    QJsonObject nodes;
    for (const QJsonObject &event : events) {
        const QString node = event.value("node").toString();
        const QString kind = event.value("event").toString();
        if (node.isEmpty() || !kind.startsWith("node.") || kind == "node.preview")
            continue;
        QJsonObject entry = nodes.value(node).toObject();
        entry.insert("state", kind == "node.progress" ? QStringLiteral("running") : kind.mid(5));
        if (kind == "node.progress") {
            for (const char *key : {"value", "stage", "message"})
                if (event.contains(key))
                    entry.insert(key, event.value(key));
        } else if (kind == "node.skipped") {
            entry.insert("reason", event.value("reason"));
        } else if (kind == "node.error") {
            entry.insert("error", event.value("error"));
        }
        if (kind == "node.done" || kind == "node.cached")
            entry.remove("value");
        nodes.insert(node, entry);
    }
    return nodes;
}

Reply runsStatus(const Command &command, Context &context) {
    const Result<RunFolder> folder = openRun(command, context);
    if (!folder)
        return Reply::failure(folder.errors());
    endDeadRuns(context);
    const QList<QJsonObject> events = folder->events();
    QJsonObject data = folder->state();
    data.insert("id", folder->id());
    data.insert("status", runStatus(*folder, events));
    data.insert("nodes", foldNodes(events));
    return Reply::success(data, QStringLiteral("%1  %2").arg(folder->id(), data.value("status").toString()));
}

Reply runsResult(const Command &command, Context &context) {
    const Result<RunFolder> folder = openRun(command, context);
    if (!folder)
        return Reply::failure(folder.errors());
    const QList<QJsonObject> events = folder->events();
    // The sinks of the compiled graph that ran (a "run to here" graph has its target
    // instead); with no readable compiled.json, every node's outputs.
    QSet<QString> wanted;
    bool filter = false;
    if (const QJsonObject json = folder->compiled(); !json.isEmpty()) {
        Errors ignored;
        if (const auto graph = CompiledGraph::fromJson(json, ignored)) {
            filter = true;
            for (const QString &sink : graph->sinks())
                wanted.insert(sink);
            if (wanted.isEmpty() && !graph->target.isEmpty())
                wanted.insert(graph->target);
        }
    }
    QJsonObject outputs;
    QJsonArray errors;
    for (const QJsonObject &event : events) {
        const QString kind = event.value("event").toString();
        if (kind == "node.done" || kind == "node.cached") {
            if (!filter || wanted.contains(event.value("node").toString()))
                outputs.insert(event.value("node").toString(), event.value("outputs"));
        } else if (kind == "run.done")
            errors = event.value("errors").toArray();
    }
    QJsonObject data{{"id", folder->id()}, {"status", runStatus(*folder, events)}, {"outputs", outputs}};
    if (!errors.isEmpty())
        data.insert("errors", errors);
    return Reply::success(data, QString::fromUtf8(writeJson(outputs)));
}

Reply runsEvents(const Command &command, Context &context) {
    const Result<RunFolder> folder = openRun(command, context);
    if (!folder)
        return Reply::failure(folder.errors());
    if (command.has("follow")) {
        Reply reply;
        reply.streamed = true;
        reply.exitCode = streamEvents(*folder, context) == "ok" ? 0 : 1;
        return reply;
    }
    const QList<QJsonObject> events = folder->events();
    QJsonArray array;
    QStringList lines;
    for (const QJsonObject &event : events) {
        array.append(event);
        lines << compact(event);
    }
    return Reply::success(QJsonObject{{"events", array}}, lines.join('\n'));
}

} // namespace

Reply execute(const Command &command, Context &context) {
    if (command.later)
        return Reply::failure(makeError("usage", QStringLiteral("`fugue %1` is not available in this version.").arg(QString(command.name).replace('.', ' ')),
                                        "It arrives in a later release."));
    const QString &name = command.name;
    if (name == "node.list")
        return nodeList(command, context);
    if (name == "node.describe")
        return nodeDescribe(command, context);
    if (name == "validate")
        return validate(command, context);
    if (name == "compile")
        return compile(command, context);
    if (name == "run")
        return startRun(command, context);
    if (name == "workflow.new")
        return workflowNew(command, context);
    if (name == "workflow.describe")
        return workflowDescribe(command, context);
    if (name == "__worker") {
        Reply reply;
        reply.streamed = true;
        reply.exitCode = runWorker(command.positionals.at(0), context);
        return reply;
    }
    if (name == "runs.list")
        return runsList(command, context);
    if (name == "runs.status")
        return runsStatus(command, context);
    if (name == "runs.events")
        return runsEvents(command, context);
    if (name == "runs.result")
        return runsResult(command, context);
    if (name == "runs.cancel")
        return cancelRun(command, context);
    return Reply::failure(makeError("internal", QStringLiteral("Command %1 has no handler.").arg(name)));
}

} // namespace fugue::cli
