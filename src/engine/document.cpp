#include "document.h"

#include "engine/canonical.h"
#include "engine/hash.h"
#include "engine/ids.h"
#include "engine/json.h"
#include "engine/port_type.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>

#include <cmath>

namespace fugue {

namespace {

const QString invalidDocument = QStringLiteral("invalid_document");

QString shown(const QJsonValue &value) {
    if (value.isUndefined())
        return QStringLiteral("nothing");
    return QString::fromUtf8(canonicalJson(value).value_or("?"));
}

// Where an error is: the message prefix and the node it is about.
struct Place {
    QString prefix; // "Group local/x: " inside a group definition, else empty
    QString node;   // the node id, or empty
    QString input;
    QString output;

    QString nodeText() const {
        return prefix + (prefix.isEmpty() ? QStringLiteral("Node ") : QStringLiteral("node ")) + node +
               QStringLiteral(": ");
    }
};

class Checker {
public:
    explicit Checker(Errors &errors) : m_errors(errors) {}

    void fail(const Place &place, const QString &message, const QJsonArray &valid = {}) {
        Error error;
        error.code = invalidDocument;
        error.node = place.node;
        error.input = place.input;
        error.output = place.output;
        error.message = message;
        error.valid = valid;
        m_errors << error;
        m_failed = true;
    }

    void fail(const QString &message) { fail(Place{}, message); }

    bool failed() const { return m_failed; }
    qsizetype count() const { return m_errors.size(); }

private:
    Errors &m_errors;
    bool m_failed = false;
};

bool isWhole(const QJsonValue &v) {
    return v.isDouble() && v.toDouble() == std::trunc(v.toDouble());
}

// --- input values -----------------------------------------------------------

std::optional<InputValue> readValue(Checker &c, const Place &place, const QJsonValue &v, bool inList) {
    const QString at = place.node.isEmpty() ? place.prefix + QStringLiteral("Exposed input ") + place.input +
                                                  QStringLiteral(": ")
                                            : place.nodeText() + QStringLiteral("input ") + place.input +
                                                  QStringLiteral(": ");
    if (v.isNull()) {
        c.fail(place, at + QStringLiteral("null is not a value; leave the input out."));
        return std::nullopt;
    }
    if (v.isBool() || v.isDouble() || v.isString())
        return InputValue::fromJson(v);
    if (v.isArray()) {
        if (inList) {
            c.fail(place, at + QStringLiteral("a list cannot hold a list."));
            return std::nullopt;
        }
        bool ok = true;
        for (const QJsonValue item : v.toArray())
            ok &= readValue(c, place, item, true).has_value();
        return ok ? std::optional<InputValue>(InputValue::fromJson(v)) : std::nullopt;
    }

    const QJsonObject o = v.toObject();
    const auto onlyKey = [&](const char *key) {
        bool ok = true;
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key() != QLatin1String(key)) {
                c.fail(place, at + QStringLiteral("an object with \"%1\" has no other keys; remove \"%2\".")
                                       .arg(QLatin1String(key), it.key()));
                ok = false;
            }
        return ok;
    };

    if (o.contains("from")) {
        const std::optional<PortRef> from = PortRef::parse(o.value("from").toString());
        bool ok = onlyKey("from");
        if (!o.value("from").isString() || !from || !isNodeId(from->node) || !isPortName(from->port)) {
            c.fail(place, at + QStringLiteral("\"from\" must be \"<node id>.<output name>\", not %1.")
                                   .arg(shown(o.value("from"))));
            ok = false;
        }
        return ok ? std::optional<InputValue>(InputValue::fromJson(v)) : std::nullopt;
    }
    if (o.contains("auto")) {
        bool ok = onlyKey("auto");
        if (o.value("auto") != QJsonValue("random")) {
            c.fail(place, at + QStringLiteral("\"auto\" must be \"random\", not %1.").arg(shown(o.value("auto"))),
                   QJsonArray{"random"});
            ok = false;
        }
        return ok ? std::optional<InputValue>(InputValue::fromJson(v)) : std::nullopt;
    }
    if (o.contains("resource")) {
        bool ok = true;
        const std::optional<PortType> type = PortType::parse(o.value("resource").toString());
        if (!o.value("resource").isString() || !type || type->isBuiltin() || type->isAny()) {
            c.fail(place, at + QStringLiteral("\"resource\" must be a declared resource type id, not %1.")
                                   .arg(shown(o.value("resource"))));
            ok = false;
        }
        if (o.contains("hash") && !isSha256(o.value("hash").toString())) {
            c.fail(place, at + QStringLiteral("\"hash\" must be \"sha256:\" and 64 lowercase hex digits, not %1.")
                                   .arg(shown(o.value("hash"))));
            ok = false;
        }
        if (o.contains("name") && !o.value("name").isString()) {
            c.fail(place, at + QStringLiteral("a resource handle's \"name\" must be a string."));
            ok = false;
        }
        for (const char *key : {"derived", "needs"})
            if (o.contains(QLatin1String(key)) && !o.value(QLatin1String(key)).isObject()) {
                c.fail(place, at + QStringLiteral("a resource handle's \"%1\" must be an object.")
                                       .arg(QLatin1String(key)));
                ok = false;
            }
        if (o.contains("path")) {
            c.fail(place, at + QStringLiteral("a resource handle never holds a path; Fugue finds the file by "
                                              "its hash. Remove \"path\"."));
            ok = false;
        }
        return ok ? std::optional<InputValue>(InputValue::fromJson(v)) : std::nullopt;
    }
    c.fail(place, at + QStringLiteral("an object value must be a link {\"from\": ...}, {\"auto\": \"random\"} "
                                      "or a resource handle {\"resource\": ...}, not %1.")
                           .arg(shown(v)));
    return std::nullopt;
}

// --- nodes ------------------------------------------------------------------

std::optional<Node> readNode(Checker &c, const QString &prefix, const QString &id, const QJsonValue &v) {
    const Place place{prefix, id, {}, {}};
    if (!v.isObject()) {
        c.fail(place, place.nodeText().chopped(2) + QStringLiteral(" must be an object."));
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    const QString at = place.nodeText();
    bool ok = true;
    const auto fail = [&](const QString &message, const QJsonArray &valid = {}) {
        c.fail(place, at + message, valid);
        ok = false;
    };

    Node node;
    node.type = o.value("type").toString();
    if (!isTypeId(node.type))
        fail(QStringLiteral("\"type\" must be a node type or group id such as \"fugue/text\", not %1.")
                 .arg(shown(o.value("type"))));
    if (const std::optional<Version> version = Version::parse(o.value("version").toString()))
        node.version = *version;
    else
        fail(QStringLiteral("\"version\" must be a semver version such as \"1.0.0\", not %1.")
                 .arg(shown(o.value("version"))));

    if (o.contains("mode")) {
        // "normal" is the absence of `mode`, never written.
        const std::optional<NodeMode> mode = parseNodeMode(o.value("mode").toString());
        if (mode && *mode != NodeMode::Normal)
            node.mode = *mode;
        else
            fail(QStringLiteral("\"mode\" must be \"bypass\" or \"mute\", not %1; leave it out for normal.")
                     .arg(shown(o.value("mode"))),
                 QJsonArray{nodeModeName(NodeMode::Bypass), nodeModeName(NodeMode::Mute)});
    }
    for (const auto &[key, out] : {std::pair<const char *, QString *>{"provider", &node.provider},
                                   std::pair<const char *, QString *>{"frozen", &node.frozen}}) {
        const QJsonValue value = o.value(QLatin1String(key));
        if (value.isUndefined())
            continue;
        if (!value.isString() || value.toString().isEmpty())
            fail(QStringLiteral("\"%1\" must be a non-empty string, not %2.").arg(QLatin1String(key), shown(value)));
        *out = value.toString();
    }
    const QJsonValue inputs = o.value("inputs");
    if (!inputs.isUndefined() && !inputs.isObject())
        fail(QStringLiteral("\"inputs\" must be an object keyed by input name."));
    const QJsonObject inputObject = inputs.toObject();
    for (auto it = inputObject.begin(); it != inputObject.end(); ++it) {
        Place inputPlace = place;
        inputPlace.input = it.key();
        if (!isPortName(it.key())) {
            c.fail(inputPlace, at + QStringLiteral("input name \"%1\" must match [a-z][a-z0-9_]*.").arg(it.key()));
            ok = false;
            continue;
        }
        if (const std::optional<InputValue> value = readValue(c, inputPlace, it.value(), false))
            node.inputs.insert(it.key(), *value);
        else
            ok = false;
    }

    for (auto it = o.begin(); it != o.end(); ++it)
        if (!QStringList{"type", "version", "inputs", "mode", "provider", "frozen"}.contains(it.key()))
            node.extra.insert(it.key(), it.value());
    return ok ? std::optional<Node>(node) : std::nullopt;
}

Nodes readNodes(Checker &c, const QString &prefix, const QJsonValue &v, const QString &where) {
    Nodes nodes;
    if (!v.isObject()) {
        c.fail(prefix + QStringLiteral("\"%1\" must be an object keyed by node id.").arg(where));
        return nodes;
    }
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!isNodeId(it.key())) {
            c.fail(prefix + QStringLiteral("Node id \"%1\" must match [a-z][a-z0-9_-]{0,31}; '.' and '/' are "
                                           "separators.")
                                .arg(it.key()));
            continue;
        }
        if (const std::optional<Node> node = readNode(c, prefix, it.key(), it.value()))
            nodes.insert(it.key(), *node);
    }
    return nodes;
}

// --- groups -----------------------------------------------------------------

bool isInnerRef(const std::optional<PortRef> &ref) {
    return ref && isNodeId(ref->node) && isPortName(ref->port);
}

std::optional<GroupDefinition> readGroup(Checker &c, const QString &id, const QJsonValue &v) {
    const QString prefix = QStringLiteral("Group ") + id + QStringLiteral(": ");
    if (!v.isObject()) {
        c.fail(QStringLiteral("Group %1 must be an object.").arg(id));
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    bool ok = true;
    const auto fail = [&](const Place &place, const QString &message) {
        c.fail(place, prefix + message);
        ok = false;
    };

    GroupDefinition group;
    if (const std::optional<Version> version = Version::parse(o.value("version").toString()))
        group.version = *version;
    else
        fail({}, QStringLiteral("\"version\" must be a semver version such as \"1.0.0\", not %1.")
                     .arg(shown(o.value("version"))));
    group.name = o.value("name").toString();
    if (!o.value("name").isString() || group.name.isEmpty())
        fail({}, QStringLiteral("\"name\" must be a display name."));
    for (const auto &[key, out] : {std::pair<const char *, QString *>{"description", &group.description},
                                   std::pair<const char *, QString *>{"category", &group.category}}) {
        const QJsonValue value = o.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isString())
            fail({}, QStringLiteral("\"%1\" must be a string.").arg(QLatin1String(key)));
        *out = value.toString();
    }

    // Exposed inputs.
    if (!o.value("inputs").isArray())
        fail({}, QStringLiteral("\"inputs\" must be a list of exposed inputs (it may be empty)."));
    QSet<QString> seen;
    for (const QJsonValue entry : o.value("inputs").toArray()) {
        const QJsonObject e = entry.toObject();
        GroupInput input;
        input.name = e.value("name").toString();
        const Place place{prefix, {}, input.name, {}};
        const QString at = QStringLiteral("exposed input ") + input.name + QStringLiteral(": ");
        bool inputOk = true;
        const auto inputFail = [&](const QString &message) {
            fail(place, at + message);
            inputOk = false;
        };
        if (!entry.isObject() || !isPortName(input.name)) {
            fail(place, QStringLiteral("an exposed input needs a \"name\" matching [a-z][a-z0-9_]*, not %1.")
                            .arg(shown(e.value("name"))));
            continue;
        }
        if (seen.contains(input.name))
            inputFail(QStringLiteral("the name is used more than once."));
        seen.insert(input.name);

        const QJsonValue targets = e.value("targets");
        if (!targets.isArray() || targets.toArray().isEmpty())
            inputFail(QStringLiteral("\"targets\" must list at least one \"<inner node id>.<input name>\"."));
        for (const QJsonValue target : targets.toArray()) {
            const std::optional<PortRef> ref = PortRef::parse(target.toString());
            if (!target.isString() || !isInnerRef(ref))
                inputFail(QStringLiteral("target %1 must be \"<inner node id>.<input name>\".").arg(shown(target)));
            else
                input.targets << *ref;
        }
        for (const auto &[key, out] : {std::pair<const char *, QString *>{"label", &input.label},
                                       std::pair<const char *, QString *>{"section", &input.section}}) {
            const QJsonValue value = e.value(QLatin1String(key));
            if (!value.isUndefined() && !value.isString())
                inputFail(QStringLiteral("\"%1\" must be a string.").arg(QLatin1String(key)));
            *out = value.toString();
        }
        for (const auto &[key, out] : {std::pair<const char *, std::optional<double> *>{"min", &input.min},
                                       std::pair<const char *, std::optional<double> *>{"max", &input.max}}) {
            const QJsonValue value = e.value(QLatin1String(key));
            if (value.isUndefined())
                continue;
            if (!value.isDouble())
                inputFail(QStringLiteral("\"%1\" must be a number.").arg(QLatin1String(key)));
            else
                *out = value.toDouble();
        }
        if (e.contains("choices")) {
            QStringList choices;
            bool strings = e.value("choices").isArray();
            for (const QJsonValue choice : e.value("choices").toArray()) {
                strings &= choice.isString();
                choices << choice.toString();
            }
            if (!strings || choices.isEmpty())
                inputFail(QStringLiteral("\"choices\" must be a non-empty list of strings."));
            input.choices = choices;
        }
        if (e.contains("default")) {
            const std::optional<InputValue> value = readValue(c, place, e.value("default"), false);
            if (!value)
                inputOk = ok = false;
            else if (value->kind() == InputValue::Kind::Link)
                inputFail(QStringLiteral("a default cannot be a link."));
            input.defaultValue = value;
        }
        for (auto it = e.begin(); it != e.end(); ++it)
            if (!QStringList{"name", "targets", "label", "section", "default", "min", "max", "choices"}.contains(
                    it.key()))
                input.extra.insert(it.key(), it.value());
        if (inputOk)
            group.inputs << input;
    }

    // Exposed outputs.
    if (!o.value("outputs").isArray())
        fail({}, QStringLiteral("\"outputs\" must be a list of exposed outputs (it may be empty)."));
    seen.clear();
    for (const QJsonValue entry : o.value("outputs").toArray()) {
        const QJsonObject e = entry.toObject();
        GroupOutput output;
        output.name = e.value("name").toString();
        const Place place{prefix, {}, {}, output.name};
        const QString at = QStringLiteral("exposed output ") + output.name + QStringLiteral(": ");
        if (!entry.isObject() || !isPortName(output.name)) {
            fail(place, QStringLiteral("an exposed output needs a \"name\" matching [a-z][a-z0-9_]*, not %1.")
                            .arg(shown(e.value("name"))));
            continue;
        }
        if (seen.contains(output.name))
            fail(place, at + QStringLiteral("the name is used more than once."));
        seen.insert(output.name);
        const std::optional<PortRef> from = PortRef::parse(e.value("from").toString());
        if (!e.value("from").isString() || !isInnerRef(from)) {
            fail(place, at + QStringLiteral("\"from\" must be \"<inner node id>.<output name>\", not %1.")
                                 .arg(shown(e.value("from"))));
            continue;
        }
        output.from = *from;
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.key() != "name" && it.key() != "from")
                output.extra.insert(it.key(), it.value());
        group.outputs << output;
    }

    const qsizetype before = c.count();
    group.nodes = readNodes(c, prefix, o.value("nodes"), QStringLiteral("nodes"));
    if (c.count() != before)
        ok = false;

    for (auto it = o.begin(); it != o.end(); ++it)
        if (!QStringList{"version", "name", "description", "category", "inputs", "outputs", "nodes"}.contains(
                it.key()))
            group.extra.insert(it.key(), it.value());
    return ok ? std::optional<GroupDefinition>(group) : std::nullopt;
}

// --- layout and requires: shape only ------------------------------------------

bool isNumbers(const QJsonValue &v, int count) {
    const QJsonArray array = v.toArray();
    if (!v.isArray() || array.size() != count)
        return false;
    for (const QJsonValue item : array)
        if (!item.isDouble())
            return false;
    return true;
}

class ShapeCheck {
public:
    explicit ShapeCheck(Checker &c) : m_c(c) {}

    // Checks o[key] when present; `test` says whether the value is right.
    template <typename Test>
    void field(const QJsonObject &o, const QString &path, const char *key, Test test, const char *what) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isUndefined() && !test(v))
            m_c.fail(QStringLiteral("%1.%2 must be %3.").arg(path, QLatin1String(key), QLatin1String(what)));
    }

    bool object(const QJsonValue &v, const QString &path) {
        if (v.isObject())
            return true;
        m_c.fail(QStringLiteral("%1 must be an object.").arg(path));
        return false;
    }

    bool list(const QJsonValue &v, const QString &path) {
        if (v.isArray())
            return true;
        m_c.fail(QStringLiteral("%1 must be a list.").arg(path));
        return false;
    }

    void layout(const QJsonObject &o, const QString &path, bool top) {
        const auto isPair = [](const QJsonValue &v) { return isNumbers(v, 2); };
        const auto isString = [](const QJsonValue &v) { return v.isString(); };
        const auto isBool = [](const QJsonValue &v) { return v.isBool(); };
        const auto isStrings = [](const QJsonValue &v) {
            if (!v.isArray())
                return false;
            for (const QJsonValue item : v.toArray())
                if (!item.isString())
                    return false;
            return true;
        };

        if (o.contains("nodes") && object(o.value("nodes"), path + ".nodes")) {
            const QJsonObject nodes = o.value("nodes").toObject();
            for (auto it = nodes.begin(); it != nodes.end(); ++it) {
                const QString at = path + ".nodes." + it.key();
                if (!object(it.value(), at))
                    continue;
                const QJsonObject n = it.value().toObject();
                field(n, at, "pos", isPair, "a pair of numbers [x, y]");
                field(n, at, "size", isPair, "a pair of numbers [width, height]");
                field(n, at, "collapsed", isBool, "true or false");
                field(n, at, "pinned", isBool, "true or false");
                field(n, at, "color", isString, "a theme role name");
                field(n, at, "title", isString, "a string");
            }
        }
        if (o.contains("frames") && list(o.value("frames"), path + ".frames")) {
            const QJsonArray frames = o.value("frames").toArray();
            for (qsizetype i = 0; i < frames.size(); ++i) {
                const QString at = QStringLiteral("%1.frames[%2]").arg(path).arg(i);
                if (!object(frames.at(i), at))
                    continue;
                const QJsonObject f = frames.at(i).toObject();
                field(f, at, "id", isString, "a string");
                field(f, at, "title", isString, "a string");
                field(f, at, "color", isString, "a theme role name");
                field(f, at, "rect", [](const QJsonValue &v) { return isNumbers(v, 4); },
                      "four numbers [x, y, width, height]");
                field(f, at, "nodes", isStrings, "a list of node ids");
            }
        }
        if (o.contains("links") && object(o.value("links"), path + ".links")) {
            const QJsonObject links = o.value("links").toObject();
            for (auto it = links.begin(); it != links.end(); ++it) {
                const QString at = path + ".links." + it.key();
                if (!object(it.value(), at))
                    continue;
                field(it.value().toObject(), at, "points",
                      [&](const QJsonValue &v) {
                          if (!v.isArray())
                              return false;
                          for (const QJsonValue point : v.toArray())
                              if (!isPair(point))
                                  return false;
                          return true;
                      },
                      "a list of [x, y] points");
            }
        }
        if (o.contains("comments") && list(o.value("comments"), path + ".comments")) {
            const QJsonArray comments = o.value("comments").toArray();
            for (qsizetype i = 0; i < comments.size(); ++i) {
                const QString at = QStringLiteral("%1.comments[%2]").arg(path).arg(i);
                if (!object(comments.at(i), at))
                    continue;
                const QJsonObject n = comments.at(i).toObject();
                field(n, at, "id", isString, "a string");
                field(n, at, "text", isString, "a string");
                field(n, at, "pos", isPair, "a pair of numbers [x, y]");
                field(n, at, "size", isPair, "a pair of numbers [width, height]");
            }
        }
        if (!top)
            return;
        if (o.contains("view") && object(o.value("view"), path + ".view")) {
            const QJsonObject view = o.value("view").toObject();
            field(view, path + ".view", "center", isPair, "a pair of numbers [x, y]");
            field(view, path + ".view", "zoom",
                  [](const QJsonValue &v) { return v.isDouble() && v.toDouble() > 0; }, "a number above 0");
        }
        if (o.contains("groups") && object(o.value("groups"), path + ".groups")) {
            const QJsonObject groups = o.value("groups").toObject();
            for (auto it = groups.begin(); it != groups.end(); ++it)
                if (object(it.value(), path + ".groups." + it.key()))
                    layout(it.value().toObject(), path + ".groups." + it.key(), false);
        }
    }

    void requirements(const QJsonObject &o) {
        const auto isVersion = [](const QJsonValue &v) { return Version::parse(v.toString()).has_value(); };
        field(o, "requires", "fugue", isVersion, "the version of Fugue, such as \"0.1.0\"");
        if (o.contains("nodes") && object(o.value("nodes"), "requires.nodes")) {
            const QJsonObject nodes = o.value("nodes").toObject();
            for (auto it = nodes.begin(); it != nodes.end(); ++it)
                if (!isTypeId(it.key()) || !isVersion(it.value()))
                    m_c.fail(QStringLiteral("requires.nodes.%1 must map a node type id to a version such as "
                                            "\"1.0.0\".")
                                 .arg(it.key()));
        }
        if (o.contains("packs") && object(o.value("packs"), "requires.packs")) {
            const QJsonObject packs = o.value("packs").toObject();
            for (auto it = packs.begin(); it != packs.end(); ++it) {
                const QString at = "requires.packs." + it.key();
                if (!object(it.value(), at))
                    continue;
                field(it.value().toObject(), at, "version", isVersion, "a version such as \"1.0.0\"");
                field(it.value().toObject(), at, "source", [](const QJsonValue &v) { return v.isString(); },
                      "a string");
            }
        }
        if (o.contains("models") && list(o.value("models"), "requires.models")) {
            const QJsonArray models = o.value("models").toArray();
            for (qsizetype i = 0; i < models.size(); ++i) {
                const QString at = QStringLiteral("requires.models[%1]").arg(i);
                if (!object(models.at(i), at))
                    continue;
                const QJsonObject m = models.at(i).toObject();
                field(m, at, "hash", [](const QJsonValue &v) { return isSha256(v.toString()); },
                      "\"sha256:\" and 64 lowercase hex digits");
                for (const char *key : {"resource", "name", "source"})
                    field(m, at, key, [](const QJsonValue &v) { return v.isString(); }, "a string");
            }
        }
    }

private:
    Checker &m_c;
};

// --- writing ------------------------------------------------------------------

QJsonObject nodeJson(const Node &node) {
    QJsonObject o = node.extra;
    o.insert("type", node.type);
    o.insert("version", node.version.toString());
    QJsonObject inputs;
    for (auto it = node.inputs.begin(); it != node.inputs.end(); ++it)
        inputs.insert(it.key(), it.value().toJson());
    o.insert("inputs", inputs);
    if (node.mode != NodeMode::Normal)
        o.insert("mode", nodeModeName(node.mode));
    if (!node.provider.isEmpty())
        o.insert("provider", node.provider);
    if (!node.frozen.isEmpty())
        o.insert("frozen", node.frozen);
    return o;
}

QJsonObject nodesJson(const Nodes &nodes) {
    QJsonObject o;
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
        o.insert(it.key(), nodeJson(it.value()));
    return o;
}

QJsonObject graphJson(const Graph &graph) {
    QJsonObject o = graph.extra;
    o.insert("nodes", nodesJson(graph.nodes));
    if (!graph.provider.isEmpty())
        o.insert("provider", graph.provider);
    return o;
}

QJsonObject groupJson(const GroupDefinition &group) {
    QJsonObject o = group.extra;
    o.insert("version", group.version.toString());
    o.insert("name", group.name);
    if (!group.description.isEmpty())
        o.insert("description", group.description);
    if (!group.category.isEmpty())
        o.insert("category", group.category);
    QJsonArray inputs;
    for (const GroupInput &input : group.inputs) {
        QJsonObject i = input.extra;
        i.insert("name", input.name);
        QJsonArray targets;
        for (const PortRef &target : input.targets)
            targets << target.toString();
        i.insert("targets", targets);
        if (!input.label.isEmpty())
            i.insert("label", input.label);
        if (!input.section.isEmpty())
            i.insert("section", input.section);
        if (input.defaultValue)
            i.insert("default", input.defaultValue->toJson());
        if (input.min)
            i.insert("min", *input.min);
        if (input.max)
            i.insert("max", *input.max);
        if (input.choices)
            i.insert("choices", QJsonArray::fromStringList(*input.choices));
        inputs << i;
    }
    o.insert("inputs", inputs);
    QJsonArray outputs;
    for (const GroupOutput &output : group.outputs) {
        QJsonObject out = output.extra;
        out.insert("name", output.name);
        out.insert("from", output.from.toString());
        outputs << out;
    }
    o.insert("outputs", outputs);
    o.insert("nodes", nodesJson(group.nodes));
    return o;
}

QJsonObject groupsJson(const QMap<QString, GroupDefinition> &groups) {
    QJsonObject o;
    for (auto it = groups.begin(); it != groups.end(); ++it)
        o.insert(it.key(), groupJson(it.value()));
    return o;
}

// --- Unicode ------------------------------------------------------------------

bool validUnicode(const QString &text) {
    return canonicalJson(QJsonValue(text)).has_value();
}

// Every place in `v` that holds a lone surrogate, as a dotted path.
void findLoneSurrogates(const QJsonValue &v, const QString &path, Errors &errors) {
    const auto report = [&](const QString &where) {
        Error error;
        error.code = invalidDocument;
        error.message = QStringLiteral("The text at %1 is not valid Unicode (a lone UTF-16 surrogate).").arg(where);
        errors << error;
    };
    if (v.isString() && !validUnicode(v.toString())) {
        report(path);
    } else if (v.isArray()) {
        const QJsonArray array = v.toArray();
        for (qsizetype i = 0; i < array.size(); ++i)
            findLoneSurrogates(array.at(i), QStringLiteral("%1[%2]").arg(path).arg(i), errors);
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (!validUnicode(it.key())) {
                report(QStringLiteral("a key in ") + (path.isEmpty() ? QStringLiteral("the top level") : path));
                continue;
            }
            findLoneSurrogates(it.value(), path.isEmpty() ? it.key() : path + u'.' + it.key(), errors);
        }
    }
}

// --- requirements -------------------------------------------------------------

// Every handle with a hash anywhere inside `v` (derived handles nest them).
void collectModels(const QJsonValue &v, QMap<QString, QJsonObject> &models) {
    if (v.isArray()) {
        for (const QJsonValue item : v.toArray())
            collectModels(item, models);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    const QString hash = o.value("hash").toString();
    if (o.value("resource").isString() && isSha256(hash) && !models.contains(hash)) {
        QJsonObject entry{{"resource", o.value("resource")}, {"hash", hash}};
        if (o.value("name").isString())
            entry.insert("name", o.value("name"));
        models.insert(hash, entry);
    }
    for (auto it = o.begin(); it != o.end(); ++it)
        collectModels(it.value(), models);
}

void collectResources(const InputValue &value, QMap<QString, QJsonObject> &models) {
    if (value.kind() == InputValue::Kind::Resource)
        collectModels(value.handle(), models);
    else if (value.kind() == InputValue::Kind::List)
        for (const InputValue &item : value.items())
            collectResources(item, models);
}

} // namespace

// --- PortRef and InputValue ---------------------------------------------------

std::optional<PortRef> PortRef::parse(const QString &text) {
    const QStringList parts = text.split(u'.');
    if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty())
        return std::nullopt;
    return PortRef{parts[0], parts[1]};
}

QString PortRef::toString() const {
    return node + u'.' + port;
}

InputValue InputValue::literal(const QJsonValue &value) {
    return InputValue(Kind::Literal, value);
}

InputValue InputValue::link(const PortRef &from) {
    return InputValue(Kind::Link, QJsonObject{{"from", from.toString()}});
}

InputValue InputValue::autoRandom() {
    return InputValue(Kind::Auto, QJsonObject{{"auto", "random"}});
}

InputValue InputValue::resource(const QJsonObject &handle) {
    return InputValue(Kind::Resource, handle);
}

InputValue InputValue::list(const QList<InputValue> &items) {
    QJsonArray array;
    for (const InputValue &item : items)
        array << item.toJson();
    return InputValue(Kind::List, array);
}

InputValue InputValue::fromJson(const QJsonValue &json) {
    if (json.isArray())
        return InputValue(Kind::List, json);
    if (json.isObject()) {
        const QJsonObject o = json.toObject();
        if (o.contains("from"))
            return InputValue(Kind::Link, json);
        if (o.contains("auto"))
            return InputValue(Kind::Auto, json);
        return InputValue(Kind::Resource, json);
    }
    return InputValue(Kind::Literal, json);
}

QJsonValue InputValue::literal() const {
    return m_kind == Kind::Literal ? m_json : QJsonValue(QJsonValue::Undefined);
}

PortRef InputValue::from() const {
    if (m_kind != Kind::Link)
        return {};
    return PortRef::parse(m_json.toObject().value("from").toString()).value_or(PortRef{});
}

QString InputValue::autoMode() const {
    return m_kind == Kind::Auto ? m_json.toObject().value("auto").toString() : QString();
}

QJsonObject InputValue::handle() const {
    return m_kind == Kind::Resource ? m_json.toObject() : QJsonObject();
}

QList<InputValue> InputValue::items() const {
    QList<InputValue> list;
    if (m_kind == Kind::List)
        for (const QJsonValue item : m_json.toArray())
            list << fromJson(item);
    return list;
}

QString newNodeId(const Nodes &taken) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    QRandomGenerator *random = QRandomGenerator::global();
    while (true) {
        QString id(1, QLatin1Char(letters[random->bounded(26)]));
        for (int i = 1; i < 6; ++i)
            id += QLatin1Char(alphabet[random->bounded(36)]);
        if (!taken.contains(id))
            return id;
    }
}

QString nodeModeName(NodeMode mode) {
    switch (mode) {
    case NodeMode::Bypass:
        return QStringLiteral("bypass");
    case NodeMode::Mute:
        return QStringLiteral("mute");
    case NodeMode::Normal:
        break;
    }
    return QStringLiteral("normal");
}

std::optional<NodeMode> parseNodeMode(const QString &text) {
    for (const NodeMode mode : {NodeMode::Normal, NodeMode::Bypass, NodeMode::Mute})
        if (text == nodeModeName(mode))
            return mode;
    return std::nullopt;
}

// --- Document -----------------------------------------------------------------

const GroupDefinition *Document::group(const QString &id) const {
    const auto found = groups.constFind(id);
    return found == groups.constEnd() ? nullptr : &found.value();
}

std::optional<Document> Document::load(const QByteArray &bytes, Errors &errors) {
    const Result<QJsonDocument> parsed = parseJson(bytes, QStringLiteral("The file"));
    if (!parsed) {
        errors << parsed.error();
        return std::nullopt;
    }
    const QJsonDocument &json = parsed.value();
    Checker c(errors);
    if (!json.isObject()) {
        c.fail(QStringLiteral("A workflow must be a JSON object."));
        return std::nullopt;
    }
    const qsizetype before = errors.size();
    std::optional<Document> document = fromJson(json.object(), errors);
    for (qsizetype i = before; i < errors.size(); ++i)
        if (errors.at(i).code == QLatin1String("unsupported_format_version"))
            return std::nullopt;
    Errors unicode;
    findLoneSurrogates(json.object(), {}, unicode);
    if (unicode.isEmpty())
        return document;
    errors << unicode;
    return std::nullopt;
}

std::optional<Document> Document::fromJson(const QJsonObject &json, Errors &errors) {
    Checker c(errors);
    Document document;

    const QJsonValue format = json.value("fugue");
    if (isWhole(format) && format.toDouble() > 1) {
        Error error;
        error.code = QStringLiteral("unsupported_format_version");
        error.message = QStringLiteral("This workflow uses format %1; this Fugue reads format 1.")
                            .arg(shown(format));
        error.hint = QStringLiteral("Open it with a newer Fugue.");
        errors << error;
        return std::nullopt;
    }
    if (format != QJsonValue(1))
        c.fail(QStringLiteral("\"fugue\" must be the format version 1, not %1.").arg(shown(format)));

    const QJsonValue graph = json.value("graph");
    if (!graph.isObject()) {
        c.fail(QStringLiteral("\"graph\" must be an object with \"nodes\"."));
    } else {
        const QJsonObject g = graph.toObject();
        document.graph.nodes = readNodes(c, {}, g.value("nodes"), QStringLiteral("graph.nodes"));
        const QJsonValue provider = g.value("provider");
        if (!provider.isUndefined() && (!provider.isString() || provider.toString().isEmpty()))
            c.fail(QStringLiteral("\"graph.provider\" must be the name of a provider instance, not %1.")
                       .arg(shown(provider)));
        document.graph.provider = provider.toString();
        for (auto it = g.begin(); it != g.end(); ++it)
            if (it.key() != "nodes" && it.key() != "provider")
                document.graph.extra.insert(it.key(), it.value());
    }

    ShapeCheck shape(c);
    const auto section = [&](const char *key, const char *what) -> std::optional<QJsonObject> {
        const QJsonValue v = json.value(QLatin1String(key));
        if (v.isUndefined())
            return std::nullopt;
        if (!v.isObject()) {
            c.fail(QStringLiteral("\"%1\" must be %2.").arg(QLatin1String(key), QLatin1String(what)));
            return std::nullopt;
        }
        return v.toObject();
    };
    if (const auto groups = section("groups", "an object keyed by group id")) {
        document.present.groups = true;
        for (auto it = groups->begin(); it != groups->end(); ++it) {
            if (!isTypeId(it.key())) {
                c.fail(QStringLiteral("Group id \"%1\" must be \"<namespace>/<name>\", such as "
                                      "\"local/two-pass\".")
                           .arg(it.key()));
                continue;
            }
            if (const std::optional<GroupDefinition> group = readGroup(c, it.key(), it.value()))
                document.groups.insert(it.key(), *group);
        }
    }
    if (const auto layout = section("layout", "an object")) {
        document.present.layout = true;
        document.layout = *layout;
        shape.layout(*layout, QStringLiteral("layout"), true);
    }
    if (const auto requirements = section("requires", "an object")) {
        document.present.requirements = true;
        document.requirements = *requirements;
        shape.requirements(*requirements);
    }
    if (const auto run = section("run", "an object")) {
        document.present.run = true;
        document.run = *run;
    }
    for (auto it = json.begin(); it != json.end(); ++it)
        if (!QStringList{"fugue", "graph", "groups", "layout", "requires", "run"}.contains(it.key()))
            document.extra.insert(it.key(), it.value());

    if (c.failed())
        return std::nullopt;
    return document;
}

std::optional<Document> Document::loadFile(const QString &path, Errors &errors) {
    const QFileInfo info(path);
    if (!info.exists() || info.isDir()) {
        Error error;
        error.code = QStringLiteral("file_not_found");
        error.message = info.isDir() ? QStringLiteral("%1 is a folder, not a workflow file.").arg(path)
                                     : QStringLiteral("There is no file %1.").arg(path);
        errors << error;
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Error error;
        error.code = QStringLiteral("permission_denied");
        error.message = QStringLiteral("%1 cannot be read: %2.").arg(path, file.errorString());
        errors << error;
        return std::nullopt;
    }
    return load(file.readAll(), errors);
}

QJsonObject Document::toJson() const {
    QJsonObject o = extra;
    o.insert("fugue", format);
    o.insert("graph", graphJson(graph));
    if (present.groups || !groups.isEmpty())
        o.insert("groups", groupsJson(groups));
    if (present.layout || !layout.isEmpty())
        o.insert("layout", layout);
    if (present.requirements || !requirements.isEmpty())
        o.insert("requires", requirements);
    if (present.run || !run.isEmpty())
        o.insert("run", run);
    return o;
}

QByteArray Document::save() const {
    return writeJson(toJson());
}

bool Document::saveFile(const QString &path, Errors &errors) const {
    const Errors failed = writeJsonFile(path, toJson());
    errors << failed;
    return failed.isEmpty();
}

QString Document::sourceHash() const {
    return canonicalHash(QJsonObject{{"graph", graphJson(graph)}, {"groups", groupsJson(groups)}});
}

void Document::refreshRequirements(const QString &appVersion) {
    QMap<QString, Version> types;
    QMap<QString, QJsonObject> models;
    const auto addNodes = [&](const Nodes &nodes) {
        for (const Node &node : nodes) {
            if (!groups.contains(node.type) && (!types.contains(node.type) || types[node.type] < node.version))
                types[node.type] = node.version;
            for (const InputValue &value : node.inputs)
                collectResources(value, models);
        }
    };
    addNodes(graph.nodes);
    for (const GroupDefinition &group : groups) {
        addNodes(group.nodes);
        for (const GroupInput &input : group.inputs)
            if (input.defaultValue)
                collectResources(*input.defaultValue, models);
    }

    QJsonObject nodes;
    QSet<QString> packsUsed;
    for (auto it = types.begin(); it != types.end(); ++it) {
        nodes.insert(it.key(), it.value().toString());
        const QString pack = typeNamespace(it.key());
        if (pack != "fugue" && pack != "local")
            packsUsed.insert(pack);
    }

    QMap<QString, QString> sources;
    for (const QJsonValue entry : requirements.value("models").toArray())
        if (entry.toObject().value("source").isString())
            sources.insert(entry.toObject().value("hash").toString(), entry.toObject().value("source").toString());
    QJsonArray modelList;
    for (auto it = models.begin(); it != models.end(); ++it) {
        QJsonObject entry = it.value();
        if (sources.contains(it.key()))
            entry.insert("source", sources.value(it.key()));
        modelList << entry;
    }

    QJsonObject packs;
    const QJsonObject oldPacks = requirements.value("packs").toObject();
    for (auto it = oldPacks.begin(); it != oldPacks.end(); ++it)
        if (packsUsed.contains(it.key()))
            packs.insert(it.key(), it.value());

    requirements.insert("fugue", appVersion);
    requirements.insert("nodes", nodes);
    if (modelList.isEmpty())
        requirements.remove("models");
    else
        requirements.insert("models", modelList);
    if (packs.isEmpty())
        requirements.remove("packs");
    else
        requirements.insert("packs", packs);
    present.requirements = true;
}

} // namespace fugue
