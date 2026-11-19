// The shared engine pieces: hashes, the on-disk JSON form and the error envelope.

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/error.h"
#include "engine/hash.h"
#include "engine/json.h"

using namespace fugue;

class TestFoundation : public QObject {
    Q_OBJECT

private slots:
    void hashOfBytesIsPrefixedSha256() {
        QCOMPARE(sha256("abc"),
                 QStringLiteral("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QVERIFY(isSha256(sha256("")));
        QVERIFY(!isSha256("sha256:ABC"));
        QVERIFY(!isSha256("md5:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    void hashOfFileMatchesHashOfItsBytes() {
        QTemporaryDir dir;
        const QString path = dir.filePath("f.bin");
        QByteArray bytes(3 * 1024 * 1024 + 7, 'x');
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();
        QCOMPARE(sha256File(path), sha256(bytes));
        QCOMPARE(sha256File(dir.filePath("missing")), QString());
    }

    void jsonIsSortedTwoSpaceWithTrailingNewline() {
        const QJsonObject value{
            {"b", QJsonArray{1, 2.5, "x"}},
            {"a", QJsonObject{}},
            {"c", QJsonObject{{"z", true}, {"y", QJsonValue::Null}}},
            {"d", QJsonArray{}},
        };
        const QByteArray expected = "{\n"
                                    "  \"a\": {},\n"
                                    "  \"b\": [\n"
                                    "    1,\n"
                                    "    2.5,\n"
                                    "    \"x\"\n"
                                    "  ],\n"
                                    "  \"c\": {\n"
                                    "    \"y\": null,\n"
                                    "    \"z\": true\n"
                                    "  },\n"
                                    "  \"d\": []\n"
                                    "}\n";
        QCOMPARE(writeJson(value), expected);
    }

    void jsonKeepsLargeIntegersExactAndEscapesStrings() {
        const qint64 seed = 9007199254740993; // 2^53 + 1: lost if stored as a double
        QCOMPARE(writeJson(QJsonValue(seed)), QByteArray("9007199254740993\n"));
        QCOMPARE(writeJson(QJsonValue(3.0)), QByteArray("3\n"));
        QCOMPARE(writeJson(QJsonValue(QStringLiteral("a\"b\né"))),
                 QByteArray("\"a\\\"b\\n\xc3\xa9\"\n"));
    }

    void errorCarriesOnlyTheFieldsThatAreSet() {
        Error error;
        error.code = "value_not_in_choices";
        error.node = "g7/shrink";
        error.input = "method";
        error.message = "\"lancos\" is not a valid method.";
        error.valid = QJsonArray{"nearest", "bilinear", "bicubic", "lanczos", "area"};
        error.hint = "Did you mean \"lanczos\"?";

        const QJsonObject json = error.toJson();
        QCOMPARE(json.value("severity").toString(), QStringLiteral("error"));
        QVERIFY(!json.contains("output"));
        QCOMPARE(json.value("valid").toArray().size(), 5);

        const Error back = Error::fromJson(json);
        QCOMPARE(back.toJson(), json);
    }

    void envelopes() {
        QCOMPARE(okEnvelope(QJsonObject{{"run", "r1"}}),
                 (QJsonObject{{"ok", true}, {"data", QJsonObject{{"run", "r1"}}}}));

        Error warning;
        warning.code = "not_part_of_output";
        warning.severity = Error::Severity::Warning;
        warning.message = "Node n1 feeds no output.";
        QVERIFY(!hasErrors({warning}));

        Error failure;
        failure.code = "cycle";
        failure.message = "The graph has a cycle.";
        QVERIFY(hasErrors({warning, failure}));

        const QJsonObject envelope = errorEnvelope({warning, failure});
        QCOMPARE(envelope.value("ok").toBool(), false);
        QCOMPARE(envelope.value("errors").toArray().size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestFoundation)
#include "test_foundation.moc"
