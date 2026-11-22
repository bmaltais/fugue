#pragma once

// The input files of a workflow (Load Image's picture, a painted mask; spec Appendix A, 14.4): the
// literals that name files, which saving, opening a template and copying one out each rewrite or read.
// Which inputs hold one is InputSpec::holdsInputFile, the one predicate for all of them.

#include <QString>

#include <functional>

#include "engine/document.h"

namespace fugue {

class Registry;

// Calls `visit` with the node id, the input name and the value of every input that holds an input
// file (InputSpec::holdsInputFile: widget `file` or `mask`) and whose value is a text literal, in the graph and in each group definition. A node
// whose type is not installed is skipped. `visit` may change the value.
void forEachFileLiteral(Document &document, const Registry &registry,
                        const std::function<void(const QString &node, const QString &input, InputValue &value)> &visit);

} // namespace fugue
