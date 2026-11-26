// Ids (spec 3.1, 4.2, 6.1), port types (2.1, 2.2) and compatibility (2.3).

#include <QtTest>

#include "engine/ids.h"
#include "engine/port_type.h"

using namespace fugue;

namespace {

PortSpec port(const char *type, bool list = false) {
    return PortSpec{*PortType::parse(QString::fromLatin1(type)), list};
}

} // namespace

class TestPortType : public QObject {
    Q_OBJECT

private slots:
    void nodeIds() {
        QVERIFY(isNodeId("prompt"));
        QVERIFY(isNodeId("k3f9x2"));
        QVERIFY(isNodeId("a_b-c"));
        QVERIFY(isNodeId(QString(32, 'a')));
        QVERIFY(!isNodeId(QString(33, 'a')));
        QVERIFY(!isNodeId(""));
        QVERIFY(!isNodeId("9lives"));
        QVERIFY(!isNodeId("Prompt"));
        QVERIFY(!isNodeId("g7.first"));
        QVERIFY(!isNodeId("g7/first"));
    }

    void pathIds() {
        QVERIFY(isPathId("save"));
        QVERIFY(isPathId("g7/first"));
        QVERIFY(isPathId("g7/inner3/s2"));
        QVERIFY(!isPathId(""));
        QVERIFY(!isPathId("g7/"));
        QVERIFY(!isPathId("/first"));
        QVERIFY(!isPathId("g7//first"));
        QVERIFY(!isPathId("../outside"));
        QVERIFY(!isPathId("g7.first"));
    }

    void typeIds() {
        QVERIFY(isTypeId("fugue/text"));
        QVERIFY(isTypeId("local/two-pass"));
        QVERIFY(isTypeId("io.github.alice.tools/sharpen"));
        QVERIFY(isTypeId("io.github.alice-b.tools/sharpen-2"));
        QVERIFY(!isTypeId("sharpen"));
        QVERIFY(!isTypeId("tools/sharpen")); // one word, not reserved
        QVERIFY(!isTypeId("io.github.Alice/sharpen"));
        QVERIFY(!isTypeId("io.github.alice/Sharpen"));
        QVERIFY(!isTypeId("io.github.alice/2sharpen"));
        QVERIFY(!isTypeId("io.github.alice/sharp_en"));
        QVERIFY(!isTypeId("io..alice/sharpen"));
        QVERIFY(!isTypeId("fugue/text/more"));
        QCOMPARE(typeNamespace("io.github.alice.tools/sharpen"), QStringLiteral("io.github.alice.tools"));
    }

    void portNames() {
        QVERIFY(isPortName("image"));
        QVERIFY(isPortName("min_items2"));
        QVERIFY(!isPortName("Image"));
        QVERIFY(!isPortName("max-items"));
        QVERIFY(!isPortName("_x"));
        QVERIFY(!isPortName(""));
    }

    void builtInTypesAreBareNames() {
        for (const char *name : {"text", "number", "boolean", "image", "video", "audio"}) {
            const std::optional<PortType> type = PortType::parse(QString::fromLatin1(name));
            QVERIFY2(type, name);
            QVERIFY(type->isBuiltin());
            QCOMPARE(type->toString(), QString::fromLatin1(name));
        }
        QVERIFY(!PortType::parse("mask"));
        QVERIFY(!PortType::parse("text@2"));
        QVERIFY(!PortType::parse("Text"));
    }

    void declaredTypesHaveANamespaceAndVersion() {
        const PortType palette = *PortType::parse("io.github.alice.tools/palette@2");
        QCOMPARE(palette.id, QStringLiteral("io.github.alice.tools/palette"));
        QCOMPARE(palette.version, 2);
        QVERIFY(!palette.isBuiltin());
        QCOMPARE(palette.toString(), QStringLiteral("io.github.alice.tools/palette@2"));

        // @1 may be omitted, and is omitted when written.
        QCOMPARE(*PortType::parse("fugue/latent@1"), *PortType::parse("fugue/latent"));
        QCOMPARE(PortType::parse("fugue/latent@1")->toString(), QStringLiteral("fugue/latent"));

        QVERIFY(!PortType::parse("fugue/latent@0"));
        QVERIFY(!PortType::parse("fugue/latent@01"));
        QVERIFY(!PortType::parse("fugue/latent@x"));
        QVERIFY(!PortType::parse("fugue/latent@"));
        QVERIFY(!PortType::parse("tools/latent"));
    }

    void anyIsAType() {
        const PortType any = *PortType::parse("any");
        QVERIFY(any.isAny());
        QVERIFY(!any.isBuiltin());
        QCOMPARE(any.toString(), QStringLiteral("any"));
    }

    void sameTypeConnects() {
        QCOMPARE(compatibility(port("image"), port("image")), Compatibility::Compatible);
        QCOMPARE(compatibility(port("fugue/latent"), port("fugue/latent@1")), Compatibility::Compatible);
    }

    void differentTypesDoNotConnect() {
        QCOMPARE(compatibility(port("image"), port("video")), Compatibility::DifferentType);
        // No implicit conversion, not even number to text.
        QCOMPARE(compatibility(port("number"), port("text")), Compatibility::DifferentType);
        // The version is part of the type.
        QCOMPARE(compatibility(port("fugue/latent@2"), port("fugue/latent")), Compatibility::DifferentType);
        QCOMPARE(compatibility(port("fugue/latent"), port("io.github.alice/latent")),
                 Compatibility::DifferentType);
    }

    void listsAreAModifier() {
        QCOMPARE(compatibility(port("image"), port("image", true)), Compatibility::Compatible);
        QCOMPARE(compatibility(port("image", true), port("image", true)), Compatibility::Compatible);
        QCOMPARE(compatibility(port("image", true), port("image")), Compatibility::ListIntoSingle);
        QCOMPARE(compatibility(port("image", true), port("video")), Compatibility::DifferentType);
    }

    void anyInputTakesEveryTypeButKeepsTheListRule() {
        QCOMPARE(compatibility(port("image"), port("any")), Compatibility::Compatible);
        QCOMPARE(compatibility(port("fugue/latent@3"), port("any")), Compatibility::Compatible);
        QCOMPARE(compatibility(port("text", true), port("any", true)), Compatibility::Compatible);
        QCOMPARE(compatibility(port("text", true), port("any")), Compatibility::ListIntoSingle);
        PortSpec side = port("any");
        side.takesList = true; // a Switch side: a list is one value to it
        QCOMPARE(compatibility(port("text", true), side), Compatibility::Compatible);
        QCOMPARE(compatibility(port("text"), side), Compatibility::Compatible);
        QVERIFY(side == port("any")); // not part of what the type is
    }

    void reasonsNameBothSides() {
        const QString reason = explain(Compatibility::DifferentType, port("number"), port("text"));
        QVERIFY2(reason.contains("number") && reason.contains("text"), qPrintable(reason));
        const QString list = explain(Compatibility::ListIntoSingle, port("image", true), port("image"));
        QVERIFY2(list.contains("list"), qPrintable(list));
        QCOMPARE(port("image", true).toString(), QStringLiteral("list of image"));
    }
};

QTEST_GUILESS_MAIN(TestPortType)
#include "test_port_type.moc"
