#include "nodes/builtin.h"

#include <cstdio>

#include "engine/command_node.h"
#include "engine/pack_loader.h"
#include "engine/prompt_templates.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

void addBuiltinNodeTypes(Registry &registry) {
    const LoadedPack pack = loadPack(registry, QStringLiteral(":/fugue/nodes"), PackOrigin::BuiltIn);
    for (const Error &error : pack.errors)
        std::fprintf(stderr, "fugue pack: %s: %s\n", qPrintable(error.code), qPrintable(error.message));
    for (const PackProblem &node : pack.disabled)
        std::fprintf(stderr, "fugue pack: %s: %s\n", qPrintable(node.node), qPrintable(node.reason));
    // Enhance Prompt's and Describe Image's Style are the prompt templates: Fugue's and the person's (#140).
    const PromptTemplates templates = PromptTemplates::load();
    for (const QString &problem : templates.problems())
        std::fprintf(stderr, "fugue prompts: %s\n", qPrintable(problem));
    registry.setInputChoices(QStringLiteral("fugue/enhance-prompt"), QStringLiteral("style"), templates.ids(promptFamilies()),
                             QStringLiteral("What the prompt is for:\n") + styleSummary(templates, promptFamilies()));
    // Describe Image's Style is the templates of the family "describe".
    const QStringList describe{QStringLiteral("describe")};
    registry.setInputChoices(QStringLiteral("fugue/describe-image"), QStringLiteral("style"), templates.ids(describe),
                             QStringLiteral("What to write about the picture:\n") + styleSummary(templates, describe));
}

void addBuiltinKernels(Kernels &kernels) {
    addBasicKernels(kernels);
    addCanvasKernels(kernels);
    addColorMatchKernels(kernels);
    addComponentKernels(kernels);
    addCropStitchKernels(kernels);
    addImageGenerationKernels(kernels);
    addImageSizeKernels(kernels);
    addLutKernels(kernels);
    addMaskKernels(kernels);
    addMediaGenerationKernels(kernels);
    addPixelKernels(kernels);
    addSaveMediaKernels(kernels);
    addTextGenerationKernels(kernels);
    addTextTemplateKernels(kernels);
    addUpscaleKernels(kernels);
    addRestoreKernels(kernels);
    addUpscaleVideoModelKernels(kernels);
    // The programs of the packs on disk (7).
    kernels.setCommandKernel(std::make_shared<CommandKernel>());
}

} // namespace fugue::nodes
