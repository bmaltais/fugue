// Dynamic Prompt (spec 14.1): a template of {a|b} choices, __wildcard__ files and ${variables}
// turned into one prompt, the same one for the same template, seed and wildcard files. The
// language and its limits are engine/dynamic_prompt.h.

#include "engine/dynamic_prompt.h"
#include "engine/node_kernel.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

dynprompt::Options currentOptions() {
    dynprompt::Options options;
    options.folders = dynprompt::wildcardFolders();
    return options;
}

class DynamicPromptKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const double seed = context.input("seed").json().toDouble();
        const Result<QString> text =
            dynprompt::generate(context.input("template").text(), quint64(seed < 0 ? 0 : seed), currentOptions());
        if (!text.ok())
            return Result<Values>(text.errors());
        return Values{{"text", Value(text.value())}};
    }

    Errors check(const NodeContext &context) const override {
        if (!context.inputs.contains("template"))
            return {};
        return dynprompt::check(context.input("template").text(), currentOptions());
    }

    // The wildcard files decide the answer too: an edited file is a new result.
    QString keyExtra(const Values &inputs) const override {
        if (!inputs.value("template").text().contains(QStringLiteral("__")))
            return {};
        return dynprompt::wildcardSignature(dynprompt::wildcardFolders());
    }
};

} // namespace

void addDynamicPromptKernels(Kernels &kernels) {
    kernels.add("fugue/dynamic-prompt", std::make_shared<DynamicPromptKernel>());
}

} // namespace fugue::nodes
