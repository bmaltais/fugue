#include "engine/node_versions.h"

#include <QHash>

#include "engine/document.h"
#include "engine/registry.h"

namespace fugue {

namespace {

template <typename Specs> QHash<QString, QString> portsOf(const Specs &specs) {
    QHash<QString, QString> found;
    for (const auto &spec : specs)
        found.insert(spec.name, spec.type.toString());
    return found;
}

// Names `b` has that `a` lacks or has under another type, and the other way round, sorted.
void diff(const QHash<QString, QString> &a, const QHash<QString, QString> &b, QStringList *added, QStringList *removed) {
    for (auto it = a.cbegin(); it != a.cend(); ++it)
        if (b.value(it.key()) != it.value() || !b.contains(it.key()))
            *removed << it.key();
    for (auto it = b.cbegin(); it != b.cend(); ++it)
        if (a.value(it.key()) != it.value() || !a.contains(it.key()))
            *added << it.key();
    added->sort();
    removed->sort();
}

} // namespace

PortChanges portChanges(const NodeType &before, const NodeType &after) {
    PortChanges changes;
    diff(portsOf(before.inputs), portsOf(after.inputs), &changes.addedInputs, &changes.removedInputs);
    diff(portsOf(before.outputs), portsOf(after.outputs), &changes.addedOutputs, &changes.removedOutputs);
    return changes;
}

std::optional<Version> newerMajorFor(const Registry &registry, const Node &node) {
    const NodeType *newest = registry.nodeType(node.type);
    if (!newest || newest->version.major <= node.version.major || !registry.nodeTypeAt(node.type, node.version.major))
        return std::nullopt;
    return newest->version;
}

} // namespace fugue
