#pragma once

// `--use-on-disk` and the hints beside it (#605, spec 13.1): a model a workflow names that is not on disk, whose same weights are in
// another file, is said so in the error ("the same model is on disk as <path>; set <input> to it, or pass --use-on-disk"), and
// `fugue run`, `fugue app run` and `fugue validate` take --use-on-disk to run with that file for this command only. The decision
// and the swap are engine/model_use.h's; nothing here writes the workflow file.

#include "cli/commands.h"
#include "engine/document.h"
#include "engine/error.h"

namespace fugue::cli {

// Whether this command swaps a missing model for the file of the same weights: `--use-on-disk` says yes, `--no-use-on-disk` says no, and
// with neither the setting `use_on_disk` decides ("use", the default: yes; "ask": no, the hint is all there is) (#618).
bool usesOnDisk(const Command &command);

// The error of passing both flags, or none.
Errors onDiskFlagConflict(const Command &command);

// The document with each missing model that has a file of the same weights on disk replaced by it; each substitution is printed
// (`context.log`). `said` gets one line per substitution.
Document useOnDisk(const Document &document, Context &context, QStringList *said = nullptr);

// A warning per missing model of `document` that has a match (code model_missing, the node and input, the hint above). A run that
// is going to fail on it says so before it starts.
Errors onDiskNotes(const Document &document, Context &context);

// Adds the hint to each model_missing error of `errors` (a node and an input, or a model's name) that has a match, and appends nothing else.
void addOnDiskHints(Errors &errors, const Document &document, Context &context);

} // namespace fugue::cli
