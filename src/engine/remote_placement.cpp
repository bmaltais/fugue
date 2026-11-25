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

void collectLinks(const InputValue &value, QStringList &nodes) {
    if (value.kind() == InputValue::Kind::Link) {
        nodes.append(value.from().node);
    } else if (value.kind() == InputValue::Kind::List) {
        for (const InputValue &item : value.items())
            collectLinks(item, nodes);
    }
}

} // namespace

Errors remoteRunProblems(const Document &document, const ReadinessPlan &plan, const RemotePlacement &placement) {
    const QString host = pairedHelperName(placement.instances);
    if (host.isEmpty() || !placement.capabilityOf)
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

    // The nodes that hold a missing model's handle, then everything that reads from them, with the models that reach each node.
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
    const Nodes &nodes = document.graph.nodes;
    for (auto node = nodes.cbegin(); node != nodes.cend(); ++node) {
        QSet<QString> held;
        for (const InputValue &value : node->inputs)
            collectHashes(value.toJson(), held);
        for (const QString &hash : std::as_const(held))
            if (missing.contains(hash))
                add(node.key(), {missing.value(hash)});
    }
    bool changed = !reaching.isEmpty();
    while (changed) { // links point upstream: a node takes what its sources have
        changed = false;
        for (auto node = nodes.cbegin(); node != nodes.cend(); ++node) {
            QStringList sources;
            for (const InputValue &value : node->inputs)
                collectLinks(value, sources);
            for (const QString &source : std::as_const(sources))
                if (reaching.contains(source) && add(node.key(), reaching.value(source)))
                    changed = true;
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
        if (!pin.isEmpty() && pinned != placement.instances.cend() && !isPairedHelper(*pinned))
            why = QStringLiteral("Node %1 is pinned to %2, which runs on this machine, and the model %3 it needs is not here.")
                      .arg(node.key(), pin, models);
        else if (placement.hostCapabilities && !placement.hostCapabilities->contains(capability))
            why = QStringLiteral("Node %1 needs the model %2, which is not here, and the paired host %3 does not offer %4.")
                      .arg(node.key(), models, host, capability);
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
