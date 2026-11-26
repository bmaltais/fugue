#include "engine/lazy_reach.h"

#include <QList>

namespace fugue {

QSet<QString> conditionalNodes(const CompiledGraph &graph, const Registry &registry) {
    QSet<QString> readFrom; // nodes some node reads
    for (const CompiledNode &node : graph.nodes)
        for (const CompiledLink &link : node.links())
            readFrom.insert(link.from.node);
    QSet<QString> certain;
    QList<QString> pending;
    for (auto it = graph.nodes.constBegin(); it != graph.nodes.constEnd(); ++it)
        if (it->sink || !readFrom.contains(it.key())) {
            certain.insert(it.key());
            pending.append(it.key());
        }
    while (!pending.isEmpty()) {
        const CompiledNode &node = graph.nodes.value(pending.takeLast());
        const NodeType *type = registry.nodeTypeFor(node.type, node.version.major);
        for (const CompiledLink &link : node.links()) {
            const InputSpec *spec = type ? type->input(link.input) : nullptr;
            if ((spec && spec->lazy) || certain.contains(link.from.node))
                continue;
            certain.insert(link.from.node);
            pending.append(link.from.node);
        }
    }
    QSet<QString> conditional;
    for (auto it = graph.nodes.constBegin(); it != graph.nodes.constEnd(); ++it)
        if (!certain.contains(it.key()))
            conditional.insert(it.key());
    return conditional;
}

} // namespace fugue
