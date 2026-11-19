#include "registry.h"

#include "engine/ids.h"

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

    if (m_nodeTypes.count(type.id))
        fail(QStringLiteral("a node type with this id is already registered, at version %1.")
                 .arg(m_nodeTypes.at(type.id).version.toString()));

    const bool builtin = typeNamespace(type.id) == QLatin1String("fugue");
    for (const InputSpec &input : type.inputs) {
        if (input.type.type.isAny() && !builtin)
            fail(QStringLiteral("input %1 is of type \"any\", which only built-in fugue/ nodes may use.")
                     .arg(input.name),
                 input.name);
        else if (portKind(input.type.type) == PortKind::Unknown)
            fail(QStringLiteral("input %1 has the port type %2, which no pack declares.")
                     .arg(input.name, input.type.type.toString()),
                 input.name);
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
        if (owner != m_aliases.end())
            errors << problem(QStringLiteral("%1: the alias %2 is already claimed by %3, which keeps it.")
                                  .arg(type.id, alias, owner->second),
                              Error::Severity::Warning);
        else
            m_aliases.emplace(alias, type.id);
    }
    m_nodeTypes.emplace(type.id, type);
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

const NodeType *Registry::nodeType(const QString &id) const {
    auto found = m_nodeTypes.find(id);
    if (found == m_nodeTypes.end()) {
        const auto alias = m_aliases.find(id);
        if (alias == m_aliases.end())
            return nullptr;
        found = m_nodeTypes.find(alias->second);
    }
    return found == m_nodeTypes.end() ? nullptr : &found->second;
}

QList<const NodeType *> Registry::nodeTypes() const {
    QList<const NodeType *> list;
    for (const auto &entry : m_nodeTypes)
        list << &entry.second;
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

const DeclaredPortType *Registry::portType(const PortType &type) const {
    const auto found = m_portTypes.find(type.toString());
    return found == m_portTypes.end() ? nullptr : &found->second;
}

} // namespace fugue
