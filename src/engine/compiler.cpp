#include "compiler.h"

#include "engine/canonical.h"
#include "engine/ids.h"
#include "engine/result.h"
#include "engine/suggest.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <deque>
#include <functional>
#include <set>

namespace fugue {

namespace {

QString shown(const QJsonValue &value) {
    if (value.isUndefined())
        return QStringLiteral("nothing");
    return QString::fromUtf8(canonicalJson(value).value_or("?"));
}

QString joined(const QStringList &items) {
    return items.join(QStringLiteral(", "));
}

Error nodeError(const QString &code, const QString &node, const QString &message) {
    Error error;
    error.code = code;
    error.node = node;
    error.message = message;
    return error;
}

void addHint(Error &error, const QString &value, const QStringList &options) {
    const QString near = nearest(value, options);
    if (!near.isEmpty())
        error.hint = QStringLiteral("Did you mean \"%1\"?").arg(near);
}

QStringList inputNames(const NodeType &type) {
    QStringList names;
    for (const InputSpec &input : type.inputs)
        names << input.name;
    return names;
}

QStringList outputNames(const NodeType &type) {
    QStringList names;
    for (const OutputSpec &output : type.outputs)
        names << output.name;
    return names;
}

const GroupInput *exposedInput(const GroupDefinition &group, const QString &name) {
    for (const GroupInput &input : group.inputs)
        if (input.name == name)
            return &input;
    return nullptr;
}

const GroupOutput *groupOutput(const GroupDefinition &group, const QString &name) {
    for (const GroupOutput &output : group.outputs)
        if (output.name == name)
            return &output;
    return nullptr;
}

// The node path ids a set of inputs links from, sorted, each once.
QStringList sources(const QMap<QString, InputValue> &inputs) {
    QStringList result;
    const auto add = [&](const InputValue &value) {
        if (value.kind() == InputValue::Kind::Link && !result.contains(value.from().node))
            result << value.from().node;
    };
    for (const InputValue &value : inputs) {
        add(value);
        if (value.kind() == InputValue::Kind::List)
            for (const InputValue &item : value.items())
                add(item);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// --- the flattening ----------------------------------------------------------

// One graph level: the top graph, or the inside of one group instance.
struct Scope {
    QString prefix;       // "" at the top, else the instance path and '/'
    QString instancePath; // "" at the top, else "g7" or "g7/inner3"
    QString instanceId;   // the last segment of instancePath
    const Nodes *nodes = nullptr;
    const GroupDefinition *definition = nullptr; // null at the top
    Scope *parent = nullptr;
    bool muted = false;    // this instance or one around it is muted
    bool bypassed = false; // this instance is bypassed (and not muted)
    bool hidden = false;   // inside a bypassed instance: checked, never run
    QString provider;      // the pin its nodes inherit, or empty
    QMap<QString, Scope *> children; // by inner instance id
};

enum class Status { Leaf, Group, Broken };

// An input value bound to where it was written.
struct Bound {
    InputValue value;
    Scope *scope = nullptr;              // where its links resolve
    QString writtenAt;                   // path id of the node it was written on
    QString writtenInput;                // the input name it was written under
    QList<const GroupInput *> narrowing; // the exposed inputs it came through
};

// Where a link leads once groups are seen through.
struct Source {
    enum class Status { Ok, Broken, Error } status = Status::Broken;
    QString path; // path id of a node that runs, or of a bypassed group instance
    QString port;
    PortSpec spec;
    Error error; // for Error: code, message, valid and hint; the caller places it
};

// A node of the flattened graph before modes are resolved.
struct Flat {
    QString path;
    const NodeType *type = nullptr; // the installed type; null for a face
    std::optional<NodeType> face;   // a bypassed group instance as seen from outside
    NodeMode mode = NodeMode::Normal;
    bool inGroup = false;
    bool hidden = false; // inside a bypassed group instance
    QString provider;
    QString frozen;
    QMap<QString, InputValue> inputs; // links by path id
    bool emptied = false;             // a required input lost its value to a bypass

    const NodeType &spec() const { return face ? *face : *type; }
};

InputSpec narrowed(InputSpec spec, const QList<const GroupInput *> &narrowing) {
    for (const GroupInput *group : narrowing) {
        if (group->min)
            spec.min = spec.min ? std::max(*spec.min, *group->min) : *group->min;
        if (group->max)
            spec.max = spec.max ? std::min(*spec.max, *group->max) : *group->max;
        if (group->choices) {
            if (spec.choices) {
                QStringList both;
                for (const QString &choice : *group->choices)
                    if (spec.choices->contains(choice))
                        both << choice;
                spec.choices = both;
            } else {
                spec.choices = *group->choices;
            }
        }
    }
    return spec;
}

class Compiler {
public:
    Compiler(const Document &document, const Registry &registry, const CompileOptions &options)
        : m_doc(document), m_registry(registry), m_options(options) {}

    Compilation run();

private:
    void fail(Error error) { m_errors << std::move(error); }
    void warn(Error error) {
        error.severity = Error::Severity::Warning;
        m_errors << std::move(error);
    }

    void checkRecursion();
    void checkProviders();
    void checkTarget();
    void build(Scope *scope);
    bool checkVersion(const QString &path, const Node &node, const QString &typeName, const Version &installed);
    void check(Scope *scope);
    void checkInstance(Scope *scope, const QString &id, Scope *child);
    void checkNode(Scope *scope, const QString &id, Flat &flat, bool face);
    void checkValue(Flat &flat, const InputSpec &spec, const Bound &bound);
    std::optional<InputValue> resolveItem(const Flat &flat, const InputSpec &spec, const Bound &bound,
                                          const InputValue &item, int index);

    Status status(const Scope *scope, const QString &id) const {
        return m_status.value(scope->prefix + id, Status::Broken);
    }
    QMap<QString, Bound> effective(Scope *scope, const QString &id) const;
    std::optional<std::pair<QString, QString>> exposedAt(const Scope *scope, const QString &id,
                                                         const QString &port) const;
    Source resolve(Scope *scope, const PortRef &ref) const;
    std::optional<InputSpec> exposedSpec(Scope *child, const GroupInput &input) const;
    NodeType faceOf(Scope *child, const Node &instance) const;
    QStringList knownTypes() const;

    bool findCycles();
    CompiledGraph shape();
    InputValue bypassValue(const QString &path, const QString &port, int depth) const;
    InputValue rewire(const InputValue &value, int depth) const;

    const Document &m_doc;
    const Registry &m_registry;
    const CompileOptions &m_options;
    Errors m_errors;

    std::deque<Scope> m_scopes;
    QSet<QString> m_recursive; // group ids in a recursion cycle
    QHash<QString, Status> m_status;
    QHash<QString, const NodeType *> m_types; // leaf path id -> installed type
    QMap<QString, Flat> m_flat;               // by path id
    QSet<QString> m_reported;                 // link errors already given, by where they were written
};

// Group recursion (5.2): a definition that contains itself, directly or
// through other groups. One error per cycle; the groups in it are not expanded.
void Compiler::checkRecursion() {
    QMap<QString, QStringList> uses;
    for (auto it = m_doc.groups.constBegin(); it != m_doc.groups.constEnd(); ++it) {
        QStringList inner;
        for (const Node &node : it->nodes)
            if (m_doc.groups.contains(node.type) && !inner.contains(node.type))
                inner << node.type;
        std::sort(inner.begin(), inner.end());
        uses[it.key()] = inner;
    }

    // Tarjan's strongly connected components.
    QHash<QString, int> index, low;
    QStringList stack;
    QSet<QString> onStack;
    int next = 0;
    std::function<void(const QString &)> visit = [&](const QString &group) {
        index[group] = low[group] = next++;
        stack << group;
        onStack.insert(group);
        for (const QString &inner : uses[group]) {
            if (!index.contains(inner)) {
                visit(inner);
                low[group] = std::min(low[group], low[inner]);
            } else if (onStack.contains(inner)) {
                low[group] = std::min(low[group], index[inner]);
            }
        }
        if (low[group] != index[group])
            return;
        QStringList component;
        QString member;
        do {
            member = stack.takeLast();
            onStack.remove(member);
            component << member;
        } while (member != group);
        std::sort(component.begin(), component.end());
        const bool selfLoop = component.size() == 1 && uses[group].contains(group);
        if (component.size() < 2 && !selfLoop)
            return;
        for (const QString &id : component)
            m_recursive.insert(id);
        Error error;
        error.code = QStringLiteral("group_recursion");
        error.message = selfLoop ? QStringLiteral("Group %1 contains an instance of itself.").arg(group)
                                 : QStringLiteral("Groups %1 contain each other.").arg(joined(component));
        error.valid = QJsonArray::fromStringList(component);
        error.hint = QStringLiteral("A group cannot contain itself, directly or through another group; "
                                    "remove the inner instance that closes the loop.");
        fail(error);
    };
    for (auto it = uses.constBegin(); it != uses.constEnd(); ++it)
        if (!index.contains(it.key()))
            visit(it.key());
}

// Pins name configured instances (8.3), when the caller says which exist.
void Compiler::checkProviders() {
    if (!m_options.providers)
        return;
    const QStringList &names = *m_options.providers;
    const auto check = [&](const QString &pin, const QString &path) {
        if (pin.isEmpty() || names.contains(pin))
            return;
        Error error = nodeError(QStringLiteral("unknown_provider"), path,
                                path.isEmpty()
                                    ? QStringLiteral("The workflow pins provider \"%1\", which is not configured.").arg(pin)
                                    : QStringLiteral("Node %1 pins provider \"%2\", which is not configured.").arg(path, pin));
        error.valid = QJsonArray::fromStringList(names);
        addHint(error, pin, names);
        if (error.hint.isEmpty())
            error.hint = QStringLiteral("Pin one of the configured provider instances, or remove the pin for auto.");
        fail(error);
    };
    check(m_options.provider.isEmpty() ? m_doc.graph.provider : m_options.provider, {});
    for (const Scope &scope : m_scopes)
        for (auto it = scope.nodes->constBegin(); it != scope.nodes->constEnd(); ++it)
            check(it->provider, scope.prefix + it.key());
}

// "Run to here" needs a node that runs.
void Compiler::checkTarget() {
    if (m_options.target.isEmpty())
        return;
    const auto it = m_flat.constFind(m_options.target);
    if (it != m_flat.constEnd() && !it->hidden && it->mode != NodeMode::Bypass)
        return;
    QStringList paths;
    for (const Flat &flat : m_flat)
        if (!flat.hidden && flat.mode != NodeMode::Bypass)
            paths << flat.path;
    Error error;
    error.code = QStringLiteral("usage");
    error.message = it == m_flat.constEnd()
                        ? QStringLiteral("There is no node %1 to run to.").arg(m_options.target)
                        : QStringLiteral("Node %1 is bypassed, so a run cannot stop at it.").arg(m_options.target);
    error.valid = QJsonArray::fromStringList(paths);
    addHint(error, m_options.target, paths);
    fail(error);
}

QStringList Compiler::knownTypes() const {
    QStringList ids;
    for (const NodeType *type : m_registry.nodeTypes())
        ids << type->id;
    for (auto it = m_doc.groups.constBegin(); it != m_doc.groups.constEnd(); ++it)
        if (!ids.contains(it.key()))
            ids << it.key();
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 3.6: a different major version needs review before the node runs.
bool Compiler::checkVersion(const QString &path, const Node &node, const QString &typeName,
                            const Version &installed) {
    if (node.version.major == installed.major)
        return true;
    Error error = nodeError(QStringLiteral("node_version_mismatch"), path,
                            QStringLiteral("Node %1 was made with %2 %3, but %4 is installed, a different major version.")
                                .arg(path, typeName, node.version.toString(), installed.toString()));
    error.valid = QJsonArray{installed.toString()};
    error.hint = QStringLiteral("Review what changed, then set the node's version to %1 to accept it.")
                     .arg(installed.toString());
    fail(error);
    return false;
}

// Classifies every node of a scope and makes the scopes of its group instances.
void Compiler::build(Scope *scope) {
    for (auto it = scope->nodes->constBegin(); it != scope->nodes->constEnd(); ++it) {
        const QString path = scope->prefix + it.key();
        const Node &node = *it;
        if (const GroupDefinition *group = m_doc.group(node.type)) {
            // A group of the workflow wins over an installed type (5.3).
            if (m_recursive.contains(node.type) ||
                !checkVersion(path, node, QStringLiteral("group ") + node.type, group->version)) {
                m_status[path] = Status::Broken;
                continue;
            }
            m_status[path] = Status::Group;
            m_scopes.push_back(Scope{});
            Scope *child = &m_scopes.back();
            child->prefix = path + QLatin1Char('/');
            child->instancePath = path;
            child->instanceId = it.key();
            child->nodes = &group->nodes;
            child->definition = group;
            child->parent = scope;
            child->muted = scope->muted || node.mode == NodeMode::Mute;
            child->bypassed = !child->muted && node.mode == NodeMode::Bypass;
            child->hidden = scope->hidden || scope->bypassed;
            child->provider = node.provider.isEmpty() ? scope->provider : node.provider;
            scope->children[it.key()] = child;
            build(child);
            continue;
        }
        if (const NodeType *type = m_registry.nodeType(node.type)) {
            if (!checkVersion(path, node, type->id, type->version)) {
                m_status[path] = Status::Broken;
                continue;
            }
            m_status[path] = Status::Leaf;
            m_types[path] = type;
            continue;
        }
        m_status[path] = Status::Broken;
        const QStringList known = knownTypes();
        Error error = nodeError(QStringLiteral("unknown_node_type"), path,
                                QStringLiteral("Node %1 has type %2, which is not installed and is not a group of "
                                               "this workflow.")
                                    .arg(path, node.type));
        error.valid = QJsonArray::fromStringList(known);
        addHint(error, node.type, known);
        if (error.hint.isEmpty())
            error.hint = QStringLiteral("Install the pack that provides %1, or change the node's type.").arg(node.type);
        fail(error);
    }
}

// The values a node's inputs take: what is written on it, overridden by the
// exposed inputs of the group around it (the instance's value, else the
// group's default).
QMap<QString, Bound> Compiler::effective(Scope *scope, const QString &id) const {
    QMap<QString, Bound> result;
    const QString path = scope->prefix + id;
    const Node &node = *scope->nodes->constFind(id);
    for (auto it = node.inputs.constBegin(); it != node.inputs.constEnd(); ++it)
        result[it.key()] = Bound{*it, scope, path, it.key(), {}};
    if (!scope->definition)
        return result;
    const QMap<QString, Bound> outer = effective(scope->parent, scope->instanceId);
    for (const GroupInput &exposed : scope->definition->inputs) {
        for (const PortRef &target : exposed.targets) {
            if (target.node != id)
                continue;
            if (outer.contains(exposed.name)) {
                Bound bound = outer.value(exposed.name);
                bound.narrowing << &exposed;
                result[target.port] = bound;
            } else if (exposed.defaultValue) {
                result[target.port] = Bound{*exposed.defaultValue, scope, scope->instancePath, exposed.name, {&exposed}};
            }
        }
    }
    return result;
}

// The outermost exposed input that drives id.port, as (instance path, name).
std::optional<std::pair<QString, QString>> Compiler::exposedAt(const Scope *scope, const QString &id,
                                                               const QString &port) const {
    if (!scope->definition)
        return std::nullopt;
    for (const GroupInput &exposed : scope->definition->inputs)
        for (const PortRef &target : exposed.targets)
            if (target.node == id && target.port == port) {
                if (auto outer = exposedAt(scope->parent, scope->instanceId, exposed.name))
                    return outer;
                return std::make_pair(scope->instancePath, exposed.name);
            }
    return std::nullopt;
}

Source Compiler::resolve(Scope *scope, const PortRef &ref) const {
    Source source;
    const auto error = [&](const QString &code, const QString &message, const QStringList &valid,
                           const QString &near) {
        source.status = Source::Status::Error;
        source.error.code = code;
        source.error.message = message;
        source.error.valid = QJsonArray::fromStringList(valid);
        addHint(source.error, near, valid);
        return source;
    };
    if (!scope->nodes->contains(ref.node)) {
        const QStringList ids = scope->nodes->keys();
        return error(QStringLiteral("unknown_link_source"),
                     scope->definition
                         ? QStringLiteral("the link from %1 names node %2, which group %3 does not have.")
                               .arg(ref.toString(), ref.node, scope->instancePath)
                         : QStringLiteral("the link from %1 names node %2, which the graph does not have.")
                               .arg(ref.toString(), ref.node),
                     ids, ref.node);
    }
    const QString path = scope->prefix + ref.node;
    switch (status(scope, ref.node)) {
    case Status::Broken:
        return source;
    case Status::Leaf: {
        const NodeType *type = m_types.value(path);
        const OutputSpec *output = type->output(ref.port);
        if (!output)
            return error(QStringLiteral("unknown_output"),
                         QStringLiteral("the link from %1 names output %2, which %3 (%4) does not have.")
                             .arg(ref.toString(), ref.port, path, type->id),
                         outputNames(*type), ref.port);
        source.status = Source::Status::Ok;
        source.path = path;
        source.port = ref.port;
        source.spec = output->type;
        return source;
    }
    case Status::Group: {
        Scope *child = scope->children.value(ref.node);
        const GroupOutput *output = groupOutput(*child->definition, ref.port);
        if (!output) {
            QStringList names;
            for (const GroupOutput &o : child->definition->outputs)
                names << o.name;
            return error(QStringLiteral("unknown_output"),
                         QStringLiteral("the link from %1 names output %2, which group %3 does not have.")
                             .arg(ref.toString(), ref.port, scope->nodes->constFind(ref.node)->type),
                         names, ref.port);
        }
        Source inner = resolve(child, output->from);
        if (inner.status != Source::Status::Ok)
            return Source{}; // reported once, on the instance (checkInstance)
        if (child->bypassed) {
            inner.path = path;
            inner.port = ref.port;
        }
        return inner;
    }
    }
    return source;
}

// The input spec an exposed input takes from its first target, narrowed.
std::optional<InputSpec> Compiler::exposedSpec(Scope *child, const GroupInput &input) const {
    if (input.targets.isEmpty())
        return std::nullopt;
    const PortRef &target = input.targets.first();
    if (!child->nodes->contains(target.node))
        return std::nullopt;
    std::optional<InputSpec> spec;
    switch (status(child, target.node)) {
    case Status::Broken:
        return std::nullopt;
    case Status::Leaf:
        if (const InputSpec *found = m_types.value(child->prefix + target.node)->input(target.port))
            spec = *found;
        break;
    case Status::Group: {
        Scope *grandchild = child->children.value(target.node);
        if (const GroupInput *inner = exposedInput(*grandchild->definition, target.port))
            spec = exposedSpec(grandchild, *inner);
        break;
    }
    }
    if (!spec)
        return std::nullopt;
    spec = narrowed(*spec, {&input});
    spec->name = input.name;
    if (input.defaultValue && input.defaultValue->kind() == InputValue::Kind::Literal)
        spec->defaultValue = input.defaultValue->literal();
    return spec;
}

// A bypassed group instance as seen from outside: its exposed inputs and its
// outputs, so that bypass can map one onto the other (3.5).
NodeType Compiler::faceOf(Scope *child, const Node &instance) const {
    NodeType face;
    face.id = instance.type;
    face.version = child->definition->version;
    face.name = child->definition->name;
    for (const GroupInput &input : child->definition->inputs)
        if (std::optional<InputSpec> spec = exposedSpec(child, input))
            face.inputs << *spec;
    for (const GroupOutput &output : child->definition->outputs) {
        const Source source = resolve(child, output.from);
        if (source.status == Source::Status::Ok)
            face.outputs << OutputSpec{output.name, source.spec, {}, {}, false};
    }
    return face;
}

// The face of a group instance: names the instance sets that the group does
// not expose, exposed inputs whose targets do not resolve or differ in type,
// and outputs that lead nowhere. Reported on the instance's path id.
void Compiler::checkInstance(Scope *scope, const QString &id, Scope *child) {
    const QString path = scope->prefix + id;
    const Node &node = *scope->nodes->constFind(id);
    const GroupDefinition &group = *child->definition;

    QStringList exposedNames;
    for (const GroupInput &input : group.inputs)
        exposedNames << input.name;
    for (auto it = node.inputs.constBegin(); it != node.inputs.constEnd(); ++it) {
        if (exposedNames.contains(it.key()))
            continue;
        Error error = nodeError(QStringLiteral("unknown_input"), path,
                                QStringLiteral("Group %1 has no exposed input %2.").arg(node.type, it.key()));
        error.input = it.key();
        error.valid = QJsonArray::fromStringList(exposedNames);
        addHint(error, it.key(), exposedNames);
        fail(error);
    }

    for (const GroupInput &input : group.inputs) {
        std::optional<PortSpec> first;
        for (const PortRef &target : input.targets) {
            Error error = nodeError(QStringLiteral("unknown_input"), path, {});
            error.input = input.name;
            if (!child->nodes->contains(target.node)) {
                error.message = QStringLiteral("Exposed input %1 of group %2 drives %3, but the group has no node %4.")
                                    .arg(input.name, node.type, target.toString(), target.node);
                error.valid = QJsonArray::fromStringList(child->nodes->keys());
                addHint(error, target.node, child->nodes->keys());
                fail(error);
                continue;
            }
            std::optional<PortSpec> spec;
            QStringList names;
            switch (status(child, target.node)) {
            case Status::Broken:
                continue;
            case Status::Leaf: {
                const NodeType *type = m_types.value(child->prefix + target.node);
                if (const InputSpec *found = type->input(target.port))
                    spec = found->type;
                names = inputNames(*type);
                break;
            }
            case Status::Group: {
                Scope *grandchild = child->children.value(target.node);
                for (const GroupInput &inner : grandchild->definition->inputs)
                    names << inner.name;
                if (const GroupInput *inner = exposedInput(*grandchild->definition, target.port)) {
                    const std::optional<InputSpec> innerSpec = exposedSpec(grandchild, *inner);
                    if (!innerSpec)
                        continue; // its own problem, reported on the inner instance
                    spec = innerSpec->type;
                }
                break;
            }
            }
            if (!spec) {
                error.message = QStringLiteral("Exposed input %1 of group %2 drives %3, but node %4 has no input %5.")
                                    .arg(input.name, node.type, target.toString(), target.node, target.port);
                error.valid = QJsonArray::fromStringList(names);
                addHint(error, target.port, names);
                fail(error);
                continue;
            }
            if (!first) {
                first = spec;
            } else if (first->type != spec->type || first->list != spec->list) {
                Error mismatch = nodeError(
                    QStringLiteral("type_mismatch"), path,
                    QStringLiteral("Exposed input %1 of group %2 drives inputs of different types: %3 and %4 (%5).")
                        .arg(input.name, node.type, first->toString(), spec->toString(), target.toString()));
                mismatch.input = input.name;
                mismatch.hint = QStringLiteral("All targets of one exposed input must have the same type.");
                fail(mismatch);
            }
        }
    }

    for (const GroupOutput &output : group.outputs) {
        const Source source = resolve(child, output.from);
        if (source.status != Source::Status::Error)
            continue;
        Error error = source.error;
        error.node = path;
        error.output = output.name;
        error.message = QStringLiteral("Output %1 of group %2: %3").arg(output.name, node.type, error.message);
        fail(error);
    }
}

// One value or list item: a literal is checked against the input; a link is
// followed through groups and its type checked. Nothing when it cannot be used.
std::optional<InputValue> Compiler::resolveItem(const Flat &flat, const InputSpec &spec, const Bound &bound,
                                                const InputValue &item, int index) {
    if (item.kind() != InputValue::Kind::Link) {
        const Errors errors = checkLiteral(spec, item.toJson(), m_registry.portKind(spec.type.type));
        for (Error error : errors) {
            error.node = flat.path;
            error.input = spec.name;
            fail(error);
        }
        if (!errors.isEmpty())
            return std::nullopt;
        return item;
    }
    const Source source = resolve(bound.scope, item.from());
    if (source.status == Source::Status::Error) {
        // Report it once where it was written, even when one exposed input
        // carries it to several inner nodes.
        const QString key = bound.writtenAt + QLatin1Char('.') + bound.writtenInput + QLatin1Char('#') +
                            QString::number(index);
        if (!m_reported.contains(key)) {
            m_reported.insert(key);
            Error error = source.error;
            error.node = bound.writtenAt;
            error.input = bound.writtenInput;
            error.message = QStringLiteral("Input %1 of %2: %3").arg(bound.writtenInput, bound.writtenAt, error.message);
            fail(error);
        }
        return std::nullopt;
    }
    if (source.status == Source::Status::Broken)
        return std::nullopt;
    PortSpec into = spec.type;
    if (index >= 0)
        into.list = true; // an item of a list input
    const Compatibility compatible = compatibility(source.spec, into);
    if (compatible != Compatibility::Compatible) {
        Error error = nodeError(QStringLiteral("type_mismatch"), flat.path,
                                QStringLiteral("Input %1 of %2 is linked from %3.%4. %5")
                                    .arg(spec.name, flat.path, source.path, source.port,
                                         explain(compatible, source.spec, into)));
        error.input = spec.name;
        error.hint = compatible == Compatibility::ListIntoSingle
                         ? QStringLiteral("A list output feeds list inputs only; there is no implicit iteration in v0.1.")
                         : QStringLiteral("Link an output of type %1; there is no implicit conversion.")
                               .arg(into.type.toString());
        fail(error);
        return std::nullopt;
    }
    return InputValue::link(PortRef{source.path, source.port});
}

void Compiler::checkValue(Flat &flat, const InputSpec &base, const Bound &bound) {
    const InputSpec spec = narrowed(base, bound.narrowing);
    const InputValue &value = bound.value;
    if (value.kind() != InputValue::Kind::List) {
        if (std::optional<InputValue> resolved = resolveItem(flat, spec, bound, value, -1))
            flat.inputs[spec.name] = *resolved;
        return;
    }
    if (!spec.type.list) {
        Error error = nodeError(QStringLiteral("type_mismatch"), flat.path,
                                QStringLiteral("Input %1 of %2 takes one %3, not a list.")
                                    .arg(spec.name, flat.path, spec.type.toString()));
        error.input = spec.name;
        fail(error);
        return;
    }
    const QList<InputValue> items = value.items();
    const auto count = [&](const QString &message) {
        Error error = nodeError(QStringLiteral("value_out_of_range"), flat.path, message);
        error.input = spec.name;
        fail(error);
    };
    if (spec.minItems && items.size() < *spec.minItems)
        count(QStringLiteral("Input %1 of %2 takes at least %3 items, not %4.")
                  .arg(spec.name, flat.path)
                  .arg(*spec.minItems)
                  .arg(items.size()));
    if (spec.maxItems && items.size() > *spec.maxItems)
        count(QStringLiteral("Input %1 of %2 takes at most %3 items, not %4.")
                  .arg(spec.name, flat.path)
                  .arg(*spec.maxItems)
                  .arg(items.size()));
    QList<InputValue> resolved;
    bool complete = true;
    for (int i = 0; i < items.size(); ++i) {
        if (std::optional<InputValue> item = resolveItem(flat, spec, bound, items[i], int(i)))
            resolved << *item;
        else
            complete = false;
    }
    if (complete)
        flat.inputs[spec.name] = InputValue::list(resolved);
}

// Inputs of one node: names its type does not have, required inputs that are
// not set, and each value. For a bypassed group's face only the values.
void Compiler::checkNode(Scope *scope, const QString &id, Flat &flat, bool face) {
    const NodeType &type = flat.spec();
    const Node &node = *scope->nodes->constFind(id);
    const QMap<QString, Bound> values = effective(scope, id);
    const bool olderMinor = !face && type.version.minor < node.version.minor;

    if (!face) {
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            // A value that came through a group's face and names an input the
            // node lacks is a bad target, already reported on the instance.
            if (type.input(it.key()) || !it->narrowing.isEmpty())
                continue;
            Error error = nodeError(QStringLiteral("unknown_input"), flat.path, {});
            error.input = it.key();
            error.valid = QJsonArray::fromStringList(inputNames(type));
            if (olderMinor) {
                // 3.6: the value is kept in the file and flagged, not used.
                error.message = QStringLiteral("Node %1 was made with %2 %3; the installed %4 has no input %5, so "
                                               "its value is not used.")
                                    .arg(flat.path, type.id, node.version.toString(), type.version.toString(), it.key());
                error.hint = QStringLiteral("Update %1 to %2 or newer.").arg(type.id, node.version.toString());
                warn(error);
            } else {
                error.message = QStringLiteral("Node %1 (%2) has no input %3.").arg(flat.path, type.id, it.key());
                addHint(error, it.key(), inputNames(type));
                fail(error);
            }
        }
    }

    for (const InputSpec &spec : type.inputs) {
        if (values.contains(spec.name)) {
            checkValue(flat, spec, values.value(spec.name));
            continue;
        }
        if (face || !spec.required())
            continue;
        Error error = nodeError(QStringLiteral("missing_input"), flat.path,
                                QStringLiteral("Input %1 of %2 is required and not set.").arg(spec.name, flat.path));
        error.input = spec.name;
        if (const auto exposed = exposedAt(scope, id, spec.name))
            error.hint = QStringLiteral("Set exposed input %1 on %2.").arg(exposed->second, exposed->first);
        else
            error.hint = QStringLiteral("Give it a value or link an output of type %1.").arg(spec.type.toString());
        fail(error);
    }
}

void Compiler::check(Scope *scope) {
    for (auto it = scope->nodes->constBegin(); it != scope->nodes->constEnd(); ++it) {
        const QString path = scope->prefix + it.key();
        const Node &node = *it;
        const Status kind = status(scope, it.key());
        if (kind == Status::Broken)
            continue;
        if (kind == Status::Group) {
            Scope *child = scope->children.value(it.key());
            checkInstance(scope, it.key(), child);
            if (child->bypassed && !child->hidden) {
                Flat flat;
                flat.path = path;
                flat.face = faceOf(child, node);
                flat.mode = NodeMode::Bypass;
                flat.inGroup = scope->definition != nullptr;
                checkNode(scope, it.key(), flat, true);
                m_flat[path] = flat;
            }
            check(child);
            continue;
        }
        Flat flat;
        flat.path = path;
        flat.type = m_types.value(path);
        flat.mode = scope->muted ? NodeMode::Mute : node.mode;
        flat.inGroup = scope->definition != nullptr;
        flat.hidden = scope->hidden || scope->bypassed;
        flat.provider = node.provider.isEmpty() ? scope->provider : node.provider;
        flat.frozen = node.frozen;
        checkNode(scope, it.key(), flat, false);
        m_flat[path] = flat;
    }
}

// --- after checking: cycles and modes ----------------------------------------

// One `cycle` error per strongly connected component of the flattened graph.
bool Compiler::findCycles() {
    QHash<QString, int> index, low;
    QStringList stack;
    QSet<QString> onStack;
    int next = 0;
    bool found = false;
    std::function<void(const QString &)> visit = [&](const QString &path) {
        index[path] = low[path] = next++;
        stack << path;
        onStack.insert(path);
        const QStringList deps = sources(m_flat.value(path).inputs);
        for (const QString &dep : deps) {
            if (!m_flat.contains(dep))
                continue;
            if (!index.contains(dep)) {
                visit(dep);
                low[path] = std::min(low[path], low[dep]);
            } else if (onStack.contains(dep)) {
                low[path] = std::min(low[path], index[dep]);
            }
        }
        if (low[path] != index[path])
            return;
        QStringList component;
        QString member;
        do {
            member = stack.takeLast();
            onStack.remove(member);
            component << member;
        } while (member != path);
        std::sort(component.begin(), component.end());
        if (component.size() < 2 && !deps.contains(path))
            return;
        found = true;
        Error error = nodeError(QStringLiteral("cycle"), component.first(),
                                component.size() == 1
                                    ? QStringLiteral("Node %1 feeds its own input.").arg(path)
                                    : QStringLiteral("Nodes %1 feed each other in a cycle.").arg(joined(component)));
        error.valid = QJsonArray::fromStringList(component);
        error.hint = QStringLiteral("Remove one of the links that close the loop.");
        fail(error);
    };
    for (auto it = m_flat.constBegin(); it != m_flat.constEnd(); ++it)
        if (!it->hidden && !index.contains(it.key()))
            visit(it.key());
    return found;
}

// What a link to output `port` of a bypassed node carries (3.5): the value of
// the first input, in declaration order, with the same type; empty when there
// is none, or when that input has no value and no default.
InputValue Compiler::bypassValue(const QString &path, const QString &port, int depth) const {
    const Flat &flat = *m_flat.constFind(path);
    const OutputSpec *output = flat.spec().output(port);
    if (!output || depth > m_flat.size())
        return {};
    for (const InputSpec &input : flat.spec().inputs) {
        if (input.type.type != output->type.type || input.type.list != output->type.list)
            continue;
        if (flat.inputs.contains(input.name))
            return rewire(flat.inputs.value(input.name), depth + 1);
        if (!input.defaultValue.isUndefined())
            return InputValue::literal(input.defaultValue);
        return {};
    }
    return {};
}

// A value with every link into a bypassed node replaced by what that output
// carries. Empty items leave a list; an empty link gives an empty value.
InputValue Compiler::rewire(const InputValue &value, int depth) const {
    if (value.kind() == InputValue::Kind::Link) {
        const PortRef from = value.from();
        const auto it = m_flat.constFind(from.node);
        if (it != m_flat.constEnd() && it->mode == NodeMode::Bypass)
            return bypassValue(from.node, from.port, depth);
        return value;
    }
    if (value.kind() != InputValue::Kind::List)
        return value;
    QList<InputValue> items;
    for (const InputValue &item : value.items()) {
        const InputValue rewired = rewire(item, depth);
        if (rewired.isEmpty())
            continue;
        if (rewired.kind() == InputValue::Kind::List && item.kind() == InputValue::Kind::Link)
            items << rewired.items(); // a list carried through into a list input
        else
            items << rewired;
    }
    return InputValue::list(items);
}

// Bypass, mute, sinks inside groups, and what no sink needs (4.8).
CompiledGraph Compiler::shape() {
    CompiledGraph graph;
    graph.source = m_doc.sourceHash();
    graph.target = m_options.target;

    // Bypass: rewire the links, then leave the bypassed nodes out.
    QMap<QString, Flat> live;
    for (auto it = m_flat.constBegin(); it != m_flat.constEnd(); ++it)
        if (!it->hidden && it->mode != NodeMode::Bypass)
            live[it.key()] = *it;
    for (Flat &flat : live) {
        QMap<QString, InputValue> rewired;
        for (auto it = flat.inputs.constBegin(); it != flat.inputs.constEnd(); ++it) {
            const InputValue value = rewire(*it, 0);
            const bool emptiedList = value.kind() == InputValue::Kind::List && value.items().isEmpty() &&
                                     !it->items().isEmpty();
            if (!value.isEmpty() && !emptiedList) {
                rewired[it.key()] = value;
                continue;
            }
            const InputSpec *spec = flat.spec().input(it.key());
            if (spec && spec->required())
                flat.emptied = true;
        }
        flat.inputs = rewired;
    }

    // Sinks inside a group never run (5.5), unless the run stops at one.
    for (auto it = live.begin(); it != live.end();) {
        if (it->inGroup && it->spec().sink && it.key() != m_options.target)
            it = live.erase(it);
        else
            ++it;
    }

    // Mute (3.5) and inputs a bypass left empty, in dependency order.
    CompiledGraph ordering;
    for (const Flat &flat : live)
        ordering.nodes[flat.path].inputs = flat.inputs;
    QMap<QString, QString> dropped;
    for (const QString &path : ordering.order()) {
        const Flat &flat = live[path];
        QString reason;
        bool upstreamEmpty = flat.emptied;
        for (const QString &dep : sources(flat.inputs)) {
            const QString why = dropped.value(dep);
            if (why == QLatin1String("muted") || why == QLatin1String("upstream_muted"))
                reason = QStringLiteral("upstream_muted");
            else if (why == QLatin1String("upstream_empty"))
                upstreamEmpty = true;
        }
        if (flat.mode == NodeMode::Mute)
            reason = QStringLiteral("muted");
        else if (reason.isEmpty() && upstreamEmpty)
            reason = QStringLiteral("upstream_empty");
        if (!reason.isEmpty())
            dropped[path] = reason;
    }
    for (auto it = dropped.constBegin(); it != dropped.constEnd(); ++it) {
        live.remove(it.key());
        graph.skipped[it.key()] = *it;
    }

    // Only what a sink, or the target, needs (3.4).
    QStringList roots;
    if (!m_options.target.isEmpty()) {
        if (live.contains(m_options.target))
            roots << m_options.target;
    } else {
        for (const Flat &flat : live)
            if (flat.spec().sink)
                roots << flat.path;
    }
    QSet<QString> needed;
    while (!roots.isEmpty()) {
        const QString path = roots.takeLast();
        if (needed.contains(path) || !live.contains(path))
            continue;
        needed.insert(path);
        roots << sources(live[path].inputs);
    }
    for (const Flat &flat : live) {
        if (!needed.contains(flat.path)) {
            graph.skipped[flat.path] = QStringLiteral("not_part_of_output");
            if (m_options.target.isEmpty())
                warn(nodeError(QStringLiteral("not_part_of_output"), flat.path,
                               QStringLiteral("Node %1 is not part of any output: no sink depends on it.")
                                   .arg(flat.path)));
            continue;
        }
        CompiledNode node;
        node.type = flat.spec().id;
        node.version = flat.spec().version;
        node.inputs = flat.inputs;
        node.sink = flat.spec().sink;
        node.provider = flat.provider;
        node.frozen = flat.frozen;
        graph.nodes[flat.path] = node;
    }
    return graph;
}

Compilation Compiler::run() {
    checkRecursion();
    m_scopes.push_back(Scope{});
    Scope *top = &m_scopes.back();
    top->nodes = &m_doc.graph.nodes;
    top->provider = m_options.provider.isEmpty() ? m_doc.graph.provider : m_options.provider;
    build(top);
    check(top);
    checkProviders();
    checkTarget();

    // Cycles are looked for even when there are other errors: links that did
    // not resolve are not in the flattened graph, so what is there is sound.
    const bool cyclic = findCycles();
    Compilation result;
    if (!hasErrors(m_errors) && !cyclic) {
        CompiledGraph graph = shape();
        if (!hasErrors(m_errors))
            result.graph = std::move(graph);
    }
    result.errors = m_errors;
    return result;
}

// --- reading compiled.json ---------------------------------------------------

bool isPathId(const QString &text) {
    for (const QString &segment : text.split(QLatin1Char('/')))
        if (!isNodeId(segment))
            return false;
    return true;
}

bool isValueJson(const QJsonValue &value, bool allowList) {
    if (value.isString() || value.isDouble() || value.isBool())
        return true;
    if (value.isArray()) {
        if (!allowList)
            return false;
        for (const QJsonValue item : value.toArray())
            if (!isValueJson(item, false))
                return false;
        return true;
    }
    if (!value.isObject())
        return false;
    const QJsonObject object = value.toObject();
    if (object.contains("from")) {
        const std::optional<PortRef> ref = PortRef::parse(object.value("from").toString());
        return object.value("from").isString() && ref && isPathId(ref->node) && isPortName(ref->port);
    }
    if (object.contains("auto"))
        return object.value("auto") == QJsonValue("random");
    return object.value("resource").isString();
}

} // namespace

// --- CompiledNode and CompiledGraph ------------------------------------------

QList<CompiledLink> CompiledNode::links() const {
    QList<CompiledLink> result;
    for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it) {
        if (it->kind() == InputValue::Kind::Link)
            result << CompiledLink{it.key(), -1, it->from()};
        if (it->kind() == InputValue::Kind::List) {
            const QList<InputValue> items = it->items();
            for (qsizetype i = 0; i < items.size(); ++i)
                if (items[i].kind() == InputValue::Kind::Link)
                    result << CompiledLink{it.key(), int(i), items[i].from()};
        }
    }
    return result;
}

QStringList CompiledGraph::sinks() const {
    QStringList result;
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it)
        if (it->sink)
            result << it.key();
    return result;
}

QStringList CompiledGraph::dependencies(const QString &path) const {
    return sources(nodes.value(path).inputs);
}

QList<CompiledGraph::Consumer> CompiledGraph::consumers(const QString &path) const {
    QList<Consumer> result;
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it)
        for (const CompiledLink &link : it->links())
            if (link.from.node == path)
                result << Consumer{it.key(), link};
    return result;
}

QStringList CompiledGraph::order() const {
    QMap<QString, int> waiting; // dependencies not yet placed
    QMap<QString, QStringList> fed; // node -> the nodes that read it
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it) {
        int count = 0;
        for (const QString &dep : dependencies(it.key()))
            if (nodes.contains(dep)) {
                ++count;
                fed[dep] << it.key();
            }
        waiting[it.key()] = count;
    }
    std::set<QString> ready;
    for (auto it = waiting.constBegin(); it != waiting.constEnd(); ++it)
        if (*it == 0)
            ready.insert(it.key());
    QStringList result;
    while (!ready.empty()) {
        const QString path = *ready.begin();
        ready.erase(ready.begin());
        result << path;
        for (const QString &node : fed.value(path))
            if (--waiting[node] == 0)
                ready.insert(node);
    }
    return result;
}

QJsonObject CompiledGraph::toJson() const {
    QJsonObject jsonNodes;
    for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it) {
        QJsonObject jsonInputs;
        for (auto input = it->inputs.constBegin(); input != it->inputs.constEnd(); ++input)
            jsonInputs[input.key()] = input->toJson();
        QJsonObject node{{"type", it->type}, {"version", it->version.toString()}, {"inputs", jsonInputs}};
        if (it->sink)
            node["sink"] = true;
        if (!it->provider.isEmpty())
            node["provider"] = it->provider;
        if (!it->frozen.isEmpty())
            node["frozen"] = it->frozen;
        jsonNodes[it.key()] = node;
    }
    QJsonObject jsonSkipped;
    for (auto it = skipped.constBegin(); it != skipped.constEnd(); ++it)
        jsonSkipped[it.key()] = *it;
    QJsonObject result{{"fugue-compiled", format}, {"source", source}, {"nodes", jsonNodes}, {"skipped", jsonSkipped}};
    if (!target.isEmpty())
        result["target"] = target;
    return result;
}

std::optional<CompiledGraph> CompiledGraph::fromJson(const QJsonObject &json, Errors &errors) {
    const qsizetype before = errors.size();
    const auto fail = [&](const QString &message, const QString &node = {}, const QString &input = {}) {
        Error error = makeError(QStringLiteral("invalid_document"), message);
        error.node = node;
        error.input = input;
        errors << error;
    };
    const QJsonValue formatValue = json.value("fugue-compiled");
    if (formatValue.isDouble() && formatValue.toDouble() > format) {
        errors << makeError(QStringLiteral("unsupported_format_version"),
                            QStringLiteral("This compiled graph has format %1; this Fugue reads format %2.")
                                .arg(shown(formatValue))
                                .arg(format),
                            QStringLiteral("Update Fugue, or compile the workflow again with this version."));
        return std::nullopt;
    }
    if (formatValue != QJsonValue(format))
        fail(QStringLiteral("\"fugue-compiled\" must be %1, not %2.").arg(format).arg(shown(formatValue)));

    CompiledGraph graph;
    if (!json.value("source").isString())
        fail(QStringLiteral("\"source\" must be the workflow's hash."));
    graph.source = json.value("source").toString();
    if (!json.value("target").isUndefined() && !isPathId(json.value("target").toString()))
        fail(QStringLiteral("\"target\" must be a path id, not %1.").arg(shown(json.value("target"))));
    graph.target = json.value("target").toString();

    if (!json.value("nodes").isObject())
        fail(QStringLiteral("\"nodes\" must be an object keyed by path id."));
    const QJsonObject jsonNodes = json.value("nodes").toObject();
    for (auto it = jsonNodes.constBegin(); it != jsonNodes.constEnd(); ++it) {
        const QString path = it.key();
        if (!isPathId(path) || !it->isObject()) {
            fail(QStringLiteral("Node %1 must be an object keyed by a path id.").arg(path), path);
            continue;
        }
        const QJsonObject o = it->toObject();
        CompiledNode node;
        node.type = o.value("type").toString();
        if (!isTypeId(node.type))
            fail(QStringLiteral("Node %1: \"type\" must be a node type id, not %2.").arg(path, shown(o.value("type"))),
                 path);
        if (const std::optional<Version> version = Version::parse(o.value("version").toString()))
            node.version = *version;
        else
            fail(QStringLiteral("Node %1: \"version\" must be a semver version, not %2.")
                     .arg(path, shown(o.value("version"))),
                 path);
        if (!o.value("inputs").isObject())
            fail(QStringLiteral("Node %1: \"inputs\" must be an object.").arg(path), path);
        const QJsonObject jsonInputs = o.value("inputs").toObject();
        for (auto input = jsonInputs.constBegin(); input != jsonInputs.constEnd(); ++input) {
            if (!isPortName(input.key()) || !isValueJson(*input, true))
                fail(QStringLiteral("Node %1: input %2 has an invalid value %3.").arg(path, input.key(), shown(*input)),
                     path, input.key());
            else
                node.inputs[input.key()] = InputValue::fromJson(*input);
        }
        for (const char *key : {"provider", "frozen"})
            if (!o.value(QLatin1String(key)).isUndefined() && !o.value(QLatin1String(key)).isString())
                fail(QStringLiteral("Node %1: \"%2\" must be a string.").arg(path, QLatin1String(key)), path);
        if (!o.value("sink").isUndefined() && !o.value("sink").isBool())
            fail(QStringLiteral("Node %1: \"sink\" must be true or false.").arg(path), path);
        node.sink = o.value("sink").toBool();
        node.provider = o.value("provider").toString();
        node.frozen = o.value("frozen").toString();
        graph.nodes[path] = node;
    }

    static const QStringList reasons{"muted", "upstream_muted", "upstream_empty", "not_part_of_output"};
    if (!json.value("skipped").isUndefined() && !json.value("skipped").isObject())
        fail(QStringLiteral("\"skipped\" must be an object keyed by path id."));
    const QJsonObject jsonSkipped = json.value("skipped").toObject();
    for (auto it = jsonSkipped.constBegin(); it != jsonSkipped.constEnd(); ++it) {
        if (!isPathId(it.key()) || !reasons.contains(it->toString())) {
            Error error = makeError(QStringLiteral("invalid_document"),
                                    QStringLiteral("Skipped node %1 has reason %2, which is not one of the reasons.")
                                        .arg(it.key(), shown(*it)));
            error.node = it.key();
            error.valid = QJsonArray::fromStringList(reasons);
            errors << error;
            continue;
        }
        graph.skipped[it.key()] = it->toString();
    }

    if (errors.size() > before)
        return std::nullopt;
    return graph;
}

Compilation compile(const Document &document, const Registry &registry, const CompileOptions &options) {
    return Compiler(document, registry, options).run();
}

} // namespace fugue
