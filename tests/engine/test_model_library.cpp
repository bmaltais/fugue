// Models on disk (spec 8.9, 2.5): the model folders, the files of a resource type in them, and
// finding the file a handle names by its content hash.
//
// Seam: ModelLibrary (list, folders, resolve) over real folders, with a Registry that declares the
// resource types the way pack.json does.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

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

} // namespace

class TestModelLibrary : public QObject {
    Q_OBJECT

private slots:
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
