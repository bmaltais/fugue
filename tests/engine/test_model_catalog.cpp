// The model catalog (spec 8.9, #134): what a catalog file may hold, how files merge, and which
// entries a person still lacks. Seam: ModelCatalog over JSON text and real folders. No network.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_hashes.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

const QString kType = QStringLiteral("fugue/upscale-model");
const QString kHash = QString(64, 'a');

QJsonObject entry(const QString &id, const QString &file = QStringLiteral("a.safetensors")) {
    return QJsonObject{{"id", id}, {"resource", kType}, {"title", "Model " + id}, {"file", file},
                       {"url", "https://example.org/" + file}, {"size", 1000}, {"sha256", kHash},
                       {"scale", 4}, {"architecture", "ESRGAN"}, {"license", "MIT"}};
}

QByteArray catalogOf(const QJsonArray &models) {
    return QJsonDocument(QJsonObject{{"catalog", 1}, {"models", models}}).toJson(QJsonDocument::Compact);
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

class TestModelCatalog : public QObject {
    Q_OBJECT

private slots:
    void aValidEntryIsKeptWithItsHandleAndSummary() {
        const ModelCatalog catalog = ModelCatalog::parse(catalogOf({entry("one")}), "t");
        QVERIFY(catalog.problems().isEmpty());
        QCOMPARE(catalog.all().size(), 1);
        const CatalogModel &model = catalog.all().first();
        QCOMPARE(model.handle(), (QJsonObject{{"resource", kType}, {"hash", "sha256:" + kHash}, {"name", "a.safetensors"}}));
        QCOMPARE(model.summary(), QString("4x · ESRGAN · MIT · 1 MB"));
        QVERIFY(model.keywords().contains("esrgan", Qt::CaseInsensitive));
        QCOMPARE(licenseLine(model), QString("MIT"));
    }
    void licenseLineSaysWhenCommercialUseIsNotAllowedOrCreditIsAsked() {
        CatalogModel model;
        model.license = "CC-BY-NC-SA-4.0";
        model.commercial = false;
        QCOMPARE(licenseLine(model), QString("CC-BY-NC-SA-4.0, not for commercial use"));
        model.commercial = true;
        model.attribution = true;
        model.license = "CC-BY-4.0";
        QCOMPARE(licenseLine(model), QString("CC-BY-4.0, credit the author"));
        model.license.clear();
        model.attribution = false;
        QCOMPARE(licenseLine(model), QString("license not stated"));
    }

    void anEntryThatCannotBeVerifiedOrFetchedSafelyIsLeftOutAndSaid_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("why");
        QTest::newRow("http url") << "url" << QJsonValue("http://example.org/a") << "https";
        QTest::newRow("file url") << "url" << QJsonValue("file:///etc/passwd") << "https";
        QTest::newRow("short hash") << "sha256" << QJsonValue("abc") << "64 hex";
        QTest::newRow("no hash") << "sha256" << QJsonValue("") << "64 hex";
        QTest::newRow("no size") << "size" << QJsonValue(0) << "size";
        QTest::newRow("parent folder") << "file" << QJsonValue("../a.safetensors") << "plain relative";
        QTest::newRow("absolute") << "file" << QJsonValue("/etc/a.safetensors") << "plain relative";
        QTest::newRow("hidden") << "file" << QJsonValue(".a.safetensors") << "plain relative";
        QTest::newRow("no title") << "title" << QJsonValue("") << "needs an id";
    }
    void anEntryThatCannotBeVerifiedOrFetchedSafelyIsLeftOutAndSaid() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, why);
        QJsonObject bad = entry("bad");
        bad.insert(field, value);
        const ModelCatalog catalog = ModelCatalog::parse(catalogOf({bad, entry("good", "b.safetensors")}), "t");
        QCOMPARE(catalog.all().size(), 1);
        QCOMPARE(catalog.all().first().id, QString("good"));
        QCOMPARE(catalog.problems().size(), 1);
        QVERIFY2(catalog.problems().first().contains(why), qPrintable(catalog.problems().first()));
    }
    void aRepeatedIdAndANonCatalogAreProblems() {
        const ModelCatalog twice = ModelCatalog::parse(catalogOf({entry("x"), entry("x", "b.safetensors")}), "t");
        QCOMPARE(twice.all().size(), 1);
        QVERIFY(twice.problems().first().contains("twice"));
        const ModelCatalog nothing = ModelCatalog::parse("{\"models\": []}", "t.json");
        QVERIFY(nothing.all().isEmpty());
        QVERIFY(nothing.problems().first().contains("t.json"));
    }

    void aLaterFileReplacesAnEntryOfTheSameIdAndAddsTheRest() {
        ModelCatalog catalog = ModelCatalog::parse(catalogOf({entry("one"), entry("two", "b.safetensors")}), "base");
        QJsonObject newer = entry("one", "c.safetensors");
        newer.insert("title", "Newer");
        catalog.merge(ModelCatalog::parse(catalogOf({newer, entry("three", "d.safetensors")}), "user"));
        QCOMPARE(catalog.all().size(), 3);
        QCOMPARE(catalog.find("one")->title, QString("Newer"));
        QCOMPARE(catalog.all().first().id, QString("one")); // in its place
        QCOMPARE(catalog.forType(kType).size(), 3);
        QVERIFY(catalog.forType("fugue/lora").isEmpty());
    }

    void theCatalogsFugueShipsAreValidAndHaveVerifiedFields() {
        QTemporaryDir empty;
        const ModelCatalog catalog = ModelCatalog::load(empty.path());
        QVERIFY2(catalog.problems().isEmpty(), qPrintable(catalog.problems().join("\n")));
        const QList<CatalogModel> upscalers = catalog.forType(kType);
        QVERIFY(upscalers.size() >= 5);
        for (const CatalogModel &model : upscalers) {
            QVERIFY2(model.url.startsWith("https://"), qPrintable(model.id));
            QCOMPARE(model.sha256.size(), 64);
            QVERIFY2(!model.license.isEmpty() && !model.licenseUrl.isEmpty() && !model.source.isEmpty(), qPrintable(model.id));
            QVERIFY2(model.scale > 0 && !model.architecture.isEmpty(), qPrintable(model.id));
        }
        QVERIFY(!catalog.find("4x-ultrasharp")->commercial); // CC BY-NC-SA: said
        QVERIFY(catalog.find("realesrgan-x4plus")->commercial);
    }
    void aFileInTheUserFolderIsReadAfterFuguesOwn() {
        QTemporaryDir user;
        writeFile(user.filePath("mine.json"), catalogOf({entry("mine", "m.safetensors")}));
        writeFile(user.filePath("notes.txt"), "not a catalog");
        const ModelCatalog catalog = ModelCatalog::load(user.path());
        QVERIFY(catalog.find("mine"));
        QVERIFY(catalog.find("realesrgan-x4plus"));
        QVERIFY(catalog.problems().isEmpty());
    }

    void missingListsWhatIsNotOnDiskByNameOrKnownHash() {
        QTemporaryDir dir;
        Registry registry;
        DeclaredPortType declared;
        declared.type = *PortType::parse(kType);
        declared.kind = PortKind::Resource;
        declared.name = "Upscale model";
        declared.folders = {"upscale_models"};
        QVERIFY(registry.add(declared).isEmpty());
        writeFile(dir.filePath("models/upscale_models/a.safetensors"), "x"); // by name
        writeFile(dir.filePath("models/upscale_models/renamed.safetensors"), "yy"); // by remembered hash
        const ModelLibrary library({dir.filePath("models")}, &registry);
        FileHashes hashes(dir.filePath("hashes.json"));
        const QString renamedHash = hashes.hash(dir.filePath("models/upscale_models/renamed.safetensors"));
        QJsonObject byHash = entry("byhash", "other.safetensors");
        byHash.insert("sha256", renamedHash.mid(7));
        const ModelCatalog catalog = ModelCatalog::parse(
            catalogOf({entry("byname"), byHash, entry("absent", "c.safetensors")}), "t");
        const QList<CatalogModel> missing = catalog.missing(kType, library, *PortType::parse(kType), hashes);
        QCOMPARE(missing.size(), 1);
        QCOMPARE(missing.first().id, QString("absent"));
    }
};

QTEST_GUILESS_MAIN(TestModelCatalog)
#include "test_model_catalog.moc"
