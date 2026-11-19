#include "workflow/workflows.h"

#include "engine/embed.h"
#include "engine/paths.h"
#include "workflow/content_hash.h"
#include "workflow/workflow_io.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QUuid>

namespace fugue {

namespace {

QString appVersion() {
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("0.1.0") : version;
}

QString newTabId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// Every node type the document uses that is not a group and not installed.
QStringList missingTypes(const Document &document, const Registry &registry) {
    QSet<QString> missing;
    const auto scan = [&](const Nodes &nodes) {
        for (const Node &node : nodes)
            if (!document.groups.contains(node.type) && !registry.nodeType(node.type))
                missing.insert(node.type);
    };
    scan(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        scan(group.nodes);
    QStringList list(missing.begin(), missing.end());
    list.sort();
    return list;
}

QString firstMessage(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.message;
    return errors.isEmpty() ? QString() : errors.first().message;
}

} // namespace

Workflows::Workflows(CanvasModel *canvas, const Registry *registry, CommandRegistry *commands,
                     WorkflowLocations locations, QObject *parent)
    : QObject(parent), m_canvas(canvas), m_registry(registry), m_commands(commands),
      m_recovery(locations.recoveryDir), m_trust(locations.trustPath) {
    connect(m_canvas, &CanvasModel::edited, this, &Workflows::refreshUnsaved);
    m_images = new ImageInputs(m_canvas, m_registry, this);
    connect(m_images, &ImageInputs::message, this, &Workflows::setNotice);
    m_timer.setInterval(locations.recoveryIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &Workflows::writeRecoveryCopies);
    m_timer.start();
    if (m_commands)
        connect(m_commands, &CommandRegistry::closeWorkflowRequested, this, &Workflows::closeCurrent);
    for (const RecoveryEntry &entry : m_recovery.list())
        m_recoverable.append(QVariantMap{{"id", entry.id},
                                         {"title", entry.title},
                                         {"path", entry.originalPath},
                                         {"modified", entry.modified}});
    newWorkflow();
}

// --- state for the window ---

QVariantList Workflows::tabs() const {
    QVariantList list;
    for (int i = 0; i < count(); ++i) {
        const Tab &tab = m_tabs[size_t(i)];
        list.append(QVariantMap{{"id", tab.id},
                                {"title", tab.title},
                                {"path", tab.path},
                                {"unsaved", unsaved(i)},
                                {"current", i == m_current}});
    }
    return list;
}

bool Workflows::unsaved(int index) const {
    return index >= 0 && index < count() && m_tabs[size_t(index)].unsaved;
}

bool Workflows::unsaved() const {
    for (int i = 0; i < count(); ++i)
        if (unsaved(i))
            return true;
    return false;
}

QString Workflows::pathOf(int index) const {
    return index >= 0 && index < count() ? m_tabs[size_t(index)].path : QString();
}

QString Workflows::windowTitle() const {
    if (m_current < 0)
        return QStringLiteral("Fugue");
    return QStringLiteral("%1%2 — Fugue").arg(unsaved(m_current) ? QStringLiteral("• ") : QString(),
                                              m_tabs[size_t(m_current)].title);
}

QUrl Workflows::suggestedFolder() const {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString fugue = documents + "/Fugue";
    return QUrl::fromLocalFile(QFileInfo(fugue).isDir() ? fugue : (documents.isEmpty() ? QDir::homePath() : documents));
}

void Workflows::setNotice(const QString &text) {
    if (m_notice == text)
        return;
    m_notice = text;
    emit noticeChanged();
}

void Workflows::dismissNotice() { setNotice({}); }

QString Workflows::toPath(const QString &pathOrUrl) {
    if (pathOrUrl.startsWith(QStringLiteral("file:")))
        return QUrl(pathOrUrl).toLocalFile();
    return expandHome(pathOrUrl);
}

QString Workflows::untitledName() const {
    const QString base = tr("Untitled");
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n);
        bool taken = false;
        for (const Tab &tab : m_tabs)
            taken |= tab.path.isEmpty() && tab.title == name;
        if (!taken)
            return name;
    }
}

// --- tabs ---

Document Workflows::documentOf(int index) const {
    return index == m_current ? m_canvas->document() : m_tabs[size_t(index)].document;
}

void Workflows::snapshotCurrent() {
    if (m_current < 0)
        return;
    refreshUnsaved();
    m_tabs[size_t(m_current)].document = m_canvas->document();
}

void Workflows::refreshUnsaved() {
    if (m_current < 0)
        return;
    Tab &tab = m_tabs[size_t(m_current)];
    const bool now = contentHash(m_canvas->document()) != tab.savedHash;
    if (now == tab.unsaved)
        return;
    tab.unsaved = now;
    refreshRegistry();
    emit tabsChanged();
}

void Workflows::refreshRegistry() {
    if (!m_commands)
        return;
    m_commands->setUnsavedWork(unsaved());
    // Ctrl+W on a lone, empty, untitled workflow has nothing to close: it quits.
    m_commands->setWorkflowOpen(!(count() == 1 && isPristine(0)));
}

bool Workflows::isPristine(int index) const {
    if (index < 0 || index >= count())
        return false;
    const Tab &tab = m_tabs[size_t(index)];
    if (!tab.path.isEmpty() || tab.unsaved)
        return false;
    const Document doc = documentOf(index);
    QStringList layoutKeys = doc.layout.keys();
    layoutKeys.removeAll(QStringLiteral("view"));
    return doc.graph.nodes.isEmpty() && doc.groups.isEmpty() && layoutKeys.isEmpty();
}

int Workflows::addTab(const Document &document, const QString &path, const QString &title, bool forceUnsaved,
                      bool replacePristine, const QString &id) {
    const int from = m_current;
    int at = count();
    if (replacePristine && isPristine(m_current)) {
        at = m_current;
        const QString gone = m_tabs[size_t(at)].id;
        m_tabs.erase(m_tabs.begin() + at);
        m_current = -1;
        emit tabClosed(gone);
    } else {
        snapshotCurrent();
        // The tab being left keeps its undo stack; the new one starts with an empty one.
        if (m_current >= 0)
            m_tabs[size_t(m_current)].undo = m_canvas->takeUndoStack();
    }
    Tab tab;
    tab.id = id.isEmpty() ? newTabId() : id;
    tab.path = path;
    tab.title = title.isEmpty() ? untitledName() : title;
    tab.document = document;
    m_tabs.insert(m_tabs.begin() + at, std::move(tab));
    m_current = at;
    m_canvas->setDocument(document);
    // The baseline is what the canvas makes of the document (it places nodes the file has no
    // position for), so opening a file never starts out unsaved. A recovered tab has none.
    Tab &added = m_tabs[size_t(at)];
    added.savedHash = forceUnsaved ? QString() : contentHash(m_canvas->document());
    added.unsaved = forceUnsaved;
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, at);
    return at;
}

void Workflows::setCurrent(int index) {
    if (index == m_current || index < 0 || index >= count())
        return;
    const int from = m_current;
    snapshotCurrent();
    m_tabs[size_t(m_current)].undo = m_canvas->takeUndoStack();
    m_current = index;
    m_canvas->setDocument(m_tabs[size_t(index)].document);
    m_canvas->setUndoStack(std::move(m_tabs[size_t(index)].undo));
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, index);
}

void Workflows::removeTab(int index) {
    if (index < 0 || index >= count())
        return;
    const QString id = m_tabs[size_t(index)].id;
    m_recovery.remove(id);
    if (count() == 1) {
        // The window always has a workflow: closing the last one leaves a fresh one.
        m_tabs.clear();
        m_current = -1;
        emit tabClosed(id);
        addTab(Document(), {}, {}, false, false);
        return;
    }
    const int from = m_current;
    const bool wasCurrent = index == m_current;
    if (!wasCurrent)
        snapshotCurrent();
    m_tabs.erase(m_tabs.begin() + index);
    emit tabClosed(id);
    if (wasCurrent) {
        m_current = qMin(index, count() - 1);
        m_canvas->setDocument(m_tabs[size_t(m_current)].document);
        m_canvas->setUndoStack(std::move(m_tabs[size_t(m_current)].undo));
    } else if (index < m_current) {
        --m_current;
    }
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, m_current);
}

void Workflows::newWorkflow() { addTab(Document(), {}, {}, false, false); }

// --- open ---

bool Workflows::openPath(const QString &pathOrUrl) {
    const QString path = toPath(pathOrUrl);
    const QFileInfo info(path);
    if (info.isFile()) {
        for (int i = 0; i < count(); ++i) {
            const QString &open = m_tabs[size_t(i)].path;
            if (!open.isEmpty() && QFileInfo(open).canonicalFilePath() == info.canonicalFilePath()) {
                setCurrent(i);
                return true;
            }
        }
    }
    const Result<ImportedWorkflow> imported = readWorkflowFile(path);
    if (!imported.ok()) {
        setNotice(tr("Could not open %1: %2").arg(info.fileName().isEmpty() ? path : info.fileName(),
                                                    firstMessage(imported.errors())));
        return false;
    }
    const Document &document = imported->document;
    // An output's workflow is a new, unnamed one: saving asks where, never touches the image.
    const bool fromImage = imported->kind == SourceKind::Image;
    addTab(document, fromImage ? QString() : info.absoluteFilePath(),
           fromImage ? info.completeBaseName() : info.fileName(), false, true);
    analyze(document, m_tabs[size_t(m_current)].title);
    return true;
}

void Workflows::openUrls(const QVariantList &urls) {
    for (const QVariant &url : urls) {
        const QString text = url.toString();
        if (url.canConvert<QUrl>() && !text.startsWith(QStringLiteral("file:")) && url.toUrl().isLocalFile()) {
            openPath(url.toUrl().toLocalFile());
        } else if (text.startsWith(QStringLiteral("file:")) || text.startsWith('/') || text.startsWith('~')) {
            openPath(text);
        } else {
            setNotice(tr("Only files can be opened, not %1.").arg(text));
        }
    }
}

void Workflows::dropUrls(const QVariantList &urls, qreal x, qreal y) {
    const QString onNode = m_canvas->nodeAt(x, y);
    const bool onLoadImage = m_images->accepts(onNode, QStringLiteral("file"));
    QStringList images, workflows;
    for (const QVariant &url : urls) {
        const QString text = url.toString();
        const bool isFile = text.startsWith(QStringLiteral("file:")) || text.startsWith('/') || text.startsWith('~');
        const QString path = isFile ? toPath(text) : QString();
        if (path.isEmpty()) {
            setNotice(tr("Only files can be opened, not %1.").arg(text));
            continue;
        }
        QFile file(path);
        const bool image = QFileInfo(path).isFile() && file.open(QIODevice::ReadOnly) && sniffImage(file.read(16));
        file.close();
        // A picture with a workflow in it opens as that workflow, except onto a Load Image.
        if (image && (onLoadImage || !readEmbeddedWorkflow(path).ok()))
            images.append(path);
        else
            workflows.append(path);
    }
    if (!images.isEmpty())
        m_images->dropFiles(images, QPointF(x, y), onNode);
    for (const QString &path : workflows)
        openPath(path);
}

bool Workflows::openText(const QString &text) {
    const Result<Document> document = readWorkflowText(text);
    if (!document.ok()) {
        setNotice(tr("The text is not a Fugue workflow: %1").arg(firstMessage(document.errors())));
        return false;
    }
    addTab(document.value(), {}, {}, false, true);
    analyze(document.value(), m_tabs[size_t(m_current)].title);
    return true;
}

bool Workflows::openClipboard() {
    return openText(QGuiApplication::clipboard()->text());
}

void Workflows::openStartup(const QStringList &paths) {
    for (const QString &path : paths)
        openPath(path);
}

void Workflows::analyze(const Document &document, const QString &title) {
    const QStringList missing = missingTypes(document, *m_registry);
    setNotice(missing.isEmpty()
                  ? QString()
                  : tr("%1 uses nodes that are not installed (%2). They open as placeholders and keep their inputs.")
                        .arg(title, missing.join(QStringLiteral(", "))));
    bool changed = false;
    for (const PackUse &use : m_trust.untrusted(document)) {
        bool known = false;
        for (const QVariant &entry : m_pendingTrust)
            known |= entry.toMap().value("id") == use.id && entry.toMap().value("version") == use.version;
        if (known)
            continue;
        m_pendingTrust.append(QVariantMap{
            {"id", use.id}, {"version", use.version}, {"source", use.source}, {"types", use.types}});
        changed = true;
    }
    if (changed)
        emit pendingTrustChanged();
}

// --- save and close ---

bool Workflows::saveTab(int index, const QString &path) {
    Document document = documentOf(index);
    // A saved workflow stays portable: the images it takes from Fugue's inputs folder are copied
    // into an `inputs` folder beside it, and its nodes point there (Appendix A, always, no question).
    const Document before = document;
    if (const Errors copied = relocateInputs(document, *m_registry, pathOf(index), path);
        hasErrors(copied)) {
        setNotice(tr("Could not save %1: %2").arg(QFileInfo(path).fileName(), firstMessage(copied)));
        return false;
    }
    const bool moved = contentHash(document) != contentHash(before);
    const Document relocated = document;
    document.refreshRequirements(appVersion());
    const Errors errors = document.saveFile(path);
    if (hasErrors(errors)) {
        setNotice(tr("Could not save %1: %2").arg(QFileInfo(path).fileName(), firstMessage(errors)));
        return false;
    }
    Tab &tab = m_tabs[size_t(index)];
    tab.path = path;
    tab.title = QFileInfo(path).fileName();
    // After the path: the repointed inputs are relative to the new place (Appendix A).
    if (index == m_current && moved)
        m_canvas->commit(relocated, tr("Move images"));
    tab.savedHash = contentHash(document);
    tab.unsaved = false;
    tab.recoveryHash.clear();
    if (index != m_current)
        tab.document = document;
    m_recovery.remove(tab.id);
    setNotice({});
    refreshRegistry();
    emit tabsChanged();
    return true;
}

bool Workflows::save() {
    if (m_current < 0)
        return false;
    if (m_tabs[size_t(m_current)].path.isEmpty()) {
        emit saveAsRequested();
        return false;
    }
    return saveTab(m_current, m_tabs[size_t(m_current)].path);
}

bool Workflows::saveAs(const QString &pathOrUrl) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".fugue");
    const QString id = m_tabs[size_t(m_current)].id;
    if (!saveTab(m_current, path))
        return false;
    if (m_closeAfterSave == id) {
        m_closeAfterSave.clear();
        removeTab(m_current);
    }
    return true;
}

void Workflows::closeTab(int index) {
    if (index < 0 || index >= count())
        return;
    if (unsaved(index))
        emit closeConfirmationRequested(index, m_tabs[size_t(index)].title);
    else
        removeTab(index);
}

void Workflows::discardAndClose(int index) { removeTab(index); }

void Workflows::saveAndClose(int index) {
    if (index < 0 || index >= count())
        return;
    if (m_tabs[size_t(index)].path.isEmpty()) {
        setCurrent(index);
        m_closeAfterSave = m_tabs[size_t(index)].id;
        emit saveAsRequested();
        return;
    }
    if (saveTab(index, m_tabs[size_t(index)].path))
        removeTab(index);
}

// --- recovery ---

void Workflows::recover() {
    const QVariantList entries = m_recoverable;
    m_recoverable.clear();
    emit recoverableChanged();
    QStringList failed;
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value("id").toString();
        const Result<Document> document = m_recovery.read(id);
        if (!document.ok()) {
            failed << entry.value("title").toString();
            m_recovery.remove(id);
            continue;
        }
        // The copy keeps its id, so the next 30-second write replaces it in place.
        addTab(document.value(), entry.value("path").toString(), entry.value("title").toString(), true, true, id);
        analyze(document.value(), m_tabs[size_t(m_current)].title);
    }
    if (!failed.isEmpty())
        setNotice(tr("These recovery copies could not be read and were removed: %1").arg(failed.join(", ")));
}

void Workflows::discardRecovery() {
    for (const QVariant &value : m_recoverable)
        m_recovery.remove(value.toMap().value("id").toString());
    m_recoverable.clear();
    emit recoverableChanged();
}

void Workflows::writeRecoveryCopies() {
    refreshUnsaved();
    for (int i = 0; i < count(); ++i) {
        Tab &tab = m_tabs[size_t(i)];
        if (!tab.unsaved) {
            if (!tab.recoveryHash.isEmpty()) {
                m_recovery.remove(tab.id);
                tab.recoveryHash.clear();
            }
            continue;
        }
        const Document document = documentOf(i);
        const QString hash = contentHash(document);
        if (hash == tab.recoveryHash)
            continue;
        const Errors errors = m_recovery.write(tab.id, document, tab.path, tab.title);
        if (hasErrors(errors))
            qWarning("Fugue: recovery copy of %s not written: %s", qPrintable(tab.title),
                     qPrintable(firstMessage(errors)));
        else
            tab.recoveryHash = hash;
    }
}

// --- trust ---

void Workflows::allowPendingTrust() {
    for (const QVariant &value : m_pendingTrust) {
        const QVariantMap use = value.toMap();
        const Errors errors = m_trust.trust(use.value("id").toString(), use.value("version").toString());
        if (hasErrors(errors))
            setNotice(tr("Could not remember your choice: %1").arg(firstMessage(errors)));
    }
    dismissPendingTrust();
}

void Workflows::dismissPendingTrust() {
    m_pendingTrust.clear();
    emit pendingTrustChanged();
}

bool Workflows::isTrusted(const QString &packId, const QString &version) const {
    return m_trust.isTrusted(packId, version);
}

// --- commands ---

void registerFileCommands(CommandRegistry &registry, Workflows &workflows) {
    registry.setHandler("file.new", [&workflows] { workflows.newWorkflow(); });
    registry.setHandler("file.open", [&workflows] { emit workflows.openDialogRequested(); });
    registry.setHandler("file.save", [&workflows] { workflows.save(); });
    registry.setHandler("file.save-as", [&workflows] { emit workflows.saveAsRequested(); });
    if (!registry.contains("file.open-clipboard")) {
        CommandSpec spec;
        spec.id = QStringLiteral("file.open-clipboard");
        spec.description = QT_TRANSLATE_NOOP("Command", "Open the workflow on the clipboard");
        spec.versus = QStringLiteral("added");
        const QString problem = registry.add(spec, [&workflows] { workflows.openClipboard(); });
        if (!problem.isEmpty())
            qWarning("Fugue: %s", qPrintable(problem));
    } else {
        registry.setHandler("file.open-clipboard", [&workflows] { workflows.openClipboard(); });
    }
}

} // namespace fugue
