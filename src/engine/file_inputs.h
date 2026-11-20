#pragma once

// The `file` inputs of a workflow (Load Image's picture, spec Appendix A): the literals that name
// files, which saving, opening a template and copying one out each rewrite or read.

#include <QString>

#include <functional>

#include "engine/document.h"

namespace fugue {

class Registry;

// Calls `visit` with the node id, the input name and the value of every input whose widget is
// `file` and whose value is a text literal, in the graph and in each group definition. A node
// whose type is not installed is skipped. `visit` may change the value.
void forEachFileLiteral(Document &document, const Registry &registry,
                        const std::function<void(const QString &node, const QString &input, InputValue &value)> &visit);

} // namespace fugue
