#include "engine/canvas_revisions.h"

#include "engine/canvas_store.h"

namespace fugue {

CanvasRevisionResolver sessionCanvasResolver() {
    return [](const QString &canvas, const QString &ref) {
        return canvas::CanvasStore::forSession().resolve(canvas, ref);
    };
}

Errors resolveCanvasRevisions(CompiledGraph &graph, const CanvasRevisionResolver &resolve) {
    Errors problems;
    if (!resolve)
        return problems;
    for (auto it = graph.nodes.begin(); it != graph.nodes.end(); ++it) {
        CompiledNode &node = it.value();
        if (node.type != QLatin1String(canvasRegionType))
            continue;
        const InputValue canvas = node.inputs.value(QStringLiteral("canvas"));
        const InputValue revision = node.inputs.value(QStringLiteral("revision"));
        // A canvas that is not chosen yet (a template's blank) is left for the kernel to refuse at run time.
        const bool canvasKnown = canvas.kind() == InputValue::Kind::Literal && canvas.literal().isString() && !canvas.literal().toString().trimmed().isEmpty();
        const bool revisionKnown = revision.isEmpty() || (revision.kind() == InputValue::Kind::Literal && revision.literal().isString());
        if (!canvasKnown || !revisionKnown)
            continue;
        const QString ref = revision.isEmpty() ? QStringLiteral("head") : revision.literal().toString().trimmed();
        const Result<QString> resolved = resolve(canvas.literal().toString().trimmed(), ref.isEmpty() ? QStringLiteral("head") : ref);
        if (!resolved) {
            for (Error error : resolved.errors()) {
                error.node = it.key();
                error.input = QStringLiteral("revision");
                problems << error;
            }
            continue;
        }
        node.inputs.insert(QStringLiteral("revision"), InputValue::literal(resolved.value()));
    }
    return problems;
}

} // namespace fugue
