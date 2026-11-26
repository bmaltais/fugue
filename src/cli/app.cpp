// `fugue app list`, `fugue app info <id|file>` and `fugue app run <id|file> --input key=value ...` (#392, epic #397, spec 4.9): an app
// is a workflow with an `app` block (engine/app_binding.h), so these commands add no runner of their own. The binding is app::bindApp,
// the run is the queue's (`startBoundRun` and `waitForRun`, as `fugue canvas act` uses), the readiness is the one `fugue template
// prepare` shows (engine/readiness.h), and the results are the run's own: the files the app's output nodes made.
//
// An app is named by the id of a template whose workflow has the block (`fugue template list` ids; a person's own first), or by a
// workflow file (a path that exists wins over an id, as for `canvas act`).

#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

#include <utility>

#include "cli/canvas_run.h"
#include "cli/compile_step.h"
#include "cli/commands.h"
#include "cli/use_on_disk.h"
#include "engine/app_binding.h"
#include "engine/app_input.h"
#include "engine/document.h"
#include "engine/file_roots.h"
#include "engine/providers.h"
#include "engine/readiness.h"
#include "engine/paired_placement.h"
#include "engine/registry.h"
#include "engine/share_package.h"
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
    return std::optional<Loaded>(Loaded{id, QFileInfo(file).absoluteFilePath(), folder, carried, *document, **block, {}}); // stale: filled by loadApp
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

// One input as `fugue app info` lists it: the description of engine/app_input.h in the CLI's words.
QJsonObject inputJson(const InputBinding &given, const Document &document, const Registry &registry) {
    const app::AppInput described = app::describeInput(given, document, &registry);
    const InputBinding &input = described.binding;
    QJsonObject row{{"key", input.key()},
                    {"role", input.role},
                    {"label", input.label.isEmpty() ? input.key() : input.label},
                    {"type", app::cliTypeOf(described)},
                    {"required", described.required},
                    {"optional", input.optional},
                    {"advanced", described.advanced}};
    if (!described.defaultValue.isUndefined())
        row.insert("default", described.defaultValue);
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

// Where the app runs when its models are not here (#395, #491, #581): the one verdict of engine/paired_placement.h, waited for here.
// The pins and what the hosts last said decide first; the hosts are asked (10 s at most each) only when those pass. `probe` keeps the
// answers, so one command asks the hosts once whatever number of times it looks.
PairedVerdict pairedVerdict(const ReadinessPlan &plan, const Document &document, Context &context, const PairedProbe &probe) {
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    PairedPlacementRequest request;
    request.instances = instances ? instances.value() : QList<ProviderInstance>();
    request.registry = &context.registry;
    request.requirementOf = [&context](const NodeType &type, const Values &inputs) { return kernelRequirement(context.kernels, type, inputs); };
    // The answer may come after the limit (the 10 s is Qt's transfer timeout, silence only): it then lands in this shared state, never
    // in this frame, and quits no loop.
    struct Wait {
        std::optional<PairedVerdict> verdict;
        QPointer<QEventLoop> loop;
    };
    const auto wait = std::make_shared<Wait>();
    QEventLoop loop;
    wait->loop = &loop;
    QTimer limit;
    limit.setSingleShot(true);
    QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
    decidePairedPlacement(document, plan, request, probe, [wait](const PairedVerdict &made) {
        wait->verdict = made;
        if (wait->loop)
            wait->loop->quit();
    });
    limit.start(12000); // a describe on a paired host gives up after 10 s of silence
    if (!wait->verdict)
        loop.exec();
    wait->loop = nullptr;
    if (wait->verdict)
        return *wait->verdict;
    PairedVerdict none;
    none.applies = true; // no word from the hosts in time: they are silent, the run goes ahead
    for (const ProviderInstance &instance : request.instances)
        if (instance.enabled && isPairedHelper(instance))
            none.silent << instance.name;
    return none;
}

// The paired hosts that ran a node of the run (run.json: the nodes a provider that does not share this machine's files served), once each.
QStringList hostsOfRun(const RunFolder &folder) {
    QStringList hosts;
    const QJsonObject providers = folder.state().value("providers").toObject();
    for (auto it = providers.begin(); it != providers.end(); ++it) {
        const QJsonObject served = it.value().toObject();
        const QString instance = served.value("instance").toString();
        if (served.value("remote").toBool() && !instance.isEmpty() && !hosts.contains(instance))
            hosts << instance;
    }
    return hosts;
}

Error notReady(const Loaded &loaded, const ReadinessPlan &plan) {
    const bool template_ = !QFileInfo(loaded.id).isFile();
    QString message = QStringLiteral("%1 is not ready: %2").arg(titleOf(loaded), plan.empty() ? QStringLiteral("something it needs cannot be got.") : plan.headline() + QLatin1Char('.'));
    for (const Error &problem : plan.problems)
        message += QLatin1Char(' ') + problem.message;
    return makeError(QStringLiteral("app_not_ready"), message,
                     template_ ? QStringLiteral("`fugue template prepare %1` lists what is missing and gets it ready with one question.").arg(loaded.id)
                               : QStringLiteral("`fugue workflow prepare %1` lists what is missing and gets it ready with one question.").arg(loaded.id));
}

// Why a run is refused: the nodes the paired host cannot serve (each named, with the `prepare` hint of the app), else what is not ready.
Errors refused(const Loaded &loaded, const ReadinessPlan &plan, const PairedVerdict &verdict) {
    if (verdict.blocked.isEmpty())
        return {notReady(loaded, plan)};
    const QString hint = notReady(loaded, plan).hint;
    Errors errors = verdict.blocked;
    for (Error &error : errors)
        error.hint = QStringLiteral("%1 %2").arg(error.hint, hint);
    return errors;
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

// One `--input` value as the input takes it (engine/app_input.h): `@<file>` is the file's text (for a picture, the file itself); a model
// may be given by its file name.
Result<QJsonValue> valueOf(const InputBinding &input, const QString &raw, const Document &document, Context &context) {
    const app::AppInput described = app::describeInput(input, document, &context.registry);
    if (app::isPictureRole(input))
        return described.fromText(raw.startsWith(QLatin1Char('@')) ? raw.mid(1) : raw);
    QString text = raw;
    if (raw.startsWith(QLatin1Char('@'))) {
        const Result<QString> read = textOf(raw.mid(1));
        if (!read)
            return read.errors();
        text = *read;
    }
    return described.fromText(text, [&context](const PortType &type, const QString &word) { return modelByName(type, word, context); });
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
        if (!values.contains(input.key()) && app::describeInput(input, loaded.document, &context.registry).required)
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
        else if (row.value("optional").toBool())
            facts << QStringLiteral("optional");
        if (row.contains("default") && !app::isBlank(row.value("default")))
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
    PairedVerdict placement;
    if (!ready)
        placement = pairedVerdict(plan, loaded->document, context, livePairedProbe());
    if (placement.runsOn()) {
        // Not here, but the paired helpers take it: Run goes there and the host checks the files (#395).
        const QString hosts = placement.hosts().join(QStringLiteral(", "));
        QJsonObject nodes;
        for (auto it = placement.hostOf.cbegin(); it != placement.hostOf.cend(); ++it)
            nodes.insert(it.key(), it.value());
        data.insert("runs_on", hosts);
        data.insert("runs_on_nodes", nodes);
        lines << QStringLiteral("Runs on %1: its models are not on this machine.").arg(hosts);
    }
    if (!placement.silent.isEmpty()) {
        // The only host that could take it does not answer (#581): not named as the host. A run still starts (a cached re-run can work).
        const QString hosts = placement.silent.join(QStringLiteral(", "));
        data.insert("silent_host", hosts);
        lines << QStringLiteral("%1 is not answering: its models are not on this machine.").arg(hosts);
    }
    if (!ready) {
        data.insert("needs", plan.toJson());
        lines << QStringLiteral("Not ready: %1").arg(notReady(*loaded, plan).message);
        // A paired helper is there, but it cannot take these nodes (#491): which node and why.
        if (!placement.blocked.isEmpty()) {
            data.insert("host_problems", toJson(placement.blocked));
            for (const Error &problem : placement.blocked)
                lines << problem.message;
        }
    }
    Reply reply = Reply::success(data, lines.join(QLatin1Char('\n')));
    reply.errors = loaded->stale;
    return reply;
}

// `fugue app export <id|file> <file.fugueapp>` (#394, spec 11.5): the app as a bundle that holds what it needs but the models.
Reply appExport(const Command &command, Context &context) {
    const QString ref = command.positionals.at(0);
    const Result<Loaded> loaded = loadApp(ref, context);
    if (!loaded)
        return Reply::failure(loaded.errors());
    QString out = command.positionals.at(1);
    const QString suffix = QFileInfo(out).suffix();
    if (suffix.compare(QLatin1String(appPackageSuffix), Qt::CaseInsensitive) != 0
        && suffix.compare(QLatin1String(workflowPackageSuffix), Qt::CaseInsensitive) != 0)
        out += QLatin1Char('.') + QString::fromLatin1(appPackageSuffix); // its own extension, which the desktop opens as an app
    out = QFileInfo(out).absoluteFilePath();

    Document document = loaded->document;
    if (!loaded->stale.isEmpty())
        app::setApp(document, loaded->app); // what the graph no longer has is not shared
    ShareRequest request;
    request.title = titleOf(*loaded);
    request.workflowPath = loaded->file;
    request.carried = loaded->carried;
    request.app = true;
    request.appVersion = QCoreApplication::applicationVersion();
    if (!QFileInfo(ref).isFile()) // a template's own picture; a loose file has none of its own
        request.preview = previewFilesIn(loaded->folder).still;
    QStringList included, packs;
    const Errors errors = writeSharePackage(document, context.registry, request, out, &included, &packs);
    if (hasErrors(errors))
        return Reply::failure(errors);
    Reply reply = Reply::success(QJsonObject{{"file", out},
                                             {"app", loaded->id},
                                             {"packs", QJsonArray::fromStringList(packs)},
                                             {"files", QJsonArray::fromStringList(included)}},
                                 QStringLiteral("Wrote %1. It carries %2 and %3; models are not in it: who opens it downloads them after one approval.")
                                     .arg(out, packs.isEmpty() ? QStringLiteral("no packs") : QStringLiteral("the pack%1 %2").arg(packs.size() == 1 ? QString() : QStringLiteral("s"), packs.join(QStringLiteral(", "))),
                                          included.isEmpty() ? QStringLiteral("no sample files") : QStringLiteral("the sample files %1").arg(included.join(QStringLiteral(", ")))));
    reply.errors = errors; // warnings: what was left out
    return reply;
}

Reply appRun(const Command &command, Context &context) {
    Result<Loaded> read = loadApp(command.positionals.at(0), context);
    if (!read)
        return Reply::failure(read.errors());
    // --use-on-disk (#605): a model that is not on disk, whose same weights are, is swapped for this run only; the app's file is not written.
    if (command.has("use-on-disk"))
        read->document = useOnDisk(read->document, context);
    const Result<Loaded> &loaded = read;
    const auto failure = [&](Errors errors) {
        if (!command.has("use-on-disk"))
            addOnDiskHints(errors, loaded->document, context);
        return Reply::failure(errors);
    };
    const Result<QMap<QString, QJsonValue>> values = givenValues(command, *loaded, context);
    if (!values)
        return Reply::failure(values.errors());

    // Ready before the block is checked: a missing pack leaves nodes the block names unknown.
    const ReadinessPlan plan = planFor(*loaded, loaded->document, context);
    const PairedProbe probe = keptPairedProbe(livePairedProbe(), std::chrono::minutes(1)); // one question to the hosts per command
    if (!isReady(plan))
        if (const PairedVerdict placement = pairedVerdict(plan, loaded->document, context, probe); !placement.allowsRun())
            return failure(refused(*loaded, plan, placement));
    if (const Errors invalid = app::checkApp(loaded->app, loaded->document, context.registry); hasErrors(invalid))
        return Reply::failure(invalid);

    Result<Document> bound = app::bindApp(loaded->document, loaded->app, *values);
    if (!bound)
        return Reply::failure(bound.errors());
    // A field's default may hold the missing model, and binding writes it over the file chosen above: put the file in again, saying
    // nothing twice, before the readiness check that follows.
    if (command.has("use-on-disk")) {
        const auto log = std::exchange(context.log, nullptr);
        *bound = useOnDisk(*bound, context);
        context.log = log;
    }
    // A model picked with --input has to be here too.
    if (!values->isEmpty())
        if (const ReadinessPlan chosen = planFor(*loaded, *bound, context); !isReady(chosen))
            if (const PairedVerdict placement = pairedVerdict(chosen, *bound, context, probe); !placement.allowsRun())
                return failure(refused(*loaded, chosen, placement));
    if (command.has("out")) {
        const QString dir = QDir(command.value("out")).absolutePath();
        // 13.6: judged before the folder is made, so a refused --out leaves nothing behind.
        if (const Result<QString> allowed = FileRoots::defaults(QDir::currentPath(), loaded->file).checkWrite(dir, QStringLiteral("out")); !allowed)
            return Reply::failure(allowed.errors());
        if (!QDir().mkpath(dir))
            return Reply::failure(makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot make the folder %1.").arg(dir)));
        const Result<int> taken = putOutputsIn(*bound, *loaded, dir, context.registry);
        if (*taken == 0)
            context.log(QStringLiteral("--out: no output of this app writes files, so it changes nothing."));
    }

    const Result<QJsonObject> allowRemote = allowRemoteFingerprints(command.options.value("allow-remote"), context);
    if (!allowRemote)
        return Reply::failure(allowRemote.errors());
    installStopHandlers();
    const Result<RunFolder> folder = startBoundRun(*bound, loaded->file, context, *allowRemote);
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
    const QStringList hosts = hostsOfRun(*folder);
    if (!hosts.isEmpty())
        context.log(QStringLiteral("Ran on %1.").arg(hosts.join(QStringLiteral(", "))));
    Reply reply = Reply::success(QJsonObject{{"app", loaded->id}, {"run", folder->id()}, {"status", "ok"}, {"ran_on", QJsonArray::fromStringList(hosts)}, {"outputs", results}, {"files", QJsonArray::fromStringList(files)}},
                                 files.join(QLatin1Char('\n')));
    reply.errors = loaded->stale;
    return reply;
}

} // namespace fugue::cli
