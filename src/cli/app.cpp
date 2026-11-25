// `fugue app list`, `fugue app info <id|file>` and `fugue app run <id|file> --input key=value ...` (#392, epic #397, spec 4.9): an app
// is a workflow with an `app` block (engine/app_binding.h), so these commands add no runner of their own. The binding is app::bindApp,
// the run is the queue's (`startBoundRun` and `waitForRun`, as `fugue canvas act` uses), the readiness is the one `fugue template
// prepare` shows (engine/readiness.h), and the results are the run's own: the files the app's output nodes made.
//
// An app is named by the id of a template whose workflow has the block (`fugue template list` ids; a person's own first), or by a
// workflow file (a path that exists wins over an id, as for `canvas act`).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <optional>

#include "cli/canvas_run.h"
#include "cli/commands.h"
#include "engine/app_binding.h"
#include "engine/document.h"
#include "engine/readiness.h"
#include "engine/registry.h"
#include "engine/templates.h"

namespace fugue::cli {

namespace {

using app::App;

struct Loaded {
    QString id;   // the template's id, or the file as given
    QString file; // absolute path of the workflow
    QString folder; // what a relative pack source is read against
    QList<CarriedPack> carried;
    Document document;
    App app;
    Errors stale; // what pruneApp dropped (`app_stale` warnings): shown, never silent
};

Error usageProblem(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

// The workflow with its block: nothing when it has none, errors when it cannot be read or the block is malformed.
Result<std::optional<Loaded>> readApp(const QString &id, const QString &file, const QString &folder, const QList<CarriedPack> &carried) {
    const Result<Document> document = Document::loadFile(file);
    if (!document)
        return document.errors();
    const Result<std::optional<App>> block = app::appOf(*document);
    if (!block)
        return block.errors();
    if (!*block)
        return std::optional<Loaded>();
    return std::optional<Loaded>(Loaded{id, QFileInfo(file).absoluteFilePath(), folder, carried, *document, **block});
}

// The apps of the template library, in its order: the templates the catalog flags (`Template::app`, 11.10). A flagged one that
// cannot be read is a warning, and is left out.
QList<Loaded> templateApps(Context &context, Errors *problems) {
    QList<Loaded> apps;
    for (const Template &t : TemplateLibrary(TemplateLibrary::standardRoots(), &context.registry).list()) {
        if (!t.app)
            continue;
        const Result<std::optional<Loaded>> read = readApp(t.id, t.workflow, t.folder, t.carriedPacks);
        if (!read) {
            if (problems)
                for (Error problem : read.errors()) {
                    problem.severity = Error::Severity::Warning;
                    problem.message = QStringLiteral("%1: %2").arg(t.id, problem.message);
                    problems->append(problem);
                }
        } else if (*read) {
            apps.append(**read);
        }
    }
    return apps;
}

Result<Loaded> readNamedApp(const QString &ref, Context &context) {
    const auto noApp = [](const QString &what) {
        return makeError(QStringLiteral("no_app"), QStringLiteral("%1 has no app block.").arg(what),
                         QStringLiteral("An app is a workflow with a top-level \"app\" block (spec 4.9); `fugue app list` shows the apps there are."));
    };
    if (QFileInfo(ref).isFile()) {
        const Result<std::optional<Loaded>> read = readApp(ref, ref, QFileInfo(ref).absolutePath(), {});
        if (!read)
            return read.errors();
        if (!*read)
            return noApp(ref);
        return **read;
    }
    const TemplateLibrary library(TemplateLibrary::standardRoots(), &context.registry);
    if (const std::optional<Template> t = library.find(ref)) {
        // Read again from the file, whatever the catalog says: a block that does not read is said in its own words.
        const Result<std::optional<Loaded>> read = readApp(t->id, t->workflow, t->folder, t->carriedPacks);
        if (!read)
            return read.errors();
        if (!*read)
            return noApp(QStringLiteral("The template %1").arg(t->id));
        return **read;
    }
    Error error = makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no app %1.").arg(ref),
                            QStringLiteral("`fugue app list` shows the apps; give one's id, or a workflow file with an `app` block."));
    for (const Loaded &loaded : templateApps(context, nullptr))
        error.valid.append(loaded.id);
    return error;
}

// The app as the workflow now is: a target or output the graph no longer has is dropped and said (spec 4.9), as the App view does.
Result<Loaded> loadApp(const QString &ref, Context &context) {
    Result<Loaded> loaded = readNamedApp(ref, context);
    if (!loaded)
        return loaded;
    Loaded value = *loaded;
    value.stale = app::pruneApp(value.app, value.document, context.registry);
    return value;
}

// The spec of the first target, through a group instance to its inner input (spec 5.4).
const InputSpec *specOf(const InputBinding &input, const Document &document, const Registry &registry) {
    return input.targets.isEmpty() ? nullptr : targetInputSpec(document, registry, input.targets.first());
}

bool isFileRole(const InputBinding &input) { return input.role == QLatin1String("image") || input.role == QLatin1String("mask"); }

// What a form would offer: image and mask by role, else the target's port type (a resource is "model"), `choice` when it has words.
QString typeOf(const InputBinding &input, const Document &document, const Registry &registry) {
    if (isFileRole(input))
        return input.role;
    const InputSpec *spec = specOf(input, document, registry);
    if (input.choices || (spec && spec->choices))
        return QStringLiteral("choice");
    if (spec)
        return registry.portKind(spec->type.type) == PortKind::Resource ? QStringLiteral("model") : spec->type.type.toString();
    return input.role == QLatin1String("model") ? QStringLiteral("model") : QStringLiteral("any");
}

// What the input holds when nothing is given: the block's default, else the first target's own value in the workflow.
QJsonValue defaultOf(const InputBinding &input, const Document &document, const Registry &registry) {
    if (!input.defaultValue.isUndefined())
        return input.defaultValue;
    if (input.targets.isEmpty())
        return QJsonValue::Undefined;
    const PortRef &target = input.targets.first();
    const auto node = document.graph.nodes.constFind(target.node);
    if (node != document.graph.nodes.constEnd()) {
        const InputValue held = node->inputs.value(target.port);
        if (held.kind() == InputValue::Kind::Literal)
            return held.literal();
        if (held.kind() == InputValue::Kind::Auto) // a seed left to the engine: the workflow's own value is {"auto": "random"}
            return held.toJson();
    }
    const InputSpec *spec = specOf(input, document, registry);
    return spec ? spec->defaultValue : QJsonValue(QJsonValue::Undefined);
}

bool isBlank(const QJsonValue &value) { return value.isUndefined() || value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()); }

// An input a run cannot do without: a picture or a mask with none in the workflow, a prompt that is not optional with no fallback.
bool isRequired(const InputBinding &input, const Document &document, const Registry &registry) {
    if (!isBlank(defaultOf(input, document, registry)))
        return false;
    if (isFileRole(input))
        return true;
    return input.role == QLatin1String("prompt") && !input.optional && input.fallback.isEmpty();
}

QJsonObject inputJson(const InputBinding &given, const Document &document, const Registry &registry) {
    const InputBinding input = completedBy(given, specOf(given, document, registry));
    QJsonObject row{{"key", input.key()},
                    {"role", input.role},
                    {"label", input.label.isEmpty() ? input.key() : input.label},
                    {"type", typeOf(input, document, registry)},
                    {"required", isRequired(input, document, registry)},
                    {"advanced", input.section.compare(QStringLiteral("Advanced"), Qt::CaseInsensitive) == 0}};
    if (const QJsonValue value = defaultOf(input, document, registry); !value.isUndefined())
        row.insert("default", value);
    if (input.min)
        row.insert("min", *input.min);
    if (input.max)
        row.insert("max", *input.max);
    if (input.choices)
        row.insert("choices", QJsonArray::fromStringList(*input.choices));
    return row;
}

QJsonArray outputsJson(const Loaded &loaded) {
    QJsonArray out;
    for (const app::AppOutput &output : loaded.app.outputs)
        out.append(QJsonObject{{"node", output.node}, {"label", output.label.isEmpty() ? output.node : output.label}});
    return out;
}

QString titleOf(const Loaded &loaded) { return loaded.app.title.isEmpty() ? loaded.id : loaded.app.title; }

// What the app still needs here, by the one question `prepare` asks. Empty plan and no problems: ready.
ReadinessPlan planFor(const Loaded &loaded, const Document &document, Context &context) {
    return readinessPlanFor(requestFor(document, context.registry, loaded.carried, loaded.folder), context);
}

bool isReady(const ReadinessPlan &plan) { return plan.empty() && plan.problems.isEmpty(); }

Error notReady(const Loaded &loaded, const ReadinessPlan &plan) {
    const bool template_ = !QFileInfo(loaded.id).isFile();
    QString message = QStringLiteral("%1 is not ready: %2").arg(titleOf(loaded), plan.empty() ? QStringLiteral("something it needs cannot be got.") : plan.headline() + QLatin1Char('.'));
    for (const Error &problem : plan.problems)
        message += QLatin1Char(' ') + problem.message;
    return makeError(QStringLiteral("app_not_ready"), message,
                     template_ ? QStringLiteral("`fugue template prepare %1` lists what is missing and gets it ready with one question.").arg(loaded.id)
                               : QStringLiteral("`fugue workflow prepare %1` lists what is missing and gets it ready with one question.").arg(loaded.id));
}

// The words of a file: the text as written, one trailing line break dropped.
Result<QString> textOf(const QString &path) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return Result<QString>(makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no file %1.").arg(path)));
    QString text = QString::fromUtf8(file.readAll());
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(text.endsWith(QStringLiteral("\r\n")) ? 2 : 1);
    return text;
}

// The resource type a model input takes: the port's declared type.
std::optional<PortType> resourceTypeOf(const InputBinding &input, const Document &document, const Registry &registry) {
    if (const InputSpec *spec = specOf(input, document, registry); spec && registry.portKind(spec->type.type) == PortKind::Resource)
        return spec->type.type;
    return std::nullopt;
}

// One `--input` value as the input takes it: a picture or mask is a file (made absolute, which must exist); `@<file>` is the file's text;
// a model is its file name (or a whole handle as JSON); a number, a choice word and text are as typed.
Result<QJsonValue> valueOf(const InputBinding &input, const QString &raw, const Document &document, Context &context) {
    const QString type = typeOf(input, document, context.registry);
    if (isFileRole(input)) {
        const QString path = QFileInfo(raw.startsWith(QLatin1Char('@')) ? raw.mid(1) : raw).absoluteFilePath();
        if (!QFileInfo(path).isFile())
            return Result<QJsonValue>(makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no file %1 for the input \"%2\".").arg(path, input.key())));
        return QJsonValue(path);
    }
    QString text = raw;
    if (raw.startsWith(QLatin1Char('@'))) {
        const Result<QString> read = textOf(raw.mid(1));
        if (!read)
            return read.errors();
        text = *read;
    }
    if (input.choices)
        return QJsonValue(text);
    if (type == QLatin1String("model")) {
        const QJsonDocument parsed = QJsonDocument::fromJson(text.toUtf8());
        if (parsed.isObject())
            return QJsonValue(parsed.object());
        const std::optional<PortType> resource = resourceTypeOf(input, document, context.registry);
        if (!resource)
            return Result<QJsonValue>(usageProblem(QStringLiteral("The input \"%1\" takes a whole resource handle as JSON.").arg(input.key())));
        const Result<QJsonObject> handle = modelByName(*resource, text, context);
        if (!handle)
            return handle.errors();
        return QJsonValue(*handle);
    }
    if (type == QLatin1String("number")) {
        bool ok = false;
        const double number = text.trimmed().toDouble(&ok);
        if (!ok)
            return Result<QJsonValue>(usageProblem(QStringLiteral("The input \"%1\" takes a number, not \"%2\".").arg(input.key(), text)));
        return QJsonValue(number);
    }
    if (type == QLatin1String("bool")) {
        const QString word = text.trimmed().toLower();
        if (word != QLatin1String("true") && word != QLatin1String("false"))
            return Result<QJsonValue>(usageProblem(QStringLiteral("The input \"%1\" takes true or false, not \"%2\".").arg(input.key(), text)));
        return QJsonValue(word == QLatin1String("true"));
    }
    return QJsonValue(text);
}

// `--input key=value` pairs, bound to the app's inputs: unknown keys, a key given twice, and inputs left out that a run cannot do without.
Result<QMap<QString, QJsonValue>> givenValues(const Command &command, const Loaded &loaded, Context &context) {
    const auto keys = [&] {
        QStringList names;
        for (const InputBinding &input : loaded.app.inputs)
            names << input.key();
        return names;
    };
    QMap<QString, QJsonValue> values;
    for (const QString &pair : command.options.value("input")) {
        const int equals = pair.indexOf(QLatin1Char('='));
        if (equals <= 0)
            return Result<QMap<QString, QJsonValue>>(usageProblem(QStringLiteral("--input takes <key>=<value> or <key>=@<file>, not \"%1\".").arg(pair),
                                                                  QStringLiteral("`fugue app info %1` lists the keys.").arg(loaded.id)));
        const QString key = pair.left(equals);
        const InputBinding *input = loaded.app.input(key);
        if (!input) {
            Error error = makeError(QStringLiteral("unknown_input"), QStringLiteral("%1 has no input \"%2\".").arg(titleOf(loaded), key),
                                    QStringLiteral("`fugue app info %1` lists the inputs.").arg(loaded.id));
            for (const QString &name : keys())
                error.valid.append(name);
            return Result<QMap<QString, QJsonValue>>(error);
        }
        if (values.contains(key))
            return Result<QMap<QString, QJsonValue>>(usageProblem(QStringLiteral("--input %1 is given twice.").arg(key)));
        const Result<QJsonValue> value = valueOf(*input, pair.mid(equals + 1), loaded.document, context);
        if (!value)
            return value.errors();
        values.insert(key, *value);
    }
    Errors missing;
    for (const InputBinding &input : loaded.app.inputs)
        if (!values.contains(input.key()) && isRequired(input, loaded.document, context.registry))
            missing << makeError(QStringLiteral("missing_input"),
                                 QStringLiteral("%1 needs the input \"%2\"%3.").arg(titleOf(loaded), input.key(),
                                                                                    input.label.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(input.label)),
                                 QStringLiteral("Give it with `--input %1=<value>`; `fugue app info %2` lists the inputs.").arg(input.key(), loaded.id));
    if (!missing.isEmpty())
        return missing;
    return values;
}

// The files in a value: every `path` of a media handle in it, in order.
void collectPaths(const QJsonValue &value, QStringList &paths) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            collectPaths(item, paths);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (const QString path = object.value("path").toString(); !path.isEmpty() && !paths.contains(path))
            paths << path;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != QLatin1String("path"))
                collectPaths(it.value(), paths);
    }
}

// What each output node of the app made, from the run's own events (the same node.done a `runs result` reads).
QJsonArray resultsOf(const Loaded &loaded, const RunFolder &folder, QStringList &all) {
    QJsonArray results;
    const QList<QJsonObject> events = folder.events();
    for (const app::AppOutput &output : loaded.app.outputs) {
        QStringList paths;
        for (const QJsonObject &event : events) {
            const QString kind = event.value("event").toString();
            if ((kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) && event.value("node").toString() == output.node) {
                // A Save node's file is its output; a Preview has none, so what it shows is its result (as `runs result` reads).
                const qsizetype before = paths.size();
                collectPaths(event.value("outputs"), paths);
                if (paths.size() == before)
                    collectPaths(event.value("shown"), paths);
            }
        }
        for (const QString &path : std::as_const(paths))
            if (!all.contains(path))
                all << path;
        results.append(QJsonObject{{"node", output.node}, {"label", output.label.isEmpty() ? output.node : output.label},
                                   {"files", QJsonArray::fromStringList(paths)}});
    }
    return results;
}

// `--out <dir>`: the folder of every output node that writes files (a Save node's `folder`); a Preview has none.
Result<int> putOutputsIn(Document &bound, const Loaded &loaded, const QString &dir, const Registry &registry) {
    int taken = 0;
    for (const QString &node : loaded.app.outputNodes()) {
        const auto found = bound.graph.nodes.find(node);
        if (found == bound.graph.nodes.end())
            continue;
        const NodeType *type = registry.nodeTypeFor(found->type, found->version.major);
        if (!type || !type->input(QStringLiteral("folder")))
            continue;
        found->inputs.insert(QStringLiteral("folder"), InputValue::literal(dir));
        ++taken;
    }
    return taken;
}

} // namespace

Reply appList(const Command &, Context &context) {
    Errors problems;
    const QList<Loaded> apps = templateApps(context, &problems);
    QJsonArray listed;
    QStringList lines;
    for (const Loaded &loaded : apps) {
        listed.append(QJsonObject{{"id", loaded.id},
                                  {"title", titleOf(loaded)},
                                  {"description", loaded.app.description},
                                  {"file", loaded.file},
                                  {"inputs", int(loaded.app.inputs.size())},
                                  {"outputs", int(loaded.app.outputs.size())}});
        lines << (loaded.app.description.isEmpty() ? QStringLiteral("%1  %2").arg(loaded.id, titleOf(loaded))
                                                   : QStringLiteral("%1  %2: %3").arg(loaded.id, titleOf(loaded), loaded.app.description));
    }
    Reply reply = Reply::success(QJsonObject{{"apps", listed}}, lines.join(QLatin1Char('\n')));
    reply.errors = problems;
    return reply;
}

Reply appInfo(const Command &command, Context &context) {
    const Result<Loaded> loaded = loadApp(command.positionals.at(0), context);
    if (!loaded)
        return Reply::failure(loaded.errors());
    const ReadinessPlan plan = planFor(*loaded, loaded->document, context);
    const bool ready = isReady(plan);
    // A missing pack leaves its nodes unknown, so the block cannot be checked against them: say what is missing first.
    if (ready) {
        const Errors invalid = app::checkApp(loaded->app, loaded->document, context.registry);
        if (hasErrors(invalid))
            return Reply::failure(invalid);
    }
    QJsonArray inputs;
    QStringList lines{QStringLiteral("%1 (%2)").arg(titleOf(*loaded), loaded->id)};
    if (!loaded->app.description.isEmpty())
        lines << loaded->app.description;
    lines << QStringLiteral("Inputs:");
    for (const InputBinding &input : loaded->app.inputs) {
        const QJsonObject row = inputJson(input, loaded->document, context.registry);
        inputs.append(row);
        QStringList facts{row.value("type").toString()};
        if (row.value("required").toBool())
            facts << QStringLiteral("required");
        if (row.contains("default") && !isBlank(row.value("default")))
            facts << QStringLiteral("default %1").arg(QString::fromUtf8(QJsonDocument(QJsonArray{row.value("default")}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
        if (row.contains("min") || row.contains("max"))
            facts << QStringLiteral("%1 to %2").arg(row.contains("min") ? QString::number(row.value("min").toDouble()) : QStringLiteral("..."),
                                                    row.contains("max") ? QString::number(row.value("max").toDouble()) : QStringLiteral("..."));
        if (row.contains("choices"))
            {
            QStringList words;
            for (const QJsonValue &word : row.value("choices").toArray())
                words << word.toString();
            facts << QStringLiteral("one of %1").arg(words.join(QStringLiteral(", ")));
        }
        lines << QStringLiteral("  %1  %2  (%3)").arg(row.value("key").toString(), row.value("label").toString(), facts.join(QStringLiteral(", ")));
    }
    lines << QStringLiteral("Outputs:");
    for (const QJsonValue &output : outputsJson(*loaded))
        lines << QStringLiteral("  %1  %2").arg(output.toObject().value("node").toString(), output.toObject().value("label").toString());
    QJsonObject data{{"id", loaded->id},
                     {"file", loaded->file},
                     {"title", titleOf(*loaded)},
                     {"description", loaded->app.description},
                     {"default_view", loaded->app.defaultView},
                     {"inputs", inputs},
                     {"outputs", outputsJson(*loaded)},
                     {"ready", ready}};
    if (!ready) {
        data.insert("needs", plan.toJson());
        lines << QStringLiteral("Not ready: %1").arg(notReady(*loaded, plan).message);
    }
    Reply reply = Reply::success(data, lines.join(QLatin1Char('\n')));
    reply.errors = loaded->stale;
    return reply;
}

Reply appRun(const Command &command, Context &context) {
    const Result<Loaded> loaded = loadApp(command.positionals.at(0), context);
    if (!loaded)
        return Reply::failure(loaded.errors());
    const Result<QMap<QString, QJsonValue>> values = givenValues(command, *loaded, context);
    if (!values)
        return Reply::failure(values.errors());

    // Ready before the block is checked: a missing pack leaves nodes the block names unknown.
    const ReadinessPlan plan = planFor(*loaded, loaded->document, context);
    if (!isReady(plan))
        return Reply::failure(notReady(*loaded, plan));
    if (const Errors invalid = app::checkApp(loaded->app, loaded->document, context.registry); hasErrors(invalid))
        return Reply::failure(invalid);

    Result<Document> bound = app::bindApp(loaded->document, loaded->app, *values);
    if (!bound)
        return Reply::failure(bound.errors());
    // A model picked with --input has to be here too.
    if (!values->isEmpty())
        if (const ReadinessPlan chosen = planFor(*loaded, *bound, context); !isReady(chosen))
            return Reply::failure(notReady(*loaded, chosen));
    if (command.has("out")) {
        const QString dir = QDir(command.value("out")).absolutePath();
        if (!QDir().mkpath(dir))
            return Reply::failure(makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot make the folder %1.").arg(dir)));
        const Result<int> taken = putOutputsIn(*bound, *loaded, dir, context.registry);
        if (*taken == 0)
            context.log(QStringLiteral("--out: no output of this app writes files, so it changes nothing."));
    }

    installStopHandlers();
    const Result<RunFolder> folder = startBoundRun(*bound, loaded->file, context);
    if (!folder)
        return Reply::failure(folder.errors());
    const canvas::BatchRunResult result = waitForRun(*folder, context, [&context](const QJsonObject &event) {
        const QString kind = event.value("event").toString(), node = event.value("node").toString();
        if (kind == QLatin1String("node.running"))
            context.log(node + QStringLiteral(" ..."));
        else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached"))
            context.log(node + (kind == QLatin1String("node.cached") ? QStringLiteral(" (cached)") : QStringLiteral(" done")));
    });
    if (result.status == QLatin1String("cancelled"))
        return Reply::failure(makeError(QStringLiteral("cancelled"), QStringLiteral("The run was cancelled.")));
    if (result.status != QLatin1String("ok"))
        return Reply::failure(makeError(result.code.isEmpty() ? QStringLiteral("node_failed") : result.code, result.message));

    QStringList files;
    const QJsonArray results = resultsOf(*loaded, *folder, files);
    Reply reply = Reply::success(QJsonObject{{"app", loaded->id}, {"run", folder->id()}, {"status", "ok"}, {"outputs", results}, {"files", QJsonArray::fromStringList(files)}},
                                 files.join(QLatin1Char('\n')));
    reply.errors = loaded->stale;
    return reply;
}

} // namespace fugue::cli
