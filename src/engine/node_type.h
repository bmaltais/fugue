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

#include "engine/shown_when.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
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
    // A lazy input (3.2, 9.9): the scheduler starts what feeds it only when the node asks for it (NodeKernel::lazyInputs).
    bool lazy = false;
    std::optional<int> minItems; // list inputs only
    std::optional<int> maxItems;
    std::optional<double> min; // number inputs only
    std::optional<double> max;
    std::optional<double> step;
    bool integer = false;
    std::optional<QStringList> choices; // text inputs only: makes it a choice
    QString widgetName; // as declared; empty means the type's default (6.2)
    QString syntax; // a text input's prompt syntax for the text editors to colour: "dynamic-prompt" or "template" (6.1); empty: none
    QString section;
    bool advanced = false;
    QString role; // "mask" on an image input that takes a mask (6.1, 14.4); empty for none
    QString over; // widget `mask` only: the image input the mask is painted over; empty: the node's own `file` input
    // `shown_when` (6.1, #429): the face hides the row while this does not hold. Empty: always shown.
    ShownWhen shownWhen;

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
    // What the value on this output carries, such as "mask" on a latent that holds a mask (6.1, #429), so a
    // `shown_when` on a downstream input can ask without running anything. `carries_when` is a condition on
    // the node's own inputs for it to hold (VAE Encode's latent carries a mask when its `mask` is linked).
    QStringList carries;
    ShownWhen carriesWhen;
    // Only on an output of type "any" (built-in nodes): the inputs whose link type this output takes (the first one
    // linked), so a Switch's value is an image when an image feeds it (2.3).
    QStringList follows;
};

// The most rows a repeating group of inputs takes, and the highest row number (6.1, #714).
inline constexpr int kMaxRepeatRows = 99;

// A repeating group of inputs (6.1, #714): rows of the same item inputs, each row's inputs named
// `<item>_<row>` with the row counted from 1 (Apply LoRAs' `lora_1`, `strength_1`, `enabled_1`). The
// first item is the row's key: a row is there when its key has a value (a literal or a link), and every
// other item of a row that is not there is not used. Rows are not renumbered: a row taken out leaves a
// gap, so a link, a `--set` or a group's exposed input naming `lora_3` keeps meaning the same row.
struct RepeatSpec {
    QString name;  // [a-z][a-z0-9_]*: the block's name ("loras"); never an input's
    QString label; // display text
    int position = 0; // the rows come after this many of the type's declared inputs
    int max = kMaxRepeatRows; // the most rows, and the highest row number
    QList<InputSpec> items;   // the template; items.first() is the key
};

// A row input's place (NodeType::rowInput).
struct RowInput {
    int repeat = 0; // index in NodeType::repeats
    int row = 0;    // from 1
    int item = 0;   // index in RepeatSpec::items
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
    bool viewOnly = false; // a sink whose only effect is showing (Compare Images): kept inside a group when a node reads its outputs (5.5)
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
    // The names of the secrets the node's pack declares in pack.json `settings` (6.4, #402): the only
    // ones the runner hands to this node's program. Set by the pack loader.
    QStringList packSecrets;
    // Of those, the ones pack.json says need an origin (`"origin": "required"`): a value stored without one is not handed over.
    QStringList packSecretsOriginRequired;
    // Where the pack was installed from (its `.fugue-source.json` location), empty for one copied in by hand: with the pack id
    // it is the key the pack's secret values are stored under (6.4.1).
    QString packSource;
    // pack.json `community: true`: the pack came through the reviewed community channel (#731). It is always sandboxed:
    // the per-pack "run without a sandbox" yes does not apply to it.
    bool packCommunity = false;
    // What the pack loader saw of a command node's files (nodeSeal in command_node.h); the
    // runner refuses to run when they no longer match. Empty: not checked.
    QString seal;
    QJsonObject runtime; // has at least "kind"
    QJsonObject needs;   // 6.3, raw
    QJsonValue capabilities = QJsonValue::Undefined; // 8.1, raw
    QList<InputSpec> inputs; // the declared inputs; the rows of `repeats` are not among them
    QList<OutputSpec> outputs;
    // Repeating groups of inputs (6.1, #714), and every row's inputs made from them (row 1's items, row 2's, ... up to
    // each group's `max`), so input() gives a row's input a spec of its own name. Made by fromJson; not in toJson.
    QList<RepeatSpec> repeats;
    QList<InputSpec> rowInputs;

    // A declared input, or a row's input (`lora_2`); null for a name the type does not have.
    const InputSpec *input(const QString &name) const;
    const OutputSpec *output(const QString &name) const;

    // `<item>_<row>`.
    static QString rowInputName(const QString &item, int row);
    // Where a row's input sits, or nothing for a name that is not one (a declared input, a row past `max`).
    std::optional<RowInput> rowInput(const QString &name) const;
    // The input `item` of row `row` of repeat `repeat`, or null past its `max`.
    const InputSpec *rowSpec(int repeat, int row, int item) const;
    // The rows of a repeat that are there: those whose key has a value, by `hasValue` (true when the named input holds one), in
    // row order.
    QList<int> rowsOf(int repeat, const std::function<bool(const QString &)> &hasValue) const;
    // The inputs a node of this type has (3.2): the declared ones with, at each repeat's place, every item of the rows that
    // are there, in row order. A type with no repeats gives its declared inputs. Every loop over a node's input values reads
    // this, not `inputs`, so a row's values count as any input's.
    QList<const InputSpec *> inputsWhere(const std::function<bool(const QString &)> &hasValue) const;
    template <typename Map> QList<const InputSpec *> inputsOf(const Map &values) const {
        return inputsWhere([&values](const QString &name) {
            const auto found = values.constFind(name);
            return found != values.constEnd() && !found->isEmpty();
        });
    }

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
