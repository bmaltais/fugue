#include "panels/chrome_commands.h"

#include <QCoreApplication>
#include <QDebug>

#include "commands/command_registry.h"
#include "panels/chrome_visibility.h"

namespace fugue {

namespace {

struct Row {
    const char *id;
    const char *description;
    const char *label;
    const char *chromeId;
    QStringList keys;
};

QList<Row> rows() {
    return {
        {"view.toggle-menu-bar", QT_TRANSLATE_NOOP("Command", "Keep the menu bar shown, or let it auto-hide (F10 slides it in)"),
         QT_TRANSLATE_NOOP("Command", "Menu bar"), "menu-bar", {}},
        {"view.toggle-tabs", QT_TRANSLATE_NOOP("Command", "Show or hide the workflow tabs"),
         QT_TRANSLATE_NOOP("Command", "Workflow tabs"), "workflow-tabs", {}},
        {"view.toggle-run-toolbar", QT_TRANSLATE_NOOP("Command", "Show or hide the run toolbar"),
         QT_TRANSLATE_NOOP("Command", "Run toolbar"), "run-toolbar", {}},
        {"view.toggle-selection-toolbar", QT_TRANSLATE_NOOP("Command", "Show or hide the toolbar above a selection"),
         QT_TRANSLATE_NOOP("Command", "Selection toolbar"), "selection-toolbar", {}},
        {"view.toggle-breadcrumb", QT_TRANSLATE_NOOP("Command", "Show or hide the group breadcrumb"),
         QT_TRANSLATE_NOOP("Command", "Group breadcrumb"), "breadcrumb", {}},
        {"view.toggle-run-status", QT_TRANSLATE_NOOP("Command", "Show or hide the run status at the top right"),
         QT_TRANSLATE_NOOP("Command", "Run status"), "run-status", {}},
        {"view.toggle-zoom-readout", QT_TRANSLATE_NOOP("Command", "Show or hide the zoom readout at the bottom left"),
         QT_TRANSLATE_NOOP("Command", "Zoom readout"), "zoom-readout", {}},
    };
}

QList<Row> otherRows() {
    return {
        {"view.focus-mode", QT_TRANSLATE_NOOP("Command", "Hide all bars and panels, or bring them back as they were"),
         QT_TRANSLATE_NOOP("Command", "Focus mode"), nullptr, {"H"}},
        {"view.reset-bars", QT_TRANSLATE_NOOP("Command", "Show the bars and close the panels, as at the first start"),
         QT_TRANSLATE_NOOP("Command", "Reset bars and panels"), nullptr, {}},
    };
}

void addRow(CommandRegistry &commands, const Row &row) {
    CommandSpec spec;
    spec.id = QString::fromLatin1(row.id);
    if (commands.contains(spec.id))
        return;
    spec.description = QString::fromUtf8(row.description);
    spec.label = QString::fromUtf8(row.label);
    spec.keys = row.keys;
    // The keyed one is a canvas key, as L and N are.
    spec.context = row.keys.isEmpty() ? QStringLiteral("app") : QStringLiteral("canvas");
    spec.versus = QStringLiteral("added");
    if (const QString problem = commands.add(spec); !problem.isEmpty())
        qCritical().noquote() << "chrome command:" << problem;
}

} // namespace

void addChromeCommands(CommandRegistry &commands) {
    for (const Row &row : rows())
        addRow(commands, row);
    for (const Row &row : otherRows())
        addRow(commands, row);
}

void registerChromeCommands(CommandRegistry &commands, ChromeVisibility &chrome) {
    addChromeCommands(commands);
    for (const Row &row : rows()) {
        const QString id = QString::fromLatin1(row.chromeId);
        chrome.add(id);
        if (id == QLatin1String("menu-bar"))
            chrome.setAutoHide(id); // it slides away to a slim edge, never disappears (#160)
        const QString command = QString::fromLatin1(row.id);
        commands.setHandler(command, [&chrome, id] { chrome.toggle(id); });
        commands.setCheckedWhen(command, [&chrome, id] { return chrome.isVisible(id); });
    }
    commands.setHandler("view.focus-mode", [&chrome] { chrome.toggleFocusMode(); });
    commands.setCheckedWhen("view.focus-mode", [&chrome] { return chrome.focusMode(); });
    commands.setHandler("view.reset-bars", [&chrome] { chrome.reset(); });

    // F10 opens the menu bar; an auto-hidden bar slides in for it (the window does that) and
    // slides out again after, so the remembered mode does not change.
    commands.setHandler("app.menu-bar", [&commands] { emit commands.windowRequested(QStringLiteral("app.menu-bar")); });
}

} // namespace fugue
