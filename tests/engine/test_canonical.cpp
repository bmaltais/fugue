// The canonical JSON form used for hashing (spec 1.5, RFC 8785). Expected values are
// the RFC's own samples (section 3.2.2, 3.2.3 and appendix B).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <cstring>

#include "engine/canonical.h"
#include "engine/hash.h"
#include "engine/json.h"

using namespace fugue;

namespace {

double fromBits(quint64 bits) {
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

QJsonValue parse(const QByteArray &text) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text, &error);
    if (error.error != QJsonParseError::NoError)
        qFatal("bad test JSON: %s", qPrintable(error.errorString()));
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
}

// Readable in a failure message, unlike std::optional.
const QByteArray noForm = "<no canonical form>";
QByteArray canon(const QJsonValue &value) {
    return canonicalJson(value).value_or(noForm);
}

} // namespace

class TestCanonical : public QObject {
    Q_OBJECT

private slots:
    void numbersFollowEcmaScript_data() {
        QTest::addColumn<quint64>("bits");
        QTest::addColumn<QByteArray>("expected");
        const struct {
            quint64 bits;
            const char *text;
        } samples[] = {
            {0x0000000000000000, "0"},
            {0x8000000000000000, "0"},
            {0x0000000000000001, "5e-324"},
            {0x8000000000000001, "-5e-324"},
            {0x7fefffffffffffff, "1.7976931348623157e+308"},
            {0xffefffffffffffff, "-1.7976931348623157e+308"},
            {0x4340000000000000, "9007199254740992"},
            {0xc340000000000000, "-9007199254740992"},
            {0x4430000000000000, "295147905179352830000"},
            {0x44b52d02c7e14af5, "9.999999999999997e+22"},
            {0x44b52d02c7e14af6, "1e+23"},
            {0x44b52d02c7e14af7, "1.0000000000000001e+23"},
            {0x444b1ae4d6e2ef4e, "999999999999999700000"},
            {0x444b1ae4d6e2ef4f, "999999999999999900000"},
            {0x444b1ae4d6e2ef50, "1e+21"},
            {0x3eb0c6f7a0b5ed8c, "9.999999999999997e-7"},
            {0x3eb0c6f7a0b5ed8d, "0.000001"},
            {0x41b3de4355555553, "333333333.3333332"},
            {0x41b3de4355555554, "333333333.33333325"},
            {0x41b3de4355555555, "333333333.3333333"},
            {0x41b3de4355555556, "333333333.3333334"},
            {0x41b3de4355555557, "333333333.33333343"},
            {0xbecbf647612f3696, "-0.0000033333333333333333"},
            {0x43143ff3c1cb0959, "1424953923781206.2"},
        };
        for (const auto &sample : samples)
            QTest::addRow("%016llx", sample.bits) << sample.bits << QByteArray(sample.text);
    }

    void numbersFollowEcmaScript() {
        QFETCH(quint64, bits);
        QFETCH(QByteArray, expected);
        QCOMPARE(canon(QJsonValue(fromBits(bits))), QByteArray(expected));
    }

    void integersAreNumbers() {
        // Qt keeps integers as qint64; JCS sees every number as a double.
        QCOMPARE(canon(QJsonValue(qint64(1024))), QByteArray("1024"));
        QCOMPARE(canon(QJsonValue(qint64(-7))), QByteArray("-7"));
    }

    void rfcSampleObject() {
        const QJsonValue value = parse("{"
                                   "\"numbers\": [333333333.33333329, 1E30, 4.50, 2e-3, 0.000000000000000000000000001],"
                                   "\"string\": \"\\u20ac$\\u000F\\u000aA'\\u0042\\u0022\\u005c\\\\\\\"\\/\","
                                   "\"literals\": [null, true, false]"
                                   "}");
        const QByteArray expected = "{\"literals\":[null,true,false],\"numbers\":[333333333.3333333,"
                                    "1e+30,4.5,0.002,1e-27],\"string\":\"\xe2\x82\xac$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\"}";
        QCOMPARE(canon(value), QByteArray(expected));
    }

    void keysSortByUtf16CodeUnits() {
        const QJsonValue value = parse("{"
                                   "\"\\u20ac\": \"Euro Sign\","
                                   "\"\\r\": \"Carriage Return\","
                                   "\"\\ufb33\": \"Hebrew Letter Dalet With Dagesh\","
                                   "\"1\": \"One\","
                                   "\"\\ud83d\\ude00\": \"Emoji: Grinning Face\","
                                   "\"\\u0080\": \"Control\","
                                   "\"\\u00f6\": \"Latin Small Letter O With Diaeresis\""
                                   "}");
        const std::optional<QByteArray> text = canonicalJson(value);
        QVERIFY(text);
        const QStringList expected{"Carriage Return", "One", "Control",
                                   "Latin Small Letter O With Diaeresis", "Euro Sign",
                                   "Emoji: Grinning Face", "Hebrew Letter Dalet With Dagesh"};
        qsizetype from = 0;
        for (const QString &name : expected) {
            const qsizetype at = text->indexOf(name.toUtf8(), from);
            QVERIFY2(at > from, qPrintable(name));
            from = at;
        }
    }

    void onDiskOrderIsTheCanonicalOrder() {
        // Spec 4.6: the on-disk form sorts keys the same way.
        const QJsonObject value{{QString::fromUtf16(u"\ufb33"), 1},
                                {QString::fromUtf16(u"\U0001F600"), 2}};
        QCOMPARE(writeJson(value), QByteArray("{\n  \"\xf0\x9f\x98\x80\": 2,\n  \"\xef\xac\xb3\": 1\n}\n"));
    }

    void controlCharactersAndUnicodeSeparators() {
        const QString text = QString::fromUtf16(u"\b\t\n\f\r\u0001\u001f\u2028\u007f");
        QCOMPARE(canon(text),
                 QByteArray("\"\\b\\t\\n\\f\\r\\u0001\\u001f\xe2\x80\xa8\x7f\""));
    }

    void loneSurrogateHasNoCanonicalForm() {
        const QChar lone(char16_t(0xd800));
        QCOMPARE(canon(QJsonValue(QString(lone))), noForm);
        QCOMPARE(canon(QJsonObject{{QString(lone), 1}}), noForm);
        QCOMPARE(canonicalHash(QJsonArray{QString(lone)}), QString());
    }

    void hashIsSha256OfCanonicalBytes() {
        const QJsonObject value{{"b", 1}, {"a", QJsonArray{true, 2.5}}};
        QCOMPARE(canonicalHash(value), sha256("{\"a\":[true,2.5],\"b\":1}"));
    }
};

QTEST_GUILESS_MAIN(TestCanonical)
#include "test_canonical.moc"
