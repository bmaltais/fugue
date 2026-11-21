#include "workflow/workflows.h"

#include "engine/atomic_file.h"
#include "engine/compiler.h"
#include "engine/embed.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/group_library.h"
#include "engine/file_hashes.h"
#include "engine/json.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/settings.h"
#include "engine/share_package.h"
#include "workflow/content_hash.h"
#include "workflow/workflow_io.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QTemporaryDir>
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

// Writes the workflow file atomically. Without `replace`, false and nothing written when a
// file is already at `path`.
Result<bool> writeWorkflow(const Document &document, const QString &path, bool replace) {
    if (replace) {
        const Errors errors = document.saveFile(path);
        return hasErrors(errors) ? Result<bool>(errors) : Result<bool>(true);
    }
    const QFileInfo info(path);
    PartialFile partial(info.absolutePath(), info.fileName());
    if (const Errors errors = partial.write(writeJson(document.toJson()), info.fileName()); hasErrors(errors))
        return errors;
    return partial.commit(path, Overwrite::Never);
}

// The button for the models of `missing` that have a source: "Download 3 models, 19.4 GB", the
// size when every one records it. Empty for none.
QString offerText(const QList<MissingModel> &missing) {
    int count = 0;
    qint64 bytes = 0;
    bool sized = true;
    for (const MissingModel &model : missing) {
        if (model.source.isEmpty())
            continue;
        ++count;
        sized = sized && model.size >= 0;
        bytes += std::max<qint64>(0, model.size);
    }
    if (count == 0)
        return {};
    return sized ? Workflows::tr("Download %n models, %1", nullptr, count).arg(ModelDownloads::sizeText(bytes))
                 : Workflows::tr("Download %n models", nullptr, count);
}

// The bytes of the models of `missing` that have a source, as the card's button says them;
// empty unless every one records its size.
QString downloadSize(const QList<MissingModel> &missing) {
    qint64 bytes = 0;
    int count = 0;
    for (const MissingModel &model : missing) {
        if (model.source.isEmpty())
            continue;
        if (model.size < 0)
            return {};
        bytes += model.size;
        ++count;
    }
    return count ? ModelDownloads::sizeText(bytes) : QString();
}

// A model folder as the banner's choice names it.
QString rootTitle(const QString &root, const ModelLibrary &library) {
    if (root == library.roots().value(0))
        return Workflows::tr("Fugue's models folder");
    const QString home = QDir::homePath();
    return root.startsWith(home + QLatin1Char('/')) ? QLatin1Char('~') + root.mid(home.size()) : root;
}

// Each message with its hint, for the banner.
QString withHints(const Errors &errors) {
    QStringList lines;
    for (const Error &error : errors)
        lines << (error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint);
    return lines.join(QLatin1Char(' '));
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
      m_templateRoots(locations.templateRoots), m_recovery(locations.recoveryDir), m_trust(locations.trustPath) {
    connect(m_canvas, &CanvasModel::edited, this, &Workflows::refreshUnsaved);
    connect(m_canvas, &CanvasModel::pasted, this, [this](const Document &document) {
        if (m_current >= 0)
            analyze(document, m_tabs[size_t(m_current)].title);
    });
    m_images = new ImageInputs(m_canvas, m_registry, this);
    m_downloads = new ModelDownloads(this);
    // The banner says what arrived, or what went wrong; what is still missing comes back.
    connect(m_downloads, &ModelDownloads::finished, this, [this](int arrived, const Errors &all) {
        refreshMissing(false);
        // What could not be planned, then what failed; a cancel is the person's own, not news.
        Errors errors = m_refused;
        m_refused.clear();
        for (const Error &error : all)
            if (error.code != QLatin1String("cancelled"))
                errors << error;
        if (!errors.isEmpty())
            setNotice(withHints(errors));
        else if (arrived > 0)
            setNotice(m_missingNotice.isEmpty() ? tr("Downloaded %n models.", nullptr, arrived) : m_missingNotice);
        emit noticeChanged(); // the offer, with the same line
    });
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
    if (info.isFile() && isSharePackage(path))
        return openPackage(path);
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

bool Workflows::openPackage(const QString &path) {
    const QString shown = QFileInfo(path).fileName();
    // Unpacked only for as long as it opens: its samples go into the inputs folder (openTemplate).
    // Under the cache, not /tmp, which is memory on Arch.
    QDir().mkpath(cacheDir());
    QTemporaryDir unpacked(cacheDir() + QStringLiteral("/package-XXXXXX"));
    if (!unpacked.isValid()) {
        setNotice(tr("Could not open %1: %2").arg(shown, unpacked.errorString()));
        return false;
    }
    const Result<QString> folder = unpackSharePackage(path, unpacked.path());
    if (!folder) {
        setNotice(tr("Could not open %1: %2").arg(shown, firstMessage(folder.errors())));
        return false;
    }
    const TemplateLibrary library({{QFileInfo(folder.value()).absolutePath(), false}}, m_registry);
    const std::optional<Template> chosen = library.find(QFileInfo(folder.value()).fileName());
    if (!chosen) {
        const Errors problems = library.problems();
        setNotice(tr("Could not open %1: %2")
                      .arg(shown, problems.isEmpty() ? tr("it was made by a newer Fugue; update Fugue to open it.")
                                                     : firstMessage(problems)));
        return false;
    }
    return openTemplate(*chosen, true);
}

bool Workflows::exportShare(const QString &pathOrUrl, const QString &preview) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    if (QFileInfo(path).suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) != 0)
        path += QStringLiteral(".zip");
    const Tab &tab = m_tabs[size_t(m_current)];
    ShareRequest request;
    request.title = tab.title.endsWith(QStringLiteral(".fugue")) ? tab.title.chopped(6) : tab.title;
    request.workflowPath = tab.path;
    request.preview = preview;
    request.appVersion = appVersion();
    QStringList included;
    const Errors errors = writeSharePackage(documentOf(m_current), *m_registry, request, path, &included);
    if (hasErrors(errors)) {
        setNotice(tr("Could not export %1: %2").arg(request.title, withHints(errors)));
        return false;
    }
    QString line = tr("Exported %1 for sharing to %2. Models are not in it: who opens it can download them.")
                       .arg(request.title, path);
    if (!included.isEmpty())
        line += QLatin1Char(' ') + tr("Images in it: %1.").arg(included.join(QStringLiteral(", ")));
    if (!errors.isEmpty())
        line += QLatin1Char(' ') + withHints(errors);
    setNotice(line);
    return true;
}

bool Workflows::exportCompiled(const QString &pathOrUrl) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".json");
    const QString title = m_tabs[size_t(m_current)].title;
    // As a run compiles it: a library or pack group named only by its type runs from its definition.
    Document document = documentOf(m_current);
    embedGroupTypes(document, *m_registry);
    const Compilation compilation = compile(document, *m_registry);
    if (!compilation) {
        setNotice(tr("%1 does not compile: %2").arg(title, withHints(compilation.errors())));
        return false;
    }
    const Errors errors = createFile(path, writeJson(compilation->toJson()), QFileInfo(path).fileName());
    if (hasErrors(errors)) {
        setNotice(tr("Could not export %1: %2").arg(title, withHints(errors)));
        return false;
    }
    setNotice(tr("Exported the compiled graph of %1 to %2.").arg(title, path));
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
        // Only a regular file is opened at all: a pipe would wait for a writer forever.
        if (const QFileInfo info(path); info.exists() && !info.isFile() && !info.isDir()) {
            setNotice(tr("Could not open %1: it is not a file.").arg(QFileInfo(path).fileName()));
            continue;
        }
        QFile file(path);
        const bool image = file.open(QIODevice::ReadOnly) && sniffImage(file.read(16));
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

bool Workflows::openTemplate(const Template &chosen, bool fromPackage) {
    Result<Document> document = Document::loadFile(chosen.workflow);
    Errors errors = document ? Errors() : document.errors();
    if (document) {
        makeInputsAbsolute(document.value(), *m_registry, chosen.workflow);
        errors = takeInputsFrom(document.value(), *m_registry, chosen.folder);
    }
    if (document && fromPackage) {
        const QString folder = canonicalPath(chosen.folder);
        forEachFileLiteral(document.value(), *m_registry, [&](const QString &, const QString &, InputValue &value) {
            const QString path = canonicalPath(value.literal().toString());
            if (!path.isEmpty() && isInside(path, folder))
                value = InputValue::literal(QFileInfo(path).fileName());
        });
    }
    if (!errors.isEmpty()) {
        setNotice(tr("Could not open the template %1: %2").arg(chosen.title, firstMessage(errors)));
        return false;
    }
    addTab(document.value(), {}, chosen.title, true, true);
    analyze(document.value(), chosen.title);
    return true;
}

QVariantList Workflows::templates() const {
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary models = ModelLibrary::current(m_registry);
    QVariantList out;
    for (const Template &t : TemplateLibrary(m_templateRoots, m_registry).list()) {
        QVariantList missing;
        ModelLibrary library = models;
        downloadable(t.models, library); // the folders they would go in
        const QList<MissingModel> absent = library.missing(t.models, hashes);
        for (const MissingModel &model : absent)
            missing.append(QVariantMap{{"name", model.handle.value(QStringLiteral("name")).toString()},
                                       {"source", model.source},
                                       {"folder", model.folder}});
        QStringList status;
        if (!missing.isEmpty())
            status << tr("Models missing: %1 of %2").arg(missing.size()).arg(t.models.size());
        if (!t.neededPacks.isEmpty())
            status << tr("Packs needed: %1").arg(t.neededPacks.join(QStringLiteral(", ")));
        out.append(QVariantMap{{"id", t.id},
                               {"title", t.title},
                               {"description", t.description},
                               {"category", t.category},
                               {"categoryTitle", t.categoryTitle},
                               {"tags", t.tags},
                               {"preview", t.preview.isEmpty() ? QString() : QUrl::fromLocalFile(t.preview).toString()},
                               {"own", t.own},
                               {"missingModels", missing},
                               {"download", offerText(absent)},
                               {"downloadSize", downloadSize(absent)},
                               {"neededPacks", t.neededPacks},
                               {"status", status.join(QStringLiteral(" · "))}});
    }
    return out;
}

bool Workflows::openTemplateById(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen) {
        setNotice(tr("There is no template %1.").arg(id));
        return false;
    }
    return openTemplate(*chosen);
}

bool Workflows::openClipboard() {
    return openText(QGuiApplication::clipboard()->text());
}

void Workflows::openStartup(const QStringList &paths) {
    for (const QString &path : paths)
        openPath(path);
}

QString Workflows::missingNotice(const Document &document, const QString &title, DownloadOffer *offer) const {
    QStringList lines;
    const QStringList missing = missingTypes(document, *m_registry);
    if (!missing.isEmpty())
        lines << tr("%1 uses nodes that are not installed (%2). They open as placeholders and keep their inputs.")
                     .arg(title, missing.join(QStringLiteral(", ")));
    // Spec 4.3: the models it names that are not here, without reading a model file.
    FileHashes hashes(FileHashes::defaultPath());
    ModelLibrary library = ModelLibrary::current(m_registry);
    QList<PortType> types;
    for (const MissingModel &model : downloadable(document.models(), library))
        if (const std::optional<PortType> type = PortType::parse(model.handle.value(QStringLiteral("resource")).toString()))
            types << *type;
    const QList<MissingModel> absent = library.missing(document, hashes); // with the fitted folders
    QStringList models;
    for (const MissingModel &model : absent) {
        const QString name = model.handle.value(QStringLiteral("name")).toString();
        models << (model.source.isEmpty() ? tr("%1, into %2").arg(name, model.folder)
                                          : tr("%1 from %2, into %3").arg(name, model.source, model.folder));
    }
    if (!models.isEmpty())
        lines << tr("%1 needs models that are not in the model folders: %2.").arg(title, models.join(QStringLiteral("; ")));
    if (offer) {
        // The first missing pack the workflow says where to get (4.3).
        offer->pack.clear();
        const QJsonObject packs = document.requirements.value(QStringLiteral("packs")).toObject();
        for (const QString &type : missing) {
            const QString pack = type.section(QLatin1Char('/'), 0, 0);
            const QJsonObject entry = packs.value(pack).toObject();
            if (entry.value(QStringLiteral("source")).toString().isEmpty() || !m_registry->packVersion(pack).isEmpty())
                continue;
            offer->pack = QVariantMap{{"id", pack},
                                      {"source", entry.value(QStringLiteral("source")).toString()},
                                      {"ref", entry.value(QStringLiteral("ref")).toString()},
                                      {"text", tr("Install %1…").arg(pack)}};
            break;
        }
        offer->text = offerText(absent);
        offer->models = document.models();
        offer->root = library.downloadRoot();
        offer->roots.clear();
        if (!offer->text.isEmpty())
            for (const QString &root : library.downloadRoots(types))
                offer->roots.append(QVariantMap{{"path", root}, {"title", rootTitle(root, library)}});
    }
    return lines.join(QLatin1Char(' '));
}

void Workflows::refreshMissing(bool force) {
    if (m_current < 0)
        return;
    const QString missing = missingNotice(documentOf(m_current), m_tabs[size_t(m_current)].title, &m_offer);
    const bool shown = m_notice == m_missingNotice;
    m_missingNotice = missing;
    if (force || shown)
        setNotice(missing);
    emit noticeChanged(); // the offer may differ with the same line
}

QVariantMap Workflows::packOffer() const {
    return !m_notice.isEmpty() && m_notice == m_missingNotice ? m_offer.pack : QVariantMap();
}

void Workflows::installPack() {
    const QVariantMap pack = packOffer();
    if (!pack.isEmpty())
        emit packInstallRequested(pack.value("source").toString(), pack.value("ref").toString());
}

QString Workflows::downloadOffer() const {
    return !m_notice.isEmpty() && m_notice == m_missingNotice && !m_downloads->busy() ? m_offer.text : QString();
}

QList<MissingModel> Workflows::downloadable(const QJsonArray &models, ModelLibrary &library) const {
    FileHashes hashes(FileHashes::defaultPath());
    QList<PortType> types;
    for (const MissingModel &model : library.missing(models, hashes))
        if (const std::optional<PortType> type = PortType::parse(model.handle.value(QStringLiteral("resource")).toString());
            type && !model.source.isEmpty())
            types << *type;
    library.fitDownloadRoot(types);
    QList<MissingModel> out;
    for (const MissingModel &model : library.missing(models, hashes))
        if (!model.source.isEmpty())
            out << model;
    return out;
}

void Workflows::startDownloads(const QJsonArray &models, const QString &what) {
    ModelLibrary library = ModelLibrary::current(m_registry);
    QList<DownloadTarget> targets;
    Errors refused;
    for (const MissingModel &model : downloadable(models, library)) {
        const Result<DownloadTarget> target = planDownload(library, model);
        if (target)
            targets << target.value();
        else
            refused << target.errors();
    }
    if (!refused.isEmpty())
        setNotice(withHints(refused));
    // Said again when they end, beside what failed then; at once when nothing is to come.
    if (targets.isEmpty())
        refused.clear();
    m_refused += refused;
    m_downloads->add(targets, what);
    emit noticeChanged(); // the offer hides while they download
}

void Workflows::downloadMissing() {
    if (!downloadOffer().isEmpty())
        startDownloads(m_offer.models, QStringLiteral("workflow"));
}

void Workflows::downloadTemplate(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (chosen)
        startDownloads(chosen->models, QStringLiteral("template:") + id);
}

void Workflows::setDownloadRoot(const QString &root) {
    const ModelLibrary library = ModelLibrary::current(m_registry);
    if (!library.roots().contains(root))
        return;
    // Fugue's own folder is the default: no key.
    const Errors errors = changeSetting(settingsPath(), QStringLiteral("download_folder"),
                                        root == library.roots().value(0) ? QJsonValue(QJsonValue::Undefined)
                                                                         : QJsonValue(root));
    if (!errors.isEmpty()) {
        setNotice(withHints(errors));
        return;
    }
    refreshMissing(false); // the line names the folders they go in
}

void Workflows::analyze(const Document &document, const QString &title) {
    m_missingNotice = missingNotice(document, title, &m_offer);
    setNotice(m_missingNotice);
    emit noticeChanged(); // the offer may differ with the same line
    if (addPendingTrust(m_trust.untrusted(document, *m_registry)))
        emit pendingTrustChanged();
}

bool Workflows::addPendingTrust(const QList<PackUse> &uses) {
    bool changed = false;
    for (const PackUse &use : uses) {
        bool known = false;
        for (const QVariant &entry : std::as_const(m_pendingTrust))
            known |= entry.toMap().value("id") == use.id && entry.toMap().value("version") == use.version;
        if (known)
            continue;
        m_pendingTrust.append(QVariantMap{
            {"id", use.id}, {"version", use.version}, {"source", use.source}, {"types", use.types}});
        changed = true;
    }
    return changed;
}

// --- save and close ---

bool Workflows::saveTab(int index, const QString &path, bool replace) {
    const QString taken = tr("Could not save %1: a file of that name already exists; choose another name.")
                              .arg(QFileInfo(path).fileName());
    if (!replace && QFileInfo::exists(path)) {
        setNotice(taken);
        return false;
    }
    // Into another folder, the relative paths the tab and its undo steps hold would name other
    // files: they become the full paths they name now, as no step, before the inputs move.
    const QString from = pathOf(index);
    if (index == m_current && !from.isEmpty()
        && canonicalPath(QFileInfo(from).absolutePath()) != canonicalPath(QFileInfo(path).absolutePath()))
        m_canvas->rewriteDocuments([this, from](Document &document) { makeInputsAbsolute(document, *m_registry, from); });
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
    document.refreshRequirements(appVersion(), m_registry);
    const Result<bool> written = writeWorkflow(document, path, replace);
    if (written && !*written) {
        setNotice(taken);
        return false;
    }
    if (const Errors errors = written.errors(); hasErrors(errors)) {
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
    const bool appended = QFileInfo(path).suffix().isEmpty();
    if (appended)
        path += QStringLiteral(".fugue");
    const QString id = m_tabs[size_t(m_current)].id;
    if (!saveTab(m_current, path, !appended))
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
    QString aside;
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value("id").toString();
        const Result<Document> document = m_recovery.read(id);
        if (!document.ok()) {
            // Never deleted: it may be the only copy of the work, readable by a later version.
            failed << entry.value("title").toString();
            if (const QString folder = m_recovery.setAside(id); !folder.isEmpty())
                aside = folder;
            continue;
        }
        // The copy keeps its id, so the next 30-second write replaces it in place.
        addTab(document.value(), entry.value("path").toString(), entry.value("title").toString(), true, true, id);
        analyze(document.value(), m_tabs[size_t(m_current)].title);
    }
    if (!failed.isEmpty())
        setNotice(aside.isEmpty()
                      ? tr("These recovery copies could not be read: %1").arg(failed.join(", "))
                      : tr("These recovery copies could not be read and were kept in %1: %2").arg(aside, failed.join(", ")));
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
    for (const QVariant &entry : std::as_const(m_pendingTrust))
        m_trustDismissed.insert(entry.toMap().value("id").toString() + QLatin1Char(' ')
                                + entry.toMap().value("version").toString());
    m_pendingTrust.clear();
    emit pendingTrustChanged();
}

bool Workflows::isTrusted(const QString &packId, const QString &version) const {
    return m_trust.isTrusted(packId, version);
}

void Workflows::refreshNodeTypes() {
    if (m_current < 0)
        return;
    const Document shown = documentOf(m_current);
    // The banner follows the shown workflow's missing nodes (a pack removed adds some, one
    // installed takes them away), unless it shows another line; a line dismissed comes back
    // only when what is missing changed.
    const QString missing = missingNotice(shown, m_tabs[size_t(m_current)].title, &m_offer);
    if (missing != m_missingNotice && (m_notice.isEmpty() || m_notice == m_missingNotice))
        setNotice(missing);
    m_missingNotice = missing;
    emit noticeChanged(); // the offer

    // The question follows what is installed and trusted now: a pack trusted meanwhile (the
    // Packs page, the CLI) is not asked about, nor one at a version no longer installed; a pack
    // that came, or a new version of one, is asked about (6.6, 6.7), unless the person said
    // "not now" to that version already.
    QVariantList still;
    for (const QVariant &entry : std::as_const(m_pendingTrust)) {
        const QString id = entry.toMap().value("id").toString(), version = entry.toMap().value("version").toString();
        const QString installed = m_registry->packVersion(id);
        if (!m_trust.isTrusted(id, version) && (installed.isEmpty() || installed == version))
            still << entry;
    }
    bool changed = still.size() != m_pendingTrust.size();
    m_pendingTrust = still;
    QList<PackUse> asked;
    for (const PackUse &use : m_trust.untrusted(shown, *m_registry))
        if (!m_trustDismissed.contains(use.id + QLatin1Char(' ') + use.version))
            asked << use;
    changed |= addPendingTrust(asked);
    if (changed)
        emit pendingTrustChanged();
}

// --- commands ---

void registerFileCommands(CommandRegistry &registry, Workflows &workflows) {
    registry.setHandler("file.new", [&workflows] { workflows.newWorkflow(); });
    registry.setHandler("file.open", [&workflows] { emit workflows.openDialogRequested(); });
    registry.setHandler("file.save", [&workflows] { workflows.save(); });
    // The Templates dialog is the window's (Main.qml), like Settings.
    const auto browseTemplates = [&registry] { emit registry.windowRequested(QStringLiteral("file.templates")); };
    if (!registry.contains("file.templates")) {
        CommandSpec spec;
        spec.id = QStringLiteral("file.templates");
        spec.description = QT_TRANSLATE_NOOP("Command", "Browse templates");
        spec.versus = QStringLiteral("same");
        const QString problem = registry.add(spec, browseTemplates);
        if (!problem.isEmpty())
            qWarning("Fugue: %s", qPrintable(problem));
    } else {
        registry.setHandler("file.templates", browseTemplates);
    }
    registry.setHandler("file.save-as", [&workflows] { emit workflows.saveAsRequested(); });
    registry.setHandler("file.export-share", [&workflows] { emit workflows.exportShareRequested(); });
    registry.setHandler("file.export-compiled", [&workflows] { emit workflows.exportCompiledRequested(); });
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
