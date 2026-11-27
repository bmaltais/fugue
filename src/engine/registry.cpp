#include "registry.h"

#include "engine/ids.h"
#include "engine/version.h"

#include <algorithm>

namespace fugue {

namespace {

Error problem(const QString &message, Error::Severity severity = Error::Severity::Error) {
    Error error = makeError(QStringLiteral("invalid_document"), message);
    error.severity = severity;
    return error;
}

} // namespace

Errors Registry::add(const DeclaredPortType &type) {
    const QString name = type.type.toString();
    if (type.type.isBuiltin() || type.type.isAny() || !isTypeId(type.type.id))
        return {problem(QStringLiteral("%1 cannot be declared; a declared type is \"<namespace>/<name>\".").arg(name))};
    if (type.kind != PortKind::Resource && type.kind != PortKind::Data)
        return {problem(QStringLiteral("Declared type %1 must be of kind resource or data.").arg(name))};
    if (m_portTypes.count(name))
        return {problem(QStringLiteral("Port type %1 is already registered.").arg(name))};
    m_portTypes.emplace(name, type);
    return {};
}

Errors Registry::add(const NodeType &type) {
    const Result<NodeType> valid = NodeType::fromJson(type.toJson());
    Errors errors = valid.problems();
    if (!valid)
        return errors;

    const auto fail = [&](const QString &message, const QString &input = {}, const QString &output = {}) {
        Error error = problem(type.id + QStringLiteral(": ") + message);
        error.input = input;
        error.output = output;
        errors << error;
    };

    // The exact id, not an alias of another type (an alias answers only when no type has the id).
    if (const auto majors = m_nodeTypes.find(type.id); majors != m_nodeTypes.end())
        if (const auto same = majors->second.find(type.version.major); same != majors->second.end())
            fail(QStringLiteral("a node type with this id is already registered at major version %1 (%2).")
                     .arg(type.version.major)
                     .arg(same->second.version.toString()));

    const bool builtin = typeNamespace(type.id) == QLatin1String("fugue");
    // The declared inputs and the items of each repeating input (6.1.2), which every row's inputs are made from.
    QList<InputSpec> declared = type.inputs;
    for (const RepeatSpec &repeat : type.repeats)
        declared += repeat.items;
    for (const InputSpec &input : std::as_const(declared)) {
        if (input.type.type.isAny() && !builtin)
            fail(QStringLiteral("input %1 is of type \"any\", which only built-in fugue/ nodes may use.")
                     .arg(input.name),
                 input.name);
        else if (portKind(input.type.type) == PortKind::Unknown)
            fail(QStringLiteral("input %1 has the port type %2, which no pack declares.")
                     .arg(input.name, input.type.type.toString()),
                 input.name);
    }
    for (const InputSpec &input : std::as_const(declared))
        if (input.type.takesList && !builtin)
            fail(QStringLiteral("input %1 has \"takes_list\", which only built-in fugue/ nodes may use.").arg(input.name), input.name);
    for (const OutputSpec &output : type.outputs) {
        if (output.type.type.isAny()) {
            if (!builtin)
                fail(QStringLiteral("output %1 is of type \"any\", which only built-in fugue/ nodes may use.")
                         .arg(output.name),
                     {}, output.name);
            for (const QString &name : output.follows) {
                const InputSpec *followed = type.input(name);
                if (!followed || !followed->type.type.isAny())
                    fail(QStringLiteral("output %1 follows %2, which is not an input of type \"any\".")
                             .arg(output.name, name),
                         {}, output.name);
            }
        }
    }
    for (const OutputSpec &output : type.outputs)
        if (portKind(output.type.type) == PortKind::Unknown)
            fail(QStringLiteral("output %1 has the port type %2, which no pack declares.")
                     .arg(output.name, output.type.type.toString()),
                 {}, output.name);

    if (hasErrors(errors))
        return errors;

    for (const QString &alias : type.aliases) {
        const auto owner = m_aliases.find(alias);
        if (owner != m_aliases.end() && owner->second == type.id)
            continue; // another major of the type that already has it
        if (owner != m_aliases.end())
            errors << problem(QStringLiteral("%1: the alias %2 is already claimed by %3, which keeps it.")
                                  .arg(type.id, alias, owner->second),
                              Error::Severity::Warning);
        else
            m_aliases.emplace(alias, type.id);
    }
    m_nodeTypes[type.id].emplace(type.version.major, type);
    return errors;
}

Errors Registry::addManifest(const QJsonObject &manifest) {
    const Result<NodeType> type = NodeType::fromJson(manifest);
    if (!type)
        return type.errors();
    return add(*type);
}

Errors Registry::setProviders(const QList<ProviderEntry> &providers) {
    Errors errors;
    QStringList names;
    for (const ProviderEntry &entry : providers) {
        if (entry.instance.name.isEmpty())
            errors << problem(QStringLiteral("A provider instance has no name."));
        else if (names.contains(entry.instance.name))
            errors << problem(QStringLiteral("Two providers are named %1.").arg(entry.instance.name));
        names << entry.instance.name;
    }
    if (errors.isEmpty())
        m_providers = providers;
    return errors;
}

QStringList Registry::providerNames() const {
    QStringList names;
    for (const ProviderEntry &entry : m_providers)
        names << entry.instance.name;
    return names;
}

void Registry::setNodeDir(const QString &typeId, const QString &dir, const QString &seal) {
    const auto found = m_nodeTypes.find(typeId);
    if (found != m_nodeTypes.end()) {
        NodeType &newest = found->second.rbegin()->second;
        newest.dir = dir;
        newest.seal = seal;
    }
}

void Registry::setInputChoices(const QString &typeId, const QString &input, const QStringList &choices,
                                const QString &description) {
    const auto found = m_nodeTypes.find(typeId);
    if (found == m_nodeTypes.end() || choices.isEmpty())
        return;
    for (InputSpec &spec : found->second.rbegin()->second.inputs)
        if (spec.name == input && spec.choices) {
            spec.choices = choices;
            if (!description.isEmpty())
                spec.description = description;
        }
}

void Registry::setPackVersion(const QString &packId, const QString &version) {
    if (version.isEmpty()) { // no installed version: forget the pack
        m_packVersions.remove(packId);
        return;
    }
    QStringList &versions = m_packVersions[packId];
    if (versions.contains(version))
        return;
    versions << version;
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
        const auto x = Version::parse(a), y = Version::parse(b);
        return x && y ? *x < *y : a < b;
    });
}

QString Registry::packVersion(const QString &packId) const {
    const QStringList versions = m_packVersions.value(packId);
    return versions.isEmpty() ? QString() : versions.last();
}

bool Registry::hasPackMajor(const QString &packId, int major) const {
    for (const QString &version : m_packVersions.value(packId))
        if (const auto parsed = Version::parse(version); parsed && parsed->major == major)
            return true;
    return false;
}

namespace {

// The majors of an id, resolved through aliases (an id that is an alias of a type is that
// type's): null when nothing is installed under it.
const std::map<int, NodeType> *majorsOf(const std::map<QString, std::map<int, NodeType>> &types,
                                        const std::map<QString, QString> &aliases, const QString &id) {
    auto found = types.find(id);
    if (found == types.end()) {
        const auto alias = aliases.find(id);
        if (alias == aliases.end())
            return nullptr;
        found = types.find(alias->second);
    }
    return found == types.end() ? nullptr : &found->second;
}

} // namespace

const NodeType *Registry::nodeType(const QString &id) const {
    const auto *majors = majorsOf(m_nodeTypes, m_aliases, id);
    return majors ? &majors->rbegin()->second : nullptr;
}

const NodeType *Registry::nodeTypeAt(const QString &id, int major) const {
    const auto *majors = majorsOf(m_nodeTypes, m_aliases, id);
    if (!majors)
        return nullptr;
    const auto found = majors->find(major);
    return found == majors->end() ? nullptr : &found->second;
}

const NodeType *Registry::nodeTypeFor(const QString &id, int major) const {
    const NodeType *exact = nodeTypeAt(id, major);
    return exact ? exact : nodeType(id);
}

QList<int> Registry::majors(const QString &id) const {
    QList<int> list;
    if (const auto *majors = majorsOf(m_nodeTypes, m_aliases, id))
        for (const auto &entry : *majors)
            list << entry.first;
    return list;
}

QList<const NodeType *> Registry::nodeTypes() const {
    QList<const NodeType *> list;
    for (const auto &entry : m_nodeTypes)
        list << &entry.second.rbegin()->second;
    return list;
}

QList<const NodeType *> Registry::allNodeTypes() const {
    QList<const NodeType *> list;
    for (const auto &entry : m_nodeTypes)
        for (const auto &major : entry.second)
            list << &major.second;
    return list;
}

PortKind Registry::portKind(const PortType &type) const {
    if (type.isBuiltin())
        return PortKind::Builtin;
    if (type.isAny())
        return PortKind::Any;
    const DeclaredPortType *declared = portType(type);
    return declared ? declared->kind : PortKind::Unknown;
}

QList<const DeclaredPortType *> Registry::portTypes() const {
    QList<const DeclaredPortType *> out;
    for (const auto &[key, type] : m_portTypes)
        out << &type;
    return out;
}

const DeclaredPortType *Registry::portType(const PortType &type) const {
    const auto found = m_portTypes.find(type.toString());
    return found == m_portTypes.end() ? nullptr : &found->second;
}

void Registry::replaceTypes(Registry &&from) {
    m_nodeTypes = std::move(from.m_nodeTypes);
    m_aliases = std::move(from.m_aliases);
    m_portTypes = std::move(from.m_portTypes);
    m_packVersions = std::move(from.m_packVersions);
    m_packSources = std::move(from.m_packSources);
}

bool Registry::sameTypes(const Registry &other) const {
    const auto sameType = [](const NodeType &a, const NodeType &b) {
        return a.dir == b.dir && a.seal == b.seal && a.packVersion == b.packVersion && a.toJson() == b.toJson();
    };
    const auto sameNode = [&](const auto &a, const auto &b) {
        return a.first == b.first
               && std::equal(a.second.begin(), a.second.end(), b.second.begin(), b.second.end(),
                             [&](const auto &x, const auto &y) { return x.first == y.first && sameType(x.second, y.second); });
    };
    const auto samePort = [](const auto &a, const auto &b) {
        return a.first == b.first && a.second.kind == b.second.kind && a.second.name == b.second.name
               && a.second.extensions == b.second.extensions && a.second.folders == b.second.folders;
    };
    return m_packVersions == other.m_packVersions && m_packSources == other.m_packSources && m_aliases == other.m_aliases
           && std::equal(m_nodeTypes.begin(), m_nodeTypes.end(), other.m_nodeTypes.begin(), other.m_nodeTypes.end(), sameNode)
           && std::equal(m_portTypes.begin(), m_portTypes.end(), other.m_portTypes.begin(), other.m_portTypes.end(), samePort);
}

} // namespace fugue
