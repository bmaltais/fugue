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
        {"view.zoom-in", QT_TRANSLATE_NOOP("Command", "Zoom in"), {"Alt+=", "Ctrl+="}, {}, "canvas", "Ctrl added"},
        {"view.zoom-out", QT_TRANSLATE_NOOP("Command", "Zoom out"), {"Alt+-", "Ctrl+-"}, {}, "canvas", "Ctrl added"},
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
    }
    registry.setHandler("app.quit", [&registry] { registry.requestQuit(); });
    registry.setHandler("file.close", [&registry] { registry.requestClose(); });
    registry.setHandler("app.shortcuts", [&registry] { emit registry.shortcutsToggleRequested(); });
    // The layers panel under the pointer does the work (CanvasView.qml listens for these ids).
    for (const QString &id : registry.ids())
        if (id.startsWith(QLatin1String("canvas-layers.")))
            registry.setHandler(id, [&registry, id] { emit registry.windowRequested(id); });
}

} // namespace fugue
