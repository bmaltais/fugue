#pragma once

// The open workflows of the window (spec 11, 12.10): tabs, open, save, import, recovery and
// the trust question. The canvas shows one workflow at a time; this holds the others as
// documents and swaps them in and out of the CanvasModel when the tab changes.
//
// Everything with a rule lives here, in C++; the QML (src/workflow/*.qml) only shows the tabs,
// the dialogs and the banner, and calls these methods.
//
// Seams for the neighbours:
// - #17 undo: each tab keeps its own undo stack (12.10). The canvas holds the shown tab's;
//   the others wait in their Tab, swapped with CanvasModel::takeUndoStack()/setUndoStack().
//   `contentHash()` (content_hash.h) is the one definition of "unsaved", which the canvas
//   also uses to tell whether an edit changed anything.
// - #19 run: `isTrusted()` and `trust()` answer for a pack before a node of it runs.
// - Dialogs: `openDialogRequested`, `saveAsRequested`, `closeConfirmationRequested` ask the
//   window to show the file picker (the desktop portal) or the save/discard/cancel question.

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "workflow/recovery_store.h"
#include "workflow/trust_store.h"

#include <QObject>
#include <QUndoStack>
#include <QUrl>
#include <QTimer>
#include <QVariantList>

#include <memory>

namespace fugue {

// Where Workflows keeps its files; tests point them into a temporary folder.
struct WorkflowLocations {
    QString recoveryDir = RecoveryStore::defaultDir();
    QString trustPath = TrustStore::defaultPath();
    int recoveryIntervalMs = 30000;
};

class Workflows : public QObject {
    Q_OBJECT
    // One map per tab: id, title, path, unsaved, current.
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY tabsChanged) // any tab
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY tabsChanged)
    // Recovery copies from an earlier session, offered at start: id, title, path, modified.
    Q_PROPERTY(QVariantList recoverable READ recoverable NOTIFY recoverableChanged)
    // Packs of the workflow just opened that the user has not allowed: id, version, source, types.
    Q_PROPERTY(QVariantList pendingTrust READ pendingTrust NOTIFY pendingTrustChanged)
    // A line for the banner: what failed to open, or what the workflow lacks. Empty for none.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    // Where the file picker starts: ~/Documents/Fugue as a file URL.
    Q_PROPERTY(QUrl suggestedFolder READ suggestedFolder CONSTANT)

public:
    Workflows(CanvasModel *canvas, const Registry *registry, CommandRegistry *commands,
              WorkflowLocations locations = {}, QObject *parent = nullptr);

    QVariantList tabs() const;
    int current() const { return m_current; }
    void setCurrent(int index);
    int count() const { return int(m_tabs.size()); }
    bool unsaved() const;
    bool unsaved(int index) const;
    QString windowTitle() const;
    QVariantList recoverable() const { return m_recoverable; }
    QVariantList pendingTrust() const { return m_pendingTrust; }
    QString notice() const { return m_notice; }
    QUrl suggestedFolder() const;
    QString pathOf(int index) const;

    // --- opening ---

    Q_INVOKABLE void newWorkflow();
    // Opens a workflow from a file path or file URL: a .fugue, or an image with a workflow
    // in it, by content. Switches to the tab when it is already open. False (with a notice)
    // when it cannot be read.
    Q_INVOKABLE bool openPath(const QString &pathOrUrl);
    // Drag and drop: file URLs (or paths), each opened; others are skipped with a notice.
    Q_INVOKABLE void openUrls(const QVariantList &urls);
    // JSON text, as pasted: a workflow document, opened as a new tab with no file.
    Q_INVOKABLE bool openText(const QString &text);
    Q_INVOKABLE bool openClipboard();
    // What the command line gave.
    void openStartup(const QStringList &paths);

    // --- saving and closing ---

    // Saves the current tab: asks for a place first (saveAsRequested) when it has none.
    Q_INVOKABLE bool save();
    // Saves the tab under a path or file URL; ".fugue" is added when the name has no extension.
    Q_INVOKABLE bool saveAs(const QString &pathOrUrl);
    // Closes a tab, asking first (closeConfirmationRequested) when it has unsaved content.
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void closeCurrent() { closeTab(m_current); }
    Q_INVOKABLE void discardAndClose(int index);
    Q_INVOKABLE void saveAndClose(int index);
    // The file picker or the question was cancelled.
    Q_INVOKABLE void cancelPendingClose() { m_closeAfterSave.clear(); }

    // --- recovery ---

    // Opens every recovery copy of an earlier session as an unsaved tab, or deletes them.
    Q_INVOKABLE void recover();
    Q_INVOKABLE void discardRecovery();
    // Writes a copy of each tab with unsaved content, removes the copy of a tab that has
    // none. Runs every 30 seconds.
    void writeRecoveryCopies();

    // --- trust ---

    Q_INVOKABLE void allowPendingTrust();
    Q_INVOKABLE void dismissPendingTrust();
    bool isTrusted(const QString &packId, const QString &version) const;

    Q_INVOKABLE void dismissNotice();

signals:
    void tabsChanged();
    void currentChanged(int from, int to);
    void tabClosed(const QString &id);
    void recoverableChanged();
    void pendingTrustChanged();
    void noticeChanged();
    void openDialogRequested();
    void saveAsRequested();
    void closeConfirmationRequested(int index, const QString &title);

private:
    struct Tab {
        QString id;
        QString path;
        QString title;
        Document document; // current while the tab is not shown; the canvas holds the live one
        QString savedHash; // empty: never matches, so the tab is unsaved
        QString recoveryHash;
        bool unsaved = false;
        std::unique_ptr<QUndoStack> undo; // while the tab is not shown
    };

    // Adds a tab and shows it. A current tab that is still an empty "Untitled" is replaced
    // when `replacePristine`, so opening a file from a fresh window does not leave a blank tab.
    int addTab(const Document &document, const QString &path, const QString &title, bool forceUnsaved,
               bool replacePristine, const QString &id = {});
    void removeTab(int index);
    Document documentOf(int index) const;
    bool saveTab(int index, const QString &path);
    void snapshotCurrent();
    void refreshUnsaved();
    void refreshRegistry();
    bool isPristine(int index) const;
    QString untitledName() const;
    void setNotice(const QString &text);
    void analyze(const Document &document, const QString &title);
    static QString toPath(const QString &pathOrUrl);

    CanvasModel *m_canvas;
    const Registry *m_registry;
    CommandRegistry *m_commands;
    RecoveryStore m_recovery;
    TrustStore m_trust;
    QTimer m_timer;
    std::vector<Tab> m_tabs;
    int m_current = -1;
    QString m_closeAfterSave; // the id of the tab to close once its save-as succeeds
    QVariantList m_recoverable;
    QVariantList m_pendingTrust;
    QString m_notice;
};

// Wires the file commands of the keymap (file.new, file.open, file.save, file.save-as,
// file.close) to the workflows, and adds `file.open-clipboard`.
void registerFileCommands(CommandRegistry &registry, Workflows &workflows);

} // namespace fugue
