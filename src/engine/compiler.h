#pragma once

// The compiler (spec 4.8, 9.1): turns a workflow Document into the flat
// CompiledGraph the engine runs and `fugue compile` prints.
//
// compile() checks meaning, which Document::load leaves out, and reports
// every problem at once as structured errors (13.3):
//   - every node type exists (unknown_node_type) and its major version
//     matches the installed one (node_version_mismatch, 3.6);
//   - every link names an existing node (unknown_link_source) and output
//     (unknown_output), and the types are compatible (type_mismatch, 2.3);
//   - every input is known (unknown_input), required inputs are set
//     (missing_input), literals fit their constraints and choices
//     (checkLiteral), list inputs their item counts (value_out_of_range);
//   - no cycles (cycle) and no group recursion (group_recursion);
//   - pinned providers exist (unknown_provider), when the caller passes the
//     configured names.
// Warnings: nodes not part of any output (not_part_of_output), and inputs a
// node's installed type lacks because it is an older minor version (3.6).
//
// Where errors point: an error about a node sets `node` to its path id
// ("g7/shrink" for node shrink inside group instance g7). Values are checked
// where they are used, so a bad value given to a group's exposed input is
// reported on the inner node and input it drives (13.3's example). Problems
// with the group's face (an unknown exposed input name on the instance, a
// target or output that does not resolve) are reported on the instance's
// path id with `input` or `output` set to the exposed name. A broken node
// does not cause follow-on errors: links from a node of unknown type are not
// checked further.
//
// compile() is pure: it reads no files and asks no provider anything.
// Provider selection, missing models (model_missing, a warning under
// validate, 8.5) and frozen results that are gone belong to the scheduler.

#include "engine/document.h"
#include "engine/error.h"
#include "engine/registry.h"
#include "engine/version.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace fugue {

// One link into a compiled node: its input, the list item it fills (-1 for
// a link that is the whole value), and its source by path id.
struct CompiledLink {
    QString input;
    int item = -1;
    PortRef from; // from.node is a path id

    bool operator==(const CompiledLink &o) const { return input == o.input && item == o.item && from == o.from; }
};

// A node of the compiled graph: always a node type, never a group.
struct CompiledNode {
    QString type;    // the installed type's id (an alias is resolved to it)
    Version version; // the installed version, which the cache key uses (3.6)
    // Input values by name: literals, `auto` values, resource handles, lists,
    // and links whose node part is a path id. An input left out takes the
    // node type's default when the node runs; compile does not fill it in.
    QMap<QString, InputValue> inputs;
    bool sink = false;
    QString provider; // the pinned provider instance, or empty for auto (8.3)
    QString frozen;   // a result-cache key, or empty (10.4)

    // Every link of inputs, list items included, in input name order.
    QList<CompiledLink> links() const;
};

// The flat form of 4.8.
struct CompiledGraph {
    static constexpr int format = 1; // `fugue-compiled`

    QString source;                   // Document::sourceHash()
    QMap<QString, CompiledNode> nodes; // by path id
    // Path ids left out, with the reason: muted, upstream_muted,
    // upstream_empty or not_part_of_output. Bypassed nodes and sinks inside
    // groups are removed without an entry.
    QMap<QString, QString> skipped;
    QString target; // for "run to here": the path id the run stops at, or empty

    // --- dependencies, for the scheduler ---

    // The path ids of the nodes with `sink` set, sorted. A "run to here"
    // graph has none unless its target is a sink; its end point is target.
    QStringList sinks() const;
    // The path ids a node reads from, sorted, each once.
    QStringList dependencies(const QString &path) const;
    // Who reads a node's outputs: each consuming node and the link it reads.
    struct Consumer {
        QString node; // path id
        CompiledLink link;
    };
    QList<Consumer> consumers(const QString &path) const;
    // Every node once, each after all of its dependencies; ties in path id
    // order, so the order is the same on every run.
    QStringList order() const;

    // --- JSON (4.8) ---

    // {"fugue-compiled": 1, "source", "nodes", "skipped", "target"?}. Each
    // node has `type`, `version`, `inputs`, and `sink`, `provider` and
    // `frozen` only when set. Write it with writeJson (RunFolder::writeCompiled).
    QJsonObject toJson() const;
    // Reads what toJson wrote (`fugue run compiled.json`). Checks the shape
    // only: invalid_document for a malformed graph, unsupported_format_version
    // for a `fugue-compiled` newer than 1. The registry is not consulted.
    static Result<CompiledGraph> fromJson(const QJsonObject &json);
};

struct CompileOptions {
    // "Run to here" (3.4): the path id to stop at. Only it and what it needs
    // are kept, and no sink is added. A path id that is not a node of the
    // flattened graph is the error `usage`.
    QString target;
    // `fugue run --provider <name>`: replaces the workflow's pin (graph.provider)
    // for this compile.
    QString provider;
    // The configured provider instance names (providers.json). When set,
    // every pin must be one of them (unknown_provider); when not, pins are
    // not checked.
    std::optional<QStringList> providers;
};

// What compiling gives: the compiled graph with the warnings found, or, when
// anything of severity error was found, every error and warning together.
// problems() is all that was reported either way, in a stable order.
using Compilation = Result<CompiledGraph>;

// The one entry point for `fugue validate`, `fugue compile` and every run.
Compilation compile(const Document &document, const Registry &registry, const CompileOptions &options = {});

} // namespace fugue
