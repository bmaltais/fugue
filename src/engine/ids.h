#pragma once

// The id forms of the document and the manifests.

#include <QString>

namespace fugue {

// A node id in a graph or a group definition (spec 4.2):
// [a-z][a-z0-9_-]{0,31}. No '.' or '/', which are separators.
bool isNodeId(const QString &text);

// A path id (4.8): node ids joined with '/', the group instances from the outside in and
// then the node, such as "save" or "g7/inner3/s2".
bool isPathId(const QString &text);

// A node type, group or declared port type id (3.1, 2.2): "<namespace>/<name>".
// The namespace is lowercase reverse-DNS (at least two labels) or one of the
// reserved words "fugue" and "local"; the name matches [a-z][a-z0-9-]*.
bool isTypeId(const QString &text);

// The part of a type id before the '/': the pack id ("fugue" for built-ins).
QString typeNamespace(const QString &typeId);
// The pack a node type id comes from: its namespace, empty for a built-in (`fugue`) or `local` type.
QString packOfType(const QString &typeId);

// An input or output name (6.1): [a-z][a-z0-9_]*.
bool isPortName(const QString &text);

} // namespace fugue
