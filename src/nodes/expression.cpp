// Expression (spec 14.1) and Not, And, Or: a formula over the inputs a to f, and logic on booleans.
// The formula language is in expression_ops.

#include <QJsonValue>

#include "engine/node_kernel.h"
#include "nodes/expression_ops.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

Error formulaError(const ExpressionError &e) {
    return makeInputError(QStringLiteral("formula_invalid"), QStringLiteral("formula"),
                          QStringLiteral("Column %1: %2").arg(e.column).arg(e.message),
                          QStringLiteral("The inputs are a to f; see the node's description for the operators and functions."));
}

class ExpressionKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        ExpressionError error;
        const Expression expression = Expression::parse(context.input("formula").text(), &error);
        if (!expression.valid())
            return Result<Values>(formulaError(error));
        ExpressionSlots inputs;
        for (int i = 0; i < kExpressionSlots; ++i) {
            const QString name(QChar(u'a' + i));
            const Value value = context.input(name);
            if (value.isEmpty())
                continue;
            const QJsonValue json = value.kind() == Value::Kind::Json ? value.json() : QJsonValue();
            if (json.isBool())
                inputs[size_t(i)] = ExpressionValue{json.toBool() ? 1.0 : 0.0, true};
            else if (json.isDouble())
                inputs[size_t(i)] = ExpressionValue{json.toDouble(), false};
            else
                return makeInputError(QStringLiteral("value_wrong_type"), name,
                                      QStringLiteral("The input %1 holds something other than a number or a boolean.").arg(name));
        }
        ExpressionValue result;
        if (!expression.evaluate(inputs, &result, &error))
            return Result<Values>(formulaError(error));
        return Values{{"number", Value(result.number)}, {"boolean", Value(result.truthy())}};
    }

    // Under validate: the formula must parse; its values are not known yet.
    Errors check(const NodeContext &context) const override {
        if (!context.inputs.contains("formula"))
            return {};
        ExpressionError error;
        if (Expression::parse(context.input("formula").text(), &error).valid())
            return {};
        return {formulaError(error)};
    }
};

class NotKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        return Values{{"value", Value(!context.input("value").json().toBool())}};
    }
};

// And and Or over the items of the `values` list; an empty list cannot happen (min_items 1).
class AndOrKernel : public NodeKernel {
public:
    explicit AndOrKernel(bool isAnd) : m_and(isAnd) {}
    Result<Values> run(const NodeContext &context) override {
        bool result = m_and;
        for (const Value &item : context.input("values").items()) {
            const bool on = item.json().toBool();
            result = m_and ? (result && on) : (result || on);
        }
        return Values{{"value", Value(result)}};
    }

private:
    bool m_and;
};

} // namespace

void addExpressionKernels(Kernels &kernels) {
    kernels.add("fugue/expression", std::make_shared<ExpressionKernel>());
    kernels.add("fugue/not", std::make_shared<NotKernel>());
    kernels.add("fugue/and", std::make_shared<AndOrKernel>(true));
    kernels.add("fugue/or", std::make_shared<AndOrKernel>(false));
}

} // namespace fugue::nodes
