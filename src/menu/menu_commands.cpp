#include "menu/menu_commands.h"

#include <algorithm>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>

#include "canvas/canvas_commands.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "menu/menus.h"
#include "workflow/image_inputs.h"

namespace fugue {

namespace {

const char *const docsUrl = "https://github.com/bmaltais/fugue#readme";
const char *const issuesUrl = "https://github.com/bmaltais/fugue/issues/new";

struct Row {
    const char *id;
    const char *description;
    const char *label;
    QStringList keys;
    const char *context;
    const char *versus;
};

QList<Row> menuRows() {
    return {
        // Also registered by registerFileCommands (workflows.h) when this has not run.
        {"file.open-clipboard", QT_TRANSLATE_NOOP("Command", "Open the workflow on the clipboard"),
         QT_TRANSLATE_NOOP("Command", "Open from clipboard"), {}, "app", "added"},
        // Also registered by registerFileCommands (workflows.h) when this has not run (#83).
        {"file.templates", QT_TRANSLATE_NOOP("Command", "Browse templates"), nullptr, {}, "app", "same"},
        {"file.export-share", QT_TRANSLATE_NOOP("Command", "Export for sharing: a .zip with its images, local paths removed"),
         QT_TRANSLATE_NOOP("Command", "Export for sharing…"), {}, "app", "same"},
        {"file.export-compiled", QT_TRANSLATE_NOOP("Command", "Export the compiled graph, for the CLI"),
         QT_TRANSLATE_NOOP("Command", "Export compiled graph…"), {}, "app", "same"},
        {"edit.clear", QT_TRANSLATE_NOOP("Command", "Clear the workflow (remove every node)"),
         QT_TRANSLATE_NOOP("Command", "Clear workflow"), {}, "canvas", "same"},
        {"node.color.none", QT_TRANSLATE_NOOP("Command", "Color: none"), QT_TRANSLATE_NOOP("Command", "No color"), {}, "canvas", "same"},
        {"node.color.accent", QT_TRANSLATE_NOOP("Command", "Color: accent"), QT_TRANSLATE_NOOP("Command", "Accent"), {}, "canvas", "same"},
        {"node.color.muted", QT_TRANSLATE_NOOP("Command", "Color: muted"), QT_TRANSLATE_NOOP("Command", "Muted"), {}, "canvas", "same"},
        {"node.color.red", QT_TRANSLATE_NOOP("Command", "Color: red"), QT_TRANSLATE_NOOP("Command", "Red"), {}, "canvas", "same"},
        {"node.color.orange", QT_TRANSLATE_NOOP("Command", "Color: orange"), QT_TRANSLATE_NOOP("Command", "Orange"), {}, "canvas", "same"},
        {"node.color.yellow", QT_TRANSLATE_NOOP("Command", "Color: yellow"), QT_TRANSLATE_NOOP("Command", "Yellow"), {}, "canvas", "same"},
        {"node.color.green", QT_TRANSLATE_NOOP("Command", "Color: green"), QT_TRANSLATE_NOOP("Command", "Green"), {}, "canvas", "same"},
        {"node.color.cyan", QT_TRANSLATE_NOOP("Command", "Color: cyan"), QT_TRANSLATE_NOOP("Command", "Cyan"), {}, "canvas", "same"},
        {"node.color.blue", QT_TRANSLATE_NOOP("Command", "Color: blue"), QT_TRANSLATE_NOOP("Command", "Blue"), {}, "canvas", "same"},
        {"node.color.magenta", QT_TRANSLATE_NOOP("Command", "Color: magenta"), QT_TRANSLATE_NOOP("Command", "Magenta"), {}, "canvas", "same"},
        // Handlers come from ImageActions (image_actions.h).
        {"node.open-image", QT_TRANSLATE_NOOP("Command", "Open the node's image in the default viewer"),
         QT_TRANSLATE_NOOP("Command", "Open Image"), {}, "canvas", "same"},
        {"node.copy-image", QT_TRANSLATE_NOOP("Command", "Copy the node's image to the clipboard"),
         QT_TRANSLATE_NOOP("Command", "Copy Image"), {}, "canvas", "same"},
        {"node.save-image", QT_TRANSLATE_NOOP("Command", "Save the node's image to a file"),
         QT_TRANSLATE_NOOP("Command", "Save Image…"), {}, "canvas", "same"},
        {"help.docs", QT_TRANSLATE_NOOP("Command", "Open the documentation"), QT_TRANSLATE_NOOP("Command", "Documentation"), {}, "app", "same"},
        {"help.report-issue", QT_TRANSLATE_NOOP("Command", "Report an issue"), nullptr, {}, "app", "same"},
        {"help.about", QT_TRANSLATE_NOOP("Command", "About Fugue"), nullptr, {}, "app", "same"},
        {"group.make-unique", QT_TRANSLATE_NOOP("Command", "Make the selected group unique (its own copy of the definition)"),
         QT_TRANSLATE_NOOP("Command", "Make unique"), {}, "group-selected", "same"},
        {"group.interface", QT_TRANSLATE_NOOP("Command", "Edit the inputs and outputs of the group shown or selected"),
         QT_TRANSLATE_NOOP("Command", "Group inputs and outputs…"), {}, "canvas", "added"},
        {"group.save-to-library", QT_TRANSLATE_NOOP("Command", "Save the selected group, or the group shown, to the library"),
         QT_TRANSLATE_NOOP("Command", "Save to library"), {}, "canvas", "added"},
        {"group.update", QT_TRANSLATE_NOOP("Command", "Update the selected group from the library"),
         QT_TRANSLATE_NOOP("Command", "Update group…"), {}, "group-selected", "added"},
        {"group.expose", QT_TRANSLATE_NOOP("Command", "Expose the port on the group's face"),
         QT_TRANSLATE_NOOP("Command", "Expose on the group"), {}, "group", "same"},
        {"group.unexpose", QT_TRANSLATE_NOOP("Command", "Stop exposing the port on the group's face"),
         QT_TRANSLATE_NOOP("Command", "Stop exposing"), {}, "group", "same"},
        {"frame.rename", QT_TRANSLATE_NOOP("Command", "Rename the Frame"), QT_TRANSLATE_NOOP("Command", "Rename Frame…"), {}, "canvas", "same"},
        {"frame.delete", QT_TRANSLATE_NOOP("Command", "Delete the Frame (its nodes stay)"),
         QT_TRANSLATE_NOOP("Command", "Delete Frame"), {}, "canvas", "same"},
        {"app.menu-bar", QT_TRANSLATE_NOOP("Command", "Open the menu bar"), nullptr, {"F10"}, "app", "added"},
        {"app.context-menu", QT_TRANSLATE_NOOP("Command", "Open the context menu"), nullptr, {"Shift+F10", "Menu"}, "canvas", "added"},
    };
}

// Shorter names for the menus than the keymap table's descriptions.
QList<QPair<const char *, const char *>> menuLabels() {
    return {
        {"file.close", QT_TRANSLATE_NOOP("Command", "Close workflow")},
        {"file.open", QT_TRANSLATE_NOOP("Command", "Open…")},
        {"file.save-as", QT_TRANSLATE_NOOP("Command", "Save as…")},
        {"app.quit", QT_TRANSLATE_NOOP("Command", "Quit")},
        {"app.shortcuts", QT_TRANSLATE_NOOP("Command", "Keyboard shortcuts")},
        {"app.settings", QT_TRANSLATE_NOOP("Command", "Settings…")},
        {"node.search", QT_TRANSLATE_NOOP("Command", "Add node…")},
        {"node.pin", QT_TRANSLATE_NOOP("Command", "Pin")},
        {"frame.make", QT_TRANSLATE_NOOP("Command", "Frame selection")},
        {"group.enter", QT_TRANSLATE_NOOP("Command", "Enter group")},
        {"group.leave", QT_TRANSLATE_NOOP("Command", "Leave group")},
        {"view.toggle-links", QT_TRANSLATE_NOOP("Command", "Show links")},
        {"run.start-first", QT_TRANSLATE_NOOP("Command", "Run first in the queue")},
        {"run.to-here", QT_TRANSLATE_NOOP("Command", "Run to here")},
        {"run.cancel", QT_TRANSLATE_NOOP("Command", "Cancel run")},
    };
}

} // namespace

void registerMenuCommands(CommandRegistry &registry) {
    for (const Row &row : menuRows()) {
        if (registry.contains(QString::fromLatin1(row.id)))
            continue;
        CommandSpec spec;
        spec.id = QString::fromLatin1(row.id);
        spec.description = QString::fromUtf8(row.description);
        if (row.label)
            spec.label = QString::fromUtf8(row.label);
        spec.keys = row.keys;
        spec.context = QString::fromLatin1(row.context);
        spec.versus = QString::fromLatin1(row.versus);
        if (const QString problem = registry.add(spec); !problem.isEmpty())
            qCritical().noquote() << "menu command:" << problem;
    }
    for (const auto &[id, label] : menuLabels())
        registry.setLabel(QString::fromLatin1(id), QString::fromUtf8(label));
    for (const char *id : {"app.menu-bar", "app.context-menu", "help.about"})
        registry.setHandler(QString::fromLatin1(id), [&registry, id] { emit registry.windowRequested(QString::fromLatin1(id)); });
}

void bindMenuCommands(CommandRegistry &registry, CanvasModel &canvas, std::function<void(const QUrl &)> openUrl) {
    CanvasModel *model = &canvas;
    if (!openUrl)
        openUrl = [](const QUrl &url) { QDesktopServices::openUrl(url); };
    registry.setHandler("help.docs", [openUrl] { openUrl(QUrl(QString::fromLatin1(docsUrl))); });
    registry.setHandler("help.report-issue", [openUrl] { openUrl(QUrl(QString::fromLatin1(issuesUrl))); });

    registry.setHandler("edit.clear", [model] {
        model->beginEdit();
        model->selectAll();
        model->deleteSelection();
        model->endEdit(QCoreApplication::translate("Menu", "Clear workflow"));
    });
    for (const QString &role : menus::colorRoles())
        registry.setHandler(QStringLiteral("node.color.") + role, [model, &registry, role] {
            applyLayoutColor(*model, role, registry.target().value("frame").toString());
        });

    const auto selected = [model] { return !model->selectedIds().isEmpty(); };
    const auto hasNodes = [model] { return model->nodes()->rowCount() > 0; };
    registry.setEnabledWhen("edit.undo", [model] { return model->canUndo(); });
    registry.setEnabledWhen("edit.redo", [model] { return model->canRedo(); });
    for (const char *id : {"edit.copy", "edit.clone", "edit.delete", "node.bypass", "node.mute",
                           "node.collapse", "node.pin", "frame.make", "group.make"})
        registry.setEnabledWhen(QString::fromLatin1(id), selected);
    // Rename: one node (#55), or a group's port the menu was opened on (#72).
    registry.setEnabledWhen("edit.rename", [model, &registry] {
        const QVariantMap target = registry.target();
        if (target.contains("port")
            && !model->portToRename(target.value("node").toString(), target.value("port").toString(),
                                    target.value("output").toBool()).isEmpty())
            return true;
        return model->selectedIds().size() == 1;
    });
    // Run to here runs up to one node (spec 12.4); with several selected it would do nothing.
    registry.setEnabledWhen("run.to-here", [model] { return model->selectedIds().size() == 1; });
    for (const char *id : {"edit.select-all", "edit.clear", "view.fit", "run.start", "run.start-first",
                           "file.export-share", "file.export-compiled"})
        registry.setEnabledWhen(QString::fromLatin1(id), hasNodes);
    // Paste takes Fugue's own nodes, a workflow as text (a JSON object, checked cheaply here and
    // in full by the paste), or an image (a Load Image node, 14.1); not any text (#72).
    const auto workflowOnClipboard = [](const QMimeData *data) {
        return data->hasFormat(QString::fromLatin1(canvasClipboardType))
               || data->text().trimmed().startsWith(QLatin1Char('{'));
    };
    registry.setEnabledWhen("edit.paste", [workflowOnClipboard] {
        const QMimeData *data = QGuiApplication::clipboard()->mimeData();
        return data && (workflowOnClipboard(data) || clipboardHasImage(*data));
    });
    registry.setEnabledWhen("edit.paste-connected", [workflowOnClipboard] {
        const QMimeData *data = QGuiApplication::clipboard()->mimeData();
        return data && workflowOnClipboard(data);
    });
    // A right-click on a port inside a group: the menu's target names it.
    const auto exposed = [model, &registry] {
        const QVariantMap target = registry.target();
        return model->exposedName(target.value("node").toString(), target.value("port").toString(),
                                  target.value("output").toBool());
    };
    const auto onPort = [model, &registry] { return model->inGroup() && registry.target().contains("port"); };
    registry.setEnabledWhen("group.expose", [onPort, exposed] { return onPort() && exposed().isEmpty(); });
    registry.setEnabledWhen("group.unexpose", [onPort, exposed] { return onPort() && !exposed().isEmpty(); });
    registry.setHandler("group.expose", [model, &registry] {
        const QVariantMap target = registry.target();
        if (target.value("output").toBool())
            model->exposeOutput(target.value("node").toString(), target.value("port").toString());
        else
            model->exposeInput(target.value("node").toString(), target.value("port").toString());
    });
    registry.setHandler("group.unexpose", [model, &registry, exposed] {
        const QVariantMap target = registry.target();
        if (target.value("output").toBool())
            model->unexposeOutput(exposed());
        else
            model->unexposeInput(target.value("node").toString(), target.value("port").toString());
    });
    registry.setEnabledWhen("group.save-to-library", [model] {
        const QStringList ids = model->selectedIds();
        return (ids.size() == 1 && model->isGroup(ids.first())) || model->inGroup();
    });
    // Make unique detaches an instance from a definition others share (5.3); alone, it has none to
    // leave.
    registry.setEnabledWhen("group.make-unique", [model] {
        const QStringList ids = model->selectedIds();
        return ids.size() == 1 && model->groupInstances(ids.first()) > 1;
    });
    registry.setEnabledWhen("group.interface", [model] {
        const QStringList ids = model->selectedIds();
        return model->inGroup() || (ids.size() == 1 && model->isGroup(ids.first()));
    });
    registry.setEnabledWhen("group.update", [model] {
        const QStringList ids = model->selectedIds();
        return ids.size() == 1 && !model->updateAvailable(ids.first()).isEmpty();
    });
    const auto onFrame = [&registry] { return !registry.target().value("frame").toString().isEmpty(); };
    registry.setEnabledWhen("frame.rename", onFrame);
    registry.setEnabledWhen("frame.delete", onFrame);
    registry.setEnabledWhen("group.ungroup", [model] {
        const QStringList ids = model->selectedIds();
        return std::any_of(ids.begin(), ids.end(), [model](const QString &id) { return model->isGroup(id); });
    });
    for (const QString &role : menus::colorRoles())
        registry.setEnabledWhen(QStringLiteral("node.color.") + role, [model, &registry] {
            return !model->selectedIds().isEmpty() || !registry.target().value("frame").toString().isEmpty();
        });
    // A toggle names what it will do to the selection: it undoes the state only when every
    // selected node has it, as the command itself does (spec 12.4).
    const auto toggleLabel = [model, &registry](const char *id, const char *state, const char *undo) {
        registry.setLabelWhen(QString::fromLatin1(id), [model, state, undo] {
            return model->selectionAll(QString::fromLatin1(state)) ? QString::fromUtf8(undo) : QString();
        });
    };
    toggleLabel("node.bypass", "bypass", QT_TRANSLATE_NOOP("Command", "Remove bypass"));
    toggleLabel("node.mute", "mute", QT_TRANSLATE_NOOP("Command", "Unmute"));
    toggleLabel("node.collapse", "collapsed", QT_TRANSLATE_NOOP("Command", "Expand"));
    toggleLabel("node.pin", "pinned", QT_TRANSLATE_NOOP("Command", "Unpin"));
    registry.setCheckedWhen("view.toggle-links", [model] { return !model->linksHidden(); });
}

void applyLayoutColor(CanvasModel &canvas, const QString &role, const QString &frame) {
    Document document = canvas.scopeDocument();
    QJsonObject nodes = document.layout.value("nodes").toObject();
    const auto paint = [&role](QJsonObject entry) {
        if (role == QLatin1String("none"))
            entry.remove("color");
        else
            entry.insert("color", role);
        return entry;
    };
    // From a Frame's menu the color is the Frame's alone, whatever is selected (Appendix A).
    const QStringList selected = frame.isEmpty() ? canvas.selectedIds() : QStringList();
    for (const QString &id : selected)
        nodes.insert(id, paint(nodes.value(id).toObject()));
    if (!selected.isEmpty())
        document.layout.insert("nodes", nodes);
    if (!frame.isEmpty()) {
        QJsonArray frames = document.layout.value("frames").toArray();
        for (qsizetype i = 0; i < frames.size(); ++i)
            if (frames.at(i).toObject().value("id").toString() == frame)
                frames.replace(i, paint(frames.at(i).toObject()));
        document.layout.insert("frames", frames);
    }
    document.present.layout = true;
    canvas.commit(document, QCoreApplication::translate("Menu", "Color"));
}

} // namespace fugue
