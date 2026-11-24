#include "engine/file_inputs.h"

#include "engine/node_type.h"
#include "engine/registry.h"

#include <QSet>

namespace fugue {

namespace {

// Whether group `id`'s exposed input `name` feeds, through its targets and nested groups, an input that holds a
// file. `seen` holds the (group, input) pairs on the way, so a group that contains itself ends the walk.
bool groupInputHoldsFile(const Document &document, const Registry &registry, const QString &id, const QString &name,
                         QSet<QString> &seen) {
    const GroupDefinition *definition = document.group(id);
    const QString key = id + QLatin1Char('\n') + name;
    if (!definition || seen.contains(key))
        return false;
    seen.insert(key);
    for (const GroupInput &exposed : definition->inputs) {
        if (exposed.name != name)
            continue;
        for (const PortRef &target : exposed.targets) {
            const auto inner = definition->nodes.constFind(target.node);
            if (inner == definition->nodes.constEnd())
                continue;
            if (document.groups.contains(inner->type)) {
                if (groupInputHoldsFile(document, registry, inner->type, target.port, seen))
                    return true;
                continue;
            }
            const NodeType *type = registry.nodeTypeFor(inner->type, inner->version.major);
            const InputSpec *spec = type ? type->input(target.port) : nullptr;
            if (spec && spec->holdsInputFile())
                return true;
        }
    }
    return false;
}

} // namespace

void forEachFileLiteral(Document &document, const Registry &registry,
                        const std::function<void(const QString &node, const QString &input, InputValue &value)> &visit) {
    const auto scan = [&](Nodes &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const bool instance = document.groups.contains(it->type);
            const NodeType *type = instance ? nullptr : registry.nodeTypeFor(it->type, it->version.major);
            if (!instance && !type)
                continue;
            for (auto in = it->inputs.begin(); in != it->inputs.end(); ++in) {
                if (in->kind() != InputValue::Kind::Literal || !in->literal().isString())
                    continue;
                bool holds = false;
                if (instance) {
                    QSet<QString> seen;
                    holds = groupInputHoldsFile(document, registry, it->type, in.key(), seen);
                } else {
                    const InputSpec *spec = type->input(in.key());
                    holds = spec && spec->holdsInputFile();
                }
                if (holds)
                    visit(it.key(), in.key(), *in);
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
}

} // namespace fugue
