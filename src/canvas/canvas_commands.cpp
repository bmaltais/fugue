#include "canvas/canvas_commands.h"

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

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
    canvas.paste(text, connected);
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
    commands.setContextActive(QStringLiteral("canvas"), true);
}
