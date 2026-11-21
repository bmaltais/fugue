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
#include "engine/templates.h"
#include "workflow/image_inputs.h"
#include "workflow/model_downloads.h"
#include "workflow/recovery_store.h"
#include "engine/trust_store.h"

#include <QObject>
#include <QSet>
#include <QUndoStack>
#include <QUrl>
#include <QTimer>
#include <QHash>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

namespace fugue {

// Where Workflows keeps its files; tests point them into a temporary folder.
struct WorkflowLocations {
    QString recoveryDir = RecoveryStore::defaultDir();
    QString trustPath = TrustStore::defaultPath();
    int recoveryIntervalMs = 30000;
    // The colors of the tabs of saved workflows, by file path (a view preference, not document content).
    QString tabColorsPath = TrustStore::defaultPath().section('/', 0, -2) + "/tab-colors.json";
    // Where the templates are (11.10): the person's own folder, then the bundled one.
    QList<TemplateRoot> templateRoots = TemplateLibrary::standardRoots();
};

class Workflows : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // One map per tab: id, title, path, unsaved, current, color (an index into the look's tabColors).
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
    // The models being downloaded (#89).
    Q_PROPERTY(fugue::ModelDownloads *downloads READ downloads CONSTANT)
    // While the banner names missing models of the shown workflow that have a source: the
    // button's text, "Download 3 models, 19.4 GB" (the size when every one records it). Else empty.
    Q_PROPERTY(QString downloadOffer READ downloadOffer NOTIFY noticeChanged)
    // The model folders those may go in (ModelLibrary::downloadRoots), each {path, title}, and
    // the one they go in now (the settings' download_folder, else Fugue's own).
    Q_PROPERTY(QVariantList downloadRoots READ downloadRoots NOTIFY noticeChanged)
    Q_PROPERTY(QString downloadRoot READ downloadRoot NOTIFY noticeChanged)
    // While the banner names nodes of the shown workflow that are not installed, and its
    // `requires.packs` says where the first such pack can be had (#91): {id, source, ref, text}.
    // installPack() asks the window to install it (Settings > Packs, which asks the person).
    Q_PROPERTY(QVariantMap packOffer READ packOffer NOTIFY noticeChanged)

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
    ModelDownloads *downloads() const { return m_downloads; }
    QString downloadOffer() const;
    QVariantList downloadRoots() const { return downloadOffer().isEmpty() ? QVariantList() : m_offer.roots; }
    QString downloadRoot() const { return m_offer.root; }
    QVariantMap packOffer() const;
    QString pathOf(int index) const;
    // Pasting and dropping images into the canvas (workflow/image_inputs.h).
    ImageInputs *images() const { return m_images; }

    // --- opening ---

    Q_INVOKABLE void newWorkflow();
    // Opens a workflow from a file path or file URL: a .fugue, or an image with a workflow
    // in it, by content. Switches to the tab when it is already open. False (with a notice)
    // when it cannot be read.
    Q_INVOKABLE bool openPath(const QString &pathOrUrl);
    // Drag and drop: file URLs (or paths), each opened; others are skipped with a notice.
    Q_INVOKABLE void openUrls(const QVariantList &urls);
    // Files dropped on the window at a point of the canvas (world coordinates): images go into
    // Load Image nodes (into the one at the point, when there is one), and a .fugue or an
    // output with its workflow embedded opens as a workflow, as openUrls does. Dropped on a
    // Load Image, an image always replaces its image, even one that has a workflow in it.
    Q_INVOKABLE void dropUrls(const QVariantList &urls, qreal x, qreal y);
    // A template (engine/templates.h) as a new tab with no file, named after it. The sample
    // images it names are taken into the inputs folder as a pasted image is (image_inputs.h),
    // so it runs as it opens and a save copies them beside the file. False (with a notice) when
    // it cannot be read.
    // `fromPackage`: its folder goes away once it is open (openPackage), so an input still
    // naming a file in it (a file the sender lacked, or one not an image) keeps only the bare name.
    bool openTemplate(const Template &chosen, bool fromPackage = false);
    // The templates for the Templates dialog, in TemplateLibrary::list()'s order, each a map:
    // id, title, description, category, categoryTitle, tags, preview (a file URL or empty),
    // own, missingModels (name, source, folder each), neededPacks (not installed, or at another version), and status, one line: what
    // it lacks on this machine, or empty when it is ready; download, the button's text for its
    // missing models that have a source (as downloadOffer), else empty. A check: no model file is read.
    Q_INVOKABLE QVariantList templates() const;
    // Opens the template with this id (openTemplate); false when there is none.
    Q_INVOKABLE bool openTemplateById(const QString &id);
    // The person's own templates folder, as the dialog names it.
    Q_INVOKABLE QString ownTemplatesFolder() const { return TemplateLibrary::ownFolder(); }
    // A package another Fugue exported (engine/share_package.h, #91), unpacked and opened as
    // its template is (openTemplate). openPath() takes one by its contents.
    bool openPackage(const QString &path);
    // Writes the shown workflow as a package at a path or file URL (".fuguepack" added when the
    // name does not end so), never over a file; `preview` is a picture it made, or empty. The banner says
    // where it went, and what it left out. False when nothing was written.
    Q_INVOKABLE bool exportShare(const QString &pathOrUrl, const QString &preview);
    // Writes the shown workflow's compiled graph (4.8), as `fugue compile` prints it, at a path
    // or file URL (".json" added when the name has none), never over a file. A workflow that
    // does not compile says why in the banner. False when nothing was written.
    Q_INVOKABLE bool exportCompiled(const QString &pathOrUrl);
    // JSON text, as pasted: a workflow document, opened as a new tab with no file.
    Q_INVOKABLE bool openText(const QString &text);
    Q_INVOKABLE bool openClipboard();
    // What the command line gave.
    void openStartup(const QStringList &paths);

    // --- saving and closing ---

    // Saves the current tab: asks for a place first (saveAsRequested) when it has none.
    Q_INVOKABLE bool save();
    // Saves the tab under a path or file URL; ".fugue" is added when the name has no extension,
    // and that name never replaces a file already there (the dialog asked about the name as
    // typed): a notice says so instead.
    Q_INVOKABLE bool saveAs(const QString &pathOrUrl);
    // Closes a tab, asking first (closeConfirmationRequested) when it has unsaved content.
    Q_INVOKABLE void closeTab(int index);
    // A tab's color: an index 0 .. kTabColors - 1 into the look's tabColors. A saved workflow's is
    // remembered by its path (not in the file: a color is not content, so it is no edit and no unsaved mark).
    Q_INVOKABLE void setTabColor(int index, int color);
    static constexpr int kTabColors = 6;
    Q_INVOKABLE void closeCurrent() { closeTab(m_current); }
    // The tab menu's commands (#59). Rename: a saved workflow's file is renamed in its folder
    // (".fugue" is added when the name has none, and a name already taken is refused with a
    // notice); an unsaved one just takes the name. Duplicate: an unsaved copy of the tab's
    // content in a new tab. Close others: closes the other tabs that have no unsaved content and
    // says how many it kept.
    Q_INVOKABLE bool renameTab(int index, const QString &name);
    Q_INVOKABLE void duplicateTab(int index);
    Q_INVOKABLE void closeOtherTabs(int index);
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

    // The registry was filled again, or trust given (#70): the banner's missing nodes are
    // counted again for the shown workflow, and pendingTrust asks about its packs as they are
    // installed and trusted now.
    void refreshNodeTypes();

    // --- model downloads (#89) ---

    // Downloads the missing models the banner offers (downloadOffer), into downloadRoot().
    Q_INVOKABLE void downloadMissing();
    // Downloads the missing models of the template with this id.
    Q_INVOKABLE void downloadTemplate(const QString &id);
    // Makes `root` (one of downloadRoots) where downloads go from now on: the settings'
    // download_folder.
    Q_INVOKABLE void setDownloadRoot(const QString &root);
    // Asks for the offered pack to be installed (packInstallRequested).
    Q_INVOKABLE void installPack();

    Q_INVOKABLE void dismissNotice();
    // A line for the banner from outside (the image actions, #57).
    void notify(const QString &text) { setNotice(text); }

signals:
    void tabsChanged();
    void currentChanged(int from, int to);
    void tabClosed(const QString &id);
    void recoverableChanged();
    void pendingTrustChanged();
    void noticeChanged();
    void openDialogRequested();
    void saveAsRequested();
    void exportShareRequested();
    void exportCompiledRequested();
    void packInstallRequested(const QString &source, const QString &ref);
    void closeConfirmationRequested(int index, const QString &title);
    // The tab menu's Rename and Tab color (the tab shows its own field and swatches).
    void tabRenameRequested(int index);
    void tabColorRequested(int index);

private:
    struct Tab {
        QString id;
        QString path;
        QString title;
        Document document; // current while the tab is not shown; the canvas holds the live one
        QString savedHash; // empty: never matches, so the tab is unsaved
        QString recoveryHash;
        bool unsaved = false;
        int color = 0; // index into the look's tabColors
        std::unique_ptr<QUndoStack> undo; // while the tab is not shown
    };

    // Adds a tab and shows it. A current tab that is still an empty "Untitled" is replaced
    // when `replacePristine`, so opening a file from a fresh window does not leave a blank tab.
    int addTab(const Document &document, const QString &path, const QString &title, bool forceUnsaved,
               bool replacePristine, const QString &id = {});
    void removeTab(int index);
    Document documentOf(int index) const;
    // `replace`: a file at `path` may be replaced (the dialog asked, or it is the tab's own).
    bool saveTab(int index, const QString &path, bool replace = true);
    void snapshotCurrent();
    void refreshUnsaved();
    void refreshRegistry();
    bool isPristine(int index) const;
    QString untitledName() const;
    void setNotice(const QString &text);
    void analyze(const Document &document, const QString &title);
    // What the banner offers to download: its button's text, the models, and where they may go.
    struct DownloadOffer {
        QString text;
        QJsonArray models; // the workflow's, as Document::models() gives them
        QVariantList roots;
        QString root; // where they go (ModelLibrary::fitDownloadRoot)
        QVariantMap pack; // the first missing pack it says where to get: id, source, ref, text
    };
    // The banner's line on the nodes and models of `document` that are not here, empty for none;
    // with what it may offer to download.
    QString missingNotice(const Document &document, const QString &title, DownloadOffer *offer = nullptr) const;
    // The shown workflow's missing line and offer again, after its models may have changed.
    // The banner shows it when it showed the last one, or `force`.
    void refreshMissing(bool force);
    // The library with its download root fitted to `models`' missing ones that have a source,
    // and those.
    QList<MissingModel> downloadable(const QJsonArray &models, ModelLibrary &library) const;
    // Plans the downloads of `models`' missing ones that have a source and queues them.
    void startDownloads(const QJsonArray &models, const QString &what);
    // Adds the packs not asked about yet to pendingTrust; true when one was added.
    bool addPendingTrust(const QList<PackUse> &uses);
    static QString toPath(const QString &pathOrUrl);

    CanvasModel *m_canvas;
    ImageInputs *m_images;
    const Registry *m_registry;
    CommandRegistry *m_commands;
    QList<TemplateRoot> m_templateRoots;
    RecoveryStore m_recovery;
    TrustStore m_trust;
    QTimer m_timer;
    std::vector<Tab> m_tabs;
    QString m_tabColorsPath;
    QHash<QString, int> m_tabColors; // by file path
    int freeTabColor() const;
    void rememberTabColor(const Tab &tab);
    int m_current = -1;
    QString m_closeAfterSave; // the id of the tab to close once its save-as succeeds
    QVariantList m_recoverable;
    QVariantList m_pendingTrust;
    QString m_notice;
    // The last missing-nodes line, to tell it from the banner's other lines on a reload.
    QString m_missingNotice;
    DownloadOffer m_offer; // what goes with m_missingNotice
    Errors m_refused;      // models of the downloads under way that could not be planned
    ModelDownloads *m_downloads;
    // "<pack id> <version>" the person answered "not now" for: a reload does not ask again.
    QSet<QString> m_trustDismissed;
};

// Wires the file commands of the keymap (file.new, file.open, file.save, file.save-as,
// file.close) to the workflows, and adds `file.open-clipboard`.
void registerFileCommands(CommandRegistry &registry, Workflows &workflows);

} // namespace fugue
