#pragma once

// The workflow document, a `.fugue` file (spec 4).
//
// Document::load checks the document's shape only (4.7): JSON types, id
// forms, version forms and the input value forms of 4.2. It never needs the
// registry, so a document whose node types are not installed still loads
// and saves without loss (3.6). Meaning (known types, links that resolve,
// type compatibility, values within range, cycles, group recursion) is the
// compiler's job (9.1).
//
// Unknown keys are kept on a round trip (4.1): at the top level, in `graph`,
// in each node, each group definition and each exposed input and output.
// `layout`, `requires` and `run` are kept as raw JSON; load checks the shape
// of `layout` and `requires`.
//
// save() writes the on-disk form of 4.6 (engine/json.h): loading a file in
// that form and saving it gives the same bytes, and saving twice gives
// identical bytes. Every node is written with its `inputs`, even when empty.

#include "engine/error.h"
#include "engine/version.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace fugue {

// "<node>.<port>": a link source (`from`), an exposed input's target, or a
// group output's `from`. parse() checks only that both parts are non-empty
// and that there is one '.'; the node part may be a path id ("g7/first").
struct PortRef {
    QString node;
    QString port;

    static std::optional<PortRef> parse(const QString &text);
    QString toString() const;

    bool operator==(const PortRef &other) const { return node == other.node && port == other.port; }
    bool operator!=(const PortRef &other) const { return !(*this == other); }
};

// One input value (4.2), kept exactly as written.
class InputValue {
public:
    enum class Kind {
        Literal,  // a string, number or boolean
        Link,     // {"from": "<node id>.<output name>"}
        Auto,     // {"auto": "random"}, resolved when a run starts
        Resource, // a resource handle (2.5), whose "resource" is its type id
        List,     // an array of the forms above, for list inputs
    };

    static InputValue literal(const QJsonValue &value);
    static InputValue link(const PortRef &from);
    static InputValue autoRandom();
    static InputValue resource(const QJsonObject &handle);
    static InputValue list(const QList<InputValue> &items);
    // For JSON that Document::load accepted, or that the caller has checked.
    static InputValue fromJson(const QJsonValue &json);

    Kind kind() const { return m_kind; }
    QJsonValue literal() const;     // Literal: the value
    PortRef from() const;           // Link: the source
    QString autoMode() const;       // Auto: "random"
    QJsonObject handle() const;     // Resource: the whole handle
    QList<InputValue> items() const; // List: the items
    QJsonValue toJson() const { return m_json; }

    bool operator==(const InputValue &other) const { return m_json == other.m_json; }
    bool operator!=(const InputValue &other) const { return !(*this == other); }

    // Empty: not a value, and not written on save (Qt drops an undefined
    // value from an object). It exists because QMap needs a default.
    InputValue() = default;
    bool isEmpty() const { return m_json.isUndefined(); }

private:
    InputValue(Kind kind, const QJsonValue &json) : m_kind(kind), m_json(json) {}
    Kind m_kind = Kind::Literal;
    QJsonValue m_json = QJsonValue::Undefined;
};

enum class NodeMode { Normal, Bypass, Mute };

// A node in `graph.nodes` or in a group definition's `nodes`.
struct Node {
    QString type;    // a node type id, or a group id from `groups`
    Version version; // the version the node was made with
    QMap<QString, InputValue> inputs; // by input name; absent inputs use their default
    NodeMode mode = NodeMode::Normal;
    QString provider; // a pinned provider instance, or empty
    QString frozen;   // a result-cache key, or empty
    QJsonObject extra; // unknown keys, kept
};

using Nodes = QMap<QString, Node>; // by node id

struct Graph {
    Nodes nodes;
    QString provider; // the workflow-wide provider pin, or empty
    QJsonObject extra;
};

// An exposed input of a group (4.4).
struct GroupInput {
    QString name;
    QList<PortRef> targets; // inner "<node id>.<input name>", at least one
    QString label;
    QString section;
    std::optional<InputValue> defaultValue; // never a link
    std::optional<double> min;
    std::optional<double> max;
    std::optional<QStringList> choices;
    QJsonObject extra;
};

struct GroupOutput {
    QString name;
    PortRef from; // inner "<node id>.<output name>"
    QJsonObject extra;
};

// One entry of `groups`, keyed by group id.
struct GroupDefinition {
    Version version;
    QString name;
    QString description;
    QString category;
    QList<GroupInput> inputs;
    QList<GroupOutput> outputs;
    Nodes nodes; // inner ids are local to the definition
    QJsonObject extra;
};

struct Document {
    int format = 1; // the `fugue` key
    Graph graph;
    QMap<QString, GroupDefinition> groups;
    QJsonObject layout;       // raw (4.5)
    QJsonObject requirements; // `requires`, raw (4.3)
    QJsonObject run;          // raw, only in copies embedded in outputs (11.7)
    QJsonObject extra;        // unknown top-level keys

    // Which optional sections the loaded file had. save() writes a section
    // when it was present or is not empty, so a file keeps its shape.
    struct Present {
        bool groups = false;
        bool layout = false;
        bool requirements = false;
        bool run = false;
    } present;

    const GroupDefinition *group(const QString &id) const;

    // Shape validation reports every problem, all with code
    // `invalid_document`, except: bytes that are not JSON give one
    // `invalid_json` (with the line and column), and a `fugue` format newer
    // than 1 gives `unsupported_format_version` alone, since a later format's
    // fields may mean other things. An error about a node sets `node` to its
    // id and, for an input, `input`. Inside a group definition, `node` is the
    // inner id and the message starts "Group <id>"; an error about an exposed
    // input or output of a group sets `input` or `output` to its name. When
    // there is any error, nothing is returned. A UTF-8 byte order mark is
    // accepted.
    static std::optional<Document> load(const QByteArray &bytes, Errors &errors);
    static std::optional<Document> fromJson(const QJsonObject &json, Errors &errors);
    // Adds `file_not_found` or `permission_denied` to the above.
    static std::optional<Document> loadFile(const QString &path, Errors &errors);

    QJsonObject toJson() const;
    QByteArray save() const; // writeJson(toJson())
    // Writes atomically. A failure is `permission_denied` with the reason.
    bool saveFile(const QString &path, Errors &errors) const;

    // The `source` of the compiled graph (4.8): canonicalHash() of
    // {"graph": <graph>, "groups": <groups>}, with `groups` as {} when the
    // document has none. Layout, requirements and run do not change it.
    QString sourceHash() const;

    // Rewrites `requires` from the document (4.3): `fugue` is appVersion;
    // `nodes` maps every node type used, in the graph and in group
    // definitions, to its version (a group id is not a node type and is left
    // out; a type used at two versions records the highest); `models` lists
    // every resource handle with a hash that appears in a literal, including
    // inside lists, group defaults and derived handles, once per hash, sorted
    // by hash, keeping a `source` already recorded for that hash; `packs`
    // keeps the recorded entries of packs still used (adding new ones needs
    // pack data the registry gains in M3). Unknown keys are kept.
    void refreshRequirements(const QString &appVersion);
};

} // namespace fugue
