#pragma once

// A node type as data (spec 3.1, 6.1): the manifest's identity and flags, and
// its inputs and outputs in full. `runtime`, `needs` and `capabilities` are
// kept as raw JSON here; the loader (M3) and the providers give them meaning.
//
// NodeType::fromJson validates the shape of a node.json manifest and what can
// be checked without a registry (names, types, constraints, widgets,
// defaults). The registry (engine/registry.h) adds the checks that need it.

#include "engine/result.h"
#include "engine/port_type.h"
#include "engine/version.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace fugue {

// The section whose inputs a node face folds until opened (6.1, 5.4); an `advanced` input
// with no section is in it.
inline constexpr char kAdvancedSection[] = "Advanced";

// The one port role there is (6.1): a mask is an image with one channel, and its port is marked.
inline constexpr char kMaskRole[] = "mask";

struct InputSpec {
    QString name;           // [a-z][a-z0-9_]*, unique among the inputs
    PortSpec type;          // port type and the `list` modifier
    QString label;
    QString description;
    QJsonValue defaultValue = QJsonValue::Undefined; // Undefined: no default
    bool optional = false;
    std::optional<int> minItems; // list inputs only
    std::optional<int> maxItems;
    std::optional<double> min; // number inputs only
    std::optional<double> max;
    std::optional<double> step;
    bool integer = false;
    std::optional<QStringList> choices; // text inputs only: makes it a choice
    QString widgetName; // as declared; empty means the type's default (6.2)
    QString section;
    bool advanced = false;
    QString role; // "mask" on an image input that takes a mask (6.1, 14.4); empty for none
    QString over; // widget `mask` only: the image input the mask is painted over; empty: the node's own `file` input

    // True for an input whose text literal names a file that travels with the workflow (spec 11.2,
    // 11.5, 11.10, 14.4): a `file` input's picture, a `mask` input's painted mask, or a `paint` input's .ora canvas. The one
    // predicate that saving, sharing, taking a template in and running read, so they cannot disagree.
    bool holdsInputFile() const;

    // Required unless it has a default or is optional (3.2).
    bool required() const;
    // The widget shown when unwired: the declared one, else the default for
    // the type (spin, line, choice, toggle). Empty for inputs with no
    // widget: media and data inputs are wire-only, resources show a picker.
    QString widget() const;
};

struct OutputSpec {
    QString name; // [a-z][a-z0-9_]*, unique among the outputs
    PortSpec type;
    QString label;
    QString description;
    bool optional = false; // may come back empty (3.3)
    QString role;          // "mask" on an image output that holds a mask (6.1, 14.4); empty for none
};

struct NodeType {
    QString id; // "<pack id>/<name>"
    Version version;
    QString name;
    QString description;
    QString category; // levels separated by '/'
    QStringList keywords;
    QStringList aliases; // old ids this type answers to (3.6)
    // Inputs the type used to have: a saved value for one is dropped without a word, no error and no notice (3.6).
    QStringList retiredInputs;
    bool sink = false;
    // Whether the node shows a picture (#263): "always", "result" or "never"; empty: by category
    // (node_picture.h pictureRole). Packs say it for their own types.
    QString picture;
    bool deterministic = true;
    // The folder holding node.json: set by the pack loader, empty for a type that has none
    // (the built-ins' resources). Not part of the manifest, so toJson() leaves it out.
    QString dir;
    // The version of the pack this type was loaded from (pack.json), set by the pack loader;
    // empty for a type with no pack folder. Trust and the cache key use it (6.7, 10.1).
    QString packVersion;
    // What the pack loader saw of a command node's files (nodeSeal in command_node.h); the
    // runner refuses to run when they no longer match. Empty: not checked.
    QString seal;
    QJsonObject runtime; // has at least "kind"
    QJsonObject needs;   // 6.3, raw
    QJsonValue capabilities = QJsonValue::Undefined; // 8.1, raw
    QList<InputSpec> inputs;
    QList<OutputSpec> outputs;

    const InputSpec *input(const QString &name) const;
    const OutputSpec *output(const QString &name) const;

    // Parses a node.json manifest. Every problem found is reported (the
    // code is `invalid_document`, or the literal codes for a default that
    // breaks its own constraints, or `unsupported_format_version`), each
    // message naming the type id. An unknown field is a warning, which a
    // manifest that parses carries in warnings(). No type when there is an error.
    static Result<NodeType> fromJson(const QJsonObject &manifest);

    // The manifest as data (`fugue node describe`), with only the fields that
    // are set. fromJson(toJson()) gives the same type back.
    QJsonObject toJson() const;
};

// Checks one literal value against an input (2.1, 3.2, 4.2, 6.1, 6.2): its
// JSON type, `integer`, `min` and `max` (`value_out_of_range`, integers also
// within 2^53), `choices` (`value_not_in_choices`, with every valid option
// and a hint for a near miss), and `{"auto": "random"}` only on an input with
// the seed widget. Media and data inputs take no literal; a resource input
// takes a handle of its own type. Anything else is `type_mismatch`. `kind`
// is the kind of the input's port type (Registry::portKind).
//
// For a list input pass each literal item; the caller handles links, arrays
// and item counts. Errors carry `input`; the caller sets `node`.
Errors checkLiteral(const InputSpec &input, const QJsonValue &value, PortKind kind);

// Checks a list input's item count against its `min_items` and `max_items`
// (`value_out_of_range`). Errors carry `input`; the caller sets `node`.
Errors checkItemCount(const InputSpec &input, qsizetype count);

} // namespace fugue
