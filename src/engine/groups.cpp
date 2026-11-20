#include "engine/groups.h"

#include "engine/compiler.h"
#include "engine/ids.h"
#include "engine/links.h"
#include "engine/registry.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <optional>

namespace fugue {

namespace {

Error groupError(const QString &message, const QString &hint = {}) {
    Error error;
    error.code = QStringLiteral("group_invalid");
    error.message = message;
    error.hint = hint;
    return error;
}

// Builds faces, seeing through nested groups; a group already being built (recursion, which
// the compiler reports) resolves to nothing.
struct Faces {
    const Document &document;
    const Registry &registry;
    QSet<QString> building;

    std::optional<NodeType> face(const QString &id) {
        const GroupDefinition *group = document.group(id);
        if (!group || building.contains(id))
            return std::nullopt;
        building.insert(id);
        NodeType type;
        type.id = id;
        type.version = group->version;
        type.name = group->name.isEmpty() ? id : group->name;
        type.description = group->description;
        type.category = group->category;
        type.runtime = QJsonObject{{"kind", "group"}};
        for (const GroupInput &exposed : group->inputs) {
            if (exposed.targets.isEmpty())
                continue;
            const PortRef &target = exposed.targets.first();
            std::optional<InputSpec> inner = input(*group, target);
            if (!inner)
                continue;
            const QString innerLabel = inner->label;
            InputSpec spec = exposedInputSpec(*inner, exposed);
            spec.label = !exposed.label.isEmpty() ? exposed.label : exposed.name == target.port ? innerLabel : QString();
            spec.section = exposed.section;
            spec.advanced = exposed.section == QLatin1String(kAdvancedSection);
            type.inputs << spec;
        }
        for (const GroupOutput &exposed : group->outputs) {
            if (std::optional<OutputSpec> spec = output(*group, exposed.from)) {
                spec->name = exposed.name;
                if (exposed.name != exposed.from.port)
                    spec->label.clear();
                type.outputs << *spec;
            }
        }
        building.remove(id);
        return type;
    }

    // The spec of an inner input, with the inner node's own literal as its default (an
    // instance that does not set the exposed input falls back to it, 4.4).
    std::optional<InputSpec> input(const GroupDefinition &group, const PortRef &target) {
        const auto node = group.nodes.constFind(target.node);
        if (node == group.nodes.constEnd())
            return std::nullopt;
        std::optional<InputSpec> spec;
        if (document.group(node->type)) {
            const std::optional<NodeType> nested = face(node->type);
            if (const InputSpec *found = nested ? nested->input(target.port) : nullptr)
                spec = *found;
        } else if (const NodeType *type = registry.nodeType(node->type)) {
            if (const InputSpec *found = type->input(target.port))
                spec = *found;
        }
        if (!spec)
            return std::nullopt;
        const InputValue own = node->inputs.value(target.port);
        if (!own.isEmpty() && own.kind() == InputValue::Kind::Literal)
            spec->defaultValue = own.literal();
        return spec;
    }

    std::optional<OutputSpec> output(const GroupDefinition &group, const PortRef &from) {
        const auto node = group.nodes.constFind(from.node);
        if (node == group.nodes.constEnd())
            return std::nullopt;
        if (document.group(node->type)) {
            const std::optional<NodeType> nested = face(node->type);
            if (const OutputSpec *found = nested ? nested->output(from.port) : nullptr)
                return *found;
            return std::nullopt;
        }
        if (const NodeType *type = registry.nodeType(node->type))
            if (const OutputSpec *found = type->output(from.port))
                return *found;
        return std::nullopt;
    }
};

// The port type of a node's input in a node map, through a group's face; none when unknown.
std::optional<PortSpec> inputPort(const Document &document, const Registry &registry, const Node &node,
                                  const QString &input) {
    if (document.group(node.type)) {
        const NodeType face = groupFace(document, registry, node.type);
        if (const InputSpec *spec = face.input(input))
            return spec->type;
        return std::nullopt;
    }
    if (const NodeType *type = registry.nodeType(node.type))
        if (const InputSpec *spec = type->input(input))
            return spec->type;
    return std::nullopt;
}

// Whether a node has that input or output. A node whose type is not known may have anything.
bool hasPort(const Document &document, const Registry &registry, const Node &node, const QString &port, bool output) {
    if (const GroupDefinition *group = document.group(node.type)) {
        if (output)
            return std::any_of(group->outputs.begin(), group->outputs.end(),
                               [&](const GroupOutput &o) { return o.name == port; });
        return std::any_of(group->inputs.begin(), group->inputs.end(), [&](const GroupInput &i) { return i.name == port; });
    }
    if (const NodeType *type = registry.nodeType(node.type))
        return output ? type->output(port) != nullptr : type->input(port) != nullptr;
    return true;
}

QString freeName(QSet<QString> &used, QString base) {
    if (!isPortName(base))
        base = QStringLiteral("value");
    QString name = base;
    for (int n = 2; used.contains(name); ++n)
        name = QStringLiteral("%1_%2").arg(base).arg(n);
    used.insert(name);
    return name;
}

QSet<QString> inputNames(const GroupDefinition &group) {
    QSet<QString> names;
    for (const GroupInput &input : group.inputs)
        names.insert(input.name);
    return names;
}

QSet<QString> outputNames(const GroupDefinition &group) {
    QSet<QString> names;
    for (const GroupOutput &output : group.outputs)
        names.insert(output.name);
    return names;
}

// A layout.links key (4.5): "<node>.<input>" or "<node>.<input>#<item>".
struct LinkKey {
    QString node, input;
    QString rest; // "#<item>" or empty
    QString text() const { return node + QLatin1Char('.') + input + rest; }
};

LinkKey parseLinkKey(const QString &key) {
    const QString tail = key.section(QLatin1Char('.'), 1);
    const qsizetype hash = tail.indexOf(QLatin1Char('#'));
    return {key.section(QLatin1Char('.'), 0, 0), hash < 0 ? tail : tail.left(hash), hash < 0 ? QString() : tail.mid(hash)};
}

std::optional<QPointF> pointOf(const QJsonValue &value) {
    const QJsonArray pair = value.toArray();
    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
        return std::nullopt;
    return QPointF(pair[0].toDouble(), pair[1].toDouble());
}

QJsonArray pointJson(QPointF p) { return QJsonArray{qRound64(p.x()), qRound64(p.y())}; }

QJsonObject shiftedPoints(QJsonObject entry, QPointF by) {
    QJsonArray points;
    for (const QJsonValue &p : entry.value("points").toArray())
        if (const auto at = pointOf(p))
            points.append(pointJson(*at + by));
    entry.insert("points", points);
    return entry;
}

void setOrRemove(QJsonObject &object, const QString &key, const QJsonObject &value) {
    if (value.isEmpty())
        object.remove(key);
    else
        object.insert(key, value);
}

// Every Frame of the layout loses these nodes from its list.
void leaveFrames(QJsonObject &layout, const QSet<QString> &nodes) {
    if (!layout.contains("frames"))
        return;
    QJsonArray frames;
    for (const QJsonValue &value : layout.value("frames").toArray()) {
        QJsonObject frame = value.toObject();
        if (frame.contains("nodes")) {
            QJsonArray kept;
            for (const QJsonValue &id : frame.value("nodes").toArray())
                if (!nodes.contains(id.toString()))
                    kept.append(id);
            frame.insert("nodes", kept);
        }
        frames.append(frame);
    }
    layout.insert("frames", frames);
}

// The instances of `group` in a node map.
QSet<QString> instancesIn(const Nodes &nodes, const QString &group) {
    QSet<QString> found;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        if (it->type == group)
            found.insert(it.key());
    return found;
}

// Renames layout.links keys "<instance>.<old>" in one links object.
QJsonObject renamedLinkKeys(const QJsonObject &links, const QSet<QString> &instances, const QString &from,
                            const QString &to) {
    QJsonObject renamed;
    for (auto it = links.constBegin(); it != links.constEnd(); ++it) {
        LinkKey key = parseLinkKey(it.key());
        if (instances.contains(key.node) && key.input == from)
            key.input = to;
        renamed.insert(key.text(), it.value());
    }
    return renamed;
}

// Applies `rename` to the layout.links of the graph (instances in graph.nodes) and of each
// group definition's layout (instances in its nodes).
void renameInstanceInputs(Document &document, const QString &group, const QString &from, const QString &to) {
    forEachNodes(document, [&](Nodes &nodes) {
        for (Node &node : nodes)
            if (node.type == group && node.inputs.contains(from))
                node.inputs.insert(to, node.inputs.take(from));
    });
    // An outer group whose face drives the instance's input drives it under the new name.
    for (GroupDefinition &outer : document.groups) {
        const QSet<QString> instances = instancesIn(outer.nodes, group);
        for (GroupInput &exposed : outer.inputs)
            for (PortRef &target : exposed.targets)
                if (instances.contains(target.node) && target.port == from)
                    target.port = to;
    }
    QJsonObject layout = document.layout;
    if (layout.contains("links"))
        layout.insert("links", renamedLinkKeys(layout.value("links").toObject(),
                                               instancesIn(document.graph.nodes, group), from, to));
    QJsonObject groupLayouts = layout.value("groups").toObject();
    for (auto it = groupLayouts.begin(); it != groupLayouts.end(); ++it) {
        const GroupDefinition *definition = document.group(it.key());
        QJsonObject inner = it.value().toObject();
        if (!definition || !inner.contains("links"))
            continue;
        inner.insert("links", renamedLinkKeys(inner.value("links").toObject(), instancesIn(definition->nodes, group),
                                              from, to));
        it.value() = inner;
    }
    if (!groupLayouts.isEmpty())
        layout.insert("groups", groupLayouts);
    document.layout = layout;
}

// Drops instance values of these exposed inputs and links from these exposed outputs, on
// every instance of the group.
// Drops instance values of these exposed inputs and links from these exposed outputs, on
// every instance of the group; and an outer group's face that led to them (B exposing the
// instance's port a1.x) loses that target or output, which cascades outwards.
void dropFromInstances(Document &document, const QString &group, const QSet<QString> &inputs, const QSet<QString> &outputs) {
    if (inputs.isEmpty() && outputs.isEmpty())
        return;
    forEachNodes(document, [&](Nodes &nodes) {
        const QSet<QString> instances = instancesIn(nodes, group);
        if (instances.isEmpty())
            return;
        for (Node &node : nodes) {
            if (node.type == group)
                for (const QString &name : inputs)
                    node.inputs.remove(name);
            if (!outputs.isEmpty())
                stripLinks(node, [&](const PortRef &from) {
                    return instances.contains(from.node) && outputs.contains(from.port);
                });
        }
    });
    QHash<QString, std::pair<QSet<QString>, QSet<QString>>> outer; // group -> (inputs, outputs) gone
    for (auto it = document.groups.begin(); it != document.groups.end(); ++it) {
        const QSet<QString> instances = instancesIn(it->nodes, group);
        if (instances.isEmpty())
            continue;
        for (qsizetype i = it->inputs.size() - 1; i >= 0; --i) {
            GroupInput &exposed = it->inputs[i];
            exposed.targets.removeIf([&](const PortRef &t) { return instances.contains(t.node) && inputs.contains(t.port); });
            if (exposed.targets.isEmpty()) {
                outer[it.key()].first.insert(exposed.name);
                it->inputs.removeAt(i);
            }
        }
        for (qsizetype i = it->outputs.size() - 1; i >= 0; --i)
            if (instances.contains(it->outputs[i].from.node) && outputs.contains(it->outputs[i].from.port)) {
                outer[it.key()].second.insert(it->outputs[i].name);
                it->outputs.removeAt(i);
            }
    }
    // No recursion (5.2): each step goes one group outwards, so this ends.
    for (auto it = outer.cbegin(); it != outer.cend(); ++it)
        if (it.key() != group)
            dropFromInstances(document, it.key(), it->first, it->second);
}

GroupDefinition *groupOf(Document &document, const QString &group) {
    const auto it = document.groups.find(group);
    return it == document.groups.end() ? nullptr : &it.value();
}

QJsonObject groupLayout(const Document &document, const QString &group) {
    return document.layout.value("groups").toObject().value(group).toObject();
}

void setGroupLayout(Document &document, const QString &group, const QJsonObject &inner) {
    QJsonObject groups = document.layout.value("groups").toObject();
    setOrRemove(groups, group, inner);
    setOrRemove(document.layout, QStringLiteral("groups"), groups);
}

} // namespace

NodeType groupFace(const Document &document, const Registry &registry, const QString &group) {
    Faces faces{document, registry, {}};
    return faces.face(group).value_or(NodeType());
}

bool groupContains(const Document &document, const QString &outer, const QString &inner) {
    QSet<QString> seen;
    QStringList todo{outer};
    while (!todo.isEmpty()) {
        const QString id = todo.takeLast();
        if (id == inner)
            return true;
        if (seen.contains(id))
            continue;
        seen.insert(id);
        if (const GroupDefinition *group = document.group(id))
            for (const Node &node : group->nodes)
                if (document.group(node.type))
                    todo << node.type;
    }
    return false;
}

void forEachNodes(Document &document, const std::function<void(Nodes &)> &visit) {
    visit(document.graph.nodes);
    for (GroupDefinition &group : document.groups)
        visit(group.nodes);
}

QString uniqueGroupId(const Document &document, const Registry &registry, const QString &base) {
    QString name = base.toLower();
    name.replace(QRegularExpression(QStringLiteral("[^a-z0-9-]+")), QStringLiteral("-"));
    name.remove(QRegularExpression(QStringLiteral("^[^a-z]+")));
    while (name.endsWith(QLatin1Char('-')))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("group");
    const QString stem = QStringLiteral("local/") + name;
    QString id = stem;
    for (int n = 2; document.groups.contains(id) || registry.nodeType(id); ++n)
        id = QStringLiteral("%1-%2").arg(stem).arg(n);
    return id;
}

QStringList sinksAmong(const Document &document, const Registry &registry, const QStringList &nodes) {
    QStringList sinks;
    for (const QString &id : nodes) {
        const auto node = document.graph.nodes.constFind(id);
        if (node == document.graph.nodes.constEnd() || document.group(node->type))
            continue;
        if (const NodeType *type = registry.nodeType(node->type); type && type->sink)
            sinks << id;
    }
    return sinks;
}

Result<GroupMade> makeGroup(const Document &document, const QStringList &nodes, const Registry &registry) {
    Document doc = document;
    QStringList order;
    for (const QString &id : nodes)
        if (doc.graph.nodes.contains(id) && !order.contains(id))
            order << id;
    if (order.isEmpty())
        return groupError(QStringLiteral("Select the nodes to put in a group first."));
    const QSet<QString> inside(order.begin(), order.end());

    const QString groupId = uniqueGroupId(doc, registry, QStringLiteral("group"));
    GroupDefinition group;
    group.version = Version{1, 0, 0};
    group.name = QStringLiteral("Group");
    for (const QString &id : order)
        group.nodes.insert(id, doc.graph.nodes.value(id));

    const QString instanceId = newNodeId(doc.graph.nodes);
    Node instance;
    instance.type = groupId;
    instance.version = group.version;

    // Inputs: links from outside into the selection.
    QSet<QString> usedInputs;
    QHash<QString, int> bySource; // "<source>|<type>" -> index into group.inputs
    for (auto node = group.nodes.begin(); node != group.nodes.end(); ++node) {
        for (const QString &name : node->inputs.keys()) {
            const InputValue value = node->inputs.value(name);
            const auto links = linksIn(value);
            const auto outside = std::count_if(links.begin(), links.end(),
                                               [&](const auto &link) { return !inside.contains(link.first.node); });
            if (outside == 0)
                continue;
            const PortRef target{node.key(), name};
            if (value.kind() == InputValue::Kind::List) {
                if (outside != links.size())
                    return groupError(QStringLiteral("The list input %1 of node %2 takes links from inside and outside "
                                                     "the selection, and a group cannot split it.")
                                          .arg(name, node.key()),
                                      QStringLiteral("Put every node feeding it in the selection, or none of them."));
                GroupInput exposed;
                exposed.name = freeName(usedInputs, name);
                exposed.targets << target;
                group.inputs << exposed;
                instance.inputs.insert(exposed.name, value);
                node->inputs.remove(name);
                continue;
            }
            const PortRef from = value.from();
            const std::optional<PortSpec> port = inputPort(doc, registry, document.graph.nodes.value(node.key()), name);
            // An input whose type is not known is never merged with another.
            const QString key = port ? from.toString() + QLatin1Char('|') + port->toString() : QString();
            if (!key.isEmpty() && bySource.contains(key)) {
                group.inputs[bySource.value(key)].targets << target;
            } else {
                GroupInput exposed;
                exposed.name = freeName(usedInputs, name);
                exposed.targets << target;
                if (!key.isEmpty())
                    bySource.insert(key, int(group.inputs.size()));
                group.inputs << exposed;
                instance.inputs.insert(exposed.name, value);
            }
            node->inputs.remove(name);
        }
    }

    // Outputs: links from the selection to the rest of the level.
    QSet<QString> usedOutputs;
    QHash<QString, QString> byOutput; // inner "<node>.<output>" -> exposed name
    for (auto node = doc.graph.nodes.begin(); node != doc.graph.nodes.end(); ++node) {
        if (inside.contains(node.key()))
            continue;
        for (auto in = node->inputs.begin(); in != node->inputs.end(); ++in) {
            in.value() = withLinksMapped(in.value(), [&](const PortRef &from) {
                if (!inside.contains(from.node))
                    return from;
                const QString key = from.toString();
                if (!byOutput.contains(key)) {
                    GroupOutput exposed;
                    exposed.name = freeName(usedOutputs, from.port);
                    exposed.from = from;
                    group.outputs << exposed;
                    byOutput.insert(key, exposed.name);
                }
                return PortRef{instanceId, byOutput.value(key)};
            });
        }
    }

    for (const QString &id : order)
        doc.graph.nodes.remove(id);
    doc.graph.nodes.insert(instanceId, instance);
    doc.groups.insert(groupId, group);
    doc.present.groups = true;

    // Layout: the inner nodes and their reroute points move inside, relative to the group's
    // top left, where the instance now sits.
    QJsonObject layout = doc.layout;
    QJsonObject entries = layout.value("nodes").toObject();
    QPointF corner(std::numeric_limits<qreal>::max(), std::numeric_limits<qreal>::max());
    bool placed = false;
    for (const QString &id : order)
        if (const auto pos = pointOf(entries.value(id).toObject().value("pos"))) {
            corner = QPointF(std::min(corner.x(), pos->x()), std::min(corner.y(), pos->y()));
            placed = true;
        }
    if (!placed)
        corner = QPointF();
    QJsonObject innerEntries;
    for (const QString &id : order) {
        QJsonObject entry = entries.take(id).toObject();
        if (const auto pos = pointOf(entry.value("pos")))
            entry.insert("pos", pointJson(*pos - corner));
        if (!entry.isEmpty())
            innerEntries.insert(id, entry);
    }
    if (placed)
        entries.insert(instanceId, QJsonObject{{"pos", pointJson(corner)}});
    setOrRemove(layout, QStringLiteral("nodes"), entries);
    QJsonObject links = layout.value("links").toObject();
    QJsonObject innerLinks;
    for (const QString &key : links.keys()) {
        const LinkKey parsed = parseLinkKey(key);
        if (!inside.contains(parsed.node))
            continue;
        const QJsonObject entry = links.take(key).toObject();
        // Kept when the link stays inside (its input still has it).
        const InputValue kept = group.nodes.value(parsed.node).inputs.value(parsed.input);
        if (!linksIn(kept).isEmpty())
            innerLinks.insert(key, shiftedPoints(entry, -corner));
    }
    setOrRemove(layout, QStringLiteral("links"), links);
    leaveFrames(layout, inside);
    doc.layout = layout;
    QJsonObject inner;
    if (!innerEntries.isEmpty())
        inner.insert("nodes", innerEntries);
    if (!innerLinks.isEmpty())
        inner.insert("links", innerLinks);
    setGroupLayout(doc, groupId, inner);
    if (!doc.layout.isEmpty())
        doc.present.layout = true;

    return GroupMade{doc, instanceId, groupId};
}

Result<Ungrouped> ungroup(const Document &document, const QString &instanceId) {
    Document doc = document;
    const auto found = doc.graph.nodes.constFind(instanceId);
    if (found == doc.graph.nodes.constEnd() || !doc.group(found->type))
        return groupError(QStringLiteral("Node %1 is not a group.").arg(instanceId));
    const Node instance = *found;
    const GroupDefinition group = *doc.group(instance.type);
    doc.graph.nodes.remove(instanceId);

    // New ids: the inner id itself where the level has none like it.
    QHash<QString, QString> ids;
    QStringList added;
    for (auto it = group.nodes.cbegin(); it != group.nodes.cend(); ++it) {
        const QString id = doc.graph.nodes.contains(it.key()) || it.key() == instanceId ? newNodeId(doc.graph.nodes) : it.key();
        doc.graph.nodes.insert(id, Node{});
        ids.insert(it.key(), id);
        added << id;
    }
    for (auto it = group.nodes.cbegin(); it != group.nodes.cend(); ++it) {
        Node node = it.value();
        for (auto in = node.inputs.begin(); in != node.inputs.end(); ++in)
            in.value() = withLinksMapped(in.value(), [&](const PortRef &from) {
                return ids.contains(from.node) ? PortRef{ids.value(from.node), from.port} : from;
            });
        if (instance.mode == NodeMode::Mute)
            node.mode = NodeMode::Mute;
        doc.graph.nodes.insert(ids.value(it.key()), node);
    }
    // What the face gave the inner inputs.
    for (const GroupInput &exposed : group.inputs) {
        InputValue value = instance.inputs.value(exposed.name);
        if (value.isEmpty() && exposed.defaultValue)
            value = *exposed.defaultValue;
        if (value.isEmpty())
            continue;
        for (const PortRef &target : exposed.targets)
            if (ids.contains(target.node))
                doc.graph.nodes[ids.value(target.node)].inputs.insert(target.port, value);
    }
    // Links from the instance's outputs now come from the inner nodes.
    QHash<QString, PortRef> outputs;
    for (const GroupOutput &exposed : group.outputs)
        if (ids.contains(exposed.from.node))
            outputs.insert(exposed.name, PortRef{ids.value(exposed.from.node), exposed.from.port});
    for (auto node = doc.graph.nodes.begin(); node != doc.graph.nodes.end(); ++node) {
        stripLinks(*node, [&](const PortRef &from) { return from.node == instanceId && !outputs.contains(from.port); });
        for (auto in = node->inputs.begin(); in != node->inputs.end(); ++in)
            in.value() = withLinksMapped(in.value(), [&](const PortRef &from) {
                return from.node == instanceId ? outputs.value(from.port) : from;
            });
    }

    // Layout: the inner positions, reroute points and Frames, at the instance's place.
    QJsonObject layout = doc.layout;
    QJsonObject entries = layout.value("nodes").toObject();
    const QPointF base = pointOf(entries.take(instanceId).toObject().value("pos")).value_or(QPointF());
    const QJsonObject inner = groupLayout(document, instance.type);
    const QJsonObject innerEntries = inner.value("nodes").toObject();
    int unplaced = 0;
    for (auto it = group.nodes.cbegin(); it != group.nodes.cend(); ++it) {
        QJsonObject entry = innerEntries.value(it.key()).toObject();
        const QPointF at = pointOf(entry.value("pos")).value_or(QPointF(300 * unplaced++, 0));
        entry.insert("pos", pointJson(base + at));
        entries.insert(ids.value(it.key()), entry);
    }
    setOrRemove(layout, QStringLiteral("nodes"), entries);
    const QJsonObject innerLinks = inner.value("links").toObject();
    if (!innerLinks.isEmpty()) {
        QJsonObject links = layout.value("links").toObject();
        for (auto it = innerLinks.constBegin(); it != innerLinks.constEnd(); ++it) {
            LinkKey key = parseLinkKey(it.key());
            if (!ids.contains(key.node))
                continue;
            key.node = ids.value(key.node);
            links.insert(key.text(), shiftedPoints(it.value().toObject(), base));
        }
        setOrRemove(layout, QStringLiteral("links"), links);
    }
    leaveFrames(layout, {instanceId});
    const QJsonArray innerFrames = inner.value("frames").toArray();
    if (!innerFrames.isEmpty()) {
        QJsonArray frames = layout.value("frames").toArray();
        QSet<QString> frameIds;
        for (const QJsonValue &frame : frames)
            frameIds.insert(frame.toObject().value("id").toString());
        for (const QJsonValue &value : innerFrames) {
            QJsonObject frame = value.toObject();
            QJsonArray rect = frame.value("rect").toArray();
            if (rect.size() == 4) {
                rect[0] = qRound64(rect[0].toDouble() + base.x());
                rect[1] = qRound64(rect[1].toDouble() + base.y());
                frame.insert("rect", rect);
            }
            QJsonArray members;
            for (const QJsonValue &id : frame.value("nodes").toArray())
                if (ids.contains(id.toString()))
                    members.append(ids.value(id.toString()));
            frame.insert("nodes", members);
            QString id = frame.value("id").toString();
            for (int n = 2; id.isEmpty() || frameIds.contains(id); ++n)
                id = QStringLiteral("f%1").arg(n);
            frameIds.insert(id);
            frame.insert("id", id);
            frames.append(frame);
        }
        layout.insert("frames", frames);
    }
    doc.layout = layout;
    return Ungrouped{doc, added, ids};
}

Result<GroupMade> makeUnique(const Document &document, const QString &instanceId, const Registry &registry) {
    Document doc = document;
    const auto found = doc.graph.nodes.find(instanceId);
    if (found == doc.graph.nodes.end() || !doc.group(found->type))
        return groupError(QStringLiteral("Node %1 is not a group.").arg(instanceId));
    const QString from = found->type;
    const QString id = uniqueGroupId(doc, registry, from.section(QLatin1Char('/'), 1));
    doc.groups.insert(id, *doc.group(from));
    found->type = id;
    const QJsonObject inner = groupLayout(doc, from);
    if (!inner.isEmpty())
        setGroupLayout(doc, id, inner);
    return GroupMade{doc, instanceId, id};
}

Result<Document> exposeInput(const Document &document, const QString &groupId, const PortRef &target,
                             const Registry &registry, const QString &into, QString *exposedName) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const auto node = group->nodes.constFind(target.node);
    if (node == group->nodes.constEnd() || !hasPort(doc, registry, *node, target.port, false))
        return groupError(QStringLiteral("Group %1 has no input %2.").arg(groupId, target.toString()));
    for (const GroupInput &exposed : group->inputs)
        if (exposed.targets.contains(target))
            return groupError(QStringLiteral("The input %1 is exposed already, as %2.").arg(target.toString(), exposed.name));
    if (!into.isEmpty()) {
        const auto it = std::find_if(group->inputs.begin(), group->inputs.end(),
                                     [&](const GroupInput &exposed) { return exposed.name == into; });
        if (it == group->inputs.end())
            return groupError(QStringLiteral("Group %1 has no exposed input %2.").arg(groupId, into));
        const PortRef first = it->targets.value(0);
        const std::optional<PortSpec> want = inputPort(doc, registry, group->nodes.value(first.node), first.port);
        const std::optional<PortSpec> have = inputPort(doc, registry, *node, target.port);
        if (!want || !have || want->type != have->type || want->list != have->list) {
            Error error;
            error.code = QStringLiteral("type_mismatch");
            error.message = QStringLiteral("The exposed input %1 drives %2 inputs; %3 is %4.")
                                .arg(into, want ? want->toString() : QStringLiteral("unknown"), target.toString(),
                                     have ? have->toString() : QStringLiteral("of a type not known"));
            error.hint = QStringLiteral("One exposed input drives inner inputs of one type only.");
            return error;
        }
        it->targets << target;
        if (exposedName)
            *exposedName = into;
        return doc;
    }
    QSet<QString> used = inputNames(*group);
    GroupInput exposed;
    exposed.name = freeName(used, target.port);
    exposed.targets << target;
    group->inputs << exposed;
    if (exposedName)
        *exposedName = exposed.name;
    return doc;
}

Result<Document> unexposeInput(const Document &document, const QString &groupId, const PortRef &target) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    for (qsizetype i = 0; i < group->inputs.size(); ++i) {
        GroupInput &exposed = group->inputs[i];
        if (!exposed.targets.removeOne(target))
            continue;
        if (exposed.targets.isEmpty()) {
            const QString name = exposed.name;
            group->inputs.removeAt(i);
            dropFromInstances(doc, groupId, {name}, {});
        }
        return doc;
    }
    return groupError(QStringLiteral("The input %1 is not exposed.").arg(target.toString()));
}

Result<Document> exposeOutput(const Document &document, const QString &groupId, const PortRef &from,
                              QString *exposedName) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const auto node = group->nodes.constFind(from.node);
    // Output ports are checked against the inner node's type only through the registry's
    // knowledge; a node of a type not known may have any output.
    if (node == group->nodes.constEnd())
        return groupError(QStringLiteral("Group %1 has no node %2.").arg(groupId, from.node));
    for (const GroupOutput &exposed : group->outputs)
        if (exposed.from == from)
            return groupError(QStringLiteral("The output %1 is exposed already, as %2.").arg(from.toString(), exposed.name));
    QSet<QString> used = outputNames(*group);
    GroupOutput exposed;
    exposed.name = freeName(used, from.port);
    exposed.from = from;
    group->outputs << exposed;
    if (exposedName)
        *exposedName = exposed.name;
    return doc;
}

Result<Document> unexposeOutput(const Document &document, const QString &groupId, const QString &name) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const qsizetype before = group->outputs.size();
    group->outputs.removeIf([&](const GroupOutput &exposed) { return exposed.name == name; });
    if (group->outputs.size() == before)
        return groupError(QStringLiteral("Group %1 has no exposed output %2.").arg(groupId, name));
    dropFromInstances(doc, groupId, {}, {name});
    return doc;
}

Result<Document> editExposedInput(const Document &document, const QString &groupId, const QString &name,
                                  const ExposedChange &change, const Registry &registry) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const auto it = std::find_if(group->inputs.begin(), group->inputs.end(),
                                 [&](const GroupInput &exposed) { return exposed.name == name; });
    if (it == group->inputs.end())
        return groupError(QStringLiteral("Group %1 has no exposed input %2.").arg(groupId, name));
    const QString newName = change.name.isEmpty() ? name : change.name;
    if (newName != name) {
        if (!isPortName(newName))
            return groupError(QStringLiteral("\"%1\" cannot be an input name.").arg(newName),
                              QStringLiteral("Use lowercase letters, digits and _, starting with a letter."));
        if (inputNames(*group).contains(newName))
            return groupError(QStringLiteral("Group %1 has an input named %2 already.").arg(groupId, newName));
    }
    if (change.min && change.max && *change.min > *change.max)
        return groupError(QStringLiteral("The minimum %1 is above the maximum %2.").arg(*change.min).arg(*change.max));
    if (change.choices) {
        Faces faces{doc, registry, {}};
        const std::optional<InputSpec> inner = it->targets.isEmpty() ? std::nullopt : faces.input(*group, it->targets.first());
        if (!inner || !inner->choices)
            return groupError(QStringLiteral("The input %1 is not a choice, so it has no choices to narrow.").arg(name));
        for (const QString &choice : *change.choices)
            if (!inner->choices->contains(choice))
                return groupError(QStringLiteral("\"%1\" is not one of the choices of %2.").arg(choice, name),
                                  QStringLiteral("Choose among: %1.").arg(inner->choices->join(QStringLiteral(", "))));
    }
    it->label = change.label;
    it->section = change.section.trimmed();
    it->min = change.min;
    it->max = change.max;
    it->choices = change.choices;
    if (change.defaultValue.isUndefined() || change.defaultValue.isNull()) {
        it->defaultValue.reset();
    } else {
        Faces faces{doc, registry, {}};
        if (const std::optional<InputSpec> inner = it->targets.isEmpty() ? std::nullopt : faces.input(*group, it->targets.first())) {
            const InputSpec spec = exposedInputSpec(*inner, *it);
            if (Errors errors = checkLiteral(spec, change.defaultValue, registry.portKind(spec.type.type)); hasErrors(errors))
                return errors;
        }
        it->defaultValue = InputValue::literal(change.defaultValue);
    }
    it->name = newName;
    if (newName != name)
        renameInstanceInputs(doc, groupId, name, newName);
    return doc;
}

Result<Document> renameExposedOutput(const Document &document, const QString &groupId, const QString &name,
                                     const QString &newName) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const auto it = std::find_if(group->outputs.begin(), group->outputs.end(),
                                 [&](const GroupOutput &exposed) { return exposed.name == name; });
    if (it == group->outputs.end())
        return groupError(QStringLiteral("Group %1 has no exposed output %2.").arg(groupId, name));
    if (newName == name)
        return doc;
    if (!isPortName(newName))
        return groupError(QStringLiteral("\"%1\" cannot be an output name.").arg(newName),
                          QStringLiteral("Use lowercase letters, digits and _, starting with a letter."));
    if (outputNames(*group).contains(newName))
        return groupError(QStringLiteral("Group %1 has an output named %2 already.").arg(groupId, newName));
    it->name = newName;
    // An outer group's face that carries the output out reads it under the new name.
    for (GroupDefinition &outer : doc.groups)
        for (GroupOutput &exposed : outer.outputs)
            if (instancesIn(outer.nodes, groupId).contains(exposed.from.node) && exposed.from.port == name)
                exposed.from.port = newName;
    forEachNodes(doc, [&](Nodes &nodes) {
        const QSet<QString> instances = instancesIn(nodes, groupId);
        if (instances.isEmpty())
            return;
        for (Node &node : nodes)
            for (auto in = node.inputs.begin(); in != node.inputs.end(); ++in)
                in.value() = withLinksMapped(in.value(), [&](const PortRef &from) {
                    return instances.contains(from.node) && from.port == name ? PortRef{from.node, newName} : from;
                });
    });
    return doc;
}

Result<Document> renameGroup(const Document &document, const QString &groupId, const QString &name,
                             const Registry &registry, QString *newId) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    const QString shown = name.trimmed();
    if (shown.isEmpty())
        return groupError(QStringLiteral("A group needs a name."));
    group->name = shown;
    QString id = groupId;
    if (groupId.startsWith(QLatin1String("local/"))) {
        Document without = doc;
        without.groups.remove(groupId);
        const QString wanted = uniqueGroupId(without, registry, shown);
        if (wanted != groupId && !registry.nodeType(wanted))
            id = wanted;
    }
    if (id != groupId)
        doc = renameGroupId(doc, groupId, id);
    if (newId)
        *newId = id;
    return doc;
}

Document renameGroupId(const Document &document, const QString &from, const QString &to) {
    Document doc = document;
    if (from == to || !doc.groups.contains(from) || doc.groups.contains(to))
        return doc;
    doc.groups.insert(to, doc.groups.take(from));
    forEachNodes(doc, [&](Nodes &nodes) {
        for (Node &node : nodes)
            if (node.type == from)
                node.type = to;
    });
    const QJsonObject inner = groupLayout(doc, from);
    setGroupLayout(doc, from, {});
    if (!inner.isEmpty())
        setGroupLayout(doc, to, inner);
    return doc;
}

Result<Document> moveExposed(const Document &document, const QString &groupId, bool output, const QString &name, int to) {
    Document doc = document;
    GroupDefinition *group = groupOf(doc, groupId);
    if (!group)
        return groupError(QStringLiteral("There is no group %1.").arg(groupId));
    auto move = [&](auto &list) -> bool {
        const auto it = std::find_if(list.begin(), list.end(), [&](const auto &entry) { return entry.name == name; });
        if (it == list.end())
            return false;
        list.move(it - list.begin(), std::clamp<qsizetype>(to, 0, list.size() - 1));
        return true;
    };
    if (!(output ? move(group->outputs) : move(group->inputs)))
        return groupError(QStringLiteral("Group %1 has no exposed %2 %3.")
                              .arg(groupId, output ? QStringLiteral("output") : QStringLiteral("input"), name));
    return doc;
}

bool pruneGroup(Document &document, const QString &groupId, const Registry &registry) {
    GroupDefinition *group = groupOf(document, groupId);
    if (!group)
        return false;
    bool changed = false;
    QSet<QString> goneInputs, goneOutputs;
    for (qsizetype i = group->inputs.size() - 1; i >= 0; --i) {
        GroupInput &exposed = group->inputs[i];
        const qsizetype before = exposed.targets.size();
        exposed.targets.removeIf([&](const PortRef &target) {
            const auto node = group->nodes.constFind(target.node);
            return node == group->nodes.constEnd() || !hasPort(document, registry, *node, target.port, false);
        });
        changed |= exposed.targets.size() != before;
        if (exposed.targets.isEmpty()) {
            goneInputs.insert(exposed.name);
            group->inputs.removeAt(i);
            changed = true;
        }
    }
    for (qsizetype i = group->outputs.size() - 1; i >= 0; --i) {
        const GroupOutput &exposed = group->outputs[i];
        const auto node = group->nodes.constFind(exposed.from.node);
        if (node == group->nodes.constEnd() || !hasPort(document, registry, *node, exposed.from.port, true)) {
            goneOutputs.insert(exposed.name);
            group->outputs.removeAt(i);
            changed = true;
        }
    }
    if (!goneInputs.isEmpty() || !goneOutputs.isEmpty())
        dropFromInstances(document, groupId, goneInputs, goneOutputs);
    return changed;
}

void dropExposedPorts(Document &document, const QString &group, const QSet<QString> &inputs, const QSet<QString> &outputs) {
    dropFromInstances(document, group, inputs, outputs);
}

void dropUnusedGroups(Document &document, QStringList candidates) {
    QSet<QString> used;
    QStringList todo;
    for (const Node &node : document.graph.nodes)
        todo << node.type;
    while (!todo.isEmpty()) {
        const QString id = todo.takeLast();
        const GroupDefinition *group = document.group(id);
        if (!group || used.contains(id))
            continue;
        used.insert(id);
        for (const Node &node : group->nodes)
            todo << node.type;
    }
    QSet<QString> seen;
    while (!candidates.isEmpty()) {
        const QString id = candidates.takeLast();
        const GroupDefinition *group = document.group(id);
        if (!group || used.contains(id) || seen.contains(id))
            continue;
        seen.insert(id);
        // What only it used may be unused now too.
        for (const Node &node : group->nodes)
            if (document.group(node.type))
                candidates << node.type;
        document.groups.remove(id);
        setGroupLayout(document, id, {});
    }
    // None left: no empty section either, so grouping and ungrouping leaves the file as it was.
    if (document.groups.isEmpty())
        document.present.groups = false;
}

} // namespace fugue
