// Which nodes show a picture (#263, spec 12.7): the role of a type (its manifest key, else its
// category), the mode, the node's override, and the manifest key's own checks.
//
// Seam: engine/node_picture.h on NodeType values, and NodeType::fromJson for the manifest key.

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "engine/node_picture.h"
#include "engine/node_type.h"

using namespace fugue;

namespace {

QJsonObject manifest(const QString &category, const QJsonObject &extra = {}) {
    QJsonObject m{{"manifest", 1}, {"id", "fugue/thing"}, {"version", "1.0.0"}, {"name", "Thing"},
                  {"category", category}, {"runtime", QJsonObject{{"kind", "builtin"}}},
                  {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                  {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        m.insert(it.key(), it.value());
    return m;
}

NodeType typeOf(const QJsonObject &m) {
    const Result<NodeType> type = NodeType::fromJson(m);
    if (!type.ok())
        qWarning() << type.errors().first().message;
    return type.ok() ? type.value() : NodeType();
}

} // namespace

class TestNodePicture : public QObject {
    Q_OBJECT

private slots:
    void theRoleComesFromTheCategoryUnlessTheManifestSaysSo_data() {
        QTest::addColumn<QString>("category");
        QTest::addColumn<bool>("sink");
        QTest::addColumn<QString>("key");
        QTest::addColumn<int>("role");
        const int always = int(PictureRole::Always), result = int(PictureRole::Result), never = int(PictureRole::Never);
        QTest::newRow("a transform") << "Image/Transform" << false << "" << never;
        QTest::newRow("an adjust") << "Image/Adjust" << false << "" << never;
        QTest::newRow("a mask node") << "Image/Mask" << false << "" << never;
        QTest::newRow("a deeper level of a quiet category") << "Image/Transform/Fancy" << false << "" << never;
        QTest::newRow("a generating node") << "Generate" << false << "" << result;
        QTest::newRow("a loader") << "Basic" << false << "" << result;
        QTest::newRow("a pack's own category") << "My pack/Tools" << false << "" << result;
        QTest::newRow("no category") << "" << false << "" << result;
        QTest::newRow("a sink") << "Basic" << true << "" << always;
        QTest::newRow("a sink in a quiet category") << "Image/Transform" << true << "" << always;
        QTest::newRow("the key wins over the category") << "Image/Transform" << false << "result" << result;
        QTest::newRow("the key wins over a sink") << "Basic" << true << "never" << never;
        QTest::newRow("always") << "Image/Mask" << false << "always" << always;
    }
    void theRoleComesFromTheCategoryUnlessTheManifestSaysSo() {
        QFETCH(QString, category);
        QFETCH(bool, sink);
        QFETCH(QString, key);
        QFETCH(int, role);
        QJsonObject extra{{"sink", sink}};
        if (!key.isEmpty())
            extra.insert("picture", key);
        QCOMPARE(int(pictureRole(typeOf(manifest(category, extra)))), role);
    }

    void aBadManifestKeyIsRefusedAndAGoodOneRoundTrips() {
        const Result<NodeType> bad = NodeType::fromJson(manifest("Basic", {{"picture", "sometimes"}}));
        QVERIFY(!bad.ok());
        QVERIFY2(bad.errors().first().message.contains("picture"), qPrintable(bad.errors().first().message));
        const NodeType good = typeOf(manifest("Basic", {{"picture", "never"}}));
        QCOMPARE(good.picture, QString("never"));
        QCOMPARE(good.toJson().value("picture").toString(), QString("never"));
        QVERIFY(!typeOf(manifest("Basic")).toJson().contains("picture")); // absent stays absent
    }

    void aLoadNodeIsOneWithAFileAndNoMediaInput() {
        QJsonObject m = manifest("Basic");
        m.insert("inputs", QJsonArray{QJsonObject{{"name", "file"}, {"type", "text"}, {"widget", "file"}}});
        QVERIFY(isInputNode(typeOf(m)));
        // A node that takes a picture in is not one, whatever else it has.
        m.insert("inputs", QJsonArray{QJsonObject{{"name", "file"}, {"type", "text"}, {"widget", "file"}},
                                      QJsonObject{{"name", "image"}, {"type", "image"}}});
        QVERIFY(!isInputNode(typeOf(m)));
        QVERIFY(!isInputNode(typeOf(manifest("Basic"))));
    }

    void onlyATypeWithAPictureCanShowOrHideOne() {
        QVERIFY(hasPictureSlot(typeOf(manifest("Image/Transform")))); // an image out
        QJsonObject number = manifest("Basic");
        number.insert("inputs", QJsonArray());
        number.insert("outputs", QJsonArray{QJsonObject{{"name", "value"}, {"type", "number"}}});
        QVERIFY(!hasPictureSlot(typeOf(number)));
        number.insert("sink", true);
        QVERIFY(hasPictureSlot(typeOf(number))); // Preview of a number shows its text in the slot
    }

    void theModesChooseByRole_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("role");
        QTest::addColumn<bool>("input");
        QTest::addColumn<bool>("shown");
        const int smart = int(PictureMode::Smart), sinks = int(PictureMode::Sinks), all = int(PictureMode::All);
        const int always = int(PictureRole::Always), result = int(PictureRole::Result), never = int(PictureRole::Never);
        QTest::newRow("smart always") << smart << always << false << true;
        QTest::newRow("smart result") << smart << result << false << true;
        QTest::newRow("smart never") << smart << never << false << false;
        QTest::newRow("sinks always") << sinks << always << false << true;
        QTest::newRow("sinks a loader") << sinks << result << true << true;
        QTest::newRow("sinks a generator") << sinks << result << false << false;
        QTest::newRow("sinks never") << sinks << never << false << false;
        QTest::newRow("sinks a loader that says never") << sinks << never << true << false;
        QTest::newRow("all never") << all << never << false << true;
        QTest::newRow("all result") << all << result << false << true;
    }
    void theModesChooseByRole() {
        QFETCH(int, mode);
        QFETCH(int, role);
        QFETCH(bool, input);
        QFETCH(bool, shown);
        QCOMPARE(showsPicture(PictureMode(mode), PictureRole(role), input, PictureOverride::Default), shown);
    }

    void anOverrideWinsOverTheMode() {
        for (const PictureMode mode : {PictureMode::Smart, PictureMode::Sinks, PictureMode::All})
            for (const PictureRole role : {PictureRole::Always, PictureRole::Result, PictureRole::Never}) {
                QVERIFY(showsPicture(mode, role, false, PictureOverride::Show));
                QVERIFY(!showsPicture(mode, role, true, PictureOverride::Hide));
            }
    }

    void namesRoundTripAndUnknownOnesAreTheDefault() {
        for (const PictureMode mode : {PictureMode::Smart, PictureMode::Sinks, PictureMode::All})
            QCOMPARE(pictureModeFromName(pictureModeName(mode)), mode);
        QCOMPARE(pictureModeFromName("bogus"), PictureMode::Smart);
        QCOMPARE(pictureOverrideFromName("show"), PictureOverride::Show);
        QCOMPARE(pictureOverrideFromName("hide"), PictureOverride::Hide);
        QCOMPARE(pictureOverrideFromName("maybe"), PictureOverride::Default);
        QCOMPARE(pictureOverrideName(PictureOverride::Default), QString());
        PictureRole role = PictureRole::Result;
        QVERIFY(!pictureRoleFromName("", &role));
        QVERIFY(pictureRoleFromName("never", &role));
        QCOMPARE(role, PictureRole::Never);
    }
};

QTEST_GUILESS_MAIN(TestNodePicture)
#include "test_node_picture.moc"
