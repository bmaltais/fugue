// Loading packs from disk (spec 6.4, 6.5): a pack of fake nodes loads, validates and
// registers; a bad manifest disables only what it breaks, with a reason.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/pack_loader.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

QJsonObject nodeJson(const QString &id, const QString &kind = QStringLiteral("command")) {
    QJsonObject runtime{{"kind", kind}};
    if (kind == "command")
        runtime["entry"] = "./run.sh";
    return {{"manifest", 1},
            {"id", id},
            {"version", "1.0.0"},
            {"name", id.section('/', -1)},
            {"runtime", runtime},
            {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
            {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}};
}

QJsonObject packJson(const QString &id, const QStringList &nodes) {
    return {{"manifest", 1},
            {"id", id},
            {"version", "1.2.0"},
            {"name", "Test pack"},
            {"nodes", QJsonArray::fromStringList(nodes)}};
}

void writeJson(const QString &path, const QJsonObject &object) {
    write(path, QJsonDocument(object).toJson());
}

// A pack folder `root/<folder>` with the given nodes.
QString makePack(const QString &root, const QString &folder, const QJsonObject &pack,
                 const QList<QPair<QString, QJsonObject>> &nodes) {
    const QString dir = root + "/" + folder;
    writeJson(dir + "/pack.json", pack);
    for (const auto &[name, manifest] : nodes)
        writeJson(dir + "/" + name + "/node.json", manifest);
    return dir;
}

bool hasCode(const Errors &errors, const QString &code) {
    for (const Error &error : errors)
        if (error.code == code)
            return true;
    return false;
}

QString reasonOf(const LoadedPack &pack, const QString &node) {
    for (const PackProblem &problem : pack.disabled)
        if (problem.node == node)
            return problem.reason;
    return {};
}

} // namespace

class TestPackLoader : public QObject {
    Q_OBJECT

private slots:
    void aPackOfFakeNodesLoadsAndRegisters() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id, packJson(id, {"sharpen", "denoise"}),
                                     {{"sharpen", nodeJson(id + "/sharpen")}, {"denoise", nodeJson(id + "/denoise")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY2(pack.enabled, qPrintable(pack.errors.value(0).message));
        QCOMPARE(pack.id, id);
        QCOMPARE(pack.version, QString("1.2.0"));
        QCOMPARE(pack.nodes, (QStringList{id + "/denoise", id + "/sharpen"}));
        QVERIFY(pack.disabled.isEmpty());
        QVERIFY(registry.nodeType(id + "/sharpen") != nullptr);
        QCOMPARE(registry.packVersion(id), QString("1.2.0"));
        QVERIFY(registry.packSource(id).isEmpty()); // copied in by hand: nowhere to get it
        // A pack added from git says where it came from, for a workflow's requires.packs (#91).
        write(dir + "/.fugue-source.json",
              "{\"kind\": \"git\", \"location\": \"https://github.com/alice/fugue-tools\", \"ref\": \"v1.2.0\"}");
        Registry fromGit;
        QVERIFY(loadPack(fromGit, dir).enabled);
        QCOMPARE(fromGit.packSource(id), QString("https://github.com/alice/fugue-tools"));
        QCOMPARE(fromGit.packRef(id), QString("v1.2.0"));
        // Never a local path or a credential: they would go to everyone the workflow is shared with.
        for (const auto &[location, shared] :
             {std::pair{"file:///home/alice/packs/tools", ""},
              std::pair{"https://alice:ghp_secret@github.com/alice/fugue-tools", "https://github.com/alice/fugue-tools"},
              std::pair{"ssh://git:pw@github.com/alice/fugue-tools", "ssh://git@github.com/alice/fugue-tools"},
              std::pair{"git@github.com:alice/fugue-tools.git", "git@github.com:alice/fugue-tools.git"}}) {
            write(dir + "/.fugue-source.json", QJsonDocument(QJsonObject{{"kind", "git"}, {"location", location},
                                                                          {"ref", "v1"}, {"commit", "abc123"}}).toJson());
            Registry again;
            QVERIFY(loadPack(again, dir).enabled);
            QCOMPARE(again.packSource(id), QString::fromLatin1(shared));
            if (!QString::fromLatin1(shared).isEmpty())
                QCOMPARE(again.packRef(id), QString("abc123")); // the exact commit
        }
        QFile::remove(dir + "/.fugue-source.json");
        // The node's folder, where its program is found.
        QCOMPARE(registry.nodeType(id + "/sharpen")->dir, dir + "/sharpen");
        QVERIFY(registry.nodeType(id + "/sharpen")->seal.isEmpty()); // no run.sh in this fixture
        // With its program in place, the loader seals what the node runs.
        write(dir + "/sharpen/run.sh", "#!/bin/sh\n");
        Registry sealed;
        QVERIFY(loadPack(sealed, dir).enabled);
        QVERIFY(!sealed.nodeType(id + "/sharpen")->seal.isEmpty());
    }

    void declaredTypesAreRegisteredBeforeTheNodesThatUseThem() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject pack = packJson(id, {"mix"});
        pack["types"] = QJsonArray{QJsonObject{{"id", id + "/palette"}, {"version", 1}, {"kind", "data"}, {"name", "Palette"}}};
        QJsonObject node = nodeJson(id + "/mix");
        node["outputs"] = QJsonArray{QJsonObject{{"name", "palette"}, {"type", id + "/palette"}}};
        const QString dir = makePack(root.path(), id, pack, {{"mix", node}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        QVERIFY2(loaded.enabled && loaded.disabled.isEmpty(), qPrintable(reasonOf(loaded, "mix")));
        QCOMPARE(registry.portKind(*PortType::parse(id + "/palette")), PortKind::Data);
    }

    void aResourceTypeNamesItsModelFolders() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject pack = packJson(id, {"mix"});
        pack["types"] = QJsonArray{QJsonObject{{"id", id + "/upscaler"}, {"version", 1}, {"kind", "resource"},
                                               {"name", "Upscaler"}, {"extensions", QJsonArray{".pth"}},
                                               {"folders", QJsonArray{"upscaler", "upscale_models", "ESRGAN"}}}};
        const QString dir = makePack(root.path(), id, pack, {{"mix", nodeJson(id + "/mix")}});
        Registry registry;
        QVERIFY(loadPack(registry, dir).enabled);
        const DeclaredPortType *declared = registry.portType(*PortType::parse(id + "/upscaler"));
        QVERIFY(declared);
        QCOMPARE(declared->folders, (QStringList{"upscaler", "upscale_models", "ESRGAN"}));
    }

    void aMaskRoleIsKeptOnImagePortsAndSurvivesDescribe() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject node = nodeJson(id + "/mix");
        node["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}},
                                    QJsonObject{{"name", "mask"}, {"type", "image"}, {"role", "mask"}}};
        node["outputs"] = QJsonArray{QJsonObject{{"name", "mask"}, {"type", "image"}, {"role", "mask"}}};
        const QString dir = makePack(root.path(), id, packJson(id, {"mix"}), {{"mix", node}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        QVERIFY2(loaded.enabled && loaded.disabled.isEmpty(), qPrintable(reasonOf(loaded, "mix")));
        const NodeType *type = registry.nodeType(id + "/mix");
        QVERIFY(type);
        QCOMPARE(type->inputs.at(0).role, QString());
        QCOMPARE(type->inputs.at(1).role, QString("mask"));
        QCOMPARE(type->outputs.at(0).role, QString("mask"));
        // `fugue node describe` writes it back, and reading that gives the same type.
        const QJsonObject described = type->toJson();
        QCOMPARE(described.value("inputs").toArray().at(1).toObject().value("role").toString(), QString("mask"));
        QVERIFY(!described.value("inputs").toArray().at(0).toObject().contains("role"));
        QCOMPARE(described.value("outputs").toArray().at(0).toObject().value("role").toString(), QString("mask"));
        const Result<NodeType> again = NodeType::fromJson(described);
        QVERIFY(again.ok());
        QCOMPARE(again.value().inputs.at(1).role, QString("mask"));
        QCOMPARE(again.value().outputs.at(0).role, QString("mask"));
    }

    void aRoleOtherThanMaskOrOnAnotherTypeDisablesTheNode() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject other = nodeJson(id + "/other");
        other["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}, {"role", "alpha"}}};
        QJsonObject onNumber = nodeJson(id + "/number");
        onNumber["inputs"] = QJsonArray{QJsonObject{{"name", "n"}, {"type", "number"}, {"role", "mask"}}};
        QJsonObject onOutput = nodeJson(id + "/output");
        onOutput["outputs"] = QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}, {"role", "mask"}}};
        const QString dir = makePack(root.path(), id, packJson(id, {"other", "number", "output"}),
                                     {{"other", other}, {"number", onNumber}, {"output", onOutput}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        for (const char *name : {"other", "number", "output"})
            QVERIFY2(reasonOf(loaded, name).contains("role"), name);
        QVERIFY(!registry.nodeType(id + "/other"));
    }

    // A pack's node can use the mask widget (#174): a text input with `over` naming an image input.
    void aPacksMaskWidgetNamesTheImageItIsPaintedOver() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject good = nodeJson(id + "/good");
        good["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}},
                                    QJsonObject{{"name", "region"}, {"type", "text"}, {"widget", "mask"}, {"over", "image"}, {"optional", true}}};
        QJsonObject nowhere = nodeJson(id + "/nowhere"); // over an input that is not there
        nowhere["inputs"] = QJsonArray{QJsonObject{{"name", "region"}, {"type", "text"}, {"widget", "mask"}, {"over", "image"}}};
        QJsonObject notAWidget = nodeJson(id + "/plain"); // `over` without the mask widget
        notAWidget["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}},
                                          QJsonObject{{"name", "region"}, {"type", "text"}, {"over", "image"}}};
        QJsonObject onImage = nodeJson(id + "/onimage"); // the mask widget on an image input
        onImage["inputs"] = QJsonArray{QJsonObject{{"name", "region"}, {"type", "image"}, {"widget", "mask"}}};
        const QString dir = makePack(root.path(), id, packJson(id, {"good", "nowhere", "plain", "onimage"}),
                                     {{"good", good}, {"nowhere", nowhere}, {"plain", notAWidget}, {"onimage", onImage}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        const NodeType *type = registry.nodeType(id + "/good");
        QVERIFY2(type, qPrintable(reasonOf(loaded, "good")));
        QCOMPARE(type->input("region")->over, QString("image"));
        QVERIFY(type->input("region")->holdsInputFile());
        QCOMPARE(NodeType::fromJson(type->toJson()).value().input("region")->over, QString("image"));
        for (const char *name : {"nowhere", "plain", "onimage"})
            QVERIFY2(!reasonOf(loaded, name).isEmpty(), name);
    }

    // A text input can name its prompt syntax for the text editors to colour (#715): a fixed set, on text inputs with a line widget.
    void aTextInputNamesItsPromptSyntax() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const auto node = [&](const char *name, const QJsonObject &input) {
            QJsonObject n = nodeJson(id + "/" + name);
            n["inputs"] = QJsonArray{input};
            return n;
        };
        const QJsonObject text{{"name", "t"}, {"type", "text"}, {"widget", "multiline"}};
        const auto with = [&](const QJsonValue &syntax) {
            QJsonObject input = text;
            input["syntax"] = syntax;
            return input;
        };
        QJsonObject number = QJsonObject{{"name", "t"}, {"type", "number"}, {"syntax", "template"}};
        const QString dir = makePack(root.path(), id, packJson(id, {"dyn", "tmpl", "plain", "unknown", "number", "choice", "notstring"}),
                                     {{"dyn", node("dyn", with("dynamic-prompt"))}, {"tmpl", node("tmpl", with("template"))},
                                      {"plain", node("plain", text)}, {"unknown", node("unknown", with("markdown"))},
                                      {"number", node("number", number)},
                                      {"choice", node("choice", QJsonObject{{"name", "t"}, {"type", "text"}, {"choices", QJsonArray{"a", "b"}}, {"syntax", "template"}})},
                                      {"notstring", node("notstring", with(3))}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        for (const char *name : {"dyn", "tmpl"}) {
            const NodeType *type = registry.nodeType(id + "/" + name);
            QVERIFY2(type, qPrintable(reasonOf(loaded, name)));
            QCOMPARE(type->input("t")->syntax, QString(name) == "dyn" ? QString("dynamic-prompt") : QString("template"));
            QCOMPARE(NodeType::fromJson(type->toJson()).value().input("t")->syntax, type->input("t")->syntax);
        }
        QVERIFY(registry.nodeType(id + "/plain")->input("t")->syntax.isEmpty());
        for (const char *name : {"unknown", "number", "choice", "notstring"})
            QVERIFY2(!reasonOf(loaded, name).isEmpty(), name);
        QVERIFY(reasonOf(loaded, "unknown").contains("syntax"));
    }

    // The paint widget (#181) is the mask widget's twin: text, `over` naming the picture, an input file.
    void aPacksPaintWidgetIsATextInputFileOverAnImage() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject good = nodeJson(id + "/good");
        good["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}},
                                    QJsonObject{{"name", "art"}, {"type", "text"}, {"widget", "paint"}, {"over", "image"}, {"optional", true}}};
        QJsonObject onImage = nodeJson(id + "/onimage"); // the paint widget on an image input
        onImage["inputs"] = QJsonArray{QJsonObject{{"name", "art"}, {"type", "image"}, {"widget", "paint"}}};
        QJsonObject overLine = nodeJson(id + "/overline"); // `over` on another widget still fails
        overLine["inputs"] = QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}},
                                        QJsonObject{{"name", "art"}, {"type", "text"}, {"widget", "line"}, {"over", "image"}}};
        const QString dir = makePack(root.path(), id, packJson(id, {"good", "onimage", "overline"}),
                                     {{"good", good}, {"onimage", onImage}, {"overline", overLine}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        const NodeType *type = registry.nodeType(id + "/good");
        QVERIFY2(type, qPrintable(reasonOf(loaded, "good")));
        QCOMPARE(type->input("art")->widget(), QString("paint"));
        QCOMPARE(type->input("art")->over, QString("image"));
        QVERIFY(type->input("art")->holdsInputFile());
        QCOMPARE(NodeType::fromJson(type->toJson()).value().input("art")->widget(), QString("paint"));
        for (const char *name : {"onimage", "overline"})
            QVERIFY2(!reasonOf(loaded, name).isEmpty(), name);
    }

    // The canvas widget (#211) holds a canvas id: text, and not an input file (a canvas is not copied with a workflow).
    void aPacksCanvasWidgetIsATextInputThatHoldsNoFile() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject good = nodeJson(id + "/good");
        good["inputs"] = QJsonArray{QJsonObject{{"name", "canvas"}, {"type", "text"}, {"widget", "canvas"}}};
        QJsonObject onNumber = nodeJson(id + "/onnumber");
        onNumber["inputs"] = QJsonArray{QJsonObject{{"name", "canvas"}, {"type", "number"}, {"widget", "canvas"}}};
        const QString dir = makePack(root.path(), id, packJson(id, {"good", "onnumber"}), {{"good", good}, {"onnumber", onNumber}});
        Registry registry;
        const LoadedPack loaded = loadPack(registry, dir);
        const NodeType *type = registry.nodeType(id + "/good");
        QVERIFY2(type, qPrintable(reasonOf(loaded, "good")));
        QCOMPARE(type->input("canvas")->widget(), QString("canvas"));
        QVERIFY(!type->input("canvas")->holdsInputFile());
        QVERIFY(!reasonOf(loaded, "onnumber").isEmpty());
    }

    void aBadNodeDisablesOnlyThatNode() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject bad = nodeJson(id + "/bad");
        bad["version"] = "one";
        const QString dir = makePack(root.path(), id, packJson(id, {"good", "bad", "gone"}),
                                     {{"good", nodeJson(id + "/good")}, {"bad", bad}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(pack.enabled);
        QCOMPARE(pack.nodes, QStringList{id + "/good"});
        QCOMPARE(pack.disabled.size(), 2);
        QCOMPARE(pack.disabled[0].node, QString("bad"));
        QVERIFY(!pack.disabled[0].reason.isEmpty());
        QCOMPARE(pack.disabled[1].node, QString("gone"));
        QVERIFY2(reasonOf(pack, "gone").contains("node.json"), qPrintable(reasonOf(pack, "gone")));
        QVERIFY(registry.nodeType(id + "/bad") == nullptr);
    }

    void aNodeMustBelongToItsPack() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id, packJson(id, {"x"}), {{"x", nodeJson("io.github.bob.tools/x")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(reasonOf(pack, "x").contains(id));
        QVERIFY(registry.nodeType("io.github.bob.tools/x") == nullptr);
    }

    void aNodeIdMustNameItsFolder() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id, packJson(id, {"x"}), {{"x", nodeJson(id + "/y")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(!reasonOf(pack, "x").isEmpty());
    }

    void aBrokenPackJsonDisablesThePack_data() {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("code");
        const auto with = [](const char *key, const QJsonValue &value) {
            QJsonObject pack = packJson("io.github.alice.tools", {"a"});
            if (value.isUndefined())
                pack.remove(key);
            else
                pack[key] = value;
            return QJsonDocument(pack).toJson();
        };
        QTest::newRow("not json") << QByteArray("{") << "invalid_json";
        QTest::newRow("no id") << with("id", QJsonValue::Undefined) << "invalid_document";
        QTest::newRow("uppercase id") << with("id", "Alice.Tools") << "invalid_document";
        QTest::newRow("id without dots") << with("id", "tools") << "invalid_document";
        QTest::newRow("bad version") << with("version", "1.x") << "invalid_document";
        QTest::newRow("no name") << with("name", QJsonValue::Undefined) << "invalid_document";
        QTest::newRow("no nodes") << with("nodes", QJsonValue::Undefined) << "invalid_document";
        QTest::newRow("nodes not a list") << with("nodes", "a") << "invalid_document";
        QTest::newRow("path in nodes") << with("nodes", QJsonArray{"../a"}) << "invalid_document";
        QTest::newRow("absolute node") << with("nodes", QJsonArray{"/etc"}) << "invalid_document";
        QTest::newRow("duplicate node") << with("nodes", QJsonArray{"a", "a"}) << "invalid_document";
        QTest::newRow("newer format") << with("manifest", 2) << "unsupported_format_version";
        QTest::newRow("bad setup") << with("setup", QJsonObject{{"command", "uv sync"}}) << "invalid_document";
        QTest::newRow("reserved id") << with("id", "fugue") << "invalid_document";
        QTest::newRow("look-alike fugue.") << with("id", "fugue.core") << "invalid_document";
        QTest::newRow("look-alike local.") << with("id", "local.nodes") << "invalid_document";
        QTest::newRow("trailing hyphen") << with("id", "io.a-.tools") << "invalid_document";
        QTest::newRow("type id with @") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x@5"}, {"version", 1}, {"kind", "data"}, {"name", "X"}}}) << "invalid_document";
        QTest::newRow("type of another pack") << with("types", QJsonArray{QJsonObject{{"id", "io.github.bob.tools/x"}, {"version", 1}, {"kind", "data"}, {"name", "X"}}}) << "invalid_document";
        QTest::newRow("empty extension") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x"}, {"version", 1}, {"kind", "resource"}, {"name", "X"}, {"extensions", QJsonArray{""}}}}) << "invalid_document";
        QTest::newRow("model folder with a slash") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x"}, {"version", 1}, {"kind", "resource"}, {"name", "X"}, {"folders", QJsonArray{"a/b"}}}}) << "invalid_document";
        QTest::newRow("model folder ..") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x"}, {"version", 1}, {"kind", "resource"}, {"name", "X"}, {"folders", QJsonArray{".."}}}}) << "invalid_document";
        QTest::newRow("hidden model folder") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x"}, {"version", 1}, {"kind", "resource"}, {"name", "X"}, {"folders", QJsonArray{".cache"}}}}) << "invalid_document";
        QTest::newRow("folders on a data type") << with("types", QJsonArray{QJsonObject{{"id", "io.github.alice.tools/x"}, {"version", 1}, {"kind", "data"}, {"name", "X"}, {"folders", QJsonArray{"x"}}}}) << "invalid_document";
        QTest::newRow("uppercase node folder") << with("nodes", QJsonArray{"Blend"}) << "invalid_document";
        QTest::newRow("reserved local") << with("id", "local") << "invalid_document";
    }
    void aBrokenPackJsonDisablesThePack() {
        QFETCH(QByteArray, text);
        QFETCH(QString, code);
        QTemporaryDir root;
        const QString folder = root.path() + "/io.github.alice.tools";
        write(folder + "/pack.json", text);
        write(folder + "/a/node.json", QJsonDocument(nodeJson("io.github.alice.tools/a")).toJson());
        Registry registry;
        const LoadedPack pack = loadPack(registry, folder);
        QVERIFY(!pack.enabled);
        QVERIFY2(hasCode(pack.errors, code), qPrintable(pack.errors.value(0).code + ": " + pack.errors.value(0).message));
        QVERIFY(registry.nodeTypes().isEmpty());
        QCOMPARE(registry.packVersion("io.github.alice.tools"), QString());
    }

    void aMissingPackJsonDisablesThePack() {
        QTemporaryDir root;
        QDir().mkpath(root.path() + "/io.github.alice.tools");
        Registry registry;
        const LoadedPack pack = loadPack(registry, root.path() + "/io.github.alice.tools");
        QVERIFY(!pack.enabled);
        QVERIFY(hasCode(pack.errors, "file_not_found"));
    }

    void runtimeKindsAreChecked_data() {
        QTest::addColumn<QJsonObject>("runtime");
        QTest::addColumn<bool>("loads");
        QTest::newRow("command") << QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}} << true;
        QTest::newRow("persistent") << QJsonObject{{"kind", "command"}, {"entry", "run.py"}, {"mode", "persistent"}} << true;
        QTest::newRow("command without entry") << QJsonObject{{"kind", "command"}} << false;
        QTest::newRow("entry outside") << QJsonObject{{"kind", "command"}, {"entry", "../run.sh"}} << false;
        QTest::newRow("absolute entry") << QJsonObject{{"kind", "command"}, {"entry", "/bin/sh"}} << false;
        QTest::newRow("mode not text") << QJsonObject{{"kind", "command"}, {"entry", "x"}, {"mode", 5}} << false;
        QTest::newRow("native huge minor") << QJsonObject{{"kind", "native"}, {"entry", "libx.so"}, {"abi", "1.99999999999"}} << false;
        QTest::newRow("bad mode") << QJsonObject{{"kind", "command"}, {"entry", "x"}, {"mode", "daemon"}} << false;
        QTest::newRow("group") << QJsonObject{{"kind", "group"}, {"definition", "group.json"}} << true;
        QTest::newRow("group outside") << QJsonObject{{"kind", "group"}, {"definition", "../g.json"}} << false;
        QTest::newRow("native") << QJsonObject{{"kind", "native"}, {"entry", "libx.so"}, {"abi", "1.0"}} << true;
        QTest::newRow("native newer minor") << QJsonObject{{"kind", "native"}, {"entry", "libx.so"}, {"abi", "1.9"}} << false;
        QTest::newRow("native other major") << QJsonObject{{"kind", "native"}, {"entry", "libx.so"}, {"abi", "2.0"}} << false;
        QTest::newRow("native bad abi") << QJsonObject{{"kind", "native"}, {"entry", "libx.so"}, {"abi", "one"}} << false;
        QTest::newRow("script reserved") << QJsonObject{{"kind", "script"}} << false;
        QTest::newRow("builtin only for fugue") << QJsonObject{{"kind", "builtin"}} << false;
        QTest::newRow("unknown kind") << QJsonObject{{"kind", "wasm"}} << false;
    }
    void runtimeKindsAreChecked() {
        QFETCH(QJsonObject, runtime);
        QFETCH(bool, loads);
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject node = nodeJson(id + "/n");
        node["runtime"] = runtime;
        const QString dir = makePack(root.path(), id, packJson(id, {"n"}), {{"n", node}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(pack.enabled);
        QVERIFY2(pack.disabled.isEmpty() == loads, qPrintable(reasonOf(pack, "n")));
    }

    void aPackOnDiskCannotClaimABuiltInIdOrKind() {
        QTemporaryDir root;
        QJsonObject node = nodeJson("fugue/evil", "builtin");
        const QString dir = makePack(root.path(), "fugue", packJson("fugue", {"evil"}), {{"evil", node}});
        Registry registry;
        QVERIFY(!loadPack(registry, dir).enabled);
        QVERIFY(registry.nodeType("fugue/evil") == nullptr);
        QCOMPARE(registry.packVersion("fugue"), QString());
    }

    void theBuiltInPackMayUseTheReservedIdAndKind() {
        QTemporaryDir root;
        const QString dir = makePack(root.path(), "fugue", packJson("fugue", {"thing"}),
                                     {{"thing", nodeJson("fugue/thing", "builtin")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir, PackOrigin::BuiltIn);
        QVERIFY2(pack.enabled && pack.disabled.isEmpty(), qPrintable(reasonOf(pack, "thing")));
        QVERIFY(registry.nodeType("fugue/thing") != nullptr);
    }

    void anAliasMustBelongToThePack_data() {
        QTest::addColumn<QString>("alias");
        QTest::addColumn<bool>("loads");
        QTest::newRow("own") << "io.github.alice.tools/old-name" << true;
        QTest::newRow("builtin") << "fugue/upscale" << false;
        QTest::newRow("local") << "local/foo" << false;
        QTest::newRow("another pack") << "io.github.bob.tools/old" << false;
    }
    void anAliasMustBelongToThePack() {
        QFETCH(QString, alias);
        QFETCH(bool, loads);
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject node = nodeJson(id + "/n");
        node["aliases"] = QJsonArray{alias};
        const QString dir = makePack(root.path(), id, packJson(id, {"n"}), {{"n", node}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY2(pack.disabled.isEmpty() == loads, qPrintable(reasonOf(pack, "n")));
        QCOMPARE(registry.nodeType(alias) != nullptr, loads);
    }

    void aPackFolderMustBeNamedAfterItsId() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), "aaa", packJson(id, {"a"}), {{"a", nodeJson(id + "/a")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(!pack.enabled);
        QCOMPARE(registry.packVersion(id), QString());
    }

    // 6.5, 6.6: a pack folder is named `<id>`, or `<id>@<major>` of its version; the same major
    // twice loads once, other majors load beside each other.
    void majorsOfAPackLoadSideBySide() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject v2 = packJson(id, {"a"});
        v2["version"] = "2.0.0";
        QJsonObject node2 = nodeJson(id + "/a");
        node2["version"] = "2.0.0";
        makePack(root.path(), id, packJson(id, {"a"}), {{"a", nodeJson(id + "/a")}});
        makePack(root.path(), id + "@2", v2, {{"a", node2}});
        Registry registry;
        const PackReport report = loadPacks(registry, {root.path()});
        QCOMPARE(report.packs.size(), 2);
        for (const LoadedPack &pack : report.packs)
            QVERIFY2(pack.enabled, qPrintable(pack.path));
        QCOMPARE(registry.packVersions(id), (QStringList{"1.2.0", "2.0.0"}));
        QCOMPARE(registry.majors(id + "/a"), (QList<int>{1, 2}));
        QCOMPARE(registry.nodeTypeAt(id + "/a", 1)->packVersion, QString("1.2.0"));
        QVERIFY(registry.nodeTypeAt(id + "/a", 1)->dir.endsWith("/" + id + "/a"));
        QCOMPARE(registry.nodeTypeAt(id + "/a", 2)->packVersion, QString("2.0.0"));
        QVERIFY(registry.nodeTypeAt(id + "/a", 2)->dir.contains("@2/a"));
    }

    void aFolderSuffixMustBeTheMajorOfThePack() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id + "@3", packJson(id, {"a"}), {{"a", nodeJson(id + "/a")}}); // version 1.2.0
        Registry registry;
        QVERIFY(!loadPack(registry, dir).enabled);
        QCOMPARE(parsePackRef(id + "@3").major, 3);
        QCOMPARE(parsePackRef(id + "@3").id, id);
        QCOMPARE(parsePackRef(id).major, -1);
    }

    // 3.6: a pack holds an older major of a node beside the newest, in a folder `<name>@<major>`.
    void aPackHoldsAnOlderMajorOfANodeInAnAtFolder() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject old = nodeJson(id + "/a");
        QJsonObject current = nodeJson(id + "/a");
        current["version"] = "2.1.0";
        const QString dir = makePack(root.path(), id, packJson(id, {"a", "a@1"}), {{"a", current}, {"a@1", old}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY2(pack.enabled && pack.disabled.isEmpty(), qPrintable(pack.disabled.value(0).reason));
        QCOMPARE(pack.nodes, QStringList{id + "/a"});
        QCOMPARE(registry.majors(id + "/a"), (QList<int>{1, 2}));
        QVERIFY(registry.nodeTypeAt(id + "/a", 1)->dir.endsWith("/a@1"));
        // The folder's major must be the manifest's.
        const QString wrong = makePack(root.path(), "io.github.alice.other", packJson("io.github.alice.other", {"b@2"}),
                                       {{"b@2", nodeJson("io.github.alice.other/b")}});
        Registry other;
        QVERIFY(reasonOf(loadPack(other, wrong), "b@2").contains("major version 2"));
    }

    void aSymbolicLinkAsNodeFolderIsRefused() {
        QTemporaryDir root, elsewhere;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id, packJson(id, {"a"}), {});
        writeJson(elsewhere.path() + "/node.json", nodeJson(id + "/a"));
        QVERIFY(QFile::link(elsewhere.path(), dir + "/a"));
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(pack.enabled);
        QVERIFY(reasonOf(pack, "a").contains("symbolic link"));
        QVERIFY(registry.nodeTypes().isEmpty());
    }

    void aSearchPathGivenTwiceLoadsItsPacksOnce() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        makePack(root.path(), id, packJson(id, {"a"}), {{"a", nodeJson(id + "/a")}});
        Registry registry;
        const PackReport report = loadPacks(registry, {root.path(), root.path() + "/../" + QFileInfo(root.path()).fileName()});
        QCOMPARE(report.packs.size(), 1);
        QVERIFY(!report.hasProblems());
    }

    void aFolderPackJsonDoesNotListIsAWarning() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        const QString dir = makePack(root.path(), id, packJson(id, {"a"}),
                                     {{"a", nodeJson(id + "/a")}, {"b", nodeJson(id + "/b")}});
        Registry registry;
        const LoadedPack pack = loadPack(registry, dir);
        QVERIFY(pack.enabled);
        QVERIFY(registry.nodeType(id + "/b") == nullptr);
        bool warned = false;
        for (const Error &error : pack.errors)
            warned = warned || (error.severity == Error::Severity::Warning && error.message.contains("\"b\""));
        QVERIFY(warned);
    }

    void loadPacksReadsEverySubfolderOfEveryPath() {
        QTemporaryDir one, two;
        makePack(one.path(), "io.github.a.tools", packJson("io.github.a.tools", {"x"}), {{"x", nodeJson("io.github.a.tools/x")}});
        makePack(two.path(), "io.github.b.tools", packJson("io.github.b.tools", {"x"}), {{"x", nodeJson("io.github.b.tools/x")}});
        QDir().mkpath(one.path() + "/not-a-pack");
        write(one.path() + "/a-file.txt", "x");
        Registry registry;
        const PackReport report = loadPacks(registry, {one.path(), two.path(), one.path() + "/missing"});
        QCOMPARE(report.packs.size(), 3); // not-a-pack is reported, a-file.txt is not
        QVERIFY(registry.nodeType("io.github.a.tools/x") != nullptr);
        QVERIFY(registry.nodeType("io.github.b.tools/x") != nullptr);
        QVERIFY(report.hasProblems());
    }

    void aPackIdFoundTwiceLoadsOnceFromTheEarlierPath() {
        QTemporaryDir one, two;
        const QString id = "io.github.a.tools";
        makePack(one.path(), id, packJson(id, {"x"}), {{"x", nodeJson(id + "/x")}});
        QJsonObject newer = packJson(id, {"x"});
        newer["version"] = "1.9.0"; // the same major
        makePack(two.path(), id, newer, {{"x", nodeJson(id + "/x")}});
        Registry registry;
        const PackReport report = loadPacks(registry, {one.path(), two.path()});
        QCOMPARE(report.packs.size(), 2);
        QVERIFY(report.packs[0].enabled);
        QVERIFY(!report.packs[1].enabled);
        QVERIFY(hasCode(report.packs[1].errors, "invalid_document"));
        QCOMPARE(registry.packVersion(id), QString("1.2.0"));
    }

    void aPackWhoseNodesAllFailIsStillReported() {
        QTemporaryDir root;
        const QString id = "io.github.alice.tools";
        QJsonObject bad = nodeJson(id + "/a");
        bad["version"] = "x";
        makePack(root.path(), id, packJson(id, {"a"}), {{"a", bad}});
        Registry registry;
        const PackReport report = loadPacks(registry, {root.path()});
        QVERIFY(report.hasProblems());
        QVERIFY(registry.nodeTypes().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPackLoader)
#include "test_pack_loader.moc"
