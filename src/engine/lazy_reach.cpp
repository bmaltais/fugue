#include "engine/lazy_reach.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include "engine/group_library.h"

namespace fugue {

void collectHashes(const QJsonValue &value, QSet<QString> &hashes) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            collectHashes(item, hashes);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (const QString hash = object.value("hash").toString(); !hash.isEmpty())
            hashes.insert(hash);
        for (auto it = object.begin(); it != object.end(); ++it)
            collectHashes(it.value(), hashes);
    }
}

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

QSet<QString> conditionalModelHashes(const Document &document, const Registry &registry) {
    Document embedded = document;
    embedGroupTypes(embedded, registry); // a group named only by its type id runs from its definition, as in compileForRun
    CompileOptions options;
    options.allowUnsetInputs = true; // what an app's own values fill at the run is no reason to look at nothing
    const Compilation compiled = compile(embedded, registry, options);
    if (!compiled)
        return {};
    const QSet<QString> conditional = conditionalNodes(*compiled, registry);
    QSet<QString> lazyOnly, certain;
    for (auto it = compiled->nodes.constBegin(); it != compiled->nodes.constEnd(); ++it)
        for (const InputValue &value : it->inputs)
            collectHashes(value.toJson(), conditional.contains(it.key()) ? lazyOnly : certain);
    return lazyOnly.subtract(certain);
}

QJsonArray modelsToHave(const Document &document, const Registry &registry) {
    const QSet<QString> lazyOnly = conditionalModelHashes(document, registry);
    QJsonArray models;
    for (const QJsonValue model : document.models())
        if (!lazyOnly.contains(model.toObject().value(QStringLiteral("hash")).toString()))
            models.append(model);
    return models;
}

} // namespace fugue
