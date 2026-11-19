#pragma once

// The window's commands that act on the canvas (spec 12.4): their handlers, set on the
// command registry's default commands. Every edit goes through the CanvasModel, so each is
// one step on its undo stack; copy and paste go through the system clipboard.

class CanvasModel;

namespace fugue {
class CommandRegistry;
}

// Sets the handlers of edit.undo, edit.redo, edit.copy, edit.paste, edit.paste-connected,
// edit.clone, edit.delete, edit.select-all, node.bypass, node.mute, node.collapse, view.fit,
// view.zoom-in and view.zoom-out, and turns the canvas context on.
void registerCanvasCommands(fugue::CommandRegistry &commands, CanvasModel &canvas);

// The clipboard's form of a copy: a workflow document, also put there as plain text so it
// can be pasted into an editor or written by an agent and pasted back.
inline constexpr const char *canvasClipboardType = "application/x-fugue";
