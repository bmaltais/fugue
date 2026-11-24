#include "engine/input_use.h"

#include "engine/groups.h"
#include "engine/links.h"

namespace fugue {

namespace {

constexpr int maxDepth = 8; // a source's own condition may ask about its sources

std::optional<bool> carriesAt(const Nodes &nodes, const Document &document, const Registry &registry,
                              const PortRef &source, const QString &property, int depth);

ShownContext contextFor(const Nodes &nodes, const Document &document, const Registry &registry, const Node &node,
                        const QList<InputSpec> &specs, int depth) {
    const auto spec = [&specs](const QString &name) -> const InputSpec * {
        for (const InputSpec &candidate : specs)
            if (candidate.name == name)
                return &candidate;
        return nullptr;
    };
    ShownContext context;
    context.linked = [&node](const QString &name) { return !linksIn(node.inputs.value(name)).isEmpty(); };
    context.value = [&node, spec](const QString &name) -> QJsonValue {
        const InputSpec *found = spec(name);
        if (!found)
            return QJsonValue::Undefined;
        const InputValue value = node.inputs.value(name);
        if (value.isEmpty())
            return found->defaultValue.isUndefined() ? QJsonValue::Null : found->defaultValue;
        if (value.kind() == InputValue::Kind::Literal)
            return value.literal();
        return QJsonValue::Undefined; // an auto seed, a handle: nothing to compare
    };
    context.carries = [&, depth](const QString &name, const QString &property) -> std::optional<bool> {
        const InputValue value = node.inputs.value(name);
        if (value.kind() != InputValue::Kind::Link)
            return false; // nothing wired: nothing carried
        if (depth >= maxDepth)
            return std::nullopt;
        return carriesAt(nodes, document, registry, value.from(), property, depth);
    };
    context.label = [spec](const QString &name) {
        const InputSpec *found = spec(name);
        return found ? found->label : QString();
    };
    return context;
}

std::optional<bool> carriesAt(const Nodes &nodes, const Document &document, const Registry &registry,
                              const PortRef &source, const QString &property, int depth) {
    const auto found = nodes.constFind(source.node);
    if (found == nodes.constEnd() || document.group(found->type) || found->mode == NodeMode::Bypass)
        return std::nullopt;
    const NodeType *type = registry.nodeTypeFor(found->type, found->version.major);
    const OutputSpec *output = type ? type->output(source.port) : nullptr;
    if (!output)
        return std::nullopt;
    if (!output->carries.contains(property))
        return false;
    if (output->carriesWhen.isEmpty())
        return true;
    return applies(output->carriesWhen, contextFor(nodes, document, registry, *found, type->inputs, depth + 1));
}

} // namespace

std::optional<bool> sourceCarries(const Nodes &nodes, const Document &document, const Registry &registry,
                                  const PortRef &source, const QString &property) {
    return carriesAt(nodes, document, registry, source, property, 0);
}

QList<UnusedInput> unusedInputs(const Nodes &nodes, const Document &document, const Registry &registry, const Node &node,
                                const QList<InputSpec> &specs) {
    QList<UnusedInput> unused;
    const ShownContext context = contextFor(nodes, document, registry, node, specs, 0);
    for (const InputSpec &input : specs) {
        if (input.shownWhen.isEmpty() || context.linked(input.name) || applies(input.shownWhen, context))
            continue;
        unused.append({input.name, shownReason(input.shownWhen, context)});
    }
    return unused;
}

QMap<QString, QList<UnusedInput>> unusedInputs(const Document &document, const Registry &registry) {
    QMap<QString, QList<UnusedInput>> all;
    for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it) {
        QList<InputSpec> specs;
        if (document.group(it->type))
            specs = groupFace(document, registry, it->type).inputs;
        else if (const NodeType *type = registry.nodeTypeFor(it->type, it->version.major))
            specs = type->inputs;
        const QList<UnusedInput> unused = unusedInputs(document.graph.nodes, document, registry, *it, specs);
        if (!unused.isEmpty())
            all.insert(it.key(), unused);
    }
    return all;
}

} // namespace fugue
