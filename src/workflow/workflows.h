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
#include "engine/model_offers.h"
#include "engine/model_use.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/templates.h"
#include "workflow/image_inputs.h"
#include "workflow/model_downloads.h"
#include "workflow/model_picker.h"
#include "workflow/readiness_sheet.h"
#include "workflow/recovery_store.h"
#include "engine/trust_store.h"

#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUrl>
#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>
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
    // Where the open tabs are kept for the next start (workflow/tab_session.h): the root's own state folder.
    QString sessionPath = stateDir() + QStringLiteral("/tabs.json");
    // How long a change of tabs waits before it is written, so a burst of them is one write.
    int sessionDelayMs = 400;
    // How File > Export compiled puts the provider instances into the registry it compiles with,
    // as a run does (RunSetup::registerProviders). Default: cli::registerProviders (providers.json).
    std::function<Errors(Registry &)> registerProviders = nullptr;
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
    // A file chosen in place of a missing model is being read (#605): its button waits, and a second click does nothing.
    Q_PROPERTY(bool hashingUses READ hashingUses NOTIFY hashingUsesChanged)
    // The files being read, per row (UI design 2026-11-26): the wanted model's hash -> percent read (-1 while not known). A row shows its
    // own busy state from it; the others stay pressable.
    Q_PROPERTY(QVariantMap reading READ reading NOTIFY readingChanged)
    // The banner's line says a template opened with the person's files and Undo puts the template's own models back.
    Q_PROPERTY(bool noticeUndoable READ noticeUndoable NOTIFY noticeChanged)
    // Where the file picker starts: ~/Documents/Fugue as a file URL.
    Q_PROPERTY(QUrl suggestedFolder READ suggestedFolder CONSTANT)
    // The models being downloaded (#89).
    Q_PROPERTY(fugue::ModelDownloads *downloads READ downloads CONSTANT)
    // The "Get ready" sheet (#379): one list and one yes for the packs, their trust and environments and the models the
    // shown workflow needs. The banner has one button, "Get ready…", for it while getReadyOffer.
    Q_PROPERTY(fugue::ReadinessSheet *readiness READ readiness CONSTANT)
    Q_PROPERTY(bool getReadyOffer READ getReadyOffer NOTIFY noticeChanged)
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
    // Every such pack (#266), one entry each; the banner has a button per entry (installPackById(id)).
    Q_PROPERTY(QVariantList packOffers READ packOffers NOTIFY noticeChanged)

public:
    Workflows(CanvasModel *canvas, const Registry *registry, CommandRegistry *commands,
              WorkflowLocations locations = {}, QObject *parent = nullptr);

    QVariantList tabs() const;
    int current() const { return m_current; }
    void setCurrent(int index);
    int count() const { return int(m_tabs.size()); }
    bool unsaved() const;
    bool unsaved(int index) const;
    // `<workflow> — <project> — Fugue`; the project part is left out for the Default project (spec 1.4.2).
    QString windowTitle() const;
    // The name of the project the window runs in, empty for the Default project; the title follows.
    void setProjectName(const QString &name);
    QVariantList recoverable() const { return m_recoverable; }
    QVariantList pendingTrust() const { return m_pendingTrust; }
    QString notice() const { return m_notice; }
    QUrl suggestedFolder() const;
    ModelDownloads *downloads() const { return m_downloads; }
    ReadinessSheet *readiness() const { return m_readiness; }
    // Nothing is open: the one tab is empty, untitled and unsaved. What a window shows apps for (#388, AppShelf).
    bool nothingOpen() const { return count() == 1 && isPristine(0); }
    // The shown tab is empty, untitled and unsaved: a new tab with nothing in it, among others or alone (the shelf shows there too).
    bool currentIsPristine() const { return m_current >= 0 && m_current < count() && isPristine(m_current); }
    // The banner names a pack to install or models to download for the shown workflow: Get ready… is offered.
    bool getReadyOffer() const { return !packOffers().isEmpty() || !downloadOffer().isEmpty(); }
    QString downloadOffer() const;
    QVariantList downloadRoots() const { return downloadOffer().isEmpty() ? QVariantList() : m_offer.roots; }
    QString downloadRoot() const { return m_offer.root; }
    QVariantMap packOffer() const;
    QVariantList packOffers() const;
    QString pathOf(int index) const;
    // Pasting and dropping images into the canvas (workflow/image_inputs.h).
    ImageInputs *images() const { return m_images; }

    // --- opening ---

    Q_INVOKABLE void newWorkflow();
    // Opens a workflow from a file path or file URL: a .fugue, or an image with a workflow
    // in it, by content. Switches to the tab when it is already open. False (with a notice)
    // when it cannot be read.
    Q_INVOKABLE bool openPath(const QString &pathOrUrl);
    // An image file into a Load Image node of the current workflow (ImageInputs::sendFile).
    Q_INVOKABLE bool sendImage(const QString &path) { return m_images->sendFile(path); }
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
    // openTemplate, but a template that is ready only through the person's files opens with them (what a card click does); the launcher's --app too.
    bool openTemplateWithFiles(const Template &chosen);
    // The suffix File > Export for sharing gives the shown workflow: `fugueapp` when it has an `app` block (an app bundle,
    // #394), else `fuguepack`.
    Q_INVOKABLE QString shareSuffix() const;
    // The templates for the Templates dialog, in TemplateLibrary::list()'s order, each a map:
    // id, title, description, category, categoryTitle, tags, preview (a file URL or empty),
    // own, missingModels (name, source, folder each), neededPacks (not installed, or at another version), and status, one line: what
    // it lacks on this machine, or empty when it is ready; download, the button's text for its
    // missing models that have a source (as downloadOffer), else empty. A check: no model file is read.
    // Each also says where its models are (#164, engine/model_places.h): `ready` (every model
    // is on this machine or answering from a paired host; true with no models), `local`, `hosts`
    // `offlineHosts` (host names) and `clouds` (names of the off-machine provider instances that offer a model, never a guess). A host's models are those the last loadHostModels() found.
    Q_INVOKABLE QVariantList templates() const;
    // Whether a template of templates() satisfies what was typed in the Templates window's search
    // (word starts over title, description, category, tags, id, source, families; `tag:`, `source:`, `family:`).
    Q_INVOKABLE bool templateMatches(const QString &query, const QVariantMap &templ) const;
    // Where models come from, for the paired hosts' side of templates(): the providers
    // (listOfferedModels) unless a test sets another.
    using ModelSource = std::function<void(const QString &resourceType, const QStringList &capabilities,
                                           std::function<void(QList<ListedModel>)> done)>;
    void setModelSource(ModelSource source) { m_source = std::move(source); }
    // Asks the providers for their models, then templatesChanged() so the dialog reads again.
    Q_INVOKABLE void loadHostModels();
    // Every template this Fugue can open (TemplateLibrary::list()), for what lists them in its own way (the Canvas task
    // bar's actions, #231), and the roots they come from (a test, or a project switch, may change them).
    QList<Template> templateList() const;
    void setTemplateRoots(const QList<TemplateRoot> &roots) { m_templateRoots = roots; }
    // A workflow file as a new tab with no file, named `title` (an action's, #231 Edit workflow): saving asks where, so the
    // file it came from (a bundled template, or compiled into Fugue) is never written. False (with a notice) when it
    // cannot be read.
    bool openCopyOf(const QString &file, const QString &title);
    // Opens the template with this id (openTemplate); false when there is none.
    Q_INVOKABLE bool openTemplateById(const QString &id);
    // The apps shelf's Duplicate and Rename (#470): a copy of the template as the person's own, titled `title`, and its new id
    // (empty, with a notice, when it could not be made); and the new title of one of the person's own. templatesChanged follows.
    QString duplicateTemplate(const QString &id, const QString &title);
    bool renameTemplate(const QString &id, const QString &title);
    // The person's own templates folder, as the dialog names it.
    Q_INVOKABLE QString ownTemplatesFolder() const { return TemplateLibrary::ownFolder(); }
    // A package another Fugue exported (engine/share_package.h, #91), unpacked and opened as
    // its template is (openTemplate). openPath() takes one by its contents.
    bool openPackage(const QString &path);
    // A package that carries a canvas (engine/canvas_package.h, #249): imported into the project's canvases as a new
    // canvas (a new id when its own is taken), and the banner says so. openPath() takes one by its contents.
    bool openCanvasPackage(const QString &path);
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

    // --- the last session's tabs (#212, workflow/tab_session.h) ---

    // Whether the tabs are kept and reopened: settings.json's restore_tabs. Off removes the kept file,
    // so turning it on again later never brings back an old session. Nothing is kept until
    // restoreSession() has run (on, it also keeps the tabs of a session that started with it off).
    void setSessionEnabled(bool enabled);
    // Names the canvas last used in a Canvas view, for File > Export compiled (a Canvas Region with no canvas chosen
    // reads it, #246). None: the newest canvas of the root.
    void setActiveCanvas(std::function<QString()> shown) { m_activeCanvas = std::move(shown); }
    // At start, when no file was named: opens the saved workflows the last session had open, in
    // order, and shows the one that was shown. A file that is gone is skipped, and one banner line
    // says how many. From here on the open tabs are kept, after each change and at quit. A start with
    // a file named never calls it, so it neither reads nor replaces the kept session.
    void restoreSession();
    // Writes the kept session now (at quit).
    void flushSession();

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
    // The shown workflow lacks something (the banner has a line about it): models, packs or nodes (the Needs panel looks only then).
    bool missingSomething() const { return !m_missingNotice.isEmpty(); }
    // Downloads these models (Document::models()'s form) on the one download line, into the download folder.
    void downloadModels(const QJsonArray &models) { startDownloads(models, QStringLiteral("workflow")); }
    // Downloads the missing models of the template with this id; one that needs packs is opened instead, and the
    // Get ready sheet plans the packs and the models (true: opened, the dialog should close).
    Q_INVOKABLE bool downloadTemplate(const QString &id);
    // A workflow's models (its `requires.models`) that are not on this machine, as a template card counts them (a canvas
    // action's, #231): {count (missing), total, download (the button's text, empty when none has a source), size}.
    QVariantMap missingModels(const QJsonArray &models) const;
    // Downloads those of them that have a source, as a template's Download does; `what` names who asked ("action:<id>").
    void downloadModels(const QJsonArray &models, const QString &what) { startDownloads(models, what); }
    // --- use what you have (#605, spec 2.5, 8.9) ---
    // When the shown workflow names a model that is not on disk but the same weights are, in another file, every place that offers a
    // download offers that file too. engine/model_use.h decides (findOnDisk); this applies it, so they never disagree.
    //
    // The file chosen is applied where the shown workflow is edited: in the graph, to the document as one undo step (the file is
    // saved only when the person saves); in the App view, to the app's own state (`setUseTaker`: AppView keeps it with the form's
    // values and the app's workflow is never written). `setUsesProvider` gives back what the App view keeps, so the Get ready plan,
    // the banner and the Needs panel leave those models out.
    using UsesProvider = std::function<Uses(const QString &tab)>;
    using UseTaker = std::function<bool(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved)>;
    void setUsesProvider(UsesProvider provider) { m_usesProvider = std::move(provider); }
    void setUseTaker(UseTaker taker) { m_useTaker = std::move(taker); }
    // Whether the App view of `tab` is what shows: asked when a file is chosen, so that choice goes where it was made.
    using AppShown = std::function<bool(const QString &tab)>;
    void setAppShown(AppShown shown) { m_appShown = std::move(shown); }
    // For a missing model of the shown workflow (by its hash): {same, file, path, tier, size, otherSource,
    // recommended}; empty when no file of the same weights is here.
    Q_INVOKABLE QVariantMap onDiskOffer(const QString &hash) const;
    // Uses the files on disk for the missing models with these hashes (all of the shown workflow's that have one when empty): the
    // files' hashes are read first (a worker thread, once), then the choice is applied. `usesApplied` follows.
    // `usesFinished` follows when the reads end (also when nothing could be used).
    // `token` is a sheet's own request id: `usesFinished` echoes it, so a sheet takes only the answer to what it asked (0: not a sheet's).
    Q_INVOKABLE void useOnDisk(const QStringList &hashes, int token = 0);
    // The person left the Get ready sheet while its files were read: that request's result is not applied.
    void cancelUses(int token);
    QVariantMap reading() const { return m_reading; }
    bool noticeUndoable() const;
    // Undo of the banner: the template's own models back in the tab that was opened with the person's files.
    Q_INVOKABLE void undoNotice();
    // Opens the template as it is and the Get ready sheet with every model set to Download (the details' "Download <size>…").
    Q_INVOKABLE bool openTemplateForDownload(const QString &id);
    // Opens the template with its missing models set to the files on disk (the template itself is unchanged); the files' hashes are
    // read first. False when there is no such template or nothing to use.
    Q_INVOKABLE bool openTemplateWithWhatYouHave(const QString &id);
    bool hashingUses() const { return !m_hashing.isEmpty(); }
    // What the App view keeps changed: the banner, the Get ready list and the Needs panel look again.
    void usesChanged();
    // Makes `root` (one of downloadRoots) where downloads go from now on: the settings'
    // download_folder.
    Q_INVOKABLE void setDownloadRoot(const QString &root);
    // Asks for the offered pack to be installed (packInstallRequested).
    Q_INVOKABLE void installPack();
    // How the shown workflow's nodes of `pack` (a pack id) are told on a card (#264): {id, name (display
    // name), text ("Needs the Remove Background pack"), installable (it says where to get the pack,
    // and it is not installed here), source, ref}. Not tied to the banner: the card keeps its
    // button after the banner is dismissed.
    Q_INVOKABLE QVariantMap packNeed(const QString &pack) const;
    // Asks for that pack to be installed as the banner's button does (packInstallRequested);
    // false when nothing says where to get it.
    Q_INVOKABLE bool installPackById(const QString &pack);
    // Asks for the pack at `source` (a folder or a git address, with `ref`) to be installed (packInstallRequested), as the banner's button
    // does, for a caller that knows where it is (a canvas action's template, #231): the window asks the person before anything installs.
    void requestPackInstall(const QString &source, const QString &ref) { emit packInstallRequested(source, ref); }

    Q_INVOKABLE void dismissNotice();
    // A line for the banner from outside (the image actions, #57).
    Q_INVOKABLE void notify(const QString &text) { setNotice(text); }

signals:
    // What templates() says changed: the paired hosts' models arrived.
    void templatesChanged();
    void tabsChanged();
    void currentChanged(int from, int to);
    void tabClosed(const QString &id);
    void recoverableChanged();
    void pendingTrustChanged();
    void noticeChanged();
    // A file on disk was chosen in place of a missing model and applied (#605).
    void usesApplied();
    void hashingUsesChanged();
    void readingChanged();
    // The files asked for with useOnDisk were read: the hashes of the models now using their file, and of those whose file could not be read.
    void usesFinished(const QStringList &applied, const QStringList &failed, int token);
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
    // What openPackage hands openTemplate for the tab it makes (#394).
    struct PendingPackage {
        QString path;
        std::shared_ptr<QTemporaryDir> unpacked;
    };
    PendingPackage m_pendingPackage;

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
        QList<CarriedPack> carried; // the packs the template it was opened from carries (#118), else none
        QString templateFolder;     // the folder of the template it was opened from (not a package's, which is gone), else empty (#398)
        // The package it was opened from (#394): its file, and while the tab lives the folder it was unpacked in when it carries
        // packs, which the Get ready sheet reads them from (a package that carries none is unpacked only while it opens).
        QString packagePath;
        std::shared_ptr<QTemporaryDir> unpacked;
    };

    // Adds a tab and shows it. A current tab that is still an empty "Untitled" is replaced
    // when `replacePristine`, so opening a file from a fresh window does not leave a blank tab.
    int addTab(const Document &document, const QString &path, const QString &title, bool forceUnsaved,
               bool replacePristine, const QString &id = {});
    void removeTab(int index);
    Document documentOf(int index) const;
    // The shown tab's document with the App view's chosen files in place of its missing models: what the banner, the plan and the
    // Needs panel judge.
    Document shownDocument() const;
    // The missing models of the shown workflow that have a file of the same weights on disk, kept for a moment (the Needs panel and
    // the Get ready sheet ask for each row).
    const QList<OnDiskUse> &onDiskNow() const;
    bool applyUses(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, bool inApp);
    // The uses that make `chosen` ready and nothing else is missing (no pack to get): what a card click opens with. Empty otherwise.
    QList<OnDiskUse> coveringUses(const Template &chosen) const;
    bool openWithUses(const Template &chosen, const QList<OnDiskUse> &uses);
    void finishOpenWithUses(const Template &chosen, const QList<OnDiskUse> &uses, const Uses &resolved, const QStringList &failed);
    // Runs `what` now, or when the Get ready sheet has closed.
    void after(std::function<void()> what);
    // `replace`: a file at `path` may be replaced (the dialog asked, or it is the tab's own).
    bool saveTab(int index, const QString &path, bool replace = true);
    void snapshotCurrent();
    void refreshUnsaved();
    void refreshRegistry();
    bool isPristine(int index) const;
    QString untitledName() const;
    void setNotice(const QString &text);
    // Reads the hash of each file of `uses` in turn on the worker; `done` gets the handles resolved and the names of the files that
    // could not be read.
    // `progress` is told the file of a use and the percent read as it goes (only while a file is really read).
    void hashUses(const QList<OnDiskUse> &uses, std::function<void(const Uses &, const QStringList &)> done,
                  std::function<void(const OnDiskUse &, int)> progress = {});
    void setReading(const QStringList &hashes, int percent);
    void clearReading(const QStringList &hashes);
    void setHashing(const QString &key, bool on);
    void analyze(const Document &document, const QString &title);
    // Opens the Get ready sheet when the shown workflow lacks packs or models it can get (#379).
    void offerReadiness();
    // What the banner offers to download: its button's text, the models, and where they may go.
    struct DownloadOffer {
        QString text;
        QJsonArray models; // the workflow's, as Document::models() gives them
        QVariantList roots;
        QString root; // where they go (ModelLibrary::fitDownloadRoot)
        QVariantList packs; // each missing pack it says where to get: {id, source, ref, text}
    };
    // The banner's line on the nodes and models of `document` that are not here, empty for none;
    // with what it may offer to download.
    // The first pack the shown tab's template carries that `document` still needs, as the banner
    // offers it: id, source (the folder in the template), ref (empty), text. Empty for none.
    QVariantMap carriedPackOffer(const Document &document) const;
    // The same for `pack`, and the recorded source of a pack that is not installed at all (4.3).
    QVariantMap packNeedOf(const Document &document, const QString &pack) const;
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
    std::function<Errors(Registry &)> m_registerProviders;
    std::function<QString()> m_activeCanvas;
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
    QString m_projectName;
    QString m_sessionPath;
    bool m_sessionEnabled = false;
    bool m_sessionLive = false; // restoreSession() has run
    QTimer m_sessionTimer;
    bool m_sessionWarned = false;
    void scheduleSession();
    // The last missing-nodes line, to tell it from the banner's other lines on a reload.
    QString m_missingNotice;
    DownloadOffer m_offer; // what goes with m_missingNotice
    Errors m_refused;      // models of the downloads under way that could not be planned
    ModelDownloads *m_downloads;
    ModelWorkers m_work; // reads the hash of a file chosen in place of a missing model (#605)
    UsesProvider m_usesProvider;
    UseTaker m_useTaker;
    AppShown m_appShown;
    mutable QList<OnDiskUse> m_onDisk; // onDiskNow()'s answer
    mutable QByteArray m_onDiskKey;
    mutable QElapsedTimer m_onDiskAge;
    QSet<QString> m_hashing; // the targets whose chosen file is being read ("use:<tab> <hashes>", "open:<template>")
    Uses m_openUses; // the files a template opens with (openTemplateWithWhatYouHave)
    // The asks waiting on one read of files, by its target: a sheet's request ids, how many others asked, and whether nobody is left.
    struct UseJob {
        QStringList wanted;
        QList<int> tokens;
        int plain = 0;
        bool cancelled = false;
    };
    QHash<QString, UseJob> m_useJobs;
    QVariantMap m_reading; // reading()
    // What Undo of the banner puts back: the tab opened with files, and the file handle -> the template's own handle.
    struct OpenedWith {
        QString tab;
        int undoIndex = 0; // the tab's undo index just after the open: Undo is offered while it is unchanged
        struct Change {
            QVariantList path;
            QJsonValue original, applied;
        };
        QList<Change> changes;
    };
    OpenedWith m_openedWith;
    QList<std::function<void()>> m_afterSheet;
    ReadinessSheet *m_readiness;
    QString m_readyTabId; // the tab the Get ready sheet was opened for
    ModelSource m_source; // empty: listOfferedModels
    QList<ListedModel> m_served; // what the providers offered at the last loadHostModels()
    // "<pack id> <version>" the person answered "not now" for: a reload does not ask again.
    QSet<QString> m_trustDismissed;
};

// Wires the file commands of the keymap (file.new, file.open, file.save, file.save-as,
// file.close) to the workflows, and adds `file.open-clipboard`.
void registerFileCommands(CommandRegistry &registry, Workflows &workflows);

} // namespace fugue
