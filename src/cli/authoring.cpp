// `fugue node new` and `fugue node test` (spec 6.11): the authoring tools for command nodes.

#include "cli/commands.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "engine/command_node.h"
#include "engine/file_hashes.h"
#include "engine/json.h"
#include "engine/pack_loader.h"

namespace fugue::cli {

namespace {

const QString kHelloScript = QStringLiteral(R"(#!/bin/bash
# A Fugue command node: it reads one JSON request on stdin and writes one JSON line on stdout.
# Anything written to stderr is the node's log. See docs/nodes/protocol.md.
request=$(cat)
name=$(printf '%s' "$request" | jq -r '.inputs.name // "world"')
printf '{"op": "progress", "value": 0.5, "message": "greeting"}\n'
jq -cn --arg greeting "Hello, $name!" '{outputs: {greeting: $greeting}}'
)");

bool writeFile(const QString &path, const QByteArray &bytes, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(bytes);
    file.close();
    if (executable)
        return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                                               | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
    return true;
}

Error usageError(const QString &message, const QString &hint = {}) { return makeError("usage", message, hint); }

QString titleOf(const QString &name) {
    QString title = name;
    title.replace('_', ' ');
    if (!title.isEmpty())
        title[0] = title[0].toUpper();
    return title;
}

QJsonObject nodeManifest(const QString &packId, const QString &name) {
    return QJsonObject{
        {"manifest", 1},
        {"id", packId + "/" + name},
        {"version", "0.1.0"},
        {"name", titleOf(name)},
        {"description", "Says hello. Change me."},
        {"category", "Example"},
        {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
        {"inputs", QJsonArray{QJsonObject{{"name", "name"}, {"type", "text"}, {"default", "world"}}}},
        {"outputs", QJsonArray{QJsonObject{{"name", "greeting"}, {"type", "text"}}}},
    };
}

QJsonObject nodeTestFile() {
    return QJsonObject{{"cases", QJsonArray{QJsonObject{{"name", "greets"},
                                                        {"inputs", QJsonObject{{"name", "Fugue"}}},
                                                        {"expect", QJsonObject{{"greeting", "Hello, Fugue!"}}}},
                                            QJsonObject{{"name", "default input"}, {"inputs", QJsonObject{}},
                                                        {"expect", QJsonObject{{"greeting", "Hello, world!"}}}}}}};
}

QStringList packNodeNames(const QJsonObject &pack) {
    QStringList names;
    for (const QJsonValue &value : pack.value("nodes").toArray())
        names << value.toString();
    return names;
}

// One test case: a name, the inputs to give (by name, literals; a media input names a file
// relative to the node's folder) and what the outputs should be (literals only).
struct Case {
    QString name;
    QJsonObject inputs;
    QJsonObject expect;
};

Result<QList<Case>> readCases(const NodeType &type) {
    const QString path = type.dir + "/test.json";
    QFile file(path);
    if (!file.exists())
        return QList<Case>{Case{QStringLiteral("defaults"), {}, {}}};
    if (!file.open(QIODevice::ReadOnly))
        return makeError("permission_denied", QStringLiteral("%1 cannot be read.").arg(path));
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return makeError("invalid_json", QStringLiteral("%1 is not a JSON object: %2").arg(path, parse.errorString()),
                         "It looks like {\"cases\": [{\"name\": \"x\", \"inputs\": {...}, \"expect\": {...}}]}.");
    QList<Case> cases;
    for (const QJsonValue &value : document.object().value("cases").toArray()) {
        const QJsonObject item = value.toObject();
        cases.append(Case{item.value("name").toString(QStringLiteral("case %1").arg(cases.size() + 1)),
                          item.value("inputs").toObject(), item.value("expect").toObject()});
    }
    if (cases.isEmpty())
        return makeError("invalid_document", QStringLiteral("%1 has no \"cases\".").arg(path));
    return cases;
}

// An input's value from the test file: a literal as it is, a media file by its path.
Result<Value> inputValue(const NodeType &type, const InputSpec &spec, const QJsonValue &json, FileHashes &hashes) {
    const QString kind = spec.type.type.id;
    const bool media = kind == "image" || kind == "video" || kind == "audio";
    const auto one = [&](const QJsonValue &item) -> Result<Value> {
        if (!media)
            return Value(item);
        QString path = item.toString();
        if (QFileInfo(path).isRelative())
            path = QDir(type.dir).filePath(path);
        const MediaType mediaType = kind == "image" ? MediaType::Image : kind == "video" ? MediaType::Video : MediaType::Audio;
        const Result<MediaHandle> handle = MediaHandle::probe(mediaType, path, hashes);
        if (!handle)
            return handle.error();
        return Value(handle.value());
    };
    if (!spec.type.list)
        return one(json);
    QList<Value> items;
    for (const QJsonValue &item : json.toArray()) {
        const Result<Value> made = one(item);
        if (!made)
            return made.error();
        items.append(made.value());
    }
    return Value::list(items);
}

Error caseError(const QString &code, const NodeType &type, const Case &test, const QString &message, const QString &hint = {}) {
    Error error = makeError(code, QStringLiteral("%1, case \"%2\": %3").arg(type.id, test.name, message), hint);
    error.node = type.id;
    return error;
}

// Runs one case; the case's errors, none when it passed. `outputs` gets what the node returned.
Errors runCase(CommandKernel &kernel, const NodeType &type, const Case &test, FileHashes &hashes, const QString &workRoot,
               QJsonObject &outputs, QStringList &progress) {
    Values inputs;
    for (auto it = test.inputs.begin(); it != test.inputs.end(); ++it) {
        const InputSpec *spec = type.input(it.key());
        if (!spec)
            return {caseError("unknown_input", type, test, QStringLiteral("the test gives input \"%1\", which the node does not have.").arg(it.key()))};
        const Result<Value> value = inputValue(type, *spec, it.value(), hashes);
        if (!value)
            return {caseError(value.error().code, type, test, QStringLiteral("input \"%1\": %2").arg(it.key(), value.error().message))};
        inputs.insert(it.key(), value.value());
    }
    for (const InputSpec &spec : type.inputs) {
        if (inputs.contains(spec.name))
            continue;
        if (spec.defaultValue.isUndefined()) {
            if (spec.required())
                return {caseError("missing_input", type, test, QStringLiteral("input \"%1\" has no value and no default.").arg(spec.name),
                                  "Give it in this case's \"inputs\" in test.json.")};
            continue;
        }
        inputs.insert(spec.name, Value(spec.defaultValue));
    }

    const QString workDir = workRoot + "/" + QString::number(qHash(test.name)) + "-" + QString::number(progress.size());
    QDir().mkpath(workDir);
    NodeContext context;
    context.node = "test";
    context.run = "test";
    context.type = &type;
    context.inputs = inputs;
    context.workDir = workDir;
    context.progress = [&progress](double value, const QString &, const QString &message) {
        progress << QStringLiteral("%1%% %2").arg(int(value * 100)).arg(message);
    };
    context.preview = [](const QString &) {};
    context.cancelled = [] { return false; };
    context.hashes = &hashes;

    const Result<Values> result = kernel.run(context);
    if (!result) {
        Error error = result.error();
        error.message = QStringLiteral("%1, case \"%2\": %3").arg(type.id, test.name, error.message);
        error.node = type.id;
        return {error};
    }
    Errors errors;
    for (const OutputSpec &spec : type.outputs) {
        const Value value = result.value().value(spec.name);
        if (value.isEmpty()) {
            if (!spec.optional)
                errors << caseError("node_bad_output", type, test, QStringLiteral("it returned no value for the required output \"%1\".").arg(spec.name));
            continue;
        }
        outputs.insert(spec.name, value.toJson());
    }
    for (auto it = test.expect.begin(); it != test.expect.end(); ++it) {
        if (!type.output(it.key())) {
            errors << caseError("unknown_output", type, test, QStringLiteral("\"expect\" names output \"%1\", which the node does not have.").arg(it.key()));
        } else if (outputs.value(it.key()) != it.value()) {
            errors << caseError("node_test_failed", type, test,
                                QStringLiteral("output \"%1\" is %2, expected %3.")
                                    .arg(it.key(), QString::fromUtf8(QJsonDocument(QJsonArray{outputs.value(it.key())}).toJson(QJsonDocument::Compact)).mid(1).chopped(1),
                                         QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)));
        }
    }
    return errors;
}

} // namespace

Reply nodeNew(const Command &command, Context &) {
    const QString name = command.positionals.at(0);
    static const QRegularExpression namePattern(QStringLiteral("^[a-z][a-z0-9_]*$"));
    if (!namePattern.match(name).hasMatch())
        return Reply::failure(usageError(QStringLiteral("\"%1\" is not a node name.").arg(name),
                                         "Use lowercase letters, digits and underscores, starting with a letter: sharpen, make_noise."));

    const QString cwd = QDir::currentPath();
    const QString packJsonPath = cwd + "/pack.json";
    QString packFolder, nodeFolder, packId;
    QJsonObject pack;
    const bool inPack = QFileInfo::exists(packJsonPath);
    if (inPack) {
        QFile file(packJsonPath);
        if (!file.open(QIODevice::ReadOnly))
            return Reply::failure(makeError("permission_denied", QStringLiteral("%1 cannot be read.").arg(packJsonPath)));
        pack = QJsonDocument::fromJson(file.readAll()).object();
        packId = pack.value("id").toString();
        if (packId.isEmpty())
            return Reply::failure(makeError("pack_invalid", QStringLiteral("%1 has no \"id\".").arg(packJsonPath)));
        packFolder = cwd;
        nodeFolder = cwd + "/" + name;
        if (packNodeNames(pack).contains(name))
            return Reply::failure(makeError("pack_exists", QStringLiteral("The pack %1 already has a node \"%2\".").arg(packId, name)));
    } else {
        packId = command.has("id") ? command.value("id") : QStringLiteral("io.example.%1").arg(QString(name).replace('_', '-'));
        static const QRegularExpression idPattern(QStringLiteral("^[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)+$"));
        if (!idPattern.match(packId).hasMatch())
            return Reply::failure(usageError(QStringLiteral("\"%1\" is not a pack id.").arg(packId),
                                             "A pack id is lowercase reverse-DNS: io.github.alice.tools."));
        packFolder = cwd + "/" + packId;
        nodeFolder = packFolder + "/" + name;
        pack = QJsonObject{{"manifest", 1}, {"id", packId}, {"version", "0.1.0"}, {"name", titleOf(name)}, {"license", "MIT"}};
    }
    if (QFileInfo::exists(nodeFolder) || (!inPack && QFileInfo::exists(packFolder)))
        return Reply::failure(makeError("pack_exists", QStringLiteral("%1 already exists.").arg(inPack ? nodeFolder : packFolder),
                                        "Pick another name, or remove the folder."));

    QStringList names = packNodeNames(pack);
    names << name;
    pack.insert("nodes", QJsonArray::fromStringList(names));
    if (!writeFile(packFolder + "/pack.json", QJsonDocument(pack).toJson())
        || !writeFile(nodeFolder + "/node.json", QJsonDocument(nodeManifest(packId, name)).toJson())
        || !writeFile(nodeFolder + "/run.sh", kHelloScript.toUtf8(), true)
        || !writeFile(nodeFolder + "/test.json", QJsonDocument(nodeTestFile()).toJson()))
        return Reply::failure(makeError("permission_denied", QStringLiteral("Cannot write in %1.").arg(packFolder)));

    const QString shown = QDir(cwd).relativeFilePath(nodeFolder);
    return Reply::success(QJsonObject{{"pack", packFolder}, {"node", nodeFolder}, {"id", packId + "/" + name}},
                          QStringLiteral("Made %1 (%2/%3).\nTry it:  fugue node test %1\nInstall:  fugue pack add %4")
                              .arg(shown, packId, name, QDir(cwd).relativeFilePath(packFolder)));
}

Reply nodeTest(const Command &command, Context &) {
    const QString path = QDir(command.positionals.at(0)).absolutePath();
    const bool isPack = QFileInfo::exists(path + "/pack.json");
    const QString packFolder = isPack ? path : QFileInfo(path).absolutePath();
    if (!QFileInfo(path).isDir() || (!isPack && !QFileInfo::exists(path + "/node.json")))
        return Reply::failure(makeError("file_not_found", QStringLiteral("%1 is not a node folder (no node.json) or a pack folder (no pack.json).").arg(path)));
    if (!QFileInfo::exists(packFolder + "/pack.json"))
        return Reply::failure(makeError("pack_invalid", QStringLiteral("The node folder's parent %1 has no pack.json.").arg(packFolder),
                                        "A node lives in a pack: <pack>/pack.json and <pack>/<node>/node.json. `fugue node new` makes one."));

    Registry registry;
    const LoadedPack loaded = loadPack(registry, packFolder);
    if (!loaded.enabled) {
        Errors errors = loaded.errors;
        if (errors.isEmpty())
            errors << makeError("pack_invalid", QStringLiteral("The pack %1 did not load.").arg(packFolder));
        return Reply::failure(errors);
    }
    if (!isPack && !loaded.disabled.isEmpty()) {
        for (const PackProblem &problem : loaded.disabled)
            if (QDir(packFolder + "/" + problem.node).absolutePath() == path)
                return Reply::failure(makeError("pack_invalid", QStringLiteral("The node did not load: %1").arg(problem.reason)));
    }

    QTemporaryDir scratch;
    if (!scratch.isValid())
        return Reply::failure(makeError("permission_denied", QStringLiteral("Cannot make a temporary folder.")));
    FileHashes hashes(scratch.filePath("hashes.json"));
    CommandKernel kernel(1000);

    QJsonArray report;
    QStringList lines;
    Errors errors;
    int tested = 0;
    for (const QString &id : loaded.nodes) {
        const NodeType *type = registry.nodeType(id);
        if (!type || (!isPack && QDir(type->dir).absolutePath() != path))
            continue;
        if (type->runtime.value("kind").toString() != "command") {
            lines << QStringLiteral("skip  %1 (runtime %2 is not a command node)").arg(id, type->runtime.value("kind").toString());
            continue;
        }
        const Result<QList<Case>> cases = readCases(*type);
        if (!cases) {
            errors << cases.errors();
            continue;
        }
        for (const Case &test : cases.value()) {
            ++tested;
            QJsonObject outputs;
            QStringList progress;
            QElapsedTimer timer;
            timer.start();
            const Errors failed = runCase(kernel, *type, test, hashes, scratch.path(), outputs, progress);
            const double seconds = timer.elapsed() / 1000.0;
            report.append(QJsonObject{{"node", id}, {"case", test.name}, {"ok", failed.isEmpty()}, {"outputs", outputs},
                                      {"progress", QJsonArray::fromStringList(progress)}, {"seconds", seconds}});
            lines << QStringLiteral("%1  %2  %3 (%4 s)").arg(failed.isEmpty() ? "ok  " : "FAIL", id, test.name, QString::number(seconds, 'f', 2));
            errors << failed;
        }
    }
    kernel.shutdown();
    if (tested == 0 && errors.isEmpty())
        return Reply::failure(makeError("usage", QStringLiteral("No command node to test in %1.").arg(path)));
    if (hasErrors(errors))
        return Reply::failure(errors);
    return Reply::success(QJsonObject{{"tested", tested}, {"cases", report}}, lines.join('\n'));
}

} // namespace fugue::cli
