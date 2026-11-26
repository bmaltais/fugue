// If/Else Switch (spec 14.1): the kernel as the scheduler calls it, and the node type in the registry. The scheduler's
// part (the unchosen side never runs) is tests/engine/test_lazy_inputs.cpp.

#include <QJsonDocument>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/node_kernel.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

class TestSwitch : public QObject {
    Q_OBJECT

private:
    Registry m_registry;
    Kernels m_kernels;

    Result<Values> run(bool condition, const Values &sides) {
        NodeContext c;
        c.type = m_registry.nodeType("fugue/switch");
        c.inputs = sides;
        c.inputs.insert("condition", Value(condition));
        return m_kernels.node("fugue/switch")->run(c);
    }
    QStringList asks(bool condition) {
        return m_kernels.node("fugue/switch")->lazyInputs(*m_registry.nodeType("fugue/switch"), {{"condition", Value(condition)}});
    }
    Compilation compiled(const QByteArray &nodes) {
        const Result<Document> document = Document::fromJson(QJsonDocument::fromJson("{\"fugue\": 1, \"graph\": {\"nodes\": " + nodes + "}}").object());
        if (!document)
            qFatal("fixture does not load");
        return compile(*document, m_registry);
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    void theNodeTypeHasLazySidesAndAnOutputThatFollowsThem() {
        const NodeType *type = m_registry.nodeType("fugue/switch");
        QVERIFY(type);
        QVERIFY(!type->input("condition")->lazy && type->input("condition")->required());
        for (const char *side : {"on_true", "on_false"}) {
            QVERIFY(type->input(side)->lazy && type->input(side)->optional);
            QVERIFY(type->input(side)->type.type.isAny());
        }
        QCOMPARE(type->output("value")->follows, (QStringList{"on_true", "on_false"}));
        // The manifest round trips with its lazy flags and `follows`.
        const Result<NodeType> again = NodeType::fromJson(type->toJson());
        QVERIFY(again);
        QVERIFY(again->input("on_true")->lazy);
        QCOMPARE(again->output("value")->follows, type->output("value")->follows);
    }

    void onlyTheChosenSideIsAskedFor() {
        QCOMPARE(asks(true), QStringList{"on_true"});
        QCOMPARE(asks(false), QStringList{"on_false"});
    }

    void theChosenSideIsPassedOn() {
        QCOMPARE(run(true, {{"on_true", Value(QString("yes"))}, {"on_false", Value(QString("no"))}}).value().value("value").text(), QString("yes"));
        QCOMPARE(run(false, {{"on_true", Value(QString("yes"))}, {"on_false", Value(QString("no"))}}).value().value("value").text(), QString("no"));
    }

    void aChosenSideWithNothingIsAnErrorThatNamesIt() {
        const Result<Values> bad = run(true, {{"on_false", Value(QString("no"))}});
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().code, QStringLiteral("switch_empty"));
        QCOMPARE(bad.error().input, QStringLiteral("on_true"));
        QVERIFY(bad.error().message.contains("on_true"));
    }

    // The output has the type of what feeds the sides: a text goes into a text input, not into a number input.
    void theOutputTakesTheTypeOfItsInputs() {
        const char *ok = R"({"t": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a"}},
          "sw": {"type": "fugue/switch", "version": "1.0.0", "inputs": {"condition": true, "on_true": {"from": "t.text"}}},
          "x": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "sw.value"}}},
          "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "x.text"}}}})";
        QVERIFY2(compiled(ok).ok(), "a text through a Switch feeds a text input");
        const char *bad = R"({"t": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a"}},
          "n": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 1}},
          "sw": {"type": "fugue/switch", "version": "1.0.0", "inputs": {"condition": true, "on_true": {"from": "t.text"}, "on_false": {"from": "n.value"}}},
          "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "sw.value"}}}})";
        const Compilation different = compiled(bad);
        QVERIFY(!different.ok());
        QCOMPARE(different.errors().first().code, QStringLiteral("type_mismatch"));
    }
};

QTEST_GUILESS_MAIN(TestSwitch)
#include "test_switch.moc"
