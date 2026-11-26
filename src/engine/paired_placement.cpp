#include "engine/paired_placement.h"

#include "engine/compiler.h"
#include "engine/group_library.h"

#include <algorithm>
#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <chrono>

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

// A node that holds a missing model itself (through its own Resource inputs) and asks a provider for something.
struct Need {
    QString node; // its path id
    QString models; // the names, comma separated
    Requirement requirement; // as the scheduler asks for it, with the node's pin (the workflow's merged in by the compile)
    QString capability() const { return requirement.capability; }
    QString pin() const { return requirement.nodePin; }
};

// The inputs a node holds before anything runs, as the scheduler's resolveInputs would give them for what is known: literals, handles
// and lists of them, else the spec's default. A link's value and a random seed are not known yet, so they are left out, as under validate.
Values knownInputs(const NodeType &type, const CompiledNode &node) {
    Values values;
    for (const InputSpec &spec : type.inputs) {
        const InputValue written = node.inputs.value(spec.name);
        Value value;
        if (written.isEmpty()) {
            if (!spec.defaultValue.isUndefined())
                value = Value(spec.defaultValue);
        } else if (written.kind() == InputValue::Kind::Literal || written.kind() == InputValue::Kind::Resource) {
            value = Value(written.toJson());
        } else if (written.kind() == InputValue::Kind::List) {
            QList<Value> items;
            for (const InputValue &item : written.items())
                if (item.kind() == InputValue::Kind::Literal || item.kind() == InputValue::Kind::Resource)
                    items.append(Value(item.toJson()));
            if (items.size() == written.items().size())
                value = Value::list(items);
        }
        if (!value.isEmpty())
            values.insert(spec.name, value);
    }
    return values;
}

QList<Need> nodesNeedingModels(const Document &document, const ReadinessPlan &plan, const PairedPlacementRequest &request) {
    if (!request.requirementOf || !request.registry)
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

    const Registry &registry = *request.registry;
    // The nodes the scheduler runs: groups expanded, pins merged, muted nodes out. A document that does not compile runs nothing (an input left unset is no error here: the run's values fill it).
    // A library or pack group named only by its type id runs from its definition, as in compileForRun (5.3, 6.1).
    Document embedded = document;
    embedGroupTypes(embedded, registry);
    // An input the app's own values fill at the run (a required target left for `--input`) is not a reason to place nothing (#587).
    CompileOptions options;
    options.allowUnsetInputs = true;
    const Compilation compiled = compile(embedded, registry, options);
    if (!compiled)
        return {};
    const QMap<QString, CompiledNode> &nodes = compiled->nodes;
    const auto typeOf = [&](const CompiledNode &node) { return registry.nodeTypeFor(node.type, node.version.major); };
    const auto isResourceInput = [&](const CompiledNode &node, const QString &input) {
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

    QList<Need> found;
    for (auto node = nodes.cbegin(); node != nodes.cend(); ++node) {
        if (!reaching.contains(node.key()))
            continue;
        const NodeType *type = typeOf(*node);
        std::optional<Requirement> requirement = type ? request.requirementOf(*type, knownInputs(*type, *node)) : std::nullopt;
        if (!requirement || requirement->capability.isEmpty())
            continue; // not a provider node: it reads the file on this machine itself, which is not this check's
        requirement->nodePin = node->provider;
        requirement->workflowPin.clear();
        // The scheduler's rule (#395): a node that names its model is not placed by where the files are.
        requirement->modelsElsewhere = requirement->model.isEmpty();
        found.append({node.key(), reaching.value(node.key()).join(QStringLiteral(", ")), *requirement});
    }
    return found;
}

Error modelMissing(const QString &node, const QString &why) {
    Error error = makeError(QStringLiteral("model_missing"), why, QStringLiteral("Get the model onto this machine (Get ready), or unpin the node."));
    error.node = node;
    return error;
}

QString capabilityWhy(const Need &need, const QStringList &hosts) {
    return QStringLiteral("Node %1 needs the model %2, which is not here, and the paired host %3 does not offer %4.")
        .arg(need.node, need.models, hosts.join(QStringLiteral(", ")), need.capability());
}

// The hosts one node may go to, and the hosts of those that a pin and what each last said leave.
struct Reach {
    Need need;
    QStringList candidates; // the helpers its pin allows, in the user's order
    QMap<QString, ProviderInstance> instances; // by name
};

} // namespace

bool onlyModelsMissing(const ReadinessPlan &plan) {
    if (!plan.packs.isEmpty() || (plan.models.isEmpty() && plan.problems.isEmpty()))
        return false;
    return std::all_of(plan.problems.cbegin(), plan.problems.cend(), [](const Error &problem) {
        return problem.code == QLatin1String("download_failed") || problem.code == QLatin1String("model_missing");
    });
}

std::optional<Requirement> kernelRequirement(const Kernels &kernels, const NodeType &type, const Values &inputs) {
    const std::shared_ptr<ProviderKernel> kernel = kernels.provider(type);
    if (!kernel)
        return std::nullopt;
    return kernel->requirement(type, inputs);
}

QStringList PairedVerdict::hosts() const {
    QStringList distinct;
    for (const QString &host : hostOf)
        if (!distinct.contains(host))
            distinct << host;
    distinct.sort();
    return distinct;
}

QByteArray pairedPlacementKey(const Document &document, const ReadinessPlan &plan, const QList<ProviderInstance> &instances) {
    QStringList parts;
    parts << QStringLiteral("packs=%1").arg(plan.packs.size());
    QStringList codes, hashes;
    for (const Error &problem : plan.problems)
        codes << problem.code;
    for (const ReadinessModel &model : plan.models)
        hashes << model.entry.value("hash").toString() + QLatin1Char(':') + model.name;
    for (const QJsonObject &handle : plan.unobtainable)
        hashes << handle.value("hash").toString() + QLatin1Char(':') + handle.value("name").toString();
    codes.sort();
    hashes.sort();
    parts << codes.join(QLatin1Char(',')) << hashes.join(QLatin1Char(',')) << document.graph.provider;
    parts << document.sourceHash(); // the graph and its groups: every node (type, pin, inputs, mode) and the workflow's pin
    for (const ProviderInstance &instance : instances)
        parts << QStringList{instance.name, instance.type, instance.address, instance.enabled ? "1" : "0"}.join(QLatin1Char('|'));
    return parts.join(QLatin1Char('\n')).toUtf8();
}

ReadinessPlan planUnder(ReadinessPlan plan, const PairedVerdict &verdict) {
    if (verdict.runsOn()) {
        const QString hosts = verdict.hosts().join(QStringLiteral(", "));
        for (ReadinessModel &model : plan.models)
            model.onHost = hosts;
    } else {
        plan.problems += verdict.blocked;
    }
    return plan;
}

PairedProbe livePairedProbe() {
    return [](const QList<ProviderInstance> &helpers, std::function<void(const QMap<QString, Description> &)> done) {
        probePairedHelpers(helpers, std::move(done));
    };
}

PairedProbe keptPairedProbe(PairedProbe inner, std::chrono::milliseconds ttl) {
    using Clock = std::chrono::steady_clock;
    using Done = std::function<void(const QMap<QString, Description> &)>;
    struct State {
        QMap<QString, std::pair<Description, Clock::time_point>> kept; // by name and address
        QMap<QString, QList<Done>> out;                                // a question being asked, by its helpers
    };
    const auto state = std::make_shared<State>();
    return [inner = std::move(inner), ttl, state](const QList<ProviderInstance> &helpers, Done done) {
        const auto keyOf = [](const ProviderInstance &helper) { return helper.name + QLatin1Char('@') + helper.address; };
        QStringList keys;
        for (const ProviderInstance &helper : helpers)
            keys << keyOf(helper);
        const QString question = keys.join(QLatin1Char('|'));
        QMap<QString, Description> fresh;
        for (const ProviderInstance &helper : helpers) {
            const auto it = state->kept.constFind(keyOf(helper));
            if (it != state->kept.cend() && Clock::now() - it->second < ttl)
                fresh.insert(helper.name, it->first);
        }
        if (fresh.size() == helpers.size()) {
            done(fresh);
            return;
        }
        if (state->out.contains(question)) {
            state->out[question].append(std::move(done));
            return;
        }
        state->out[question].append(std::move(done));
        inner(helpers, [state, helpers, question, keyOf](const QMap<QString, Description> &answered) {
            for (const ProviderInstance &helper : helpers)
                if (answered.contains(helper.name))
                    state->kept.insert(keyOf(helper), {answered.value(helper.name), Clock::now()});
            const QList<Done> waiting = state->out.take(question);
            for (const Done &wait : waiting)
                wait(answered);
        });
    };
}

void decidePairedPlacement(const Document &document, const ReadinessPlan &plan, const PairedPlacementRequest &request,
                           const PairedProbe &probe, std::function<void(const PairedVerdict &verdict)> done) {
    PairedVerdict verdict;
    QList<ProviderInstance> helpers;
    for (const ProviderInstance &instance : request.instances)
        if (instance.enabled && isPairedHelper(instance))
            helpers.append(instance);
    if (helpers.isEmpty() || !onlyModelsMissing(plan)) {
        done(verdict);
        return;
    }
    verdict.applies = true;
    const QList<Need> needs = nodesNeedingModels(document, plan, request);
    if (needs.isEmpty()) {
        done(verdict);
        return;
    }
    const auto instanceNamed = [&request](const QString &name) -> const ProviderInstance * {
        const auto found = std::find_if(request.instances.cbegin(), request.instances.cend(),
                                        [&name](const ProviderInstance &instance) { return instance.name == name; });
        return found == request.instances.cend() ? nullptr : &*found;
    };
    QMap<QString, std::optional<Description>> remembered; // what each host last said, read once
    for (const ProviderInstance &helper : std::as_const(helpers))
        remembered.insert(helper.name, request.remembered ? request.remembered(helper) : rememberedPairedHelper(helper));

    // First, what needs no host: the pins. What a host last said is not used here: the probe refreshes it, so a host that gained a
    // capability is not refused for good.
    QList<Reach> reaches;
    for (const Need &need : needs) {
        Reach reach{need, {}, {}};
        const ProviderInstance *pinned = need.pin().isEmpty() ? nullptr : instanceNamed(need.pin());
        if (!need.pin().isEmpty() && !pinned) {
            verdict.blocked.append(modelMissing(need.node, QStringLiteral("Node %1 is pinned to %2, but no provider is named %2, and the model %3 it needs is not here.")
                                                               .arg(need.node, need.pin(), need.models)));
            continue;
        }
        if (pinned && !isPairedHelper(*pinned)) {
            verdict.blocked.append(modelMissing(need.node, QStringLiteral("Node %1 is pinned to %2, which runs on this machine, and the model %3 it needs is not here.")
                                                               .arg(need.node, need.pin(), need.models)));
            continue;
        }
        if (pinned && !pinned->enabled) {
            verdict.blocked.append(modelMissing(need.node, QStringLiteral("Node %1 is pinned to %2, which is disabled, and the model %3 it needs is not here.")
                                                               .arg(need.node, need.pin(), need.models)));
            continue;
        }
        for (const ProviderInstance &helper : std::as_const(helpers))
            if (!pinned || helper.name == pinned->name)
                reach.candidates << helper.name;
        for (const ProviderInstance &helper : std::as_const(helpers))
            if (reach.candidates.contains(helper.name))
                reach.instances.insert(helper.name, helper);
        if (reach.candidates.isEmpty())
            verdict.blocked.append(modelMissing(need.node, capabilityWhy(need, reach.candidates)));
        else
            reaches.append(reach);
    }
    if (!verdict.blocked.isEmpty() || reaches.isEmpty()) {
        done(verdict);
        return;
    }

    // Then the hosts that could still take a node are asked what they offer now (one may have gained a capability since it last
    // answered: a refusal here never reaches the scheduler, which is what would update what is kept).
    QList<ProviderInstance> toAsk;
    for (const ProviderInstance &helper : std::as_const(helpers))
        if (std::any_of(reaches.cbegin(), reaches.cend(), [&helper](const Reach &reach) { return reach.candidates.contains(helper.name); }))
            toAsk.append(helper);
    probe(toAsk, [verdict, reaches, remembered, done = std::move(done)](const QMap<QString, Description> &answered) mutable {
        for (const Reach &reach : std::as_const(reaches)) {
            // The scheduler's own choice among the hosts the node may go to, on what they said just now: a host that did not answer
            // has the error of its describe, and what it last said (which only refuses, never places).
            QList<Candidate> candidates;
            for (const QString &name : reach.candidates) {
                Candidate candidate;
                candidate.instance = reach.instances.value(name);
                candidate.description = answered.contains(name)
                                            ? Result<Description>(answered.value(name))
                                            : Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("It did not answer.")));
                candidate.remembered = remembered.value(name);
                candidates.append(candidate);
            }
            const Result<Selection> selection = selectProvider(candidates, reach.need.requirement);
            if (selection && !selection->sharesFilesystem) {
                verdict.hostOf.insert(reach.need.node, selection->instance); // a host on this machine's files is not a place for the model files
                continue;
            }
            const bool silent = !selection && selection.errors().first().code == QLatin1String("provider_unreachable");
            if (silent) {
                const QString pin = reach.need.pin();
                const QList<const Candidate *> waiting = silentPairedHelpers(candidates, reach.need.capability()).couldServe;
                const QString name = !pin.isEmpty() ? pin : waiting.isEmpty() ? QString() : waiting.first()->instance.name;
                if (!name.isEmpty()) {
                    if (!verdict.silent.contains(name))
                        verdict.silent << name;
                    continue;
                }
            }
            const QString why = selection && selection->sharesFilesystem
                                    ? QStringLiteral("the host %1 reads this machine's files, which do not have the model.").arg(selection->instance)
                                    : (!selection && selection.errors().first().code != QLatin1String("capability_unavailable")
                                           ? selection.errors().first().message
                                           : QString());
            verdict.blocked.append(modelMissing(reach.need.node, why.isEmpty() ? capabilityWhy(reach.need, reach.candidates)
                                                                              : QStringLiteral("Node %1 needs the model %2, which is not here. %3")
                                                                                    .arg(reach.need.node, reach.need.models, why)));
        }
        done(verdict);
    });
}

} // namespace fugue
