// `shown_when` (spec 6.1, #429): reading a condition and judging it against what a node's values and
// links say, with no document or window. The graph side (what a source carries, groups) is checked on
// the real built-in types in tests/canvas/test_canvas_model.cpp and tests/cli.
//
// Seam: engine/shown_when.h, NodeType::fromJson.

#include <QJsonDocument>
#include <QtTest>

#include "engine/node_type.h"
#include "engine/shown_when.h"

using namespace fugue;

namespace {

ShownWhen parsed(const char *json, QStringList *problems = nullptr) {
    QStringList own;
    QStringList &sink = problems ? *problems : own;
    const ShownWhen condition = ShownWhen::parse(QJsonDocument::fromJson(json).object(), sink);
    if (!problems)
        Q_ASSERT(own.isEmpty());
    return condition;
}

// A node whose inputs are given by value, and the names that are wired. `carried` is what a link brings.
struct Node {
    QMap<QString, QJsonValue> values;
    QStringList wired;
    QMap<QString, bool> carried;

    ShownContext context() const {
        ShownContext c;
        c.value = [this](const QString &n) { return values.value(n, QJsonValue::Undefined); };
        c.linked = [this](const QString &n) { return wired.contains(n); };
        c.carries = [this](const QString &n, const QString &) -> std::optional<bool> {
            if (!carried.contains(n))
                return std::nullopt;
            return carried.value(n);
        };
        c.label = [](const QString &n) { return n.at(0).toUpper() + n.mid(1); };
        return c;
    }
};

} // namespace

class TestShownWhen : public QObject {
    Q_OBJECT

private slots:
    void aRowShowsOnlyWhenTheInputHasOneOfTheValues() {
        const ShownWhen blur = parsed(R"({"fill": ["edge-blur"]})");
        QVERIFY(applies(blur, Node{{{"fill", "edge-blur"}}, {}, {}}.context()));
        QVERIFY(!applies(blur, Node{{{"fill", "gray"}}, {}, {}}.context()));
        QVERIFY(!applies(blur, Node{{{"fill", QJsonValue::Null}}, {}, {}}.context())); // unset
    }

    void aComparisonReadsANumber() {
        const ShownWhen below = parsed(R"({"denoise": {"lt": 1}})");
        QVERIFY(applies(below, Node{{{"denoise", 0.5}}, {}, {}}.context()));
        QVERIFY(!applies(below, Node{{{"denoise", 1}}, {}, {}}.context()));
        const ShownWhen band = parsed(R"({"denoise": {"gt": 0, "le": 0.5}})");
        QVERIFY(!applies(band, Node{{{"denoise", 0}}, {}, {}}.context()));
        QVERIFY(applies(band, Node{{{"denoise", 0.5}}, {}, {}}.context()));
        QVERIFY(!applies(band, Node{{{"denoise", 0.6}}, {}, {}}.context()));
    }

    void aLinkDecidesWhenAskedAbout() {
        const ShownWhen free = parsed(R"({"linked": "mask", "is": false})");
        QVERIFY(applies(free, Node{}.context()));
        QVERIFY(!applies(free, Node{{}, {"mask"}, {}}.context()));
        QVERIFY(applies(parsed(R"({"linked": "image"})"), Node{{}, {"image"}, {}}.context())); // is defaults to true
    }

    void everyClauseMustHold() {
        const ShownWhen both = parsed(R"({"fill": ["edge-blur"], "linked": "mask", "is": false})");
        QVERIFY(applies(both, Node{{{"fill", "edge-blur"}}, {}, {}}.context()));
        QVERIFY(!applies(both, Node{{{"fill", "edge-blur"}}, {"mask"}, {}}.context()));
        QVERIFY(!applies(both, Node{{{"fill", "gray"}}, {}, {}}.context()));
    }

    void whatASourceCarriesComesFromTheContext() {
        const ShownWhen soft = parsed(R"({"carries": {"latent": "mask"}})");
        QVERIFY(applies(soft, Node{{}, {"latent"}, {{"latent", true}}}.context()));
        QVERIFY(!applies(soft, Node{{}, {"latent"}, {{"latent", false}}}.context()));
        // No static answer (an unknown pack): the row shows.
        QVERIFY(applies(soft, Node{{}, {"latent"}, {}}.context()));
    }

    void aValueWithNoStaticAnswerShowsTheRow() {
        const ShownWhen blur = parsed(R"({"fill": ["edge-blur"]})");
        QVERIFY(applies(blur, Node{{{"fill", QJsonValue::Undefined}}, {}, {}}.context())); // seed auto, handle, unknown
        QVERIFY(applies(blur, Node{{{"fill", "gray"}}, {"fill"}, {}}.context())); // wired: no value to read
    }

    void theReasonNamesTheInputsByLabel() {
        const Node node;
        QCOMPARE(shownReason(parsed(R"({"fill": ["edge-blur"], "linked": "mask", "is": false})"), node.context()),
                 QString("Applies only when Fill is edge-blur and Mask is not linked."));
        QCOMPARE(shownReason(parsed(R"({"denoise": {"lt": 1}})"), node.context()), QString("Applies only when Denoise is below 1."));
    }

    void badConditionsAreRefusedWithASentence() {
        for (const char *json : {R"({"fill": []})", R"({"fill": "edge"})", R"({"d": {"near": 1}})", R"({"is": true})",
                                 R"({"linked": 3})", R"({"carries": ["latent"]})"}) {
            QStringList problems;
            const ShownWhen condition = parsed(json, &problems);
            QVERIFY2(!problems.isEmpty(), json);
            QVERIFY2(condition.isEmpty(), json);
        }
    }

    void aGroupFaceRenamesInputsOrDropsTheCondition() {
        const ShownWhen blur = parsed(R"({"fill": ["edge-blur"], "carries": {"latent": "mask"}})");
        const ShownWhen face = blur.renamed([](const QString &n) { return n == "fill" ? QString("outpaint_fill") : n; });
        QCOMPARE(face.inputs().size(), 2);
        QVERIFY(face.inputs().contains("outpaint_fill"));
        QCOMPARE(face.raw.value("fill"), QJsonValue::Undefined);
        QCOMPARE(face.raw.value("outpaint_fill").toArray().at(0).toString(), QString("edge-blur"));
        // The group does not expose `latent`: no condition, the row always shows.
        QVERIFY(blur.renamed([](const QString &n) { return n == "fill" ? n : QString(); }).isEmpty());
    }

    void aManifestKeepsItsConditionsAndChecksWhatTheyRead() {
        const auto manifest = [](const QString &condition, const QString &carries) {
            return QJsonDocument::fromJson(QString(R"({"manifest": 1, "id": "io.github.t.tools/n", "version": "1.0.0", "name": "N",
                "runtime": {"kind": "command", "entry": "./n"},
                "inputs": [{"name": "fill", "type": "text", "default": "a", "choices": ["a", "b"]},
                           {"name": "blur", "type": "number", "default": 1, "shown_when": %1}],
                "outputs": [{"name": "latent", "type": "text"%2}]})").arg(condition, carries).toUtf8())
                .object();
        };
        const Result<NodeType> good = NodeType::fromJson(manifest(R"({"fill": ["a"]})", R"(, "carries": ["mask"], "carries_when": {"linked": "fill"})"));
        QVERIFY2(good.ok(), good.ok() ? "" : qPrintable(good.errors().first().message));
        QVERIFY(!good.value().input("blur")->shownWhen.isEmpty());
        // Written back as it was read.
        const Result<NodeType> again = NodeType::fromJson(good.value().toJson());
        QVERIFY(again.ok());
        QCOMPARE(again.value().input("blur")->shownWhen.raw, good.value().input("blur")->shownWhen.raw);
        QCOMPARE(again.value().output("latent")->carries, QStringList{"mask"});
        // A condition on the row's own input, on an input that is not there, or a carries_when with nothing carried.
        QVERIFY(!NodeType::fromJson(manifest(R"({"blur": [1]})", {})).ok());
        QVERIFY(!NodeType::fromJson(manifest(R"({"nope": ["a"]})", {})).ok());
        QVERIFY(!NodeType::fromJson(manifest(R"({"fill": ["a"]})", R"(, "carries_when": {"linked": "fill"})")).ok());
    }
};

QTEST_MAIN(TestShownWhen)
#include "test_shown_when.moc"
