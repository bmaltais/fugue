#include "engine/remote_placement.h"

#include <algorithm>

#include <QJsonArray>
#include <QMap>
#include <QSet>

namespace fugue {

namespace {

// The hashes of the handles anywhere inside `value` (a derived handle nests its parts).
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

void collectLinks(const InputValue &value, QList<PortRef> &nodes) {
    if (value.kind() == InputValue::Kind::Link) {
        nodes.append(value.from());
    } else if (value.kind() == InputValue::Kind::List) {
        for (const InputValue &item : value.items())
            collectLinks(item, nodes);
    }
}

} // namespace

Errors remoteRunProblems(const Document &document, const ReadinessPlan &plan, const RemotePlacement &placement) {
    QList<ProviderInstance> helpers;
    for (const ProviderInstance &instance : placement.instances)
        if (instance.enabled && isPairedHelper(instance))
            helpers.append(instance);
    if (helpers.isEmpty() || !placement.capabilityOf || !placement.registry)
        return {};
    QMap<QString, QString> missing; // hash -> the model's name
    for (const ReadinessModel &model : plan.models)
        if (const QString hash = model.entry.value("hash").toString(); !hash.isEmpty())
            missing.insert(hash, model.name);
    for (const QJsonObject &handle : plan.unobtainable)
        if (const QString hash = handle.value("hash").toString(); !hash.isEmpty())
            missing.insert(hash, handle.value("name").toString());
    if (missing.isEmpty())
        return {};

    const Registry &registry = *placement.registry;
    const Nodes &nodes = document.graph.nodes;
    const auto typeOf = [&](const Node &node) { return registry.nodeTypeFor(node.type, node.version.major); };
    const auto isResourceInput = [&](const Node &node, const QString &input) {
        const NodeType *type = typeOf(node);
        const InputSpec *spec = type ? type->input(input) : nullptr;
        return spec && registry.portKind(spec->type.type) == PortKind::Resource;
    };
    // A model reaches a node by its own Resource inputs: a handle written there, or a link from a node that holds one, through that
    // node's Resource output. Nothing is carried through a Data port (an encoded conditioning is not the model).
    QMap<QString, QStringList> reaching; // node id -> model names
    const auto add = [&](const QString &node, const QStringList &names) {
        QStringList &have = reaching[node];
        bool added = false;
        for (const QString &name : names)
            if (!have.contains(name)) {
                have.append(name);
                added = true;
            }
        return added;
    };
    for (auto node = nodes.cbegin(); node != nodes.cend(); ++node)
        for (auto input = node->inputs.cbegin(); input != node->inputs.cend(); ++input) {
            if (!isResourceInput(*node, input.key()))
                continue;
            QSet<QString> held;
            collectHashes(input->toJson(), held);
            for (const QString &hash : std::as_const(held))
                if (missing.contains(hash))
                    add(node.key(), {missing.value(hash)});
        }
    bool changed = !reaching.isEmpty();
    while (changed) {
        changed = false;
        for (auto node = nodes.cbegin(); node != nodes.cend(); ++node)
            for (auto input = node->inputs.cbegin(); input != node->inputs.cend(); ++input) {
                if (!isResourceInput(*node, input.key()))
                    continue;
                QList<PortRef> sources;
                collectLinks(*input, sources);
                for (const PortRef &source : std::as_const(sources)) {
                    const auto from = nodes.constFind(source.node);
                    if (from == nodes.cend() || !reaching.contains(source.node))
                        continue;
                    const NodeType *type = typeOf(*from);
                    const OutputSpec *out = type ? type->output(source.port) : nullptr;
                    if (out && registry.portKind(out->type.type) == PortKind::Resource && add(node.key(), reaching.value(source.node)))
                        changed = true;
                }
            }
    }

    Errors problems;
    for (auto node = nodes.cbegin(); node != nodes.cend(); ++node) {
        if (!reaching.contains(node.key()))
            continue;
        const QString capability = placement.capabilityOf(node->type);
        if (capability.isEmpty())
            continue; // not a provider node: it reads the file on this machine itself, which is not this check's
        const QString models = reaching.value(node.key()).join(QStringLiteral(", "));
        const QString pin = node->provider.isEmpty() ? document.graph.provider : node->provider;
        const auto pinned = std::find_if(placement.instances.cbegin(), placement.instances.cend(),
                                         [&](const ProviderInstance &instance) { return instance.name == pin; });
        QString why;
        if (!pin.isEmpty() && pinned != placement.instances.cend() && !isPairedHelper(*pinned)) {
            why = QStringLiteral("Node %1 is pinned to %2, which runs on this machine, and the model %3 it needs is not here.")
                      .arg(node.key(), pin, models);
        } else {
            // The helper it is pinned to, else any of them: refused only when every one that could take it is known not to offer it.
            QStringList names;
            bool refused = true;
            for (const ProviderInstance &helper : std::as_const(helpers)) {
                if (!pin.isEmpty() && pinned != placement.instances.cend() && helper.name != pin)
                    continue;
                names << helper.name;
                const auto offered = placement.hostCapabilities.constFind(helper.name);
                if (offered == placement.hostCapabilities.cend() || offered->contains(capability))
                    refused = false;
            }
            if (refused && !names.isEmpty())
                why = QStringLiteral("Node %1 needs the model %2, which is not here, and the paired host %3 does not offer %4.")
                          .arg(node.key(), models, names.join(QStringLiteral(", ")), capability);
        }
        if (why.isEmpty())
            continue;
        Error error = makeError(QStringLiteral("model_missing"), why,
                                QStringLiteral("Get the model onto this machine (Get ready), or unpin the node."));
        error.node = node.key();
        problems.append(error);
    }
    return problems;
}

} // namespace fugue
