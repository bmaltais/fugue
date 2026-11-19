// The shared engine pieces: hashes, the on-disk JSON form and the error envelope.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/error.h"
#include "engine/hash.h"
#include "engine/json.h"
#include "engine/result.h"

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

    void aJsonFileIsWrittenAtomicallyInTheOnDiskFormAndReadBack() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("a/b")));
        const QString path = dir.filePath("a/b/state.json");
        const QJsonObject value{{"b", 1}, {"a", QJsonArray{true}}};
        QVERIFY(writeJsonFile(path, value).isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), writeJson(value));
        QCOMPARE(QDir(dir.filePath("a/b")).entryList(QDir::Files), (QStringList{"state.json"})); // no temporary left

        const Result<QJsonObject> read = readJsonObject(path);
        QVERIFY(read.ok());
        QCOMPARE(read.value(), value);
    }

    void aJsonFileThatCannotBeWrittenSaysSo() {
        QTemporaryDir dir;
        const Errors errors = writeJsonFile(dir.filePath("no/such/folder/inside.json"), QJsonObject{});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("permission_denied"));
        QVERIFY(errors.first().message.contains("inside.json"));
    }

    void readingAJsonFileTellsMissingFromCorrupt() {
        QTemporaryDir dir;
        QCOMPARE(readJsonObject(dir.filePath("missing.json")).error().code, QStringLiteral("file_not_found"));
        QCOMPARE(readJsonObject(dir.path()).error().code, QStringLiteral("file_not_found")); // a folder

        const auto write = [&](const QString &name, const QByteArray &bytes) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                qFatal("cannot write %s", qPrintable(name));
            file.write(bytes);
            return dir.filePath(name);
        };
        const Result<QJsonObject> torn = readJsonObject(write("torn.json", "{\n  \"a\": 1,\n  \"b\": tru"));
        QCOMPARE(torn.error().code, QStringLiteral("invalid_json"));
        QVERIFY2(torn.error().message.contains("line 3"), qPrintable(torn.error().message));
        QCOMPARE(readJsonObject(write("list.json", "[1, 2]")).error().code, QStringLiteral("invalid_json"));
        QCOMPARE(readJsonObject(write("empty.json", "")).error().code, QStringLiteral("invalid_json"));
        // A byte order mark is skipped.
        const Result<QJsonObject> marked = readJsonObject(write("bom.json", "\xef\xbb\xbf{\"a\": 1}"));
        QVERIFY(marked.ok());
        QCOMPARE(marked.value().value("a").toInt(), 1);
    }

    // result.h: a value with its warnings, or everything a failure reported.
    void aResultCarriesWarningsOnSuccessAndEverythingOnFailure() {
        Error warning = makeError("not_part_of_output", "Node x feeds nothing.");
        warning.severity = Error::Severity::Warning;
        const Error error = makeError("missing_input", "Input text has no value.");

        const Result<int> fine(7, Errors{warning});
        QVERIFY(fine.ok());
        QCOMPARE(fine.value(), 7);
        QCOMPARE(fine.warnings().size(), 1);
        QVERIFY(fine.errors().isEmpty());
        QCOMPARE(fine.problems().size(), 1);

        const Result<int> failed(Errors{warning, error});
        QVERIFY(!failed.ok());
        QCOMPARE(failed.errors().size(), 2); // in the order they were found
        QCOMPARE(failed.error().code, QStringLiteral("not_part_of_output"));
        QVERIFY(failed.warnings().isEmpty());

        QVERIFY(Result<int>(3).problems().isEmpty());
        QCOMPARE(Result<int>(error).errors().first().code, QStringLiteral("missing_input"));

        // collected(): what code that gathered problems as it went hands back.
        QCOMPARE(collected(std::optional<int>(5), Errors{warning}).warnings().size(), 1);
        const Result<int> none = collected(std::optional<int>(), Errors{error});
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, QStringLiteral("missing_input"));
    }

    void errorsBecomeAJsonArray() {
        Error warning = makeError("model_missing", "No model.", "Add one.");
        warning.severity = Error::Severity::Warning;
        const QJsonArray array = toJson(Errors{makeError("usage", "Bad."), warning});
        QCOMPARE(array.size(), 2);
        QCOMPARE(array.at(0).toObject().value("code").toString(), QStringLiteral("usage"));
        QCOMPARE(array.at(1).toObject().value("severity").toString(), QStringLiteral("warning"));
        QCOMPARE(errorEnvelope({makeError("usage", "Bad.")}).value("errors").toArray(), toJson(Errors{makeError("usage", "Bad.")}));
        QVERIFY(toJson(Errors{}).isEmpty());
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
