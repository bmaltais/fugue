#pragma once

// Port types (spec 2.1, 2.2) and when an output may feed an input (2.3).
//
// The type system is nominal: a type is its id and version. Built-in types are
// bare names (text, number, boolean, image, video, audio) at version 1. A type
// declared by a pack is "<namespace>/<name>@N", where "@1" may be left out
// and is left out when written. "any" is the one wildcard: only inputs of
// built-in fugue/ nodes may use it, which the registry enforces.

#include <QString>

#include <optional>

namespace fugue {

struct PortType {
    QString id;      // "image", "any", or a type id such as "fugue/latent"
    int version = 1; // always 1 for built-ins and "any"

    // Nothing for text that is not a port type.
    static std::optional<PortType> parse(const QString &text);
    QString toString() const;

    bool isBuiltin() const; // one of the six built-in types
    bool isAny() const;

    bool operator==(const PortType &other) const {
        return id == other.id && version == other.version;
    }
    bool operator!=(const PortType &other) const { return !(*this == other); }
};

// What values of a port type are (2.1, 2.2). The registry knows the kind of a
// declared type.
enum class PortKind {
    Builtin,  // text, number, boolean (literals) or image, video, audio (links only)
    Any,      // the wildcard of built-in utility inputs
    Resource, // values are resource handles (2.5); a handle may be a literal
    Data,     // values are files a node produced (2.6); links only
    Unknown,  // a declared type nobody registered
};

// The type of one input or output, with the list modifier (2.3).
struct PortSpec {
    PortType type;
    bool list = false;

    QString toString() const; // "image", or "list of image"
};

enum class Compatibility {
    Compatible,
    DifferentType,  // the type ids or versions differ; no implicit conversion
    ListIntoSingle, // a list output cannot feed a non-list input
};

// Whether `output` may be linked into `input`. A single value into a list
// input is compatible (it becomes a one-item list). An "any" input takes
// every type, but the list rule still applies to it. The compiler reports
// anything but Compatible as `type_mismatch`.
Compatibility compatibility(const PortSpec &output, const PortSpec &input);

// One sentence saying why a link is not compatible, naming both types.
QString explain(Compatibility result, const PortSpec &output, const PortSpec &input);

} // namespace fugue
