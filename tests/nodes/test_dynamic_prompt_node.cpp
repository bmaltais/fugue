// Dynamic Prompt (spec 14.1): the registered kernel, called as the scheduler calls it (run, check,
// keyExtra), and the node type in the registry. The language itself is tests/engine/test_dynamic_prompt.cpp.

#include <QtTest>

#include "engine/dynamic_prompt.h"
#include "engine/node_kernel.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

class TestDynamicPromptNode : public QObject {
    Q_OBJECT

    Registry m_registry;
    Kernels m_kernels;

    NodeContext context(const QString &templ, double seed) {
        NodeContext c;
        c.type = m_registry.nodeType("fugue/dynamic-prompt");
        c.inputs.insert("template", Value(templ));
        c.inputs.insert("seed", Value(QJsonValue(seed)));
        return c;
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        QVERIFY(m_registry.nodeType("fugue/dynamic-prompt"));
    }

    void theSeedDecidesThePrompt() {
        const auto text = [&](double seed) {
            const Result<Values> r = m_kernels.node("fugue/dynamic-prompt")->run(context("{a|b|c|d|e|f|g|h|i|j}{a|b|c|d|e|f|g|h|i|j}", seed));
            return r.ok() ? r.value().value("text").text() : "ERROR: " + r.error().message;
        };
        QCOMPARE(text(5), text(5));
        QVERIFY(text(5) != text(6) || text(5) != text(7));
        QCOMPARE(text(9007199254740991.0).size(), 2);
    }

    void errorsAreOnTheTemplateAndShowUnderValidate() {
        const auto kernel = m_kernels.node("fugue/dynamic-prompt");
        const Result<Values> r = kernel->run(context("a {b|c", 0));
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, QString("template_invalid"));
        QCOMPARE(r.error().input, QString("template"));
        const Errors checked = kernel->check(context("a {b|c", 0));
        QCOMPARE(checked.size(), 1);
        QVERIFY(checked.first().message.contains("never closed"));
        QVERIFY(kernel->check(context("a {b|c}", 0)).isEmpty());
        NodeContext linked = context("x", 0);
        linked.inputs.remove("template");
        QVERIFY(kernel->check(linked).isEmpty()); // a template not known yet is not checked
    }

    void wildcardFilesAreInTheCacheKey() {
        // Only a template with a wildcard reads files; the others keep a key that never changes.
        const Values plain{{"template", Value(QString("{a|b}"))}};
        const Values wild{{"template", Value(QString("__x__"))}};
        const auto kernel = m_kernels.node("fugue/dynamic-prompt");
        QVERIFY(kernel->keyExtra(plain).isEmpty());
        QVERIFY(!kernel->keyExtra(wild).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestDynamicPromptNode)
#include "test_dynamic_prompt_node.moc"
