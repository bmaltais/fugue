#include "canvas/canvas_commands.h"

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "workflow/image_inputs.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <utility>

namespace {

// One key press zooms as one wheel step does (ComfyUI's 1.1).
constexpr qreal zoomStep = 1.1;

void copyToClipboard(const CanvasModel &canvas) {
    const QString text = canvas.copySelection();
    if (text.isEmpty())
        return;
    auto *data = new QMimeData;
    data->setData(QString::fromLatin1(canvasClipboardType), text.toUtf8());
    data->setText(text);
    QGuiApplication::clipboard()->setMimeData(data);
}

void pasteFromClipboard(CanvasModel &canvas, bool connected) {
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data)
        return;
    const QString type = QString::fromLatin1(canvasClipboardType);
    // Plain text works too, when it is a workflow; paste() ignores anything else.
    const QString text = data->hasFormat(type) ? QString::fromUtf8(data->data(type)) : data->text();
    // Workflow data first; when there is none, an image goes into a Load Image node (a plain
    // paste only: an image has no links to keep).
    if (!canvas.paste(text, connected).isEmpty())
        return;
    if (!connected && canvas.images())
        canvas.images()->paste(*data);
}

} // namespace

void registerCanvasCommands(fugue::CommandRegistry &commands, CanvasModel &canvas) {
    CanvasModel *model = &canvas;
    commands.setHandler("edit.undo", [model] { model->undo(); });
    commands.setHandler("edit.redo", [model] { model->redo(); });
    commands.setHandler("edit.copy", [model] { copyToClipboard(*model); });
    commands.setHandler("edit.paste", [model] { pasteFromClipboard(*model, false); });
    commands.setHandler("edit.paste-connected", [model] { pasteFromClipboard(*model, true); });
    commands.setHandler("edit.clone", [model] { model->cloneSelection(); });
    commands.setHandler("edit.delete", [model] { model->deleteSelection(); });
    commands.setHandler("edit.select-all", [model] { model->selectAll(); });
    commands.setHandler("node.bypass", [model] { model->toggleMode(QStringLiteral("bypass")); });
    commands.setHandler("node.mute", [model] { model->toggleMode(QStringLiteral("mute")); });
    commands.setHandler("node.collapse", [model] { model->toggleCollapsed(); });
    commands.setHandler("view.fit", [model] { model->requestFit(); });
    commands.setHandler("view.zoom-in", [model] { model->requestZoom(zoomStep); });
    commands.setHandler("view.zoom-out", [model] { model->requestZoom(1 / zoomStep); });
    commands.setHandler("view.toggle-links", [model] { model->setLinksHidden(!model->linksHidden()); });
    commands.setHandler("frame.make", [model] { model->frameSelection(); });
    commands.setHandler("frame.rename", [model, &commands] {
        emit model->frameRenameRequested(commands.target().value("frame").toString());
    });
    // Rename: the group port the menu was opened on (#72), else the one selected node (#55).
    commands.setHandler("edit.rename", [model, &commands] {
        const QVariantMap target = commands.target();
        const QString node = target.value("node").toString();
        if (target.contains("port")
            && !model->groups()->portToRename(node, target.value("port").toString(), target.value("output").toBool()).isEmpty()) {
            emit model->portRenameRequested(node, target.value("port").toString(), target.value("output").toBool());
            return;
        }
        if (model->selectedIds().size() == 1)
            emit model->nodeRenameRequested(model->selectedIds().first());
    });
    commands.setHandler("node.pin", [model] { model->togglePinned(); });
    commands.setHandler("node.picture-show", [model] { model->setPictureOverride(QStringLiteral("show")); });
    commands.setHandler("node.picture-hide", [model] { model->setPictureOverride(QStringLiteral("hide")); });
    commands.setHandler("node.picture-default", [model] { model->setPictureOverride({}); });
    for (const char *mode : {"left", "right", "top", "bottom"})
        commands.setHandler(QStringLiteral("node.align-") + QLatin1String(mode), [model, mode] { model->alignSelection(QLatin1String(mode)); });
    for (const char *mode : {"horizontal", "vertical"})
        commands.setHandler(QStringLiteral("node.distribute-") + QLatin1String(mode), [model, mode] { model->alignSelection(QLatin1String(mode)); });
    commands.setHandler("frame.delete", [model, &commands] {
        model->deleteFrame(commands.target().value("frame").toString());
    });
    commands.setHandler("group.make", [model] { model->groups()->requestMakeGroup(); });
    commands.setHandler("group.ungroup", [model] { model->groups()->ungroupSelection(); });
    commands.setHandler("group.enter", [model] {
        const QStringList selected = model->selectedIds();
        if (selected.size() == 1)
            model->groups()->enterGroup(selected.first());
    });
    commands.setHandler("group.leave", [model] { model->groups()->leaveGroup(); });
    commands.setHandler("group.make-unique", [model] { model->groups()->makeUnique(); });
    // Inside a group: its face. On a group card (#72): that group's, entered first.
    commands.setHandler("group.interface", [model] {
        const QStringList ids = model->selectedIds();
        if (!model->inGroup() && ids.size() == 1 && model->isGroup(ids.first()))
            model->groups()->enterGroup(ids.first());
        emit model->interfaceEditorRequested();
    });
    // Make app (#393): the builder over the whole workflow; Edit app is the same dialog on the block there.
    for (const char *id : {"app.make", "app.edit"})
        commands.setHandler(id, [model] { model->appBuilder()->requestOpen(); });
    commands.setHandler("group.save-to-library", [model] { model->groups()->saveGroupToLibrary(); });
    commands.setHandler("group.update", [model] { model->groups()->requestGroupUpdate(); });
    commands.setHandler("node.move-version", [model] { model->requestMajorMove(); });
    // Compare Images (#62): the node the menu was opened on, else the one selected.
    for (const auto &[id, step] : {std::pair{"compare.flip", "flip"}, {"compare.next-mode", "next-mode"},
                                   {"compare.fit", "fit"}, {"compare.actual", "actual"}})
        commands.setHandler(QString::fromLatin1(id), [model, &commands, step = QString::fromLatin1(step)] {
            const QString target = commands.target().value("node").toString();
            model->compareStep(model->isCompare(target) ? target : model->selectedCompare(), step);
        });
    commands.setContextActive(QStringLiteral("canvas"), true);
    // group-selected: one group instance is selected (Tab enters it); group: one is entered.
    auto contexts = [model, &commands] {
        const QStringList selected = model->selectedIds();
        commands.setContextActive(QStringLiteral("group-selected"), selected.size() == 1 && model->isGroup(selected.first()));
        commands.setContextActive(QStringLiteral("group"), model->inGroup());
        commands.setContextActive(QStringLiteral("compare"), !model->selectedCompare().isEmpty());
    };
    QObject::connect(model, &CanvasModel::selectionChanged, &commands, contexts);
    QObject::connect(model, &CanvasModel::scopeChanged, &commands, contexts);
    QObject::connect(model, &CanvasModel::documentReset, &commands, contexts);
    contexts();
}
