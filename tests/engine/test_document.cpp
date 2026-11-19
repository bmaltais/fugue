// The .fugue document (spec 4): load, save, round trip, shape validation and
// requirements. Fixtures are in data/.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

#include "engine/canonical.h"
#include "engine/document.h"
#include "engine/hash.h"
#include "engine/ids.h"
#include "engine/json.h"

using namespace fugue;

namespace {

QByteArray fixture(const char *name) {
    QFile file(QFINDTESTDATA(QStringLiteral("data/") + QLatin1String(name)));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", name);
    return file.readAll();
}

QString dump(const Errors &errors) {
    return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson());
}

QStringList codes(const Errors &errors) {
    QStringList list;
    for (const Error &error : errors)
        list << error.code;
    return list;
}

// The first error about this node (and input, when given) whose message has `word`.
const Error *find(const Errors &errors, const QString &node, const QString &input, const QString &word) {
    for (const Error &error : errors)
        if (error.node == node && error.input == input &&
            error.message.contains(word, Qt::CaseInsensitive))
            return &error;
    return nullptr;
}

std::optional<Document> load(const QByteArray &bytes) {
    Errors errors;
    std::optional<Document> document = Document::load(bytes, errors);
    if (!document || !errors.isEmpty())
        qWarning().noquote() << dump(errors);
    return document;
}

const QByteArray minimal = "{\n  \"fugue\": 1,\n  \"graph\": {\n    \"nodes\": {}\n  }\n}\n";

} // namespace

class TestDocument : public QObject {
    Q_OBJECT

private slots:
    void portRefs() {
        QCOMPARE(PortRef::parse("gen.image"), (std::optional<PortRef>(PortRef{"gen", "image"})));
        QCOMPARE((PortRef{"g7/first", "image"}.toString()), QStringLiteral("g7/first.image"));
        QVERIFY(!PortRef::parse("gen"));
        QVERIFY(!PortRef::parse("gen.image.x"));
        QVERIFY(!PortRef::parse(".image"));
        QVERIFY(!PortRef::parse("gen."));
    }

    void inputValueForms() {
        QCOMPARE(InputValue::literal(3).kind(), InputValue::Kind::Literal);
        QCOMPARE(InputValue::literal("x").literal(), QJsonValue("x"));

        const InputValue link = InputValue::link(PortRef{"gen", "image"});
        QCOMPARE(link.kind(), InputValue::Kind::Link);
        QCOMPARE(link.from(), (PortRef{"gen", "image"}));
        QCOMPARE(link.toJson(), QJsonValue(QJsonObject{{"from", "gen.image"}}));

        QCOMPARE(InputValue::autoRandom().toJson(), QJsonValue(QJsonObject{{"auto", "random"}}));
        QCOMPARE(InputValue::autoRandom().kind(), InputValue::Kind::Auto);

        const QJsonObject handle{{"resource", "fugue/lora"}, {"name", "x.safetensors"}};
        QCOMPARE(InputValue::resource(handle).kind(), InputValue::Kind::Resource);
        QCOMPARE(InputValue::resource(handle).handle(), handle);

        const InputValue list = InputValue::list({link, InputValue::literal(2)});
        QCOMPARE(list.kind(), InputValue::Kind::List);
        QCOMPARE(list.items().size(), 2);
        QCOMPARE(list.items().at(0).from(), (PortRef{"gen", "image"}));
        QCOMPARE(list.toJson(), QJsonValue(QJsonArray{QJsonObject{{"from", "gen.image"}}, 2}));
    }

    void specExampleLoads() {
        const std::optional<Document> document = load(fixture("full.fugue"));
        QVERIFY(document);
        QCOMPARE(document->format, 1);
        QCOMPARE(document->graph.nodes.keys(), (QStringList{"gen", "merge", "prompt", "save", "two"}));
        QCOMPARE(document->graph.provider, QStringLiteral("omarchy-local"));

        const Node &gen = document->graph.nodes["gen"];
        QCOMPARE(gen.type, QStringLiteral("fugue/generate-image"));
        QCOMPARE(gen.version.toString(), QStringLiteral("1.0.0"));
        QCOMPARE(gen.provider, QStringLiteral("omarchy-local"));
        QCOMPARE(gen.mode, NodeMode::Normal);
        QCOMPARE(gen.inputs["width"].literal(), QJsonValue(1024));
        QCOMPARE(gen.inputs["prompt"].from(), (PortRef{"prompt", "text"}));
        QCOMPARE(gen.inputs["seed"].kind(), InputValue::Kind::Auto);
        QCOMPARE(gen.inputs["lora"].kind(), InputValue::Kind::Resource);

        QCOMPARE(document->graph.nodes["merge"].mode, NodeMode::Bypass);
        QCOMPARE(document->graph.nodes["merge"].inputs["images"].items().at(1).from(), (PortRef{"two", "image"}));
        QCOMPARE(document->graph.nodes["two"].mode, NodeMode::Mute);
        QCOMPARE(document->graph.nodes["save"].frozen,
                 QStringLiteral("sha256:3333333333333333333333333333333333333333333333333333333333333333"));

        const GroupDefinition *group = document->group("local/two-pass");
        QVERIFY(group);
        QVERIFY(!document->group("local/other"));
        QCOMPARE(group->name, QStringLiteral("Two-pass generate"));
        QCOMPARE(group->inputs.size(), 4);
        QCOMPARE(group->inputs[0].targets, (QList<PortRef>{{"first", "prompt"}, {"second", "prompt"}}));
        QCOMPARE(group->inputs[1].label, QStringLiteral("Seed"));
        QCOMPARE(group->inputs[2].defaultValue->toJson(), QJsonValue("fast"));
        QCOMPARE(group->inputs[2].choices, std::optional<QStringList>(QStringList{"fast", "fine"}));
        QCOMPARE(group->inputs[3].min, std::optional<double>(1));
        QCOMPARE(group->outputs[0].from, (PortRef{"second", "image"}));
        QCOMPARE(group->nodes["second"].inputs["image"].from(), (PortRef{"first", "image"}));
    }

    void normalFormRoundTripsToTheSameBytes() {
        const QByteArray bytes = fixture("full.fugue");
        const std::optional<Document> document = load(bytes);
        QVERIFY(document);
        QCOMPARE(document->save(), bytes);
    }

    void anyLayoutSavesToTheNormalForm() {
        // Same content as full.fugue, keys shuffled, other number spellings, and
        // one node without "inputs".
        const std::optional<Document> document = load(fixture("messy.fugue"));
        QVERIFY(document);
        const QByteArray saved = document->save();
        QCOMPARE(saved, fixture("full.fugue"));
        QCOMPARE(load(saved)->save(), saved);
    }

    void absentSectionsStayAbsentAndEmptyOnesStay() {
        QCOMPARE(load(minimal)->save(), minimal);

        const QByteArray empty = "{\n  \"fugue\": 1,\n  \"graph\": {\n    \"nodes\": {}\n  },\n"
                                 "  \"groups\": {},\n  \"layout\": {},\n  \"requires\": {}\n}\n";
        QCOMPARE(load(empty)->save(), empty);
    }

    void aDocumentBuiltInCodeSaves() {
        Document document;
        Node text;
        text.type = "fugue/text";
        text.version = *Version::parse("1.0.0");
        text.inputs["text"] = InputValue::literal("fox");
        Node save;
        save.type = "fugue/save-image";
        save.version = *Version::parse("1.0.0");
        save.mode = NodeMode::Mute;
        save.inputs["image"] = InputValue::link({"text", "text"});
        document.graph.nodes["prompt"] = text;
        document.graph.nodes["save"] = save;

        const QByteArray expected = "{\n"
                                    "  \"fugue\": 1,\n"
                                    "  \"graph\": {\n"
                                    "    \"nodes\": {\n"
                                    "      \"prompt\": {\n"
                                    "        \"inputs\": {\n"
                                    "          \"text\": \"fox\"\n"
                                    "        },\n"
                                    "        \"type\": \"fugue/text\",\n"
                                    "        \"version\": \"1.0.0\"\n"
                                    "      },\n"
                                    "      \"save\": {\n"
                                    "        \"inputs\": {\n"
                                    "          \"image\": {\n"
                                    "            \"from\": \"text.text\"\n"
                                    "          }\n"
                                    "        },\n"
                                    "        \"mode\": \"mute\",\n"
                                    "        \"type\": \"fugue/save-image\",\n"
                                    "        \"version\": \"1.0.0\"\n"
                                    "      }\n"
                                    "    }\n"
                                    "  }\n"
                                    "}\n";
        QCOMPARE(document.save(), expected);

        // Reading a missing input through operator[] adds an empty value,
        // which is not written.
        QVERIFY(document.graph.nodes["prompt"].inputs["absent"].isEmpty());
        QCOMPARE(document.save(), expected);
    }

    void shapeErrorsAreAllReported() {
        Errors errors;
        QVERIFY(!Document::load(fixture("broken.fugue"), errors));
        const QString all = dump(errors);
        for (const Error &error : errors) {
            QCOMPARE(error.code, QStringLiteral("invalid_document"));
            QCOMPARE(error.severity, Error::Severity::Error);
        }

        QVERIFY2(find(errors, "", "", "Bad.Id"), qPrintable(all));
        QVERIFY2(find(errors, "a", "", "type"), qPrintable(all));
        QVERIFY2(find(errors, "a", "text", "null"), qPrintable(all));
        QVERIFY2(find(errors, "b", "", "fugue/Text"), qPrintable(all));
        QVERIFY2(find(errors, "b", "", "version"), qPrintable(all));
        QVERIFY2(find(errors, "b", "Bad", "name"), qPrintable(all));
        QVERIFY2(find(errors, "c", "", "provider"), qPrintable(all));
        QVERIFY2(find(errors, "c", "nested", "list"), qPrintable(all));
        QVERIFY2(find(errors, "c", "half", "\"a\""), qPrintable(all));
        QVERIFY2(find(errors, "c", "extra", "strength"), qPrintable(all));
        QVERIFY2(find(errors, "c", "shapeless", "link"), qPrintable(all));
        QVERIFY2(find(errors, "c", "located", "path"), qPrintable(all));
        QVERIFY2(find(errors, "c", "hashed", "hash"), qPrintable(all));
        QVERIFY2(find(errors, "c", "builtin", "image"), qPrintable(all));
        QVERIFY2(find(errors, "c", "nulls", "null"), qPrintable(all));
        QVERIFY2(find(errors, "d", "", "object"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "graph.provider"), qPrintable(all));

        const Error *mode = find(errors, "b", "", "mode");
        QVERIFY2(mode, qPrintable(all));
        QCOMPARE(mode->valid, (QJsonArray{"bypass", "mute"}));
        const Error *fixed = find(errors, "c", "fixed", "auto");
        QVERIFY2(fixed, qPrintable(all));
        QCOMPARE(fixed->valid, QJsonArray{"random"});

        // Inside a group definition `node` is the inner id and the message
        // names the group.
        QVERIFY2(find(errors, "", "", "local/g: \"name\""), qPrintable(all));
        QVERIFY2(find(errors, "", "x", "nodot"), qPrintable(all));
        QVERIFY2(find(errors, "", "x", "more than once"), qPrintable(all));
        QVERIFY2(find(errors, "", "x", "targets"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "\"bad\""), qPrintable(all));
        QVERIFY2(find(errors, "inner", "", "local/g"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "NotAnId"), qPrintable(all));

        QVERIFY2(find(errors, "", "", "layout.nodes.a.pos"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "layout.view.zoom"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "layout.frames"), qPrintable(all));
        QVERIFY2(find(errors, "", "", "requires.nodes"), qPrintable(all));
    }

    void formatVersionIsChecked() {
        Errors errors;
        QVERIFY(!Document::load("{\"fugue\": 2, \"graph\": 5}", errors));
        // A newer format is refused alone: its other fields may mean other things.
        QCOMPARE(codes(errors), QStringList{"unsupported_format_version"});

        for (const QByteArray &bad : {QByteArray("{\"fugue\": \"1\", \"graph\": {\"nodes\": {}}}"),
                                      QByteArray("{\"fugue\": 1.5, \"graph\": {\"nodes\": {}}}"),
                                      QByteArray("{\"fugue\": 0, \"graph\": {\"nodes\": {}}}"),
                                      QByteArray("{\"graph\": {\"nodes\": {}}}")}) {
            errors.clear();
            QVERIFY(!Document::load(bad, errors));
            QCOMPARE(codes(errors), QStringList{"invalid_document"});
        }
    }

    void sectionsMustHaveTheirShape() {
        Errors errors;
        QVERIFY(!Document::load("{\"fugue\": 1, \"groups\": [], \"layout\": 1, \"requires\": \"x\", \"run\": []}",
                                errors));
        const QString all = dump(errors);
        QCOMPARE(errors.size(), 5);
        for (const char *word : {"graph", "groups", "layout", "requires", "run"})
            QVERIFY2(find(errors, "", "", QString::fromLatin1(word)), word);

        errors.clear();
        QVERIFY(!Document::load("{\"fugue\": 1, \"graph\": {}}", errors));
        QVERIFY(find(errors, "", "", "graph.nodes"));
    }

    void invalidJsonSaysWhere() {
        Errors errors;
        QVERIFY(!Document::load("{\n  \"fugue\": 1,\n  \"graph\": {\n}", errors));
        QCOMPARE(codes(errors), QStringList{"invalid_json"});
        QVERIFY2(errors.first().message.contains("line 4"), qPrintable(errors.first().message));

        errors.clear();
        QVERIFY(!Document::load("[1]", errors));
        QCOMPARE(codes(errors), QStringList{"invalid_document"});

        errors.clear();
        QVERIFY(!Document::load("", errors));
        QCOMPARE(codes(errors), QStringList{"invalid_json"});
    }

    void byteOrderMarkIsAccepted() {
        QVERIFY(load("\xef\xbb\xbf" + minimal));
        QCOMPARE(load("\xef\xbb\xbf" + minimal)->save(), minimal);
    }

    void textMustBeValidUnicode() {
        // A lone surrogate is reported where it is, beside the other errors.
        Errors errors;
        QVERIFY(!Document::load("{\"fugue\": 1, \"graph\": {\"nodes\": {\"a\": {\"type\": \"fugue/text\", "
                                "\"version\": \"1.0.0\", \"inputs\": {\"text\": \"\\ud800\"}}, "
                                "\"b\": {\"type\": \"fugue/text\"}}}}",
                                errors));
        QCOMPARE(codes(errors), (QStringList{"invalid_document", "invalid_document"}));
        QVERIFY2(find(errors, "", "", "graph.nodes.a.inputs.text"), qPrintable(dump(errors)));
        QVERIFY2(find(errors, "", "", "Unicode"), qPrintable(dump(errors)));
        QVERIFY2(find(errors, "b", "", "version"), qPrintable(dump(errors)));
    }

    void newNodeIdsAreSixFreeCharacters() {
        Nodes taken;
        for (int i = 0; i < 2000; ++i) {
            const QString id = newNodeId(taken);
            QCOMPARE(id.size(), 6);
            QVERIFY2(isNodeId(id), qPrintable(id));
            QVERIFY2(!taken.contains(id), qPrintable(id));
            taken.insert(id, Node{});
        }
    }

    void files() {
        QTemporaryDir dir;
        Errors errors;
        QVERIFY(!Document::loadFile(dir.filePath("missing.fugue"), errors));
        QCOMPARE(codes(errors), QStringList{"file_not_found"});

        const std::optional<Document> document = load(fixture("full.fugue"));
        const QString path = dir.filePath("saved.fugue");
        errors.clear();
        QVERIFY(document->saveFile(path, errors));
        QVERIFY(errors.isEmpty());
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), fixture("full.fugue"));

        const std::optional<Document> again = Document::loadFile(path, errors);
        QVERIFY2(again, qPrintable(dump(errors)));
        QCOMPARE(again->save(), document->save());

        errors.clear();
        QVERIFY(!document->saveFile(dir.filePath("no/such/folder/x.fugue"), errors));
        QCOMPARE(codes(errors), QStringList{"permission_denied"});

        if (geteuid() != 0) {
            QVERIFY(QFile::setPermissions(path, QFileDevice::Permissions()));
            errors.clear();
            QVERIFY(!Document::loadFile(path, errors));
            QCOMPARE(codes(errors), QStringList{"permission_denied"});
        }
    }

    void sourceHashCoversOnlyGraphAndGroups() {
        Document document = *load(fixture("full.fugue"));
        const QString hash = document.sourceHash();
        QVERIFY(isSha256(hash));

        // The independent form: canonical JSON of {"graph", "groups"} as in the file.
        const QJsonObject file = QJsonDocument::fromJson(fixture("full.fugue")).object();
        QCOMPARE(hash, canonicalHash(QJsonObject{{"graph", file["graph"]}, {"groups", file["groups"]}}));

        Document moved = document;
        moved.layout["view"] = QJsonObject{{"zoom", 2}};
        moved.requirements = {};
        moved.run = {};
        QCOMPARE(moved.sourceHash(), hash);

        Document changed = document;
        changed.graph.nodes["gen"].inputs["width"] = InputValue::literal(512);
        QVERIFY(changed.sourceHash() != hash);

        // An empty groups section and an absent one are the same source.
        Document bare = *load(minimal);
        Document empty = bare;
        empty.present.groups = true;
        QCOMPARE(bare.sourceHash(), empty.sourceHash());
        QCOMPARE(bare.sourceHash(), canonicalHash(QJsonObject{{"graph", QJsonObject{{"nodes", QJsonObject{}}}},
                                                              {"groups", QJsonObject{}}}));
    }

    void requirementsAreRecomputed() {
        Document document = *load(fixture("full.fugue"));
        // Stale entries that must go, and a model in a group default and a
        // derived handle that must be found.
        document.requirements["nodes"] = QJsonObject{{"io.github.old.pack/gone", "1.0.0"}};
        QJsonObject packs = document.requirements["packs"].toObject();
        packs["io.github.old.pack"] = QJsonObject{{"version", "0.1.0"}};
        document.requirements["packs"] = packs;
        document.requirements["x-kept"] = true;
        GroupDefinition &group = document.groups["local/two-pass"];
        group.inputs[1].defaultValue = InputValue::resource(QJsonObject{
            {"resource", "fugue/vae"},
            {"hash", "sha256:2222222222222222222222222222222222222222222222222222222222222222"}});
        document.graph.nodes["prompt"].inputs["model"] = InputValue::list({InputValue::resource(QJsonObject{
            {"resource", "fugue/diffusion-model"},
            {"name", "served-by-name"},
            {"derived",
             QJsonObject{{"apply", QJsonArray{QJsonObject{
                                       {"lora",
                                        QJsonObject{{"resource", "fugue/lora"},
                                                    {"hash", "sha256:1111111111111111111111111111111111111111111111111111111111111111"},
                                                    {"name", "detail-tweaker.safetensors"}}},
                                       {"strength", 0.8}}}}}}})});

        document.refreshRequirements("0.2.0");

        const QJsonObject expected{
            {"fugue", "0.2.0"},
            {"models",
             QJsonArray{
                 QJsonObject{{"hash", "sha256:1111111111111111111111111111111111111111111111111111111111111111"},
                             {"name", "detail-tweaker.safetensors"},
                             {"resource", "fugue/lora"},
                             {"source", "https://example.org/detail-tweaker"}},
                 QJsonObject{{"hash", "sha256:2222222222222222222222222222222222222222222222222222222222222222"},
                             {"resource", "fugue/vae"}}}},
            // Every node type used, inside groups too, but not the group itself;
            // the same type at two versions records the highest.
            {"nodes", QJsonObject{{"fugue/generate-image", "1.1.0"},
                                  {"fugue/save-image", "1.0.0"},
                                  {"fugue/text", "1.0.0"},
                                  {"io.github.alice.tools/merge", "1.2.0"}}},
            {"packs", QJsonObject{{"io.github.alice.tools",
                                   QJsonObject{{"source", "https://github.com/alice/fugue-tools"},
                                               {"version", "1.2.0"}}}}},
            {"x-kept", true},
        };
        QCOMPARE(document.requirements, expected);
        QVERIFY(document.present.requirements);

        Document bare = *load(minimal);
        bare.refreshRequirements("0.2.0");
        QCOMPARE(bare.requirements, (QJsonObject{{"fugue", "0.2.0"}, {"nodes", QJsonObject{}}}));
    }
};

QTEST_GUILESS_MAIN(TestDocument)
#include "test_document.moc"
