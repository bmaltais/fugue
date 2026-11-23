// Text Template (spec 14.1): a template filled with the text of inputs a to f.
//
// Seam: the registered kernel, called as the scheduler calls it (run, and check for validate), and the
// node type in the registry.

#include <QtTest>

#include "engine/node_kernel.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

class TestTextTemplate : public QObject {
    Q_OBJECT

private:
    Registry m_registry;
    Kernels m_kernels;

    NodeContext context(const QString &templ, const Values &given = {}, bool trim = false) {
        NodeContext c;
        c.type = m_registry.nodeType("fugue/text-template");
        c.inputs = given;
        c.inputs.insert("template", Value(templ));
        c.inputs.insert("trim", Value(trim));
        return c;
    }
    Result<Values> run(const NodeContext &c) { return m_kernels.node("fugue/text-template")->run(c); }
    QString text(const QString &templ, const Values &given = {}, bool trim = false) {
        const Result<Values> r = run(context(templ, given, trim));
        if (!r.ok())
            return "ERROR: " + r.error().message;
        return r.value().value("text").text();
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    void placeholdersTakeTheTextOfTheirInput() {
        QCOMPARE(text("Keep {a}. Change the pose to {b}. {c}", {{"a", "the room"}, {"b", "sitting"}, {"c", "Smile."}}),
                 QString("Keep the room. Change the pose to sitting. Smile."));
        QCOMPARE(text("{a}{a}-{f}", {{"a", "x"}, {"f", "z"}}), QString("xx-z"));
    }

    void anEmptyOrMissingInputGivesNothing() {
        QCOMPARE(text("[{a}][{b}]", {{"a", ""}}), QString("[][]"));
    }

    void doubledBracesAreLiteralAndInputTextIsNotReparsed() {
        QCOMPARE(text("{{a}} {{ }} {a}", {{"a", "{b}"}, {"b", "no"}}), QString("{a} { } {b}"));
    }

    void unknownOrBrokenPlaceholdersAreErrorsThatNameThem() {
        for (const QString &bad : {QString("x {pose} y"), QString("{A}"), QString("{}"), QString("{ a }"), QString("{g}")}) {
            const Result<Values> r = run(context(bad));
            QVERIFY2(!r.ok(), qPrintable(bad));
            QCOMPARE(r.error().code, QString("template_invalid"));
            QCOMPARE(r.error().input, QString("template"));
        }
        QVERIFY(run(context("x {pose} y")).error().message.contains("{pose}"));
        QVERIFY(!run(context("open {a")).ok());
        QVERIFY(!run(context("close } here")).ok());
    }

    void validateReportsTheSameErrorsWithoutTheInputs() {
        NodeContext c;
        c.inputs = {{"template", Value("{nope}")}};
        QVERIFY(m_kernels.node("fugue/text-template")->check(c).first().message.contains("{nope}"));
        c.inputs = {{"template", Value("{a} {{")}};
        QVERIFY(m_kernels.node("fugue/text-template")->check(c).isEmpty());
        QVERIFY(m_kernels.node("fugue/text-template")->check(NodeContext()).isEmpty());
    }

    void textIsKeptAsIsUnlessTrimIsOn() {
        const Values given{{"a", "\n one \n"}};
        QCOMPARE(text("  {a}  \n", given), QString("  \n one \n  \n"));
        QCOMPARE(text("  {a}  \n", given, true), QString("one"));
    }

    void multilineUnicodeAndLargeInputs() {
        QCOMPARE(text("Ligne 1\n{a}\r\n{b}", {{"a", "été 日本語 🎨"}, {"b", "x\ny"}}), QString("Ligne 1\nété 日本語 🎨\r\nx\ny"));
        const QString big(2000000, 'q');
        QCOMPARE(text("<{a}>{a}", {{"a", big}}).size(), int(big.size() * 2 + 2));
    }

    void theNodeTypeIsRegisteredAndDiscoverable() {
        const NodeType *type = m_registry.nodeType("fugue/text-template");
        QVERIFY(type);
        QCOMPARE(type->name, QString("Text Template"));
        QCOMPARE(type->category, QString("Text"));
        QVERIFY(type->input("template") && type->input("template")->widget() == "multiline");
        QVERIFY(!type->input("template")->description.isEmpty());
        for (const char *slot : {"a", "b", "c", "d", "e", "f"})
            QVERIFY2(type->input(slot), slot);
        QVERIFY(type->input("trim")->advanced);
        QVERIFY(type->output("text"));
        QVERIFY(m_kernels.node("fugue/text-template"));
    }
};

QTEST_GUILESS_MAIN(TestTextTemplate)
#include "test_text_template.moc"
