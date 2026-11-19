#pragma once

// The share copy of a workflow document (spec 11.5, 11.7): what leaves the machine in
// an exported file or inside a saved image.
//
//   - Local absolute paths are replaced by the bare file name. A path is a literal of an
//     input whose widget is `file` or `folder`; a literal that is an absolute path
//     (`/x`, `~/x`, `C:\x`) in an input of a node type the registry does not know (a
//     pack that is not installed); also the default of a group's exposed input that
//     drives such an input. A relative path stays. A path can also reach such an input
//     through links (a Text node typed with a path, linked to Load Image's file): every
//     absolute-path literal of a node upstream of a file or folder input goes too, as does
//     every one inside a group whose instance is upstream of one, and the literal of a
//     group input that drives, inside the group, a node upstream of one. Text that feeds no
//     path stays, however it looks (a prompt may start with `/`).
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

// Whether the text is an absolute local path that names something: `/`, `~/` or a Windows
// drive (`C:\`) followed by a name. A lone separator, `~` and `C:\` alone are not.
bool isLocalAbsolutePath(const QString &text);

} // namespace fugue
