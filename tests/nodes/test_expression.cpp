// Expression, Not, And, Or (spec 14.1).
//
// Seams: the formula language (nodes/expression_ops: parse once, evaluate with six values) by table, and
// the registered kernels as the scheduler calls them (run, check), for the node's own behaviour.

#include <QtTest>

#include <cmath>

#include "engine/node_kernel.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "nodes/expression_ops.h"

using namespace fugue;
using namespace fugue::nodes;

Q_DECLARE_METATYPE(ExpressionSlots)

class TestExpression : public QObject {
    Q_OBJECT

    static ExpressionSlots given(std::initializer_list<double> numbers) {
        ExpressionSlots s;
        size_t i = 0;
        for (double n : numbers)
            s[i++] = ExpressionValue{n, false};
        return s;
    }

private slots:
    void valuesAndPrecedence_data() {
        QTest::addColumn<QString>("formula");
        QTest::addColumn<double>("number");
        QTest::addColumn<bool>("isBoolean");

        const auto row = [](const char *formula, double number, bool isBoolean = false) {
            QTest::newRow(formula) << QString(formula) << number << isBoolean;
        };
        // a=7, b=2, c=-3 in the slots (see the test)
        row("1 + 2 * 3", 7);
        row("(1 + 2) * 3", 9);
        row("10 - 4 - 3", 3);
        row("8 / 4 / 2", 1);
        row("7 % 3", 1);
        row("-7 % 3", 2);          // Python: the sign of the divisor
        row("7 % -3", -2);
        row("2 ** 3 ** 2", 512);   // right to left
        row("-2 ** 2", -4);        // ** binds tighter than a unary minus on its left
        row("2 ** -1", 0.5);
        row("--3", 3);
        row(".5 + 1e1 + 2.5e-1", 10.75);
        row("a * b + c", 11);
        row("pi", M_PI);
        row("1 == 1", 1, true);
        row("1 != 1", 0, true);
        row("1 < 2", 1, true);
        row("2 > 3", 0, true);
        row("2 <= 2", 1, true);
        row("2 >= 3", 0, true);
        row("1 < 2 < 3", 1, true);      // chained, as in Python
        row("3 > 2 > 2", 0, true);
        row("1 + 1 == 2", 1, true);     // arithmetic binds tighter than a comparison
        row("not 1 == 2", 1, true);     // not is below a comparison
        row("not 5", 0, true);
        row("true and false or true", 1, true);  // and binds tighter than or
        row("5 and 3", 1, true);                 // always a boolean, not an operand
        row("0 or 0", 0, true);
        row("true + true", 2);                   // a boolean is 1 in arithmetic
        row("true == 1", 1, true);
        row("a if a > 5 else b", 7);
        row("1 if 0 else 2 if 0 else 3", 3);     // else side chains
        row("(a > 5) * 10", 10);
        // the short-circuits keep an unreachable branch from failing
        row("1 / 0 if false else 4", 4);
        row("false and 1 / 0", 0, true);
        row("true or 1 / 0", 1, true);
        // every function
        row("round(2.5)", 2);
        row("round(3.5)", 4);
        row("round(2.567, 2)", 2.57);
        row("floor(-1.5)", -2);
        row("ceil(1.2)", 2);
        row("abs(c)", 3);
        row("min(a, b, c)", -3);
        row("max(a, b, c)", 7);
        row("clamp(a, 0, 5)", 5);
        row("clamp(c, 0, 5)", 0);
        row("sqrt(16)", 4);
        row("pow(2, 10)", 1024);
        row("log(1)", 0);
        row("log2(8)", 3);
        row("log10(1000)", 3);
        row("sin(0)", 0);
        row("cos(0)", 1);
        row("tan(0)", 0);
    }
    void valuesAndPrecedence() {
        QFETCH(QString, formula);
        QFETCH(double, number);
        QFETCH(bool, isBoolean);
        ExpressionError error;
        const Expression expression = Expression::parse(formula, &error);
        QVERIFY2(expression.valid(), qPrintable(formula + ": " + error.message));
        ExpressionValue result;
        QVERIFY2(expression.evaluate(given({7, 2, -3}), &result, &error), qPrintable(formula + ": " + error.message));
        QVERIFY2(std::fabs(result.number - number) < 1e-9, qPrintable(formula + QString(" gave %1").arg(result.number)));
        QCOMPARE(result.isBoolean, isBoolean);
    }

    void parseErrorsNameTheColumn_data() {
        QTest::addColumn<QString>("formula");
        QTest::addColumn<int>("column");
        QTest::newRow("empty") << "  " << 1;
        QTest::newRow("trailing operator") << "1 +" << 4;
        QTest::newRow("unclosed paren") << "(1 + 2" << 7;
        QTest::newRow("stray paren") << "1 + 2)" << 6;
        QTest::newRow("unknown name") << "a + g" << 5;
        QTest::newRow("unknown function") << "1 + foo(2)" << 5;
        QTest::newRow("function without a call") << "sqrt + 1" << 1;
        QTest::newRow("too few arguments") << "clamp(1, 2)" << 1;
        QTest::newRow("too many arguments") << "abs(1, 2)" << 1;
        QTest::newRow("bad character") << "1 $ 2" << 3;
        QTest::newRow("assignment") << "a = 1" << 3;
        QTest::newRow("bad number") << "1.2.3" << 1;
        QTest::newRow("missing else") << "1 if a" << 7;
        QTest::newRow("two values") << "1 2" << 3;
        QTest::newRow("number too big") << "1e16" << 1;
        QTest::newRow("too deep") << QString(80, '(') + "1" + QString(80, ')') << 33;
    }
    void parseErrorsNameTheColumn() {
        QFETCH(QString, formula);
        QFETCH(int, column);
        ExpressionError error;
        QVERIFY2(!Expression::parse(formula, &error).valid(), qPrintable(formula));
        QCOMPARE(error.column, column);
        QVERIFY(!error.message.isEmpty());
    }

    void evaluationErrors_data() {
        QTest::addColumn<QString>("formula");
        QTest::addColumn<int>("column");
        QTest::newRow("divide by zero") << "1 + 4 / (a - 7)" << 7;
        QTest::newRow("modulo by zero") << "5 % 0" << 3;
        QTest::newRow("sqrt of negative") << "sqrt(c)" << 1;
        QTest::newRow("log of zero") << "log(0)" << 1;
        QTest::newRow("log10 of negative") << "1 + log10(c)" << 5;
        QTest::newRow("exponent over the cap") << "2 ** 2000" << 3;
        QTest::newRow("result over the cap") << "10 ** 16" << 4;
        QTest::newRow("sum over the cap") << "1e15 + 1e15" << 6;
        QTest::newRow("zero to a negative power") << "0 ** -1" << 3;
        QTest::newRow("fractional power of a negative") << "pow(c, 0.5)" << 1;
        QTest::newRow("clamp limits reversed") << "clamp(1, 5, 2)" << 1;
        QTest::newRow("round digits") << "round(1.5, 20)" << 1;
        QTest::newRow("unset input") << "a + d" << 5;
    }
    void evaluationErrors() {
        QFETCH(QString, formula);
        QFETCH(int, column);
        const Expression expression = Expression::parse(formula);
        QVERIFY2(expression.valid(), qPrintable(formula));
        ExpressionValue result;
        ExpressionError error;
        QVERIFY2(!expression.evaluate(given({7, 2, -3}), &result, &error), qPrintable(formula));
        QCOMPARE(error.column, column);
    }

    void anUnsetInputOnlyFailsWhenReached() {
        const Expression expression = Expression::parse("a if true else d");
        ExpressionValue result;
        QVERIFY(expression.evaluate(given({4}), &result));
        QCOMPARE(result.number, 4.0);
    }

    // ---- the nodes ----

private:
    Registry m_registry;
    Kernels m_kernels;

    Result<Values> run(const QString &type, const Values &inputs) {
        NodeContext c;
        c.type = m_registry.nodeType(type);
        c.inputs = inputs;
        return m_kernels.node(type)->run(c);
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    void expressionGivesNumberAndBoolean() {
        Result<Values> r = run("fugue/expression", {{"formula", Value("a * 2 - b")}, {"a", Value(3)}, {"b", Value(true)}});
        QVERIFY(r.ok());
        QCOMPARE(r.value().value("number").number(), 5.0);
        QCOMPARE(r.value().value("boolean").json().toBool(), true);
        r = run("fugue/expression", {{"formula", Value("a > 1024 and b")}, {"a", Value(2048)}, {"b", Value(false)}});
        QVERIFY(r.ok());
        QCOMPARE(r.value().value("number").number(), 0.0);
        QCOMPARE(r.value().value("boolean").json().toBool(), false);
    }

    void aBadFormulaIsAnErrorOnTheFormulaUnderRunAndValidate() {
        const Result<Values> r = run("fugue/expression", {{"formula", Value("1 + * 2")}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().input, QString("formula"));
        QVERIFY2(r.error().message.startsWith("Column 5"), qPrintable(r.error().message));

        NodeContext c;
        c.type = m_registry.nodeType("fugue/expression");
        c.inputs = {{"formula", Value("1 + * 2")}};
        const Errors checked = m_kernels.node("fugue/expression")->check(c);
        QCOMPARE(checked.size(), 1);
        QVERIFY(checked.first().message.startsWith("Column 5"));
        c.inputs = {{"formula", Value("a + 1")}}; // a is not known under validate: still fine
        QVERIFY(m_kernels.node("fugue/expression")->check(c).isEmpty());
    }

    void anInputOfTheWrongKindIsAnErrorNamingIt() {
        const Result<Values> r = run("fugue/expression", {{"formula", Value("a")}, {"a", Value("text")}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().input, QString("a"));
    }

    void notAndOr() {
        QCOMPARE(run("fugue/not", {{"value", Value(true)}}).value().value("value").json().toBool(), false);
        QCOMPARE(run("fugue/not", {{"value", Value(false)}}).value().value("value").json().toBool(), true);
        const auto list = [](std::initializer_list<bool> items) {
            QList<Value> values;
            for (bool b : items)
                values.append(Value(b));
            return Value::list(values);
        };
        const auto result = [&](const char *type, std::initializer_list<bool> items) {
            return run(type, {{"values", list(items)}}).value().value("value").json().toBool();
        };
        QVERIFY(result("fugue/and", {true, true, true}));
        QVERIFY(!result("fugue/and", {true, false, true}));
        QVERIFY(result("fugue/and", {true}));
        QVERIFY(result("fugue/or", {false, false, true}));
        QVERIFY(!result("fugue/or", {false, false}));
    }
};

QTEST_GUILESS_MAIN(TestExpression)
#include "test_expression.moc"
