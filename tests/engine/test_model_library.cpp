// Models on disk (spec 8.9, 2.5): the model folders, the files of a resource type in them, and
// finding the file a handle names by its content hash.
//
// Seam: ModelLibrary (list, folders, resolve) over real folders, with a Registry that declares the
// resource types the way pack.json does.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <tuple>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/registry.h"
#include "engine/settings.h"

using namespace fugue;

namespace {

// sha256 of the three bytes "abc" (FIPS 180-2, appendix B.1).
const QString abcHash = QStringLiteral("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

void writeFile(const QString &path, const QByteArray &bytes = "abc") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

PortType type(const QString &id) { return *PortType::parse(id); }

// A registry with the resource types the tests use.
void declare(Registry &registry, const QString &id, const QStringList &extensions = {},
             const QStringList &folders = {}) {
    DeclaredPortType declared;
    declared.type = type(id);
    declared.kind = PortKind::Resource;
    declared.name = QStringLiteral("A model");
    declared.extensions = extensions;
    declared.folders = folders;
    QVERIFY(registry.add(declared).isEmpty());
}

QStringList names(const QList<ModelFile> &files) {
    QStringList out;
    for (const ModelFile &file : files)
        out << file.name;
    return out;
}

QJsonObject handle(const QString &resource, const QString &hash, const QString &name) {
    QJsonObject object{{"resource", resource}, {"name", name}};
    if (!hash.isEmpty())
        object.insert("hash", hash);
    return object;
}

// A safetensors header with these tensor names (shapes do not matter here), for a file's kind.
QByteArray safetensors(const QStringList &tensors) {
    QJsonObject header;
    for (const QString &name : tensors)
        header.insert(name, QJsonObject{{"dtype", "F16"}, {"shape", QJsonArray{4, 4}}, {"data_offsets", QJsonArray{0, 0}}});
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
    return out + json;
}
const QStringList kLora{"diffusion_model.double_blocks.0.img_attn.proj.lora_A.weight",
                        "diffusion_model.double_blocks.0.img_attn.proj.lora_B.weight"};
const QStringList kVae{"decoder.conv_in.weight"};

// The generation types as the built-in pack declares them (only their folders matter here).
void declareGenerationTypes(Registry &registry) {
    for (const auto &[id, folder, files] : {std::tuple{"fugue/lora", "loras", "lora"}, std::tuple{"fugue/vae", "vae", "vae"}}) {
        DeclaredPortType declared;
        declared.type = type(QString::fromLatin1(id));
        declared.kind = PortKind::Resource;
        declared.name = QStringLiteral("A model");
        declared.folders = {QString::fromLatin1(folder)};
        declared.files = QString::fromLatin1(files); // what a folder of one kind is told by (pack.json "files")
        QVERIFY(registry.add(declared).isEmpty());
    }
}

} // namespace

class TestModelLibrary : public QObject {
    Q_OBJECT

private slots:

    // A workflow's models that are not here (4.3, 11.6): each once, with the folder it goes in and
    // where it can be had, as `requires` records it; a check reads no model file.
    void aWorkflowsMissingModelsSayWhereToGetAndPutThem() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/here.safetensors"), "other bytes"); // taken to be it, unread
        FileHashes hashes(state.filePath("hashes.json"));
        const QString gone = QStringLiteral("sha256:") + QString(64, u'1');
        const QString unsourced = QStringLiteral("sha256:") + QString(64, u'2');
        const auto lora = [](const QString &hash, const QString &name) {
            return QJsonObject{{"resource", "io.test.pack/lora"}, {"hash", hash}, {"name", name}};
        };
        const auto node = [](const QJsonObject &inputs) {
            return QJsonObject{{"type", "fugue/text"}, {"version", "1.0.0"}, {"inputs", inputs}};
        };
        QJsonObject sourced = lora(gone, "gone.safetensors");
        sourced.insert("source", "https://example.com/gone");
        const QJsonObject json{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{
                {"a", node({{"x", lora(abcHash, "here.safetensors")}, {"y", lora(gone, "gone.safetensors")}})},
                {"b", node({{"x", QJsonArray{lora(gone, "gone.safetensors"), lora(unsourced, "other.safetensors")}}})}}}}},
            {"requires", QJsonObject{{"models", QJsonArray{sourced}}}}};
        const Result<Document> document = Document::fromJson(json);
        QVERIFY2(document, qPrintable(document ? QString() : document.errors().first().message));

        const ModelLibrary library({root.path()}, &registry);
        const QList<MissingModel> missing = library.missing(*document, hashes);
        QCOMPARE(missing.size(), 2);
        QCOMPARE(missing.at(0).handle.value("name").toString(), QStringLiteral("gone.safetensors"));
        QCOMPARE(missing.at(0).source, QStringLiteral("https://example.com/gone"));
        QCOMPARE(missing.at(0).folder, root.filePath("lora"));
        QCOMPARE(missing.at(1).handle.value("name").toString(), QStringLiteral("other.safetensors"));
        QVERIFY(missing.at(1).source.isEmpty());
        QVERIFY(hashes.known(root.filePath("lora/here.safetensors")).isEmpty()); // not read
    }
    void listsTheModelsOfATypeInItsFolderAndSubfolders() {
        QTemporaryDir root;
        Registry registry;
        declare(registry, "io.test.pack/upscaler", {".pth"});
        writeFile(root.filePath("upscaler/x.pth"));
        writeFile(root.filePath("upscaler/sub/y.PTH"));
        writeFile(root.filePath("upscaler/notes.txt"));
        writeFile(root.filePath("upscaler/.cache/hidden.pth"));
        writeFile(root.filePath("upscaler/.hidden.pth"));
        writeFile(root.filePath("other/z.pth"));

        const ModelLibrary library({root.path()}, &registry);
        const QList<ModelFile> files = library.list(type("io.test.pack/upscaler"));
        QCOMPARE(names(files), (QStringList{"sub/y.PTH", "x.pth"}));
        QCOMPARE(files.at(1).path, root.filePath("upscaler/x.pth"));
        QCOMPARE(files.at(1).size, 3);
    }

    void declaredFoldersReplaceTheDefaultSoAComfyUiFolderWorksAsARoot() {
        QTemporaryDir fugue, comfy;
        Registry registry;
        declare(registry, "io.test.pack/text-encoder", {".gguf", ".safetensors"},
                {"text-encoder", "text_encoders", "clip"});
        writeFile(fugue.filePath("text-encoder/mine.gguf"));
        writeFile(comfy.filePath("clip/Qwen3-4B-Q8_0.gguf"));
        writeFile(comfy.filePath("text_encoders/t5.safetensors"));

        const ModelLibrary library({fugue.path(), comfy.path()}, &registry);
        QCOMPARE(library.folders(type("io.test.pack/text-encoder")),
                 (QStringList{fugue.filePath("text-encoder"), fugue.filePath("text_encoders"), fugue.filePath("clip"),
                              comfy.filePath("text-encoder"), comfy.filePath("text_encoders"), comfy.filePath("clip")}));
        const QList<ModelFile> files = library.list(type("io.test.pack/text-encoder"));
        QCOMPARE(names(files), (QStringList{"mine.gguf", "t5.safetensors", "Qwen3-4B-Q8_0.gguf"}));
        // Each file knows the folder it was found in, which the model picker groups by.
        QCOMPARE(files.at(0).folder, fugue.filePath("text-encoder"));
        QCOMPARE(files.at(1).folder, comfy.filePath("text_encoders"));
        QCOMPARE(files.at(2).folder, comfy.filePath("clip"));
    }

    void withNoExtensionsDeclaredComfyUisModelFilesAndGgufAreModels() {
        QTemporaryDir root;
        Registry registry;
        declare(registry, "io.test.pack/thing");
        for (const char *name : {"a.safetensors", "b.gguf", "c.ckpt", "d.pt", "e.pth", "f.bin", "g.sft", "h.txt", "i.json"})
            writeFile(root.filePath(QStringLiteral("thing/") + name));
        const ModelLibrary library({root.path()}, &registry);
        QCOMPARE(names(library.list(type("io.test.pack/thing"))),
                 (QStringList{"a.safetensors", "b.gguf", "c.ckpt", "d.pt", "e.pth", "f.bin", "g.sft"}));
    }

    void anExtensionMatchesTheWholeSuffixWithOrWithoutItsDot() {
        QTemporaryDir root;
        Registry registry;
        declare(registry, "io.test.pack/net", {"pt"});
        writeFile(root.filePath("net/a.pt"));
        writeFile(root.filePath("net/b.ckpt"));
        const ModelLibrary library({root.path()}, &registry);
        QCOMPARE(names(library.list(type("io.test.pack/net"))), QStringList{"a.pt"});
    }

    void aFileReachedTwiceIsListedOnce() {
        QTemporaryDir root;
        Registry registry;
        declare(registry, "io.test.pack/vae", {".safetensors"});
        writeFile(root.filePath("vae/ae.safetensors"));
        QVERIFY(QFile::link(root.filePath("vae/ae.safetensors"), root.filePath("vae/link.safetensors")));
        QVERIFY(QFile::link(root.filePath("vae"), root.filePath("vae/loop"))); // a folder linking to itself
        // The same root twice, as two settings entries that name one folder.
        const ModelLibrary library({root.path(), root.path() + "/."}, &registry);
        QCOMPARE(names(library.list(type("io.test.pack/vae"))), (QStringList{"ae.safetensors"}));
    }

    void aTypeThatIsNotAResourceTypeHasNoFiles() {
        QTemporaryDir root;
        Registry registry;
        writeFile(root.filePath("unknown/x.safetensors"));
        const ModelLibrary library({root.path()}, &registry);
        QVERIFY(library.list(type("io.test.pack/unknown")).isEmpty());
        QVERIFY(library.folders(type("io.test.pack/unknown")).isEmpty());
    }

    void rootsAreFuguesFolderThenTheSettingsModelPaths() {
        Settings settings;
        settings.modelPaths = {"/mnt/models", "~/comfy/models", "relative/models", "/mnt/models/"};
        const QStringList roots = ModelLibrary::roots(settings);
        QCOMPARE(roots, (QStringList{QDir::cleanPath(qEnvironmentVariable("XDG_DATA_HOME") + "/fugue/models"),
                                     "/mnt/models", QDir::homePath() + "/comfy/models"}));
    }

    void resolveFindsTheFileByARememberedHash() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/renamed.safetensors"));
        FileHashes hashes(state.filePath("hashes.json"));
        QCOMPARE(hashes.hash(root.filePath("lora/renamed.safetensors")), abcHash);

        const ModelLibrary library({root.path()}, &registry);
        const Result<QString> found = library.resolve(handle("io.test.pack/lora", abcHash, "detail.safetensors"), hashes);
        QVERIFY2(found, qPrintable(found.error().message));
        QCOMPARE(found.value(), root.filePath("lora/renamed.safetensors"));
    }

    void resolveHashesTheFilesWithTheHandlesName() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/a/detail.safetensors"), "not it");
        writeFile(root.filePath("lora/b/detail.safetensors"));
        FileHashes hashes(state.filePath("hashes.json"));

        const ModelLibrary library({root.path()}, &registry);
        const Result<QString> found = library.resolve(handle("io.test.pack/lora", abcHash, "detail.safetensors"), hashes);
        QVERIFY2(found, qPrintable(found.error().message));
        QCOMPARE(found.value(), root.filePath("lora/b/detail.safetensors"));
    }

    void resolveDoesNotHashOtherFiles() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/other-name.safetensors")); // the right bytes, never hashed
        FileHashes hashes(state.filePath("hashes.json"));

        const ModelLibrary library({root.path()}, &registry);
        const Result<QString> found = library.resolve(handle("io.test.pack/lora", abcHash, "detail.safetensors"), hashes);
        QVERIFY(!found);
        QCOMPARE(found.error().code, QStringLiteral("model_missing"));
        QVERIFY2(found.error().message.contains("detail.safetensors"), qPrintable(found.error().message));
        QVERIFY2(found.error().hint.contains(root.filePath("lora")), qPrintable(found.error().hint));
    }

    void aCheckReadsNoFileAndTakesTheOneWithTheHandlesName() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/detail.safetensors"), "other bytes");
        FileHashes hashes(state.filePath("hashes.json"));
        const ModelLibrary library({root.path()}, &registry);
        const QJsonObject wanted = handle("io.test.pack/lora", abcHash, "detail.safetensors");

        QVERIFY(library.resolve(wanted, hashes, ModelLibrary::Hashing::RememberedOnly));
        QVERIFY(hashes.known(root.filePath("lora/detail.safetensors")).isEmpty()); // not read
        QVERIFY(!library.resolve(wanted, hashes)); // a run reads it: not the one
        // Once its hash is remembered, a check knows it is not the one either.
        QVERIFY(!library.resolve(wanted, hashes, ModelLibrary::Hashing::RememberedOnly));
        QVERIFY(!library.resolve(handle("io.test.pack/lora", abcHash, "absent.safetensors"), hashes,
                                 ModelLibrary::Hashing::RememberedOnly));
    }

    void aDerivedHandleWithABadBaseIsMissing() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        FileHashes hashes(state.filePath("hashes.json"));
        const ModelLibrary library({root.path()}, &registry);
        const QJsonObject derived{{"resource", "io.test.pack/lora"}, {"derived", QJsonObject{{"base", "not a handle"}}}};
        const Result<QJsonValue> made = library.withPaths(derived, hashes);
        QVERIFY(!made);
        QCOMPARE(made.error().code, QStringLiteral("model_missing"));
    }

    // Load Models' handle (src/nodes/components.cpp): the base and every other part get paths.
    void eachPartOfADerivedHandleGetsItsPath() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/model", {".gguf"});
        declare(registry, "io.test.pack/encoder", {".gguf"});
        writeFile(root.filePath("model/unet.gguf"));
        writeFile(root.filePath("encoder/qwen.gguf"));
        FileHashes hashes(state.filePath("hashes.json"));
        const ModelLibrary library({root.path()}, &registry);
        const QJsonObject derived{{"resource", "io.test.pack/model"},
                                  {"derived", QJsonObject{{"base", handle("io.test.pack/model", abcHash, "unet.gguf")},
                                                          {"text_encoder", handle("io.test.pack/encoder", abcHash, "qwen.gguf")}}}};
        const Result<QJsonValue> made = library.withPaths(derived, hashes);
        QVERIFY(made);
        const QJsonObject parts = made->toObject().value("derived").toObject();
        QCOMPARE(parts.value("base").toObject().value("path").toString(), root.filePath("model/unet.gguf"));
        QCOMPARE(parts.value("text_encoder").toObject().value("path").toString(), root.filePath("encoder/qwen.gguf"));

        QVERIFY(QFile::remove(root.filePath("encoder/qwen.gguf")));
        const ModelLibrary again({root.path()}, &registry);
        QCOMPARE(again.withPaths(derived, hashes).error().code, QStringLiteral("model_missing"));
    }

    // #86: a model path that is itself a LoRA folder (the owner's /mnt/windows/lora/flux.2).
    void aFolderOfOneKindIsFoundByItsFiles() {
        QTemporaryDir own, loras;
        Registry registry;
        declareGenerationTypes(registry);
        writeFile(loras.filePath("klein-restore.safetensors"), safetensors(kLora));
        writeFile(loras.filePath("styles/ink.safetensors"), safetensors(kLora));
        const ModelLibrary library({own.path(), loras.path()}, &registry);
        QCOMPARE(names(library.list(type("fugue/lora"))), (QStringList{"klein-restore.safetensors", "styles/ink.safetensors"}));
        QCOMPARE(library.list(type("fugue/lora")).value(0).folder, loras.path());
        QVERIFY(library.list(type("fugue/vae")).isEmpty());
        // Fugue's own folder keeps its layout, where a missing model's hint sends it.
        QCOMPARE(library.folders(type("fugue/vae")), QStringList{own.path() + "/vae"});
    }

    // #619: pack.json now lists the A1111, Forge, SwarmUI and Stability Matrix folder names. A folder an existing user has that
    // holds LoRAs (or checkpoints) and happens to have one such subfolder is still that one kind, not ComfyUI's layout.
    void aFolderOfOneKindWithAnotherToolsSubfolderStaysThatKind() {
        QTemporaryDir own, loras, checkpoints;
        Registry registry;
        const auto withFiles = [&](const char *id, const QString &files, const QStringList &folders) {
            DeclaredPortType declared;
            declared.type = type(QString::fromLatin1(id));
            declared.kind = PortKind::Resource;
            declared.name = QStringLiteral("A model");
            declared.extensions = {".safetensors"};
            declared.folders = folders;
            declared.files = files;
            QVERIFY(registry.add(declared).isEmpty());
        };
        withFiles("fugue/lora", "lora", {"lora", "loras", "Lora", "LyCORIS"});
        withFiles("fugue/vae", "vae", {"vae", "VAE"});
        withFiles("fugue/model", "checkpoint", {"checkpoints", "Stable-diffusion"});
        writeFile(loras.filePath("top.safetensors"), safetensors(kLora));
        QDir().mkpath(loras.filePath("LyCORIS"));
        QDir().mkpath(loras.filePath("Lora"));
        writeFile(checkpoints.filePath("top-model.safetensors"), safetensors({"model.diffusion_model.input_blocks.0.0.weight"}));
        QDir().mkpath(checkpoints.filePath("VAE"));
        const ModelLibrary library({own.path(), loras.path(), checkpoints.path()}, &registry);
        QCOMPARE(library.kindOf(loras.path()), QStringLiteral("fugue/lora")); // not "layout": its top-level LoRAs stay listed
        QCOMPARE(names(library.list(type("fugue/lora"))), QStringList{"top.safetensors"});
        QVERIFY(library.kindOf(checkpoints.path()) != ModelLibrary::kLayout);
    }

    // The same names are a layout when they are the layout: two of them, or one in a folder with no model files of its own, and
    // ComfyUI's own names as before.
    void theOtherToolsNamesAreALayoutWhenTheyAre() {
        QTemporaryDir own, a1111, onlyLora, comfy, comfyOne;
        Registry registry;
        declare(registry, "fugue/lora", {".safetensors"}, {"lora", "loras", "Lora", "LyCORIS"});
        declare(registry, "fugue/vae", {".safetensors"}, {"vae", "VAE"});
        QDir().mkpath(a1111.filePath("Lora"));
        QDir().mkpath(a1111.filePath("VAE"));
        writeFile(a1111.filePath("stray.safetensors"));
        QDir().mkpath(onlyLora.filePath("Lora"));
        QDir().mkpath(comfy.filePath("loras"));
        QDir().mkpath(comfy.filePath("vae"));
        QDir().mkpath(comfyOne.filePath("loras")); // one ComfyUI name is a layout, as it always was
        writeFile(comfyOne.filePath("stray.safetensors"));
        const ModelLibrary library({own.path(), a1111.path(), onlyLora.path(), comfy.path(), comfyOne.path()}, &registry);
        QCOMPARE(library.kindOf(a1111.path()), ModelLibrary::kLayout); // two kinds: Lora and VAE
        QVERIFY(library.kindOf(onlyLora.path()) != ModelLibrary::kLayout); // one other-tools name is not enough (found by files, here none)
        QCOMPARE(library.kindOf(comfy.path()), ModelLibrary::kLayout);
        QCOMPARE(library.kindOf(comfyOne.path()), ModelLibrary::kLayout);
    }

    // The folders of one kind that are not a layout today stay what they were, whatever subfolders they hold: no files of their own
    // (`/data/loras` of `LyCORIS/` and `SDXL/`; a checkpoints root of `VAE/`, `SD15/` and `SDXL/`) is found by its files, as before.
    void foldersOfOneKindWithOnlySubfoldersStayThatKind() {
        QTemporaryDir own, loras, checkpoints, a1111;
        Registry registry;
        const auto withFiles = [&](const char *id, const QString &files, const QStringList &folders) {
            DeclaredPortType declared;
            declared.type = type(QString::fromLatin1(id));
            declared.kind = PortKind::Resource;
            declared.name = QStringLiteral("A model");
            declared.extensions = {".safetensors"};
            declared.folders = folders;
            declared.files = files;
            QVERIFY(registry.add(declared).isEmpty());
        };
        withFiles("fugue/lora", "lora", {"lora", "loras", "Lora", "LyCORIS"});
        withFiles("fugue/vae", "vae", {"vae", "VAE"});
        withFiles("fugue/model", "checkpoint", {"checkpoints", "Stable-diffusion"});
        writeFile(loras.filePath("LyCORIS/a.safetensors"), safetensors(kLora));
        writeFile(loras.filePath("SDXL/b.safetensors"), safetensors(kLora));
        writeFile(checkpoints.filePath("VAE/v.safetensors"), safetensors(kVae));
        writeFile(checkpoints.filePath("SD15/m.safetensors"), safetensors({"model.diffusion_model.input_blocks.0.0.weight"}));
        writeFile(checkpoints.filePath("SDXL/m.safetensors"), safetensors({"model.diffusion_model.input_blocks.0.0.weight"}));
        QDir().mkpath(a1111.filePath("Lora"));
        QDir().mkpath(a1111.filePath("VAE"));
        QDir().mkpath(a1111.filePath("Stable-diffusion"));
        const ModelLibrary library({own.path(), loras.path(), checkpoints.path(), a1111.path()}, &registry);
        QCOMPARE(library.kindOf(loras.path()), QStringLiteral("fugue/lora")); // not "layout": the SDXL LoRAs stay listed
        QCOMPARE(library.list(type("fugue/lora")).size(), 2);
        QVERIFY(library.kindOf(checkpoints.path()) != ModelLibrary::kLayout);
        QCOMPARE(library.kindOf(a1111.path()), ModelLibrary::kLayout); // an A1111 root
    }

    void aKindSetInTheSettingsWinsOverTheFiles() {
        QTemporaryDir own, folder;
        Registry registry;
        declareGenerationTypes(registry);
        writeFile(folder.filePath("odd.safetensors"), safetensors(kLora));
        const ModelLibrary library({own.path(), folder.path()}, &registry, {{folder.path(), "fugue/vae"}});
        QCOMPARE(names(library.list(type("fugue/vae"))), QStringList{"odd.safetensors"});
        QVERIFY(library.list(type("fugue/lora")).isEmpty());
        // "layout": ComfyUI's folders inside, whatever the files say.
        writeFile(folder.filePath("loras/x.safetensors"), safetensors(kLora));
        const ModelLibrary laidOut({own.path(), folder.path()}, &registry, {{folder.path(), ModelLibrary::kLayout}});
        QCOMPARE(names(laidOut.list(type("fugue/lora"))), QStringList{"x.safetensors"});
    }

    // No kind most of its files share, and nothing is listed from it rather than a guess.
    void aMixedFolderHoldsNothingFugueCanTell() {
        QTemporaryDir own, folder;
        Registry registry;
        declareGenerationTypes(registry);
        writeFile(folder.filePath("a.safetensors"), safetensors(kLora));
        writeFile(folder.filePath("b.safetensors"), safetensors(kVae));
        writeFile(folder.filePath("c.safetensors"), "not a model");
        const ModelLibrary library({own.path(), folder.path()}, &registry);
        QVERIFY(library.list(type("fugue/lora")).isEmpty());
        QVERIFY(library.list(type("fugue/vae")).isEmpty());
    }

    void theSettingsSayWhatAModelPathHolds() {
        Settings settings;
        settings.modelPaths = {"~/loras/flux.2", "/mnt/models"};
        settings.modelFolderKinds = {{"~/loras/flux.2", "fugue/lora"}, {"/mnt/models/", "layout"}};
        const QHash<QString, QString> kinds = ModelLibrary::rootKinds(settings);
        QCOMPARE(kinds.value(QDir::homePath() + "/loras/flux.2"), QStringLiteral("fugue/lora"));
        QCOMPARE(kinds.value("/mnt/models"), QStringLiteral("layout"));
        QVERIFY(checkSetting("model_folder_kinds", QJsonObject{{"/x", "fugue/lora"}, {"/y", "layout"}}).isEmpty());
        QVERIFY(!checkSetting("model_folder_kinds", QJsonObject{{"/x", "loras please"}}).isEmpty());
    }

    void aChangedFileNoLongerMatchesItsRememberedHash() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        const QString path = root.filePath("lora/x.safetensors");
        writeFile(path);
        FileHashes hashes(state.filePath("hashes.json"));
        QCOMPARE(hashes.hash(path), abcHash);
        writeFile(path, "abcd"); // another size: the remembered hash is stale

        const ModelLibrary library({root.path()}, &registry);
        QVERIFY(!library.resolve(handle("io.test.pack/lora", abcHash, "elsewhere.safetensors"), hashes));
    }

    void aHandleWithNoHashOrOfAnotherKindIsMissing() {
        QTemporaryDir root, state;
        Registry registry;
        declare(registry, "io.test.pack/lora", {".safetensors"});
        writeFile(root.filePath("lora/detail.safetensors"));
        FileHashes hashes(state.filePath("hashes.json"));
        const ModelLibrary library({root.path()}, &registry);

        const Result<QString> noHash = library.resolve(handle("io.test.pack/lora", {}, "detail.safetensors"), hashes);
        QVERIFY(!noHash);
        QCOMPARE(noHash.error().code, QStringLiteral("model_missing"));
        const Result<QString> unknownType = library.resolve(handle("io.test.pack/nope", abcHash, "detail.safetensors"), hashes);
        QVERIFY(!unknownType);
        QCOMPARE(unknownType.error().code, QStringLiteral("model_missing"));
    }
};

QTEST_GUILESS_MAIN(TestModelLibrary)
#include "test_model_library.moc"
