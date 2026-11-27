// Repeating inputs in a manifest (spec 6.1, #714) and Apply LoRAs, the node that uses them: the manifest's `repeat`
// block and the row inputs it names (`lora_2`), the file, the compiler and its per-row errors, and the kernel, whose
// model must be the one a chain of Apply LoRA nodes makes.
//
// Seams: NodeType::fromJson and toJson, Document::load and save, compile(), and the kernels as the scheduler drives
// them (defaults filled in, node_kernel.h).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"

using namespace fugue;

namespace {

const QString kApplyLoras = QStringLiteral("fugue/apply-loras");
const QString kApplyLora = QStringLiteral("fugue/apply-lora");
const QString kLoadModels = QStringLiteral("fugue/load-models");

// JSON written with single quotes, so fixtures stay readable without raw string literals (which moc skips).
QJsonObject json(const char *text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        qFatal("bad test JSON: %s", text);
    return document.object();
}

// A safetensors file's header with these tensor shapes and no data: what model_shape.h reads.
QByteArray safetensors(const QJsonObject &shapes) {
    QJsonObject header;
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        header.insert(it.key(), QJsonObject{{"dtype", "F16"}, {"shape", it.value()}, {"data_offsets", QJsonArray{0, 0}}});
    const QByteArray text = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(text.size()) >> (8 * i)) & 0xff));
    return out + text;
}

// A manifest with one repeat block, `block` replacing the default one.
QJsonObject manifestWith(const QJsonObject &block, const QJsonArray &more = {}) {
    QJsonArray inputs{QJsonObject{{"name", "model"}, {"type", "number"}}, block};
    for (const QJsonValue &input : more)
        inputs.append(input);
    return QJsonObject{{"manifest", 1}, {"id", "local/rows"}, {"version", "1.0.0"}, {"name", "Rows"},
                       {"runtime", QJsonObject{{"kind", "builtin"}}}, {"inputs", inputs}, {"outputs", QJsonArray{}}};
}

QJsonObject rowsBlock() {
    return json("{'repeat': 'items', 'inputs': [{'name': 'text', 'type': 'text', 'optional': true},"
                " {'name': 'weight', 'type': 'number', 'default': 1, 'min': 0, 'max': 2}]}");
}

QStringList names(const QList<const InputSpec *> &specs) {
    QStringList list;
    for (const InputSpec *spec : specs)
        list << spec->name;
    return list;
}

QJsonObject handle(const QString &type, const QString &name) {
    return QJsonObject{{"resource", type}, {"hash", "sha256:" + QString(64, name.at(0))}, {"name", name}};
}

} // namespace

class TestApplyLoras : public QObject {
    Q_OBJECT

    Registry m_registry;
    Kernels m_kernels;
    QTemporaryDir m_dir;
    std::unique_ptr<FileHashes> m_hashes;

    const NodeType &type(const QString &id) const { return *m_registry.nodeType(id); }

    // The kernel's inputs as the scheduler hands them over: every input the node has, defaults filled in.
    NodeContext context(const QString &id, Values inputs) {
        for (const InputSpec *spec : type(id).inputsOf(inputs))
            if (!inputs.contains(spec->name) && !spec->defaultValue.isUndefined())
                inputs.insert(spec->name, Value(spec->defaultValue));
        NodeContext context;
        context.node = QStringLiteral("n");
        context.type = &type(id);
        context.inputs = inputs;
        context.workDir = m_dir.path();
        context.hashes = m_hashes.get();
        context.registry = &m_registry;
        return context;
    }
    Result<Values> run(const QString &id, const Values &inputs) { return m_kernels.node(id)->run(context(id, inputs)); }

    // A model file in the test's own models folder, and its handle.
    QJsonObject model(const QString &type, const QString &file, const QByteArray &bytes) {
        const QString path = dataDir() + "/models/" + file;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size())
            qFatal("cannot write %s", qPrintable(path));
        return QJsonObject{{"resource", type}, {"hash", sha256(bytes)}, {"name", QFileInfo(file).fileName()}};
    }

    Compilation compiled(const QJsonObject &apply) {
        QJsonObject nodes{{"models", QJsonObject{{"type", kLoadModels}, {"version", "1.0.0"}, {"inputs", QJsonObject{}}}},
                          {"apply", apply}};
        const Result<Document> document = Document::fromJson(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}});
        if (!document)
            qFatal("fixture does not load: %s", qPrintable(document.error().message));
        CompileOptions options;
        options.allowUnsetInputs = true; // Load Models' parts are not the point here
        options.target = QStringLiteral("apply");
        return compile(*document, m_registry, options);
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(m_dir.filePath("hashes.json"));
    }

    // The block parses into rows of inputs with names of their own, up to its max, and is written back as it was read.
    void aRepeatBlockNamesEachRowsInputs() {
        const NodeType &apply = type(kApplyLoras);
        QCOMPARE(apply.repeats.size(), 1);
        QCOMPARE(apply.inputs.size(), 1); // the model; the rows are not declared inputs
        const InputSpec *strength = apply.input(QStringLiteral("strength_3"));
        QVERIFY(strength);
        QCOMPARE(strength->name, QStringLiteral("strength_3"));
        QCOMPARE(*strength->max, 4.0);
        QCOMPARE(apply.input(QStringLiteral("lora_2"))->label, QStringLiteral("LoRA 2"));
        QCOMPARE(apply.input(QStringLiteral("enabled_2"))->defaultValue, QJsonValue(true));
        QCOMPARE(apply.input(QStringLiteral("lora_99"))->name, QStringLiteral("lora_99"));
        QVERIFY(!apply.input(QStringLiteral("lora_100"))); // past the most rows
        QVERIFY(!apply.input(QStringLiteral("lora_0")));
        QVERIFY(!apply.input(QStringLiteral("lora_02")));
        QVERIFY(!apply.input(QStringLiteral("lora")));
        QVERIFY(!apply.input(QStringLiteral("loras")));

        const Result<NodeType> again = NodeType::fromJson(apply.toJson());
        QVERIFY(again);
        QCOMPARE(again->toJson(), apply.toJson());
        QCOMPARE(apply.toJson().value("inputs").toArray().at(1).toObject().value("repeat").toString(), QStringLiteral("loras"));

        // The rows that are there, by their key, in row order at the block's place; gaps are kept.
        Values values{{"model", Value(1.0)}, {"lora_1", Value(1.0)}, {"strength_2", Value(1.0)}, {"lora_4", Value(1.0)}};
        QCOMPARE(names(apply.inputsOf(values)),
                 (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_4", "strength_4", "enabled_4"}));
    }

    // What a manifest cannot say, each refused with the reason.
    void aRepeatBlockThatCannotWorkIsRefused() {
        QVERIFY(NodeType::fromJson(manifestWith(rowsBlock())));
        const auto refused = [](const QJsonObject &manifest, const QString &words) {
            const Result<NodeType> type = NodeType::fromJson(manifest);
            if (type)
                return false;
            for (const Error &error : type.errors())
                if (error.message.contains(words))
                    return true;
            qWarning() << type.errors().first().message;
            return false;
        };
        QVERIFY(refused(manifestWith(rowsBlock(), {json("{'name': 'text_2', 'type': 'text'}")}), "has the name of a row"));
        QJsonObject keyDefault = rowsBlock();
        keyDefault["inputs"] = QJsonArray{json("{'name': 'text', 'type': 'text', 'default': 'x'}")};
        QVERIFY(refused(manifestWith(keyDefault), "row's key and cannot have a default"));
        QJsonObject numbered = rowsBlock();
        numbered["inputs"] = QJsonArray{json("{'name': 'text_1', 'type': 'text'}")};
        QVERIFY(refused(manifestWith(numbered), "ends in _ and a number"));
        QJsonObject list = rowsBlock();
        list["inputs"] = QJsonArray{json("{'name': 'text', 'type': 'text', 'list': true}")};
        QVERIFY(refused(manifestWith(list), "is a list or lazy"));
        QJsonObject file = rowsBlock();
        file["inputs"] = QJsonArray{json("{'name': 'path', 'type': 'text', 'widget': 'file'}")};
        QVERIFY(refused(manifestWith(file), "holds an input file"));
        QJsonObject big = rowsBlock();
        big["max"] = 100;
        QVERIFY(refused(manifestWith(big), "outside 1 to 99"));
        QVERIFY(refused(manifestWith(rowsBlock(), {json("{'name': 'items', 'type': 'text'}")}), "declared more than once"));
    }

    // A workflow with rows is an ordinary file: links and literals by the row's input name, kept byte for byte.
    void rowsRoundTripInTheFile() {
        const QByteArray file = QJsonDocument(json(
            "{'fugue': 1, 'graph': {'nodes': {'apply': {'type': 'fugue/apply-loras', 'version': '1.0.0', 'inputs': {"
            "'model': {'from': 'models.model'}, 'lora_1': {'resource': 'fugue/lora', 'name': 'a.safetensors'},"
            " 'strength_1': 0.5, 'lora_3': {'from': 'pick.lora'}, 'enabled_3': false}}}},"
            " 'layout': {'links': {'apply.lora_3': {'points': [[1, 2]]}}}}")).toJson();
        const Result<Document> loaded = Document::load(file);
        QVERIFY(loaded);
        QCOMPARE(Document::load(loaded->save())->save(), loaded->save());
        QCOMPARE(loaded->graph.nodes.value("apply").inputs.value("lora_3").from().toString(), QStringLiteral("pick.lora"));
    }

    // The compiler checks each row's values as the inputs they are, says so of a value whose row is not there, and refuses a
    // row past the most there can be.
    void eachRowIsCheckedByItsOwnName() {
        const QJsonObject lora = handle("fugue/lora", "a.safetensors");
        QJsonObject apply{{"type", kApplyLoras},
                          {"version", "1.0.0"},
                          {"inputs", QJsonObject{{"model", QJsonObject{{"from", "models.model"}}},
                                                 {"lora_1", lora},
                                                 {"strength_1", 0.5},
                                                 {"lora_2", lora},
                                                 {"strength_2", 9},
                                                 {"strength_5", 1}}}};
        const Compilation bad = compiled(apply);
        QVERIFY(!bad);
        QStringList errors, warnings;
        for (const Error &error : bad.errors())
            (error.severity == Error::Severity::Warning ? warnings : errors) << error.code + " " + error.input;
        QCOMPARE(errors, QStringList{"value_out_of_range strength_2"});
        QCOMPARE(warnings, QStringList{"unknown_input strength_5"});

        QJsonObject inputs = apply.value("inputs").toObject();
        inputs.insert("strength_2", 0.25);
        inputs.remove("strength_5");
        inputs.insert("lora_100", lora);
        apply["inputs"] = inputs;
        const Compilation past = compiled(apply);
        QVERIFY(!past);
        QCOMPARE(past.error().code, QStringLiteral("unknown_input"));
        QCOMPARE(past.error().input, QStringLiteral("lora_100"));

        inputs.remove("lora_100");
        apply["inputs"] = inputs;
        const Compilation good = compiled(apply);
        QVERIFY2(good, good ? "" : qPrintable(good.error().message));
        const QMap<QString, InputValue> made = good->nodes.value("apply").inputs;
        QCOMPARE(made.value("strength_2").literal(), QJsonValue(0.25));
        QCOMPARE(made.value("lora_2").handle(), lora);
    }

    // Every row that is on, in row order, as a chain of Apply LoRA nodes makes it: nothing downstream sees a difference.
    void theModelIsTheOneAChainMakes() {
        const QJsonObject base = handle("fugue/model", "checkpoint.safetensors");
        const QJsonObject a = handle("fugue/lora", "a.safetensors"), b = handle("fugue/lora", "b.safetensors");
        const Result<Values> first = run(kApplyLora, {{"model", Value(base)}, {"lora", Value(a)}, {"strength", Value(0.8)}});
        const Result<Values> chain = run(kApplyLora, {{"model", first->value("model")}, {"lora", Value(b)}});
        const Result<Values> rows = run(kApplyLoras, {{"model", Value(base)},
                                                      {"lora_1", Value(a)},
                                                      {"strength_1", Value(0.8)},
                                                      {"lora_3", Value(b)}}); // a gap keeps the order
        QVERIFY(chain && rows);
        QCOMPARE(rows->value("model").json(), chain->value("model").json());

        // A row that is off, at strength 0, or with no LoRA changes nothing, so it is not in the model: every key downstream,
        // which is made of the model's content (10.1), is the one without it.
        const Result<Values> off = run(kApplyLoras, {{"model", Value(base)},
                                                     {"lora_1", Value(a)},
                                                     {"strength_1", Value(0.8)},
                                                     {"lora_2", Value(handle("fugue/lora", "c.safetensors"))},
                                                     {"enabled_2", Value(false)},
                                                     {"lora_3", Value(b)},
                                                     {"lora_4", Value(handle("fugue/lora", "d.safetensors"))},
                                                     {"strength_4", Value(0.0)},
                                                     {"strength_5", Value(0.5)}});
        QVERIFY(off);
        QCOMPARE(off->value("model").json(), chain->value("model").json());
        QCOMPARE(off->value("model").identity(), chain->value("model").identity());
        // No rows: the model as it came.
        QCOMPARE(run(kApplyLoras, {{"model", Value(base)}})->value("model").json().toObject().value("derived").toObject(),
                 (QJsonObject{{"base", base}, {"apply", QJsonArray{}}}));
    }

    // A LoRA that does not fit the diffusion model is blamed on its own row, before the helper is asked; one that is off is not.
    void aLoraThatDoesNotFitIsBlamedOnItsRow() {
        const QJsonObject diffusion = model("fugue/diffusion-model", "diffusion_models/klein9b.safetensors",
                                            safetensors({{"txt_in.weight", QJsonArray{4096, 12288}},
                                                         {"img_in.weight", QJsonArray{4096, 128}},
                                                         {"double_blocks.0.img_attn.proj.weight", QJsonArray{4096, 4096}}}));
        const auto lora = [&](const QString &file, int width) {
            return model("fugue/lora", "loras/" + file,
                         safetensors({{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight", QJsonArray{16, width}},
                                      {"diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight", QJsonArray{width, 16}}}));
        };
        const QJsonObject loaded{{"resource", "fugue/model"}, {"derived", QJsonObject{{"base", diffusion}}}};
        const Values rows{{"model", Value(loaded)},
                          {"lora_1", Value(lora("klein9b-turbo.safetensors", 4096))},
                          {"lora_2", Value(lora("klein4b-rhc.safetensors", 3072))}};
        const Result<Values> wrong = run(kApplyLoras, rows);
        QVERIFY(!wrong);
        QCOMPARE(wrong.error().code, QStringLiteral("model_mismatch"));
        QCOMPARE(wrong.error().input, QStringLiteral("lora_2"));
        QCOMPARE(wrong.error().message, QStringLiteral("The LoRA klein4b-rhc.safetensors does not fit klein9b.safetensors: "
                                                       "it changes a layer 3072 to 3072 wide that is 4096 to 4096 in the model."));
        // The canvas check says the same when the model is known.
        const Errors checked = m_kernels.node(kApplyLoras)->check(context(kApplyLoras, rows));
        QCOMPARE(checked.size(), 1);
        QCOMPARE(checked.first().input, QStringLiteral("lora_2"));

        Values off = rows;
        off.insert("enabled_2", Value(false));
        QVERIFY(run(kApplyLoras, off));
    }
};

QTEST_GUILESS_MAIN(TestApplyLoras)
#include "test_apply_loras.moc"
