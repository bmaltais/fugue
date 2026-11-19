#pragma once

// The one seam where the compiler (#8) plugs in. Until it lands, validation
// covers document load and shape only (done by the caller), and compile reports
// that it is not built. `fugue validate`, `compile` and `run` all go through
// these two functions and nothing else.

#include <QJsonObject>

#include "engine/document.h"
#include "engine/registry.h"
#include "engine/result.h"

namespace fugue::cli {

// Everything the compiler finds wrong with a loaded document (errors and
// warnings, none executed). Empty means clean.
Errors validateDocument(const Document &document, const Registry &registry);

// The compiled graph (4.8) as JSON, or its errors.
Result<QJsonObject> compileDocument(const Document &document, const Registry &registry);

} // namespace fugue::cli
