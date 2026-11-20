#include "nodes/builtin.h"

#include <cstdio>

#include "engine/command_node.h"
#include "engine/pack_loader.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

void addBuiltinNodeTypes(Registry &registry) {
    const LoadedPack pack = loadPack(registry, QStringLiteral(":/fugue/nodes"), PackOrigin::BuiltIn);
    for (const Error &error : pack.errors)
        std::fprintf(stderr, "fugue pack: %s: %s\n", qPrintable(error.code), qPrintable(error.message));
    for (const PackProblem &node : pack.disabled)
        std::fprintf(stderr, "fugue pack: %s: %s\n", qPrintable(node.node), qPrintable(node.reason));
}

void addBuiltinKernels(Kernels &kernels) {
    addBasicKernels(kernels);
    addImageGenerationKernels(kernels);
    addImageSizeKernels(kernels);
    addPixelKernels(kernels);
    // The programs of the packs on disk (7).
    kernels.setCommandKernel(std::make_shared<CommandKernel>());
}

} // namespace fugue::nodes
