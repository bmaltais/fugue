#pragma once

// What every Save node shares (Save Image, Save Video, Save Audio; spec 14.1, 14.2): the file name
// and folder inputs checked and resolved the same way, the workflow text a file may carry, and the
// rule that a save takes the first free numbered name and never overwrites a file.

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "engine/atomic_file.h"
#include "engine/node_kernel.h"
#include "engine/result.h"

namespace fugue::nodes {

// The model's file name without its known extension (.gguf, .safetensors, ...) and without any
// folder, made safe as one name part: anything but letters, digits and `._-+() ` becomes `_`.
// "unknown" when `handle` names none (a run with no model upstream of the Save node).
QString modelLabel(const QJsonObject &handle);

// The `name` input made into a relative file name: `{date}` (yyyy-MM-dd), `{time}` (hh-mm-ss) and
// `{model}` (modelLabel) replaced, then split on `/` into folders and a file name. Every part must
// be plain: no empty part, no `.` or `..`, none starting with `.`, no backslash, at most 255 bytes
// (the file name with `.<extension>`). The result is `folder/folder/name`, never absolute. An error
// on the name input otherwise. A name with no token and no `/` comes back as it was typed, trimmed.
Result<QString> expandSaveName(const QString &name, const QDateTime &now, const QString &model, const QString &extension);

// Where a save writes: `folder` is the resolved, made folder (below) and `name` the file's stem
// in it (no `/`), after the `name` input's subfolders are made inside it.
struct SaveLocation {
    QString folder;
    QString name;
};

// The `name` and `folder` inputs resolved and checked. The folder is the `folder` input (a
// relative one is under the output folder and may not leave it), else the output folder, and the
// name's subfolders go below it; the result is canonical, inside the folder after links are
// followed (a link below it that leads out is refused), inside the run's write roots, and made.
Result<SaveLocation> saveLocation(const NodeContext &context, const QString &extension);

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
