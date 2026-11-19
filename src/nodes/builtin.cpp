#include "nodes/builtin.h"

#include <QByteArray>
#include <QDirIterator>
#include <QFile>
#include <QJsonObject>

#include <cstdio>

#include "engine/json.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

void addBuiltinPortTypes(Registry &registry) {
    const Errors errors = registry.add(
        DeclaredPortType{*PortType::parse(QStringLiteral("fugue/image-model")), PortKind::Resource,
                         QStringLiteral("Image model"), {}});
    for (const Error &error : errors)
        std::fprintf(stderr, "fugue/image-model: %s: %s\n", qPrintable(error.code), qPrintable(error.message));
}

void addBuiltinNodeTypes(Registry &registry) {
    addBuiltinPortTypes(registry);
    QStringList paths;
    QDirIterator it(QStringLiteral(":/fugue/nodes"), {QStringLiteral("node.json")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        paths << it.next();
    paths.sort();
    for (const QString &path : paths) {
        const Result<QJsonObject> manifest = readJsonObject(path);
        const Errors errors = manifest ? registry.addManifest(manifest.value()) : manifest.errors();
        for (const Error &error : errors)
            std::fprintf(stderr, "%s: %s: %s\n", qPrintable(path), qPrintable(error.code), qPrintable(error.message));
    }
}

void addBuiltinKernels(Kernels &kernels) {
    addBasicKernels(kernels);
    addImageGenerationKernels(kernels);
    addPixelKernels(kernels);
}

} // namespace fugue::nodes
