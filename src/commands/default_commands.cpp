// The default keymap of spec 12.4, one row per command. A ticket that builds an action sets
// its handler with CommandRegistry::setHandler; until then the overlay lists it as not
// available yet.

#include <QCoreApplication>
#include <QDebug>

#include "command_registry.h"

namespace fugue {

namespace {

struct Row {
    const char *id;
    const char *description;
    QStringList keys;
    QStringList gestures;
    const char *context;
    const char *versus;
    const char *label = nullptr; // the menu's shorter name; none: the description
};

const char *const same = "same";

QList<Row> defaultRows() {
    // Descriptions go through tr() when listed, so translators get them from one place.
    return {
        {"run.start", QT_TRANSLATE_NOOP("Command", "Run"), {"Ctrl+Return"}, {}, "app", same},
        {"run.start-first", QT_TRANSLATE_NOOP("Command", "Run, ahead of the queue"), {"Ctrl+Shift+Return"}, {}, "app", same},
        {"run.to-here", QT_TRANSLATE_NOOP("Command", "Run to the selected node"), {"Shift+Return"}, {}, "app", "added"},
        {"run.cancel", QT_TRANSLATE_NOOP("Command", "Cancel the run"), {"Ctrl+Alt+Return", "Escape"}, {}, "run", "Escape added"},
        {"run.from-here", QT_TRANSLATE_NOOP("Command", "Run from the selected node"), {}, {}, "app", "added"},
        {"file.new", QT_TRANSLATE_NOOP("Command", "New workflow"), {"Ctrl+N"}, {}, "app", "added"},
        {"file.open", QT_TRANSLATE_NOOP("Command", "Open"), {"Ctrl+O"}, {}, "app", same},
        {"file.save", QT_TRANSLATE_NOOP("Command", "Save"), {"Ctrl+S"}, {}, "app", same},
        {"file.save-as", QT_TRANSLATE_NOOP("Command", "Save as"), {"Ctrl+Shift+S"}, {}, "app", "added"},
        {"file.close", QT_TRANSLATE_NOOP("Command", "Close the workflow"), {"Ctrl+W"}, {}, "app", "Omarchy"},
        {"app.quit", QT_TRANSLATE_NOOP("Command", "Quit (asks if there is unsaved content)"), {"Q", "Ctrl+Q"}, {}, "app", "Omarchy"},
        {"app.shortcuts", QT_TRANSLATE_NOOP("Command", "Shortcuts overlay"), {"?", "F1", "Ctrl+?"}, {}, "app", "Omarchy"},
        {"app.settings", QT_TRANSLATE_NOOP("Command", "Settings"), {"Ctrl+,"}, {}, "app", same},
        {"edit.undo", QT_TRANSLATE_NOOP("Command", "Undo"), {"Ctrl+Z"}, {}, "app", same},
        {"edit.redo", QT_TRANSLATE_NOOP("Command", "Redo"), {"Ctrl+Y", "Ctrl+Shift+Z"}, {}, "app", same},
        {"edit.copy", QT_TRANSLATE_NOOP("Command", "Copy"), {"Ctrl+C"}, {}, "canvas", same},
        {"edit.paste", QT_TRANSLATE_NOOP("Command", "Paste"), {"Ctrl+V"}, {}, "canvas", same},
        {"edit.paste-connected", QT_TRANSLATE_NOOP("Command", "Paste with connections"), {"Ctrl+Shift+V"}, {}, "canvas", same},
        {"edit.clone", QT_TRANSLATE_NOOP("Command", "Clone"), {"Ctrl+D"}, {"Alt+drag"}, "canvas", "Ctrl+D added"},
        {"edit.delete", QT_TRANSLATE_NOOP("Command", "Delete"), {"Delete", "Backspace"}, {}, "canvas", same},
        {"edit.select-all", QT_TRANSLATE_NOOP("Command", "Select all"), {"Ctrl+A"}, {}, "canvas", same},
        {"edit.rename", QT_TRANSLATE_NOOP("Command", "Rename"), {"F2"}, {"Double-click a node's title"}, "canvas", "F2 added"},
        {"node.edit-text", QT_TRANSLATE_NOOP("Command", "Edit the node's text in a larger window"), {"Alt+T"}, {"The expand button on a text field"}, "canvas", "added",
         QT_TRANSLATE_NOOP("Command", "Edit text…")},
        {"node.bypass", QT_TRANSLATE_NOOP("Command", "Bypass"), {"Ctrl+B"}, {}, "canvas", same},
        {"node.mute", QT_TRANSLATE_NOOP("Command", "Mute"), {"Ctrl+M"}, {}, "canvas", same},
        {"node.collapse", QT_TRANSLATE_NOOP("Command", "Collapse"), {"Alt+C"}, {}, "canvas", same},
        {"node.pin", QT_TRANSLATE_NOOP("Command", "Pin in place"), {"P"}, {}, "canvas", same},
        {"node.search", QT_TRANSLATE_NOOP("Command", "Add a node by search"), {"Tab", "Shift+A"}, {"Double-click empty canvas"}, "canvas", "keys added"},
        {"frame.make", QT_TRANSLATE_NOOP("Command", "Frame the selection"), {"Ctrl+G"}, {}, "canvas", same},
        {"group.make", QT_TRANSLATE_NOOP("Command", "Make a group"), {"Ctrl+Shift+E"}, {}, "canvas", same},
        {"group.ungroup", QT_TRANSLATE_NOOP("Command", "Ungroup"), {"Ctrl+Shift+G"}, {}, "group-selected", "added"},
        {"group.enter", QT_TRANSLATE_NOOP("Command", "Enter the selected group"), {"Tab"}, {"Double-click the group's body"}, "group-selected", "Tab added"},
        {"group.leave", QT_TRANSLATE_NOOP("Command", "Leave the group"), {"Escape"}, {"The breadcrumb"}, "group", same},
        {"view.fit", QT_TRANSLATE_NOOP("Command", "Fit the view"), {".", "Home"}, {}, "canvas", "Home added"},
        // + and - as in every picture view (#228, docs/ui-ergonomics.md D5: one keymap), free in the registry.
        {"view.zoom-in", QT_TRANSLATE_NOOP("Command", "Zoom in"), {"Alt+=", "Ctrl+=", "+"}, {}, "canvas", "Ctrl, + added"},
        {"view.zoom-out", QT_TRANSLATE_NOOP("Command", "Zoom out"), {"Alt+-", "Ctrl+-", "-"}, {}, "canvas", "Ctrl, - added"},
        {"view.toggle-links", QT_TRANSLATE_NOOP("Command", "Hide or show links"), {"L"}, {}, "canvas", same},
        {"panel.nodes", QT_TRANSLATE_NOOP("Command", "Toggle the node library"), {"N"}, {}, "app", same},
        {"panel.workflows", QT_TRANSLATE_NOOP("Command", "Toggle the workflows panel"), {"W"}, {}, "app", same},
        {"panel.queue", QT_TRANSLATE_NOOP("Command", "Toggle the run queue"), {"Alt+Q"}, {}, "app", "added"},
        // The Canvas view's layers panel (#226): its keys while the pointer is over it (Blender's hover rule).
        {"canvas-layers.visible", QT_TRANSLATE_NOOP("Command", "Hide or show the selected layers"), {"H"}, {"Click the eye", "Alt+click the eye: that layer alone"}, "canvas-layers", "added"},
        {"canvas-layers.lock", QT_TRANSLATE_NOOP("Command", "Lock or unlock the selected layers"), {"Ctrl+/"}, {"Click the lock"}, "canvas-layers", "added"},
        {"canvas-layers.use-in-region", QT_TRANSLATE_NOOP("Command", "Generate reads this layer only, or the whole picture again"), {}, {"Right-click the layer"}, "canvas-layers", "added"},
        {"canvas-layers.rename", QT_TRANSLATE_NOOP("Command", "Rename the layer"), {"F2"}, {"Double-click its name"}, "canvas-layers", "added"},
        {"canvas-layers.duplicate", QT_TRANSLATE_NOOP("Command", "Duplicate the selected layers"), {"Ctrl+D"}, {}, "canvas-layers", "added"},
        {"canvas-layers.raise", QT_TRANSLATE_NOOP("Command", "Move the selected layers up"), {"]"}, {"Drag a layer"}, "canvas-layers", "added"},
        {"canvas-layers.lower", QT_TRANSLATE_NOOP("Command", "Move the selected layers down"), {"["}, {"Drag a layer"}, "canvas-layers", "added"},
        {"canvas-layers.delete", QT_TRANSLATE_NOOP("Command", "Delete the selected layers"), {"X", "Delete"}, {}, "canvas-layers", "added"},
        {"canvas-layers.mask", QT_TRANSLATE_NOOP("Command", "Turn the layer mask off or on"), {}, {"Shift+click the mask"}, "canvas-layers", "added"},
        {"canvas-layers.undo", QT_TRANSLATE_NOOP("Command", "Undo the last canvas change"), {"Ctrl+Z"}, {}, "canvas-layers", "added"},
        {"canvas-layers.redo", QT_TRANSLATE_NOOP("Command", "Redo the canvas change"), {"Ctrl+Shift+Z", "Ctrl+Y"}, {}, "canvas-layers", "added"},
        // The Canvas view's tool strip (#230): its keys while the pointer is over the view or it has the focus (Blender's
        // hover rule); Photoshop's keys (docs/ui-ergonomics.md References). Over the layers panel the panel's own win.
        {"canvas-view.tool-hand", QT_TRANSLATE_NOOP("Command", "Hand: drag the picture to pan"), {"H"}, {"Space held"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Hand")},
        {"canvas-view.tool-move", QT_TRANSLATE_NOOP("Command", "Move: drag to move the current layer; the arrow keys nudge it 1 px, with Shift 10 px"), {"V"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Move")},
        {"canvas-view.free-transform", QT_TRANSLATE_NOOP("Command", "Free transform the current layer: scale, turn and move it, then Enter to keep it or Escape to leave it"), {"Ctrl+T"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Free transform")},
        {"canvas-view.tool-zoom", QT_TRANSLATE_NOOP("Command", "Zoom: click to zoom in, Alt+click to zoom out"), {"Z"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom")},
        {"canvas-view.tool-region", QT_TRANSLATE_NOOP("Command", "Rectangle region"), {"M", "R"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rectangle region")},
        {"canvas-view.tool-lasso", QT_TRANSLATE_NOOP("Command", "Lasso: draw a selection freehand; Shift adds, Alt subtracts"), {"L"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Lasso")},
        {"canvas-view.tool-selection-brush", QT_TRANSLATE_NOOP("Command", "Selection brush: paint the selection, live in red; Alt subtracts"), {"A"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Selection brush")},
        {"canvas-view.tool-brush", QT_TRANSLATE_NOOP("Command", "Mask brush: paint the mask of the selected layer"), {"B"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Mask brush")},
        {"canvas-view.tool-eraser", QT_TRANSLATE_NOOP("Command", "Mask eraser"), {"E"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Mask eraser")},
        {"canvas-view.brush-swap", QT_TRANSLATE_NOOP("Command", "Swap the mask brush and eraser"), {"X"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Swap brush and eraser")},
        {"canvas-view.brush-smaller", QT_TRANSLATE_NOOP("Command", "Make the brush smaller"), {"["}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Smaller brush")},
        {"canvas-view.brush-larger", QT_TRANSLATE_NOOP("Command", "Make the brush larger"), {"]"}, {"Alt+drag"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Larger brush")},
        {"canvas-view.quick-mask", QT_TRANSLATE_NOOP("Command", "Quick Mask: paint the selection"), {"Q"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Quick Mask")},
        {"canvas-view.add-mask", QT_TRANSLATE_NOOP("Command", "Add a layer mask to the selected layer"), {"Ctrl+Shift+M"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Add layer mask")},
        {"canvas-view.mask-overlay", QT_TRANSLATE_NOOP("Command", "Show or hide the mask overlay"), {"\\"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Mask overlay")},
        {"canvas-view.mask-invert", QT_TRANSLATE_NOOP("Command", "Invert the mask"), {"Ctrl+I"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Invert mask")},
        {"canvas-view.undo", QT_TRANSLATE_NOOP("Command", "Undo in the Canvas view (a mask stroke, the selection, the region)"), {"Ctrl+Z"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Undo")},
        {"canvas-view.redo", QT_TRANSLATE_NOOP("Command", "Redo in the Canvas view"), {"Ctrl+Shift+Z", "Ctrl+Y"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Redo")},
        // The Canvas view's own menu bar (#232 slice B): File, Edit, Select, Layer and View as commands of the `canvas-view`
        // context. The menu shows each with this label; a key listed here is bound, one the view already handles by hand
        // (Home, 0, 1, + and -) is shown in the menu as text and not bound twice.
        {"canvas-view.new", QT_TRANSLATE_NOOP("Command", "New canvas in this view's store: a size and a background, the clipboard's picture or a file"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "New canvas…")},
        {"canvas-view.open", QT_TRANSLATE_NOOP("Command", "Choose the canvas to look at"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Open canvas…")},
        {"canvas-view.import", QT_TRANSLATE_NOOP("Command", "Import a picture as a new canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Import picture…")},
        {"canvas-view.export", QT_TRANSLATE_NOOP("Command", "Export the shown revision as a picture (.png, .jpg, .webp) or with its layers (.ora, .psd)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Export as…")},
        {"canvas-view.share", QT_TRANSLATE_NOOP("Command", "Save the canvas as a package to share: its layers as they are now, or with its whole history"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Share…")},
        {"canvas-view.batch", QT_TRANSLATE_NOOP("Command", "Run a canvas action over every picture of a folder and save the results to another folder"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Batch…")},
        {"canvas-view.export-options", QT_TRANSLATE_NOOP("Command", "Export the shown revision as one picture with a format, quality, size and background, and a preview"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Export with quality and scale…")},
        {"canvas-view.export-layers", QT_TRANSLATE_NOOP("Command", "Export each layer of the shown revision as a picture file in a folder"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Export layers as files…")},
        {"canvas-view.rename", QT_TRANSLATE_NOOP("Command", "Rename the canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rename canvas…")},
        {"canvas-view.duplicate", QT_TRANSLATE_NOOP("Command", "Duplicate the canvas with its whole history"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Duplicate canvas")},
        {"canvas-view.clear", QT_TRANSLATE_NOOP("Command", "Clear the canvas: one undoable step that leaves one empty layer"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Clear canvas…")},
        {"canvas-view.delete", QT_TRANSLATE_NOOP("Command", "Delete the canvas for good"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Delete canvas…")},
        {"canvas-view.close", QT_TRANSLATE_NOOP("Command", "Close the canvas view"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Close canvas")},
        {"canvas-view.select-all", QT_TRANSLATE_NOOP("Command", "Select the whole canvas"), {"Ctrl+A"}, {}, "canvas-view", "Ctrl+A added", QT_TRANSLATE_NOOP("Command", "Select all")},
        {"canvas-view.deselect", QT_TRANSLATE_NOOP("Command", "Clear the selection: the rectangle, the Region node's area and the painted selection"), {"Ctrl+D"}, {"Escape"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Deselect")},
        {"canvas-view.invert-selection", QT_TRANSLATE_NOOP("Command", "Invert the painted selection"), {"Ctrl+Shift+I"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Inverse")},
        {"canvas-view.use-selection", QT_TRANSLATE_NOOP("Command", "Give the painted selection to the followed Canvas Region (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Use selection")},
        {"canvas-view.selection-expand", QT_TRANSLATE_NOOP("Command", "Expand the selection by a number of pixels (a rectangle becomes a painted selection)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Expand…")},
        {"canvas-view.selection-contract", QT_TRANSLATE_NOOP("Command", "Contract the selection by a number of pixels"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Contract…")},
        {"canvas-view.selection-feather", QT_TRANSLATE_NOOP("Command", "Feather the selection's edge by a radius in pixels"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Feather…")},
        {"canvas-view.selection-border", QT_TRANSLATE_NOOP("Command", "Make the selection a band of a width in pixels along its edge"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Border…")},
        {"canvas-view.selection-smooth", QT_TRANSLATE_NOOP("Command", "Round the selection's corners and drop specks within a radius in pixels"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Smooth…")},
        {"canvas-view.selection-transform", QT_TRANSLATE_NOOP("Command", "Scale, rotate and move the selection"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Transform selection…")},
        {"canvas-view.mask-reveal", QT_TRANSLATE_NOOP("Command", "Reveal the whole layer mask"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Reveal all")},
        {"canvas-view.mask-hide", QT_TRANSLATE_NOOP("Command", "Hide the whole layer mask"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Hide all")},
        {"canvas-view.new-layer", QT_TRANSLATE_NOOP("Command", "New empty layer above the current one (one undo step)"), {"Ctrl+Shift+N"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "New layer")},
        {"canvas-view.merge-down", QT_TRANSLATE_NOOP("Command", "Merge the selected layer into the one under it (one undo step)"), {"Ctrl+E"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Merge down")},
        {"canvas-view.flatten", QT_TRANSLATE_NOOP("Command", "Flatten: every shown layer becomes one; hidden layers are dropped (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Flatten")},
        {"canvas-view.group", QT_TRANSLATE_NOOP("Command", "Put the selected layers in a new group"), {"Ctrl+G"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Group layers")},
        {"canvas-view.ungroup", QT_TRANSLATE_NOOP("Command", "Take the layers out of the selected groups"), {"Ctrl+Shift+G"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Ungroup layers")},
        {"canvas-view.layer-properties", QT_TRANSLATE_NOOP("Command", "Name, opacity, blend mode and layer mask of the selected layer in one dialog"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Layer properties…")},
        // #236: Edit > Paste as new layer (Ctrl+V while the view is the one in use: the Graph's own Ctrl+V pastes nodes), Paste as new
        // canvas, and Canvas > Place picture….
        // #327: Edit > Copy (Ctrl+C while the Canvas view is the one in use: the Graph's Ctrl+C still copies nodes), Copy Merged and Copy
        // selection as mask. The clipboard gets a picture the Graph's Ctrl+V turns into a Load Image node.
        {"canvas-view.copy", QT_TRANSLATE_NOOP("Command", "Copy the selected pixels of the current layer to the clipboard as a picture (the selection is its alpha)"), {"Ctrl+C"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Copy")},
        {"canvas-view.copy-merged", QT_TRANSLATE_NOOP("Command", "Copy the selected pixels of every shown layer, flattened, to the clipboard as a picture"), {"Ctrl+Shift+C"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Copy merged")},
        {"canvas-view.copy-mask", QT_TRANSLATE_NOOP("Command", "Copy the selection to the clipboard as a gray picture of the canvas's size (white is selected), to use as a mask in nodes"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Copy selection as mask")},
        {"canvas-view.paste-layer", QT_TRANSLATE_NOOP("Command", "Paste the clipboard's picture as a new layer, centered in the view (one undo step)"), {"Ctrl+V"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Paste as new layer")},
        {"canvas-view.paste-canvas", QT_TRANSLATE_NOOP("Command", "Make a new canvas of the clipboard's picture"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Paste as new canvas")},
        {"canvas-view.place-picture", QT_TRANSLATE_NOOP("Command", "Place a picture file on the canvas as a new layer (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Place picture…")},
        // #236: Layer > Transform: flip and quarter turns of the current layer, each one revision.
        {"canvas-view.flip-horizontal", QT_TRANSLATE_NOOP("Command", "Flip the current layer left to right about its center (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Flip horizontal")},
        {"canvas-view.flip-vertical", QT_TRANSLATE_NOOP("Command", "Flip the current layer top to bottom about its center (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Flip vertical")},
        {"canvas-view.turn-clockwise", QT_TRANSLATE_NOOP("Command", "Turn the current layer a quarter turn clockwise (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rotate 90° clockwise")},
        {"canvas-view.turn-counterclockwise", QT_TRANSLATE_NOOP("Command", "Turn the current layer a quarter turn counterclockwise (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rotate 90° counterclockwise")},
        {"canvas-view.fit", QT_TRANSLATE_NOOP("Command", "Fit the picture in the view"), {}, {"Home or 0"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Fit")},
        {"canvas-view.actual-size", QT_TRANSLATE_NOOP("Command", "Show the picture at 100%"), {}, {"1"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "100%")},
        {"canvas-view.zoom-in", QT_TRANSLATE_NOOP("Command", "Zoom in the picture"), {}, {"+"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom in")},
        {"canvas-view.zoom-out", QT_TRANSLATE_NOOP("Command", "Zoom out the picture"), {}, {"-"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom out")},
        {"canvas-view.history", QT_TRANSLATE_NOOP("Command", "Show or hide the history of the canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "History")},
        {"canvas-view.layers", QT_TRANSLATE_NOOP("Command", "Show or hide the layers panel"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Layers")},
        {"canvas-view.compare", QT_TRANSLATE_NOOP("Command", "Compare the shown step with the one before it"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Compare")},
        {"canvas-view.details", QT_TRANSLATE_NOOP("Command", "Show or hide the advanced details under the picture"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Advanced details")},
        {"canvas-view.snap", QT_TRANSLATE_NOOP("Command", "Snap the region's edges to the node's Multiple of"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Snap region")},
        // Rulers, grid, guides and snap to them (#253): Photoshop's keys (View menu), free in the registry.
        {"canvas-view.rulers", QT_TRANSLATE_NOOP("Command", "Show or hide the rulers, in canvas pixels"), {"Ctrl+R"}, {"Drag from a ruler: a guide"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rulers")},
        {"canvas-view.grid", QT_TRANSLATE_NOOP("Command", "Show or hide the grid"), {"Ctrl+'"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Grid")},
        {"canvas-view.guides", QT_TRANSLATE_NOOP("Command", "Show or hide the guides"), {"Ctrl+;"}, {"Drag a guide back to the ruler: remove it"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Guides")},
        {"canvas-view.snap-to", QT_TRANSLATE_NOOP("Command", "Snap the region's edges to the guides and the grid that are shown"), {"Ctrl+Shift+;"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Snap to guides and grid")},
        {"canvas-view.clear-guides", QT_TRANSLATE_NOOP("Command", "Remove every guide of this canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Clear guides")},
    };
}

} // namespace

void registerDefaultCommands(CommandRegistry &registry) {
    for (const Row &row : defaultRows()) {
        CommandSpec spec;
        spec.id = QString::fromLatin1(row.id);
        spec.description = QString::fromLatin1(row.description);
        spec.keys = row.keys;
        spec.gestures = row.gestures;
        spec.context = QString::fromLatin1(row.context);
        spec.versus = QString::fromLatin1(row.versus);
        // A bad row is a bug in this table, which the registry tests catch.
        if (const QString problem = registry.add(spec); !problem.isEmpty())
            qCritical().noquote() << "default command:" << problem;
        if (row.label)
            registry.setLabel(spec.id, QString::fromUtf8(row.label));
    }
    registry.setHandler("app.quit", [&registry] { registry.requestQuit(); });
    registry.setHandler("file.close", [&registry] { registry.requestClose(); });
    registry.setHandler("app.shortcuts", [&registry] { emit registry.shortcutsToggleRequested(); });
    // The layers panel or the Canvas view under the pointer does the work (CanvasLayersPanel.qml, CanvasView.qml listen
    // for these ids).
    for (const QString &id : registry.ids())
        if (id.startsWith(QLatin1String("canvas-layers.")) || id.startsWith(QLatin1String("canvas-view.")))
            registry.setHandler(id, [&registry, id] { emit registry.windowRequested(id); });
}

} // namespace fugue
