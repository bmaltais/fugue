#pragma once

// Which nodes show a picture (#263, spec 12.7): the picture mode (a preference), what a node type
// says of itself (the manifest's "picture"), and the node's own override (layout.nodes.<id>.picture).
// Plain functions on plain values: the canvas, the runs and the menus all ask here.
//
// Role, from the type: "always" (a sink, Preview, Compare: the picture is the point), "result" (a node
// that makes or loads a picture: Load Image, Generate Image, Decode) or "never" (a transform that only
// repeats the one before it: Resize, Blur). The manifest's key wins; without one the category decides.
//
// Mode: Smart shows always and result; "sinks" (Sinks and inputs only) shows always and the input
// nodes (a result node with a file to load); All shows every picture. An override show or hide wins.

#include <QString>

namespace fugue {

struct NodeType;

enum class PictureMode { Smart, Sinks, All };
enum class PictureRole { Always, Result, Never };
enum class PictureOverride { Default, Show, Hide };

// "smart", "sinks", "all" and back; an unknown name is Smart.
QString pictureModeName(PictureMode mode);
PictureMode pictureModeFromName(const QString &name);
// "show", "hide" (the layout's values); anything else is Default.
QString pictureOverrideName(PictureOverride value);
PictureOverride pictureOverrideFromName(const QString &name);
// "always", "result", "never"; empty or unknown gives false and leaves `role`.
bool pictureRoleFromName(const QString &name, PictureRole *role);

// The type's role: its manifest key, else a sink is Always, a transform, adjust, mask or canvas
// category is Never, and everything else is Result.
PictureRole pictureRole(const NodeType &type);
// A node that brings a file in (a Load node): a file input and no media input of its own.
bool isInputNode(const NodeType &type);

// Whether the type can have a picture at all: a sink, a node with an image, video or audio output,
// or a Load node. A number or a text node has none, so there is nothing to show or hide.
bool hasPictureSlot(const NodeType &type);

bool showsPicture(PictureMode mode, PictureRole role, bool inputNode, PictureOverride override);

} // namespace fugue
