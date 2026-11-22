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
        {"panel.nodes", QT_TRANSLATE_NOOP("Command", "Toggle the node library"), {"N"}, {}, "canvas", same},
        {"panel.workflows", QT_TRANSLATE_NOOP("Command", "Toggle the workflows panel"), {"W"}, {}, "canvas", same},
        {"panel.queue", QT_TRANSLATE_NOOP("Command", "Toggle the run queue"), {"Alt+Q"}, {}, "canvas", "added"},
        // The Canvas view's layers panel (#226): its keys while the pointer is over it (Blender's hover rule).
        {"canvas-layers.visible", QT_TRANSLATE_NOOP("Command", "Hide or show the selected layers"), {"H"}, {"Click the eye", "Alt+click the eye: that layer alone"}, "canvas-layers", "added"},
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
        {"canvas-view.tool-zoom", QT_TRANSLATE_NOOP("Command", "Zoom: click to zoom in, Alt+click to zoom out"), {"Z"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom")},
        {"canvas-view.tool-region", QT_TRANSLATE_NOOP("Command", "Rectangle region"), {"M", "R"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rectangle region")},
        {"canvas-view.tool-brush", QT_TRANSLATE_NOOP("Command", "Mask brush"), {"B"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Mask brush")},
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
        {"canvas-view.export", QT_TRANSLATE_NOOP("Command", "Export the shown revision as a picture (.png, .jpg, .webp) or with its layers (.ora)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Export as…")},
        {"canvas-view.rename", QT_TRANSLATE_NOOP("Command", "Rename the canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Rename canvas…")},
        {"canvas-view.duplicate", QT_TRANSLATE_NOOP("Command", "Duplicate the canvas with its whole history"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Duplicate canvas")},
        {"canvas-view.clear", QT_TRANSLATE_NOOP("Command", "Clear the canvas: one undoable step that leaves one empty layer"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Clear canvas…")},
        {"canvas-view.delete", QT_TRANSLATE_NOOP("Command", "Delete the canvas for good"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Delete canvas…")},
        {"canvas-view.close", QT_TRANSLATE_NOOP("Command", "Close the canvas view"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Close canvas")},
        {"canvas-view.select-all", QT_TRANSLATE_NOOP("Command", "Select the whole canvas"), {"Ctrl+A"}, {}, "canvas-view", "Ctrl+A added", QT_TRANSLATE_NOOP("Command", "Select all")},
        {"canvas-view.deselect", QT_TRANSLATE_NOOP("Command", "Clear the selection: the rectangle, the Region node's area and the painted selection"), {"Ctrl+D"}, {"Escape"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Deselect")},
        {"canvas-view.invert-selection", QT_TRANSLATE_NOOP("Command", "Invert the painted selection"), {"Ctrl+Shift+I"}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Inverse")},
        {"canvas-view.use-selection", QT_TRANSLATE_NOOP("Command", "Give the painted selection to the followed Canvas Region (one undo step)"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Use selection")},
        {"canvas-view.mask-reveal", QT_TRANSLATE_NOOP("Command", "Reveal the whole layer mask"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Reveal all")},
        {"canvas-view.mask-hide", QT_TRANSLATE_NOOP("Command", "Hide the whole layer mask"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Hide all")},
        {"canvas-view.fit", QT_TRANSLATE_NOOP("Command", "Fit the picture in the view"), {}, {"Home or 0"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Fit")},
        {"canvas-view.actual-size", QT_TRANSLATE_NOOP("Command", "Show the picture at 100%"), {}, {"1"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "100%")},
        {"canvas-view.zoom-in", QT_TRANSLATE_NOOP("Command", "Zoom in the picture"), {}, {"+"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom in")},
        {"canvas-view.zoom-out", QT_TRANSLATE_NOOP("Command", "Zoom out the picture"), {}, {"-"}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Zoom out")},
        {"canvas-view.history", QT_TRANSLATE_NOOP("Command", "Show or hide the history of the canvas"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "History")},
        {"canvas-view.layers", QT_TRANSLATE_NOOP("Command", "Show or hide the layers panel"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Layers")},
        {"canvas-view.compare", QT_TRANSLATE_NOOP("Command", "Compare the shown step with the one before it"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Compare")},
        {"canvas-view.show-candidate", QT_TRANSLATE_NOOP("Command", "Show a candidate over the latest version, or the latest version alone"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Show candidate")},
        {"canvas-view.details", QT_TRANSLATE_NOOP("Command", "Show or hide the advanced details under the picture"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Advanced details")},
        {"canvas-view.snap", QT_TRANSLATE_NOOP("Command", "Snap the region's edges to the node's Multiple of"), {}, {}, "canvas-view", "added", QT_TRANSLATE_NOOP("Command", "Snap region")},
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
