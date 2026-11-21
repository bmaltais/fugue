#pragma once

// What every Save node shares (Save Image, Save Video, Save Audio; spec 14.1, 14.2): the file name
// and folder inputs checked and resolved the same way, the workflow text a file may carry, and the
// rule that a save takes the first free numbered name and never overwrites a file.

#include <QByteArray>
#include <QString>

#include "engine/atomic_file.h"
#include "engine/node_kernel.h"
#include "engine/result.h"

namespace fugue::nodes {

// The `name` input as a plain file name that fits (with `.<extension>`) in 255 bytes. An error on
// the name input otherwise.
Result<QString> saveName(const NodeContext &context, const QString &extension);

// The folder a save writes in, made: the `folder` input (a relative one is under the output folder
// and may not leave it), else the output folder. Checked against the run's write roots.
Result<QString> saveFolder(const NodeContext &context);

// The workflow text a saved `what` ("image", "video", "audio") embeds: the share copy (11.5) with a
// `run` section holding what was used upstream (11.7). `embed` is full, no-prompts or none. Empty
// for none, and empty with a warning when the run did not start from a document (a compiled graph).
Result<QByteArray> workflowToEmbed(const NodeContext &context, const QString &embed, const QString &what);

// The names a save tries in turn: `<name>.<ext>`, then `<name>-2.<ext>`, `-3`, ...
QString numberedName(const QString &name, int n, const QString &extension);

// Gives `partial` (already written whole) the first free numbered name in `folder` and returns its
// path. The name is claimed in the same step as the file appears, so a save never overwrites a file
// (it may be the user's), not even one another Save node wrote a moment before.
Result<QString> commitAsNew(PartialFile &partial, const QString &folder, const QString &name, const QString &extension);

// Writes `bytes` under the first free numbered name in `folder` and returns its path.
Result<QString> saveAsNew(const QString &folder, const QString &name, const QString &extension, const QByteArray &bytes);

} // namespace fugue::nodes
