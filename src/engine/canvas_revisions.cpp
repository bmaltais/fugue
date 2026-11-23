#include "engine/canvas_revisions.h"

#include "engine/canvas_store.h"

namespace fugue {

CanvasRevisionResolver sessionCanvasResolver() {
    return [](const QString &canvas, const QString &ref) {
        return canvas::CanvasStore::forSession().resolve(canvas, ref);
    };
}

QString activeCanvasOrNewest(const QString &shown) {
    const canvas::CanvasStore store = canvas::CanvasStore::forSession();
    if (!shown.isEmpty() && store.info(shown))
        return shown;
    const std::vector<canvas::CanvasInfo> all = store.list();
    return all.empty() ? QString() : all.front().id;
}

Errors resolveCanvasRevisions(CompiledGraph &graph, const CanvasRevisionResolver &resolve, const ActiveCanvasProvider &active) {
    Errors problems;
    if (!resolve)
        return problems;
    for (auto it = graph.nodes.begin(); it != graph.nodes.end(); ++it) {
        CompiledNode &node = it.value();
        if (node.type != QLatin1String(canvasRegionType))
            continue;
        InputValue canvas = node.inputs.value(QStringLiteral("canvas"));
        const InputValue revision = node.inputs.value(QStringLiteral("revision"));
        // An empty canvas is the Active canvas (#246): named here, once, as the head is.
        const bool canvasEmpty = canvas.isEmpty() || (canvas.kind() == InputValue::Kind::Literal && canvas.literal().isString() && canvas.literal().toString().trimmed().isEmpty());
        if (canvasEmpty && active) {
            const QString id = active();
            if (!id.isEmpty()) {
                canvas = InputValue::literal(id);
                node.inputs.insert(QStringLiteral("canvas"), canvas);
            }
        }
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
