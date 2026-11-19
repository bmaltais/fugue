// Writing providers.json (spec 8.2): what loadProviderInstances reads back, mode 0600.

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/json.h"
#include "engine/providers.h"
#include "engine/providers_file.h"

using namespace fugue;

namespace {

ProviderInstance instance(const QString &name, const QString &address = {}, const QString &key = {}) {
    ProviderInstance made;
    made.name = name;
    made.type = QStringLiteral("omarchy");
    made.address = address;
    made.key = key;
    return made;
}

} // namespace

class TestProvidersFile : public QObject {
    Q_OBJECT

private slots:
    void savedInstancesLoadBackInOrder() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config/fugue/providers.json");
        ProviderInstance studio = instance("Studio box", "https://studio.example.ts.net", "s3cret");
        studio.enabled = false;
        studio.settings.insert("steps_limit", 40);
        QVERIFY(saveProviderInstances(path, {studio, instance("This machine")}).isEmpty());
        const auto loaded = loadProviderInstances(path);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 2);
        QCOMPARE(loaded.value().at(0).name, QStringLiteral("Studio box"));
        QCOMPARE(loaded.value().at(0).address, QStringLiteral("https://studio.example.ts.net"));
        QCOMPARE(loaded.value().at(0).key, QStringLiteral("s3cret"));
        QCOMPARE(loaded.value().at(0).enabled, false);
        QCOMPARE(loaded.value().at(0).settings.value("steps_limit").toInt(), 40);
        QCOMPARE(loaded.value().at(1).name, QStringLiteral("This machine"));
        QCOMPARE(loaded.value().at(1).enabled, true);
    }

    void theFileIsReadableByItsOwnerOnly() {
        QTemporaryDir dir;
        const QString path = dir.filePath("providers.json");
        QVERIFY(saveProviderInstances(path, {instance("This machine", {}, "k")}).isEmpty());
        QCOMPARE(QFile::permissions(path) & ~(QFile::ReadUser | QFile::WriteUser | QFile::ReadOwner | QFile::WriteOwner),
                 QFile::Permissions());
        // A rewrite keeps it so.
        QVERIFY(saveProviderInstances(path, {instance("This machine", {}, "k2")}).isEmpty());
        QCOMPARE(QFile::permissions(path) & ~(QFile::ReadUser | QFile::WriteUser | QFile::ReadOwner | QFile::WriteOwner),
                 QFile::Permissions());
    }

    void emptyFieldsAreLeftOut() {
        QTemporaryDir dir;
        const QString path = dir.filePath("providers.json");
        QVERIFY(saveProviderInstances(path, {instance("This machine")}).isEmpty());
        const auto document = readJsonObject(path);
        const QJsonObject entry = document.value().value("providers").toArray().first().toObject();
        QCOMPARE(entry, (QJsonObject{{"name", "This machine"}, {"type", "omarchy"}}));
    }

    void otherTopLevelKeysStay() {
        QTemporaryDir dir;
        const QString path = dir.filePath("providers.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"note": "mine", "providers": []})");
        file.close();
        QVERIFY(saveProviderInstances(path, {instance("This machine")}).isEmpty());
        QCOMPARE(readJsonObject(path).value().value("note").toString(), QStringLiteral("mine"));
    }

    void aListThatWouldNotLoadIsNotWritten() {
        QTemporaryDir dir;
        const QString path = dir.filePath("providers.json");
        QVERIFY(!saveProviderInstances(path, {instance("A"), instance("A")}).isEmpty());
        QVERIFY(!saveProviderInstances(path, {instance("")}).isEmpty());
        QVERIFY(!QFile::exists(path));
    }

    void aCorruptFileIsNeverOverwritten() {
        QTemporaryDir dir;
        const QString path = dir.filePath("providers.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{nope");
        file.close();
        QVERIFY(!saveProviderInstances(path, {instance("This machine")}).isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{nope"));
    }
};

QTEST_GUILESS_MAIN(TestProvidersFile)
#include "test_providers_file.moc"
