#pragma once

// The share copy of a workflow document (spec 11.5, 11.7): what leaves the machine in
// an exported file or inside a saved image.
//
//   - Local absolute paths are replaced by the bare file name. A path is a literal of an
//     input whose widget is `file` or `folder`; a literal that is an absolute path
//     (`/...`, `~`, `C:\...`) in an input of a node type the registry does not know (a
//     pack that is not installed); also the default of a group's exposed input that
//     drives such an input. A relative path stays.
//   - Optionally the prompts are blanked: the literal of every `text` input with the
//     `multiline` widget. (Node types name their prompt inputs that way: Text, Generate
//     Image, Edit Image.) Links and randomized values are untouched.
//   - Optionally the `layout` section is dropped.
//
// Everything else is kept as it is, unknown keys included, and the input is not changed.

#include <QJsonObject>

#include "engine/registry.h"

namespace fugue {

struct ShareOptions {
    bool withoutPrompts = false;
    bool withoutLayout = false;
};

// `document` is in Document::toJson() form.
QJsonObject shareCopy(const QJsonObject &document, const Registry &registry, const ShareOptions &options = {});

// Whether the text is an absolute local path: starts with `/`, is `~` or starts with
// `~/`, or is a Windows drive path.
bool isLocalAbsolutePath(const QString &text);

} // namespace fugue
