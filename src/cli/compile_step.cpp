#include "cli/compile_step.h"

namespace fugue::cli {

Errors validateDocument(const Document &, const Registry &) {
    return {};
}

Result<QJsonObject> compileDocument(const Document &, const Registry &) {
    return makeError("internal", "Compiling is not built yet.", "The compiler arrives with issue #8.");
}

} // namespace fugue::cli
