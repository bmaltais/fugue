// If/Else Switch (spec 14.1): passes on the value of one of two inputs, chosen by a boolean. Both are lazy
// (3.2, 9.9), so only what feeds the chosen one runs.

#include "engine/node_kernel.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

class SwitchKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const QString side = chosen(context.input("condition"));
        const Value value = context.input(side);
        if (value.isEmpty())
            return Result<Values>(makeInputError(
                QStringLiteral("switch_empty"), side,
                QStringLiteral("The condition is %1, so the value comes from %2, which has no value.")
                    .arg(side == QLatin1String("on_true") ? QStringLiteral("true") : QStringLiteral("false"), side),
                QStringLiteral("Link something to %1, or flip the condition.").arg(side)));
        return Values{{"value", value}};
    }

    // Only the chosen side is needed. The condition is a required input, so it is there once the node is asked.
    QStringList lazyInputs(const NodeType &, const Values &ready) const override { return {chosen(ready.value("condition"))}; }

private:
    static QString chosen(const Value &condition) {
        return condition.kind() == Value::Kind::Json && condition.json().toBool() ? QStringLiteral("on_true")
                                                                                   : QStringLiteral("on_false");
    }
};

} // namespace

void addSwitchKernels(Kernels &kernels) {
    kernels.add("fugue/switch", std::make_shared<SwitchKernel>());
}

} // namespace fugue::nodes
