#include "run/canvas_runs.h"

#include <QDir>
#include <QJsonArray>
#include <QFileInfo>
#include <QSizeF>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/file_roots.h"
#include "engine/groups.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

QString topOf(const QString &path) { return path.section('/', 0, 0); }

// The path ids from the outermost group instance down to the node: "g7", "g7/inner3",
// "g7/inner3/s2". Each level keeps its own view, so a group shows what its inside does and,
// entered, its inner nodes show their own.
// "1024 \u00d7 1024", how a picture's size reads on a node (a measured one in its chip, a predicted one in its header).
QString sizeText(int width, int height) { return QStringLiteral("%1 \u00d7 %2").arg(width).arg(height); }

// The size a node's check predicted for its picture (#601): `sizes` is {"<output>": [width, height]}. The output the node is
// known by, the picture first, then the latent, then a mask; none when it predicted nothing.
QString predictedText(const QJsonObject &sizes) {
    for (const char *port : {"image", "latent", "mask"}) {
        const QJsonArray size = sizes.value(QLatin1String(port)).toArray();
        if (size.size() == 2 && size.at(0).toInt() > 0 && size.at(1).toInt() > 0)
            return sizeText(size.at(0).toInt(), size.at(1).toInt());
    }
    return {};
}

QStringList levelsOf(const QString &path) {
    QStringList levels;
    const QStringList parts = path.split('/');
    for (qsizetype i = 1; i <= parts.size(); ++i)
        levels << QStringList(parts.mid(0, i)).join('/');
    return levels;
}

// The errors of 8.5: nothing can serve the node's capability, its model is not running or
// missing, its provider cannot be reached or is not configured. The node shows what is
// missing and how to fix it.
bool cannotRunNow(const Error &error) {
    static const QStringList codes{"capability_unavailable", "model_not_running", "model_missing",
                                   "provider_unreachable", "unknown_provider"};
    return codes.contains(error.code);
}

// What a node says of an error: its message, and for the errors of 8.5 the fix too.
QString shownMessage(const Error &error) {
    if (!cannotRunNow(error) || error.hint.isEmpty())
        return error.message;
    return error.message + QLatin1Char(' ') + error.hint;
}

// The image in a slot that shows text: one transparent pixel.
constexpr const char *kBlank = "data:image/png;base64,"
                               "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=";

bool isImagePath(const QString &path) {
    static const QStringList extensions{"png", "jpg", "jpeg", "webp", "gif", "bmp", "tif", "tiff", "avif"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

// The last value on an output port as its chip shows it (docs/canvas-look.md 4.5): numbers,
// on/off and the first line of a text. Media and resources have their own displays.
QString chipText(const QJsonValue &value) {
    if (value.isBool())
        return value.toBool() ? CanvasRuns::tr("on") : CanvasRuns::tr("off");
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (std::abs(d) < 9007199254740992.0 && d == std::floor(d))
            return QString::number(qint64(d));
        return QString::number(d, 'g', 6);
    }
    if (value.isString()) {
        const QString line = value.toString().section('\n', 0, 0);
        return line.size() > 40 ? line.left(39) + QChar(0x2026) : line;
    }
    return {};
}

} // namespace

CanvasRuns::CanvasRuns(CanvasModel *canvas, Workflows *workflows, RunService *service, const Registry *registry,
                       QObject *parent)
    : QObject(parent), m_canvas(canvas), m_workflows(workflows), m_service(service), m_registry(registry) {
    connect(m_service, &RunService::filePreviewed, this, &CanvasRuns::onFilePreviewed);
    // A new document's cards are made while its reset is under way, before documentReset says
    // so (showTab), so they are counted from the reset's start.
    connect(m_canvas->nodes(), &QAbstractItemModel::modelAboutToBeReset, this, [this] { m_rowsMade.clear(); });
    connect(m_canvas, &CanvasModel::rowsMade, this, [this](const QString &node) {
        m_rowsMade.insert(node);
        requestInputPreview(keyFor(node));
    });
    connect(m_service, &RunService::event, this, &CanvasRuns::onEvent);
    connect(m_service, &RunService::checked, this, &CanvasRuns::onChecked);
    connect(m_service, &RunService::remoteConsentNeeded, this, [this](const QString &run, const QJsonArray &services, int revision) {
        // The same run asked again: the question it is in changed what it says (a joined run sends more kinds of data), or the run
        // thread refused a yes given to an older one.
        for (auto &ask : m_remoteAsks)
            if (ask.run == run) {
                ask.services = services;
                ask.revision = revision;
                if (&ask == &m_remoteAsks.first())
                    emit remoteQuestionChanged();
                return;
            }
        // A question already answered at this revision or a newer one is settled on the run thread: a late copy of it (sent before
        // the answer arrived there) must not come back as a question whose Yes and No do nothing. A refused yes is re-asked with a
        // higher revision, so it still shows.
        if (const auto it = m_answeredRevision.constFind(run); it != m_answeredRevision.constEnd() && revision <= it.value())
            return;
        m_remoteAsks.append({run, services, revision});
        if (m_remoteAsks.size() == 1)
            emit remoteQuestionChanged();
    });
    // A run that ended without an answer (cancelled while it waited) takes its question away.
    connect(m_service, &RunService::event, this, [this](const QString &run, const QJsonObject &event) {
        if (event.value("event").toString() != QLatin1String("run.done"))
            return;
        m_answeredRevision.remove(run);
        const bool first = !m_remoteAsks.isEmpty() && m_remoteAsks.first().run == run;
        m_remoteAsks.removeIf([&](const auto &ask) { return ask.run == run; });
        if (first)
            emit remoteQuestionChanged();
    });
    m_checkTimer.setSingleShot(true);
    m_checkTimer.setInterval(300);
    connect(&m_checkTimer, &QTimer::timeout, this, &CanvasRuns::checkNow);
    m_progressTimer.setSingleShot(true);
    m_progressTimer.setInterval(33);
    connect(&m_progressTimer, &QTimer::timeout, this, &CanvasRuns::flushProgress);
    connect(m_canvas, &CanvasModel::edited, this, [this] {
        scheduleCheck(true);
        refreshInputs();
    });
    // A wire added or removed shifts the rows the running dots point at: they are made again.
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(m_canvas->links(), signal, this, [this] { if (!m_running.isEmpty()) emit runningNodesChanged(); });
    connect(m_canvas->links(), &QAbstractItemModel::modelReset, this, [this] { if (!m_running.isEmpty()) emit runningNodesChanged(); });
    // The picture mode changed (Settings, the View menu): the slots come or go.
    connect(m_canvas, &CanvasModel::pictureModeChanged, this, &CanvasRuns::refreshInputs);
    connect(m_canvas, &CanvasModel::documentReset, this, &CanvasRuns::showTab);
    connect(m_workflows, &Workflows::currentChanged, this, &CanvasRuns::showTab);
    showTab();
}

CanvasRuns::~CanvasRuns() = default;

void CanvasRuns::setError(NodeView &view, const Error &error, const QString &path, bool inner) const {
    const QString prefix = inner ? path + QStringLiteral(": ") : QString();
    view.fromCheck = false; // a run's error; onChecked marks the ones a check made
    view.error = prefix + shownMessage(error);
    view.errorDetail.clear();
    view.pack.clear();
    if (error.code != QLatin1String("unknown_node_type") || error.pack.isEmpty())
        return;
    // A node of a pack that is not installed: the card says which pack, in words; the technical
    // text, with the inner node's id, is the tooltip's.
    const QVariantMap need = m_workflows->packNeed(error.pack);
    if (need.isEmpty())
        return;
    view.errorDetail = prefix + error.message + (error.hint.isEmpty() ? QString() : QLatin1Char(' ') + error.hint);
    view.error = need.value("text").toString();
    view.pack = QVariantMap{{"id", need.value("id")}, {"name", need.value("name")},
                            {"text", need.value("text")}, {"installable", need.value("installable")}};
}

void CanvasRuns::registerCommands(CommandRegistry &commands) {
    m_commands = &commands;
    commands.setHandler(QStringLiteral("run.start"), [this] { start(false); });
    commands.setHandler(QStringLiteral("run.start-first"), [this] { start(true); });
    commands.setHandler(QStringLiteral("run.to-here"), [this] { runToSelected(); });
    commands.setHandler(QStringLiteral("run.cancel"), [this] { cancel(); });
    setContext();
}

QString CanvasRuns::currentTab() const {
    return m_workflows->tabs().value(m_workflows->current()).toMap().value("id").toString();
}

QString CanvasRuns::start(bool first) { return startRun({}, first); }

QString CanvasRuns::runToSelected() {
    const QStringList selected = m_canvas->selectedIds();
    return selected.size() == 1 ? startRun(keyFor(selected.first()), false) : QString();
}

QString CanvasRuns::startRun(const QString &target, bool first) {
    const QString tab = currentTab();
    if (tab.isEmpty())
        return {};
    QString firstId;
    for (int i = 0; i < m_batch; ++i) {
        const QString id = startOne(tab, target, first);
        if (firstId.isEmpty())
            firstId = id;
    }
    return firstId;
}

void CanvasRuns::setBatch(int batch) {
    batch = std::clamp(batch, 1, maxBatch);
    if (batch == m_batch)
        return;
    m_batch = batch;
    emit batchChanged();
}

bool CanvasRuns::showResults(const QString &run, const QString &workflow) {
    const QString tab = currentTab();
    if (tab.isEmpty() || workflow.isEmpty() || workflow != m_workflows->pathOf(m_workflows->current()))
        return false;
    // What the tab showed of an earlier run goes; the run's own results take its place.
    TabRuns &runs = m_tabs[tab];
    runs.errors.clear();
    ++runs.epoch;
    m_checkFromEdit = false;
    m_tabOf.insert(run, tab);
    m_service->replay(run);
    if (tab == m_shownTab)
        emit errorsChanged();
    return true;
}

QString CanvasRuns::startOne(const QString &tab, const QString &target, bool first) {
    RunRequest request;
    request.document = m_canvas->document();
    request.workflow = m_workflows->pathOf(m_workflows->current());
    request.target = target;
    request.entered = m_canvas->scopePath();
    request.first = first;
    request.noPictures = noPictures();
    const QString id = m_service->start(request);
    // The run has the numbers shown; the next one gets the next (spec 6.2).
    m_canvas->widgets()->advanceSeeds();
    // The run is newer than any edit so far, the seeds' own included: the check after it
    // keeps the run's errors (they go with the next edit). A check due from an edit before
    // the run cleared them when it landed after a short run.
    m_checkFromEdit = false;
    m_tabOf.insert(id, tab);
    if (first) {
        // After the run going and the ones already started ahead, as the queue sorts them.
        int at = m_active.isEmpty() ? 0 : 1;
        while (at < m_order.size() && m_first.contains(m_order.at(at)))
            ++at;
        m_order.insert(at, id);
        m_first.insert(id);
    } else {
        m_order.append(id);
    }
    TabRuns &runs = m_tabs[tab];
    ++runs.busy;
    ++runs.epoch;
    // A new run starts without the last one's errors.
    runs.errors.clear();
    runs.checkErrors.clear(); // and without the last check's: the next check lists what is still wrong
    for (auto it = runs.nodes.begin(); it != runs.nodes.end(); ++it) {
        if (!it->error.isEmpty() || it->state == QLatin1String("error")) {
            it->clearError(); // a check's error too: the run's own, if it fails, is not the check's to erase
            if (it->state == QLatin1String("error"))
                it->state = QStringLiteral("stale");
            for (auto inner = it->inner.begin(); inner != it->inner.end(); ++inner)
                if (inner.value() == QLatin1String("error"))
                    inner.value() = QStringLiteral("stale");
            show(tab, it.key());
        }
    }
    if (tab == m_shownTab)
        emit errorsChanged();
    setContext();
    emit changed();
    return id;
}

void CanvasRuns::cancel() {
    if (!m_active.isEmpty())
        m_service->cancel(m_active);
    else if (!m_order.isEmpty())
        m_service->cancel(m_order.last());
}

void CanvasRuns::setContext() {
    if (m_commands)
        m_commands->setContextActive(QStringLiteral("run"), busy());
}

QString CanvasRuns::aggregate(const NodeView &view) {
    if (view.inner.isEmpty())
        return view.state;
    if (view.state == QLatin1String("error"))
        return view.state;
    bool stale = false, cached = true;
    for (const QString &state : view.inner) {
        if (state == QLatin1String("error") || state == QLatin1String("running"))
            return state;
        stale |= state == QLatin1String("stale");
        cached &= state == QLatin1String("cached") || state == QLatin1String("frozen");
    }
    return stale ? QStringLiteral("stale") : cached ? QStringLiteral("cached") : view.state;
}

// A group's progress is its inside's (5.6): the share of its nodes done, the running one counted
// by its own progress.
qreal CanvasRuns::progressOf(const NodeView &view) {
    if (view.inner.isEmpty())
        return view.progress;
    if (aggregate(view) != QLatin1String("running"))
        return -1;
    qreal done = 0;
    for (const QString &state : view.inner)
        if (state != QLatin1String("running") && state != QLatin1String("stale") && !state.isEmpty())
            done += 1;
    return std::clamp((done + std::max<qreal>(0, view.progress)) / qreal(view.inner.size()), 0.0, 1.0);
}

QString CanvasRuns::keyFor(const QString &node) const {
    const QString scope = m_canvas->scopePath();
    return scope.isEmpty() ? node : scope + QLatin1Char('/') + node;
}

QString CanvasRuns::rowFor(const QString &key) const {
    const QString scope = m_canvas->scopePath();
    const QString rest = scope.isEmpty() ? key : key.startsWith(scope + QLatin1Char('/')) ? key.mid(scope.size() + 1) : QString();
    return rest.contains('/') ? QString() : rest;
}

QString CanvasRuns::nodeTitle(const QString &tab, const QString &path) const {
    if (tab != m_shownTab)
        return path;
    // The node as the level shown has it: itself, or the group instance holding it.
    for (const QString &level : levelsOf(path)) {
        const QString row = rowFor(level);
        if (row.isEmpty())
            continue;
        const QString title = m_canvas->nodeTitle(row);
        if (title.isEmpty())
            return path;
        return level == path ? title : title + QStringLiteral(" › ") + path.mid(level.size() + 1);
    }
    return path;
}

// The picture arriving at a Crop's image input (#73).
CanvasRuns::Picture CanvasRuns::cropSource(const QString &tab, const QString &node) const {
    return pictureOf(tab, m_nodeInfo.value(node).cropFrom);
}

// The picture a node `from` shows: its result, else the image of its input file.
CanvasRuns::Picture CanvasRuns::pictureOf(const QString &tab, const QString &from) const {
    const auto nodes = m_tabs.value(tab).nodes;
    const auto found = nodes.constFind(from);
    if (from.isEmpty() || found == nodes.constEnd())
        return {};
    if (!found->thumbnail.isEmpty() && found->text.isEmpty() && found->thumbnailWidth > 0 && found->thumbnailHeight > 0)
        return {found->thumbnail, found->imagePath, found->thumbnailWidth, found->thumbnailHeight};
    if (!found->inputThumbnail.isEmpty() && !found->inputMissing)
        return {found->inputThumbnail, expandHome(m_nodeInfo.value(from).imageFile), found->inputWidth, found->inputHeight};
    return {};
}

void CanvasRuns::show(const QString &tab, const QString &node) {
    if (tab != m_shownTab)
        return;
    // A Crop shows its input's picture: it follows the node that feeds it (a DAG, so this ends).
    for (auto it = m_nodeInfo.cbegin(); it != m_nodeInfo.cend(); ++it)
        if (it->followsInput() && it->cropFrom == node)
            show(tab, it.key());
    // `node` is a path id; only the nodes of the level shown have a card.
    const QString row = rowFor(node);
    if (row.isEmpty())
        return;
    const NodeView view = m_tabs.value(tab).nodes.value(node);
    QVariantMap state{{"state", aggregate(view)}, {"progress", progressOf(view)}, {"seconds", view.seconds},
                      {"error", view.error},      {"warnings", view.warnings}, {"values", view.values},
                      {"notice", view.notice},    {"errorDetail", view.errorDetail}, {"errorPack", view.pack}, {"runsOn", view.runsOn}};
    const auto image = [&](const QString &url, int width, int height, const QString &file) {
        const QString size = sizeText(width, height);
        state.insert("thumbnail", url);
        state.insert("thumbnailAspect", qreal(height) / width);
        state.insert("thumbnailSize", QSizeF(width, height));
        state.insert("thumbnailInfo", size);
        state.insert("thumbnailFile", file);
    };
    const auto text = [&](const QString &shown, qreal aspect, bool dim) {
        state.insert("thumbnail", QString::fromLatin1(kBlank));
        state.insert("thumbnailAspect", aspect);
        state.insert("thumbnailText", shown);
        state.insert("thumbnailDim", dim);
    };
    // One slot (docs/canvas-look.md 4.5, 4.6): the last result, else the input file's image
    // (Load Image before it runs), else a dimmed placeholder on a node made to show one.
    const NodeInfo info = m_nodeInfo.value(node);
    const Picture cropped = info.followsInput() ? cropSource(tab, node) : Picture{};
    // A node whose picture the mode hides still shows the live frame of its running step (or of a
    // running node inside a group), unless its own override hides it. The frame is an overlay beside
    // the card, never a slot in it: a live frame never changes the layout.
    const bool liveHidden = !info.picture && !info.hideOverride && view.live && !view.thumbnail.isEmpty()
                            && view.thumbnailWidth > 0 && view.thumbnailHeight > 0 && aggregate(view) == QLatin1String("running");
    if (liveHidden) {
        state.insert("liveFrame", view.thumbnail);
        state.insert("liveAspect", qreal(view.thumbnailHeight) / view.thumbnailWidth);
    } else if (!info.picture) {
        // Hidden (#263): no slot, so the node is as short as its rows.
    } else if (info.followsInput()) {
        // A Crop shows what arrives at its input, never its own result: the frame is drawn over it.
        // So does a node that paints a mask over an input (Paint Mask), the mask drawn over it.
        if (cropped.width > 0 && cropped.height > 0)
            image(cropped.thumbnail, cropped.width, cropped.height, {});
        else
            text(tr("No picture yet"), 0.5, true);
    } else if (!view.thumbnail.isEmpty() && view.thumbnailWidth > 0 && view.thumbnailHeight > 0)
        image(view.thumbnail, view.thumbnailWidth, view.thumbnailHeight, view.thumbnailFile);
    else if (!view.text.isEmpty()) {
        // A video or a sound gets a player: the slot is shaped for it (a bar for a sound).
        const bool sound = view.media.value("type") == QLatin1String("audio");
        text(view.text, sound ? 0.25 : view.textAspect, false);
        if (!view.media.isEmpty())
            state.insert("media", view.media);
    } else if (!view.inputThumbnail.isEmpty())
        image(view.inputThumbnail, view.inputWidth, view.inputHeight, {});
    else if (view.inputMissing)
        text(tr("File not found"), 0.5, true);
    else if (info.showsResult && aggregate(view) != QLatin1String("running"))
        text(tr("No result yet"), 0.5, true);
    if (info.picture && info.compares && !view.compare.isEmpty()) {
        QVariantMap compare = view.compare;
        compare.insert("labelA", info.labelA);
        compare.insert("labelB", info.labelB);
        state.insert("compare", compare);
    }
    // A size known before a run (#601) shows only while no measured one does: the picture's chip is the measure.
    if (!view.predictedSize.isEmpty() && !state.contains("thumbnailInfo"))
        state.insert("predictedSize", view.predictedSize);
    // While a node runs, what its slot shows is the last run's, at half strength, until this
    // run's preview comes.
    if (state.contains("thumbnail") && aggregate(view) == QLatin1String("running") && !view.live)
        state.insert("thumbnailDim", true);
    m_pendingProgress.remove(node);
    // A check after each edit restates every node; only the ones that changed are laid out.
    auto shown = m_shown.find(node);
    if (shown != m_shown.end() && shown.value() == state)
        return;
    m_shown.insert(node, state);
    m_canvas->setRunState(row, state);
}

void CanvasRuns::showTab() {
    m_shownTab = currentTab();
    noteRunsOn();
    readNodes();
    // The cards made already asked with the last tab's files.
    for (const QString &node : std::as_const(m_rowsMade))
        requestInputPreview(keyFor(node));
    m_pendingProgress.clear();
    m_shown.clear();
    m_canvas->clearRunState();
    const TabRuns runs = m_tabs.value(m_shownTab);
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it)
        show(m_shownTab, it.key());
    m_running = runningNodes();
    emit runningNodesChanged();
    emit errorsChanged();
    scheduleCheck(false);
}

void CanvasRuns::flushProgress() {
    for (auto it = m_pendingProgress.cbegin(); it != m_pendingProgress.cend(); ++it) {
        if (const QString row = rowFor(it.key()); !row.isEmpty())
            m_canvas->setRunProgress(row, it.value());
        if (auto shown = m_shown.find(it.key()); shown != m_shown.end())
            shown->insert("progress", it.value());
    }
    m_pendingProgress.clear();
    emit progressChanged();
}

void CanvasRuns::onEvent(const QString &run, const QJsonObject &event) {
    // A run of no tab (a canvas action's, canvas/canvas_actions.h) is not this one's to show.
    if (!m_tabOf.contains(run))
        return;
    const QString tab = m_tabOf.value(run);
    TabRuns &runs = m_tabs[tab];
    const QString kind = event.value("event").toString();
    const QString path = event.value("node").toString();

    if (kind == QLatin1String("run.started")) {
        m_active = run;
        // The pictures shown are the last run's again until this run's previews come (a group's too).
        for (NodeView &view : runs.nodes)
            view.live = false;
        emit changed();
        return;
    }
    if (kind == QLatin1String("run.done")) {
        const QString status = event.value("status").toString();
        for (const QJsonValue &value : event.value("errors").toArray()) {
            const Error error = Error::fromJson(value.toObject());
            const QString shown = topOf(error.node);
            if (error.severity == Error::Severity::Warning) {
                for (const QString &level : error.node.isEmpty() ? QStringList() : levelsOf(error.node))
                    if (!runs.nodes[level].warnings.contains(error.message)) {
                        runs.nodes[level].warnings << error.message;
                        show(tab, level);
                    }
                continue;
            }
            // The scheduler's node errors came as node.error already; the compiler's did not.
            const bool listed = std::any_of(runs.errors.cbegin(), runs.errors.cend(), [&](const QVariant &e) {
                return e.toMap().value("node") == error.node && e.toMap().value("code") == error.code;
            });
            if (listed)
                continue;
            runs.errors << QVariantMap{{"node", error.node}, {"shown", shown}, {"message", error.message},
                                       {"code", error.code}, {"hint", error.hint}};
            // The error shows on the node and on every group holding it (5.6).
            for (const QString &level : error.node.isEmpty() ? QStringList() : levelsOf(error.node)) {
                NodeView &view = runs.nodes[level];
                view.state = QStringLiteral("error");
                setError(view, error, error.node, level != error.node);
                show(tab, level);
            }
        }
        // Whatever was still running when the run ended would run again.
        for (auto it = runs.nodes.begin(); it != runs.nodes.end(); ++it) {
            bool changedHere = false;
            if (it->state == QLatin1String("running")) {
                it->state = QStringLiteral("stale");
                changedHere = true;
            }
            for (auto in = it->inner.begin(); in != it->inner.end(); ++in)
                if (in.value() == QLatin1String("running")) {
                    in.value() = QStringLiteral("stale");
                    changedHere = true;
                }
            if (changedHere) {
                it->progress = -1;
                show(tab, it.key());
            }
        }
        runs.lastStatus = status;
        if (m_order.removeAll(run) > 0)
            --runs.busy;
        m_first.remove(run);
        m_tabOf.remove(run);
        if (m_active == run) {
            m_active.clear();
            m_activeNode.clear();
            m_activeNodePath.clear();
            m_activeProgress = -1;
            emit progressChanged();
        }
        setContext();
        emit changed();
        if (tab == m_shownTab) {
            emit errorsChanged();
            updateRunning();
        }
        emit finished(run, status);
        // What a run did not reach (a run to a node, an early failure) was cleared at its start: the check lists it again, and keeps
        // the run's own errors (it is not from an edit).
        scheduleCheck(false);
        return;
    }
    if (path.isEmpty())
        return;

    const QStringList levels = levelsOf(path);
    if (kind == QLatin1String("node.progress")) {
        for (const QString &top : levels) {
            NodeView &view = runs.nodes[top];
            view.notice.clear();
            view.progress = event.value("value").toDouble();
            if (tab == m_shownTab)
                m_pendingProgress.insert(top, progressOf(view));
        }
        if (path == m_activeNodePath)
            m_activeProgress = event.value("value").toDouble();
        if (!m_progressTimer.isActive())
            m_progressTimer.start();
        return; // the canvas gets it with the next flush
    }
    static const QStringList known{"node.running", "node.preview", "node.done", "node.cached", "node.skipped", "node.error"};
    if (!known.contains(kind))
        return;
    if (kind == QLatin1String("node.running")) {
        m_activeNodePath = path;
        m_activeNode = nodeTitle(tab, path);
        m_activeProgress = -1;
        emit progressChanged();
    } else if (kind == QLatin1String("node.error")) {
        const Error error = Error::fromJson(event.value("error").toObject());
        runs.errors << QVariantMap{{"node", path}, {"shown", topOf(path)}, {"message", error.message},
                                   {"code", error.code}, {"hint", error.hint}};
        if (tab == m_shownTab)
            emit errorsChanged();
    }
    // Each level holding the node takes the event: the node itself, and each group around it
    // as its inside's state.
    for (const QString &top : levels) {
        const bool inner = top != path;
        NodeView &view = runs.nodes[top];
        // A run that reaches the node knows better than the last check: it runs, or its error
        // says what is missing.
        view.notice.clear();
        auto setState = [&](const QString &state) {
            if (inner)
                view.inner.insert(path, state);
            else
                view.state = state;
        };
        if (kind == QLatin1String("node.running")) {
            setState(QStringLiteral("running"));
            view.progress = -1;
            if (!inner)
                view.live = false;
        } else if (kind == QLatin1String("node.preview")) {
            const QJsonObject thumbnail = event.value("thumbnails").toObject().value("preview").toObject();
            // A group's card shows its inside's frame as its picture while that runs, until the
            // group's own result replaces it.
            if (thumbnail.isEmpty())
                continue;
            view.thumbnail = thumbnail.value("url").toString();
            view.thumbnailWidth = thumbnail.value("width").toInt();
            view.thumbnailHeight = thumbnail.value("height").toInt();
            view.imagePath = thumbnail.value("source").toString();
            view.live = true;
        } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
            setState(event.value("frozen").toBool() ? QStringLiteral("frozen") : QStringLiteral("cached"));
            view.progress = -1;
            if (kind == QLatin1String("node.done")) {
                const qreal seconds = event.value("seconds").toDouble();
                if (inner) {
                    view.innerSeconds.insert(path, seconds);
                    qreal sum = 0;
                    for (const qreal s : std::as_const(view.innerSeconds))
                        sum += s;
                    view.seconds = sum;
                } else {
                    view.seconds = seconds;
                }
            }
            if (!inner) {
                const QJsonObject outputs = event.value("outputs").toObject();
                for (auto it = outputs.begin(); it != outputs.end(); ++it)
                    if (const QString text = chipText(it.value()); !text.isEmpty())
                        view.values.insert(it.key(), text);
            }
            const QJsonObject thumbnails = event.value("thumbnails").toObject();
            if (!inner && m_nodeInfo.value(top).compares) {
                // Compare Images: the viewer takes A and B (the inputs' thumbnails) and the two
                // outputs; the slot's size is A's, its file the composite.
                const QJsonObject inputs = event.value("inputThumbnails").toObject();
                view.compare.clear();
                const auto take = [&](const char *key, const QJsonObject &from, const char *name) {
                    const QJsonObject t = from.value(QLatin1String(name)).toObject();
                    if (!t.value("url").toString().isEmpty())
                        view.compare.insert(QLatin1String(key), QVariantMap{{"url", t.value("url").toString()},
                            {"source", t.value("source").toString()},
                            {"fileUrl", QUrl::fromLocalFile(t.value("source").toString()).toString()}, {"width", t.value("width").toInt()},
                            {"height", t.value("height").toInt()}});
                };
                take("a", inputs, "a");
                take("b", inputs, "b");
                take("difference", thumbnails, "difference");
                take("composite", thumbnails, "composite");
                view.media.clear();
                const QVariantMap a = view.compare.value("a").toMap();
                if (!a.isEmpty()) {
                    view.thumbnail = a.value("url").toString();
                    view.thumbnailWidth = a.value("width").toInt();
                    view.thumbnailHeight = a.value("height").toInt();
                    view.imagePath = view.compare.value("composite").toMap().value("source").toString();
                    view.thumbnailFile.clear();
                    view.text.clear();
                }
            } else if (!thumbnails.isEmpty() && (!inner || m_nodeInfo.value(top).resultFrom == path)) {
                const QJsonObject thumbnail = thumbnails.contains("image")  ? thumbnails.value("image").toObject()
                                              : thumbnails.contains("file") ? thumbnails.value("file").toObject()
                                                                            : thumbnails.begin().value().toObject();
                view.thumbnail = thumbnail.value("url").toString();
                view.thumbnailWidth = thumbnail.value("width").toInt();
                view.thumbnailHeight = thumbnail.value("height").toInt();
                view.imagePath = thumbnail.value("source").toString();
                view.thumbnailFile = m_nodeInfo.value(top).savesFile ? thumbnail.value("file").toString() : QString();
                view.text = thumbnail.value("label").toString();
                view.textAspect = view.thumbnailWidth > 0 && view.thumbnail.isEmpty()
                                      ? qreal(view.thumbnailHeight) / view.thumbnailWidth : 0.5;
                view.media.clear();
                if (const QString media = thumbnail.value("mediaPath").toString(); !media.isEmpty())
                    view.media = {{"type", thumbnail.value("type").toString()},
                                  {"url", QUrl::fromLocalFile(media).toString()},
                                  {"label", view.text},
                                  {"width", view.thumbnailWidth},
                                  {"height", view.thumbnailHeight},
                                  {"duration", thumbnail.value("duration").toDouble()},
                                  {"file", view.thumbnailFile}};
            } else if (!inner) {
                // What a Preview shows when it is a text or a number: its excerpt in the slot.
                const QJsonObject shown = event.value("shown").toObject();
                for (auto it = shown.begin(); it != shown.end(); ++it) {
                    if (it.value().isObject() || it.value().isArray())
                        continue;
                    QString excerpt = it.value().isString() ? it.value().toString() : chipText(it.value());
                    // The slot wraps and elides what does not fit its box; this only bounds a huge text.
                    if (excerpt.size() > 8000)
                        excerpt = excerpt.left(7999) + QChar(0x2026);
                    view.text = excerpt;
                    view.textAspect = 0.5;
                    view.media.clear();
                    view.thumbnail.clear();
                    view.imagePath.clear();
                    break;
                }
            }
        } else if (kind == QLatin1String("node.skipped")) {
            // Cancelled before it started: it would still run.
            setState(event.value("reason") == QLatin1String("cancelled") ? QStringLiteral("stale") : QStringLiteral("skipped"));
            view.progress = -1;
        } else if (kind == QLatin1String("node.error")) {
            const Error error = Error::fromJson(event.value("error").toObject());
            setState(QStringLiteral("error"));
            view.progress = -1;
            // An inner node's error names its path id (5.6).
            setError(view, error, path, inner);
        }
        show(tab, top);
    }
    if (tab == m_shownTab)
        updateRunning();
}

void CanvasRuns::refreshNodeTypes() {
    m_service->reloadNodeTypes();
    refreshInputs();
    scheduleCheck(false);
}

void CanvasRuns::scheduleCheck(bool edit) {
    m_checkFromEdit |= edit;
    m_checkTimer.start();
}

void CanvasRuns::checkNow() {
    m_checkTimer.stop();
    const QString tab = currentTab();
    if (tab.isEmpty())
        return;
    const int ticket = ++m_ticket;
    // The check this replaces is never answered: an edit it was asked for counts here.
    const bool edit = m_checkFromEdit || (m_asked.tab == tab && m_asked.edit);
    m_asked = {tab, m_tabs[tab].epoch, edit};
    m_checkFromEdit = false;
    RunRequest request;
    request.document = m_canvas->document();
    request.workflow = m_workflows->pathOf(m_workflows->current());
    request.entered = m_canvas->scopePath();
    m_service->check(request, ticket);
}

void CanvasRuns::onChecked(int ticket, const QJsonObject &nodes, const QJsonArray &problems, const QJsonArray &checkErrors) {
    if (ticket != m_ticket || m_asked.tab.isEmpty())
        return;
    const Ticket asked = std::exchange(m_asked, {});
    TabRuns &runs = m_tabs[asked.tab];
    // A run since the check was asked knows better.
    if (runs.busy > 0 || runs.epoch != asked.epoch)
        return;
    QHash<QString, NodeView> next;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
      for (const QString &top : levelsOf(it.key())) {
        if (!next.contains(top)) {
            NodeView view = runs.nodes.value(top);
            // An error stays until the workflow is edited, a group's inner one too.
            const bool failed = aggregate(view) == QLatin1String("error") && !view.fromCheck;
            view.inner.clear();
            view.warnings.clear();
            view.notice.clear();
            view.runsOn.clear();
            view.predictedSize.clear();
            if (asked.edit || !failed) {
                view.clearError();
                view.state.clear();
            } else {
                view.state = QStringLiteral("error");
            }
            next.insert(top, view);
        }
        NodeView &view = next[top];
        const QString state = it.value().toObject().value("state").toString();
        if (top == it.key()) {
            view.runsOn = it.value().toObject().value("runsOn").toString();
            view.predictedSize = predictedText(it.value().toObject().value("sizes").toObject());
        }
        if (top != it.key()) {
            const bool failed = runs.nodes.value(top).inner.value(it.key()) == QLatin1String("error");
            view.inner.insert(it.key(), failed && !asked.edit ? QStringLiteral("error") : state);
        } else if (view.state != QLatin1String("error")) {
            view.state = state;
        }
      }
    }
    for (const QJsonValue &value : problems) {
        const Error error = Error::fromJson(value.toObject());
        if (error.severity != Error::Severity::Warning || error.node.isEmpty())
            continue;
        for (const QString &top : levelsOf(error.node)) {
            if (!next.contains(top))
                continue;
            next[top].warnings << error.message;
            if (next[top].notice.isEmpty() && cannotRunNow(error))
                next[top].notice = shownMessage(error);
        }
    }
    // Nodes the check has no state for (not part of any output) keep their last values only.
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it) {
        if (next.contains(it.key()))
            continue;
        NodeView view = it.value();
        const bool failed = aggregate(view) == QLatin1String("error") && !view.fromCheck;
        view.inner.clear();
        view.warnings.clear();
        view.notice.clear();
        view.runsOn.clear();
        view.predictedSize.clear();
        if (asked.edit || !failed) {
            view.state.clear();
            view.clearError();
        } else {
            view.state = QStringLiteral("error");
        }
        next.insert(it.key(), view);
    }
    // What a node's kernel found wrong in its own values (check(), #600) shows as the node's error, as a run's does, and goes
    // with the next check that does not find it. A node a run failed on keeps the run's words.
    runs.checkErrors.clear();
    if (!nodes.isEmpty()) {
        for (const QJsonValue &value : checkErrors) {
            const Error error = Error::fromJson(value.toObject());
            if (error.node.isEmpty())
                continue;
            runs.checkErrors << QVariantMap{{"node", error.node}, {"shown", topOf(error.node)}, {"message", error.message},
                                            {"code", error.code}, {"hint", error.hint}, {"input", error.input}};
            for (const QString &top : levelsOf(error.node)) {
                NodeView &view = next[top];
                if (view.state == QLatin1String("error") && !view.fromCheck)
                    continue;
                if (view.fromCheck && top == error.node)
                    continue; // the first of its errors is the one shown
                view.state = QStringLiteral("error");
                setError(view, error, error.node, top != error.node);
                view.fromCheck = true;
            }
        }
    }
    if (asked.edit)
        runs.errors.clear();
    runs.nodes = next;
    noteRunsOn();
    if (asked.tab == m_shownTab) {
        for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it)
            show(asked.tab, it.key());
        emit errorsChanged();
    }
    emit checkApplied();
}

QStringList CanvasRuns::runningNodes() const {
    QStringList list;
    const TabRuns runs = m_tabs.value(m_shownTab);
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it)
        if (const QString row = rowFor(it.key()); !row.isEmpty() && aggregate(it.value()) == QLatin1String("running"))
            list << row;
    return list;
}

// Only when the set changes: the dots' Repeater is remade from it.
void CanvasRuns::updateRunning() {
    const QStringList running = runningNodes();
    if (running == m_running)
        return;
    m_running = running;
    emit runningNodesChanged();
}

QVariantList CanvasRuns::runningWires() const {
    const QStringList running = runningNodes();
    QVariantList wires;
    if (running.isEmpty())
        return wires;
    const LinkList *links = m_canvas->links();
    for (const int row : m_canvas->linkRows(running)) {
        const QModelIndex index = links->index(row);
        if (running.contains(links->data(index, LinkList::ToNode).toString()))
            // The row, not the path: the dot reads the wire's live path, so it follows a node that moves or grows
            // under a run (a thumbnail does) and never travels along where the wire used to be.
            wires << QVariantMap{{"row", row}, {"path", links->data(index, LinkList::Path)}, {"color", links->data(index, LinkList::Color)}};
    }
    return wires;
}

void CanvasRuns::showError(const QString &node) {
    if (node.isEmpty())
        return;
    m_canvas->select({node});
    // Its header in the middle of the view, at the zoom the view has.
    const QPointF at = m_canvas->position(node) + QPointF(30 * m_canvas->unit(), 10 * m_canvas->unit());
    m_canvas->moveView(at.x(), at.y(), m_canvas->view().value("zoom", 1).toReal());
}

// What each node of the shown workflow is, for its slot: a node made to show a result
// (Preview, Save Image) and the image file a file input names (Load Image).
void CanvasRuns::readNodes() {
    m_nodeInfo.clear();
    if (!m_registry)
        return;
    // The level shown: its cards are the ones with a slot.
    const Document document = m_canvas->scopeDocument();
    const Document whole = m_canvas->document(); // the groups, for the face of an instance
    for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it) {
        NodeInfo info;
        info.savesFile = it->type == QLatin1String("fugue/save-image") || it->type == QLatin1String("fugue/save-video")
                         || it->type == QLatin1String("fugue/save-audio");
        info.compares = it->type == QLatin1String("fugue/compare-images");
        info.crops = it->type == QLatin1String("fugue/crop");
        if (info.crops) {
            const InputValue image = it->inputs.value(QStringLiteral("image"));
            if (image.kind() == InputValue::Kind::Link)
                info.cropFrom = keyFor(image.from().node);
        }
        info.showsResult = it->type == QLatin1String("fugue/preview") || info.savesFile || info.compares;
        info.picture = m_canvas->showsPicture(it.key());
        info.hideOverride = m_canvas->pictureOverride(it.key()) == PictureOverride::Hide;
        if (info.compares) {
            // What made A and B names them: the linked node's title, else the plain letter.
            const auto labelOf = [&](const char *name, const QString &fallback) {
                const InputValue value = it->inputs.value(QLatin1String(name));
                if (value.kind() != InputValue::Kind::Link)
                    return fallback;
                const QString title = m_canvas->nodeTitle(value.from().node);
                return title.isEmpty() ? fallback : title;
            };
            info.labelA = labelOf("a", QStringLiteral("A"));
            info.labelB = labelOf("b", QStringLiteral("B"));
        }
        // A group instance is typed by its face (its exposed ports), so a group that exposes a Load Image's
        // file shows that picture as a Load Image does, and its first image output shows its result.
        std::optional<NodeType> face;
        const NodeType *type = m_registry->nodeTypeFor(it->type, it->version.major);
        if (const GroupDefinition *group = type ? nullptr : whole.group(it->type)) {
            face = groupFace(whole, *m_registry, it->type);
            type = &*face;
            for (const GroupOutput &output : group->outputs) {
                const OutputSpec *spec = face->output(output.name);
                if (spec && spec->type.type.id == QLatin1String("image") && !spec->type.list) {
                    info.resultFrom = keyFor(it.key()) + QLatin1Char('/') + output.from.node;
                    break;
                }
            }
        }
        if (type) {
            for (const InputSpec &input : type->inputs) {
                const InputValue value = it->inputs.value(input.name);
                // A mask painted over an image input: the slot shows that input's picture (Paint Mask).
                if (input.widget() == QLatin1String("mask") && !input.over.isEmpty() && info.paintsOver.isEmpty()) {
                    info.paintsOver = input.over;
                    const InputValue over = it->inputs.value(input.over);
                    if (over.kind() == InputValue::Kind::Link)
                        info.cropFrom = keyFor(over.from().node);
                }
                if (input.widget() == QLatin1String("file") && value.kind() == InputValue::Kind::Literal
                    && value.literal().isString() && isImagePath(value.literal().toString())) {
                    // As the run will read it: a relative path is the workflow folder's.
                    const QString text = value.literal().toString();
                    const Result<QString> located = inputFilePath(text, m_workflows->pathOf(m_workflows->current()), input.name);
                    info.imageFile = located ? located.value() : text;
                    break;
                }
            }
        }
        if (info.showsResult || info.followsInput() || !info.imageFile.isEmpty() || !info.resultFrom.isEmpty() || !info.picture)
            m_nodeInfo.insert(keyFor(it.key()), info);
    }
    TabRuns &runs = m_tabs[m_shownTab];
    for (auto it = m_nodeInfo.cbegin(); it != m_nodeInfo.cend(); ++it)
        if ((it->showsResult || it->followsInput()) && !runs.nodes.contains(it.key()))
            runs.nodes.insert(it.key(), NodeView{});
}

QSet<QString> CanvasRuns::noPictures() const {
    QSet<QString> hidden, followed;
    for (auto it = m_nodeInfo.cbegin(); it != m_nodeInfo.cend(); ++it) {
        if (!it->picture) {
            hidden.insert(it.key());
            if (!it->resultFrom.isEmpty())
                hidden.insert(it->resultFrom); // a group's picture is its inside's
        }
        else if (it->followsInput() && !it->cropFrom.isEmpty())
            followed.insert(it->cropFrom); // the picture a shown Crop or Paint Mask draws over
    }
    return hidden - followed;
}

// After an edit (a paste, a drop, a pick, an undo): a Load Image whose file changed shows the
// new one; its last result goes, being of the old file.
void CanvasRuns::refreshInputs() {
    const QHash<QString, NodeInfo> before = m_nodeInfo;
    readNodes();
    TabRuns &runs = m_tabs[m_shownTab];
    QSet<QString> nodes;
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        nodes.insert(it.key());
    for (auto it = m_nodeInfo.cbegin(); it != m_nodeInfo.cend(); ++it)
        nodes.insert(it.key());
    for (const QString &node : std::as_const(nodes)) {
        const QString was = before.value(node).imageFile, now = m_nodeInfo.value(node).imageFile;
        if (was == now) {
            // A new Preview or Save Image: its placeholder. A Crop wired to another node: that picture.
            // A picture shown or hidden (the mode, the node's override): its slot comes or goes (#263).
            const bool pictureChanged = before.value(node).picture != m_nodeInfo.value(node).picture;
            if (!before.contains(node) || before.value(node).cropFrom != m_nodeInfo.value(node).cropFrom || pictureChanged)
                show(m_shownTab, node);
            if (pictureChanged)
                requestInputPreview(node);
            continue;
        }
        if (runs.nodes.contains(node)) {
            NodeView &view = runs.nodes[node];
            view.thumbnail.clear();
            view.imagePath.clear();
            view.text.clear();
            view.media.clear();
            view.inputThumbnail.clear();
            view.inputMissing = false;
        }
        requestInputPreview(node);
        show(m_shownTab, node);
    }
}

void CanvasRuns::requestInputPreview(const QString &node) {
    const QString file = m_nodeInfo.value(node).imageFile;
    // A hidden picture is not made (#263).
    if (file.isEmpty() || !m_nodeInfo.value(node).picture || !m_rowsMade.contains(rowFor(node)))
        return;
    m_service->previewFile(m_shownTab + '\n' + node, file);
}

void CanvasRuns::onFilePreviewed(const QString &token, const QString &path, const QJsonObject &thumbnail) {
    const QString tab = token.section('\n', 0, 0), node = token.section('\n', 1);
    if (tab != m_shownTab || m_nodeInfo.value(node).imageFile != path)
        return; // the file changed since
    NodeView &view = m_tabs[tab].nodes[node];
    view.inputMissing = thumbnail.value("missing").toBool();
    view.inputThumbnail = thumbnail.value("url").toString();
    view.inputWidth = thumbnail.value("width").toInt();
    view.inputHeight = thumbnail.value("height").toInt();
    show(tab, node);
    emit inputPreviewed(rowFor(node));
}

QString CanvasRuns::imageFile(const QString &row) const {
    const QString node = keyFor(row);
    const NodeView view = m_tabs.value(m_shownTab).nodes.value(node);
    QString path;
    if (m_nodeInfo.value(node).followsInput())
        path = cropSource(m_shownTab, node).path;
    else if (!view.thumbnail.isEmpty() && !view.imagePath.isEmpty())
        path = view.imagePath;
    else if (view.text.isEmpty())
        path = expandHome(m_nodeInfo.value(node).imageFile);
    return QDir::isAbsolutePath(path) && QFileInfo(path).isFile() ? path : QString();
}

QVariantMap CanvasRuns::inputPicture(const QString &row, const QString &input) const {
    const QString from = m_canvas->linkedFrom(row, input);
    if (from.isEmpty())
        return {};
    const Picture picture = pictureOf(m_shownTab, keyFor(from));
    const QString path = QDir::isAbsolutePath(picture.path) && QFileInfo(picture.path).isFile() ? picture.path : QString();
    if (path.isEmpty() || picture.width <= 0 || picture.height <= 0)
        return {};
    return {{"file", path}, {"width", picture.width}, {"height", picture.height}};
}

QString CanvasRuns::inputFile(const QString &literal) const {
    if (literal.trimmed().isEmpty())
        return {};
    const Result<QString> located = inputFilePath(literal, m_workflows->pathOf(m_workflows->current()), QStringLiteral("mask"));
    return located && QFileInfo(located.value()).isFile() ? located.value() : QString();
}

QUrl CanvasRuns::imageUrl(const QString &row) const {
    const QString path = imageFile(row);
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

QString CanvasRuns::compareFile(const QString &row, const QString &which) const {
    const QString path = m_tabs.value(m_shownTab).nodes.value(keyFor(row)).compare.value(which).toMap().value("source").toString();
    return QDir::isAbsolutePath(path) && QFileInfo(path).isFile() ? path : QString();
}

QString CanvasRuns::lastPicture() const {
    const auto nodes = m_tabs.value(m_shownTab).nodes;
    // The newest picture a Save Image or a Preview shows, by its file's time.
    QString newest;
    QDateTime when;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        const QFileInfo file(it->imagePath);
        if (!m_nodeInfo.value(it.key()).showsResult || it->thumbnail.isEmpty() || !file.isFile())
            continue;
        if (newest.isEmpty() || file.lastModified() > when) {
            newest = it->imagePath;
            when = file.lastModified();
        }
    }
    return newest;
}

QVariantList CanvasRuns::errors() const {
    const TabRuns runs = m_tabs.value(m_shownTab);
    QVariantList list = runs.errors;
    // A run that failed on a node has said what is wrong there: the run's error wins over a check's, whatever the codes (#607),
    // as on the node itself.
    for (const QVariant &found : runs.checkErrors) {
        const QVariantMap check = found.toMap();
        const bool listed = std::any_of(list.cbegin(), list.cend(), [&](const QVariant &e) {
            return e.toMap().value("node") == check.value("node");
        });
        if (!listed)
            list << found;
    }
    return list;
}

QString CanvasRuns::lastStatus() const { return m_tabs.value(m_shownTab).lastStatus; }

QString CanvasRuns::runsOn() const {
    QStringList services;
    const TabRuns runs = m_tabs.value(m_shownTab);
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it)
        if (!it->runsOn.isEmpty() && !services.contains(it->runsOn))
            services << it->runsOn;
    services.sort();
    return services.join(QStringLiteral(", "));
}

void CanvasRuns::noteRunsOn() {
    const QString now = runsOn();
    if (now == m_runsOn)
        return;
    m_runsOn = now;
    emit runsOnChanged();
}

QVariantMap CanvasRuns::remoteQuestion() const {
    if (m_remoteAsks.isEmpty())
        return {};
    return {{"run", m_remoteAsks.first().run}, {"services", m_remoteAsks.first().services.toVariantList()}};
}

void CanvasRuns::answerRemote(bool allow) {
    if (m_remoteAsks.isEmpty())
        return;
    const RemoteAsk ask = m_remoteAsks.takeFirst();
    m_answeredRevision.insert(ask.run, ask.revision);
    emit remoteQuestionChanged();
    m_service->answerRemote(ask.run, allow, ask.revision);
}

} // namespace fugue
