#include "run/canvas_runs.h"

#include <QJsonArray>
#include <QSizeF>

#include <cmath>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

QString topOf(const QString &path) { return path.section('/', 0, 0); }

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

CanvasRuns::CanvasRuns(CanvasModel *canvas, Workflows *workflows, RunService *service, QObject *parent)
    : QObject(parent), m_canvas(canvas), m_workflows(workflows), m_service(service) {
    connect(m_service, &RunService::event, this, &CanvasRuns::onEvent);
    connect(m_service, &RunService::checked, this, &CanvasRuns::onChecked);
    m_checkTimer.setSingleShot(true);
    m_checkTimer.setInterval(300);
    connect(&m_checkTimer, &QTimer::timeout, this, &CanvasRuns::checkNow);
    m_progressTimer.setSingleShot(true);
    m_progressTimer.setInterval(33);
    connect(&m_progressTimer, &QTimer::timeout, this, &CanvasRuns::flushProgress);
    connect(m_canvas, &CanvasModel::edited, this, [this] { scheduleCheck(true); });
    connect(m_canvas, &CanvasModel::documentReset, this, &CanvasRuns::showTab);
    connect(m_workflows, &Workflows::currentChanged, this, &CanvasRuns::showTab);
    showTab();
}

CanvasRuns::~CanvasRuns() = default;

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
    return selected.size() == 1 ? startRun(selected.first(), false) : QString();
}

QString CanvasRuns::startRun(const QString &target, bool first) {
    const QString tab = currentTab();
    if (tab.isEmpty())
        return {};
    RunRequest request;
    request.document = m_canvas->document();
    request.workflow = m_workflows->pathOf(m_workflows->current());
    request.target = target;
    request.first = first;
    const QString id = m_service->start(request);
    // The run has the numbers shown; the next one gets the next (spec 6.2).
    m_canvas->widgets()->advanceSeeds();
    m_tabOf.insert(id, tab);
    if (first)
        m_order.insert(m_active.isEmpty() ? 0 : 1, id);
    else
        m_order.append(id);
    TabRuns &runs = m_tabs[tab];
    ++runs.busy;
    ++runs.epoch;
    // A new run starts without the last one's errors.
    runs.errors.clear();
    for (auto it = runs.nodes.begin(); it != runs.nodes.end(); ++it) {
        if (!it->error.isEmpty() || it->state == QLatin1String("error")) {
            it->error.clear();
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

QString CanvasRuns::nodeTitle(const QString &tab, const QString &path) const {
    const QString title = tab == m_shownTab ? m_canvas->nodeTitle(topOf(path)) : QString();
    if (title.isEmpty())
        return path;
    return path.contains('/') ? title + QStringLiteral(" › ") + path.section('/', 1) : title;
}

void CanvasRuns::show(const QString &tab, const QString &node) {
    if (tab != m_shownTab)
        return;
    const NodeView view = m_tabs.value(tab).nodes.value(node);
    QVariantMap state{{"state", aggregate(view)}, {"progress", view.progress}, {"seconds", view.seconds},
                      {"error", view.error},      {"warnings", view.warnings}, {"values", view.values}};
    if (!view.thumbnail.isEmpty() && view.thumbnailWidth > 0) {
        state.insert("thumbnail", view.thumbnail);
        state.insert("thumbnailAspect", qreal(view.thumbnailHeight) / view.thumbnailWidth);
        state.insert("thumbnailSize", QSizeF(view.thumbnailWidth, view.thumbnailHeight));
        state.insert("thumbnailInfo", QStringLiteral("%1 × %2").arg(view.thumbnailWidth).arg(view.thumbnailHeight));
    }
    m_pendingProgress.remove(node);
    m_canvas->setRunState(node, state);
}

void CanvasRuns::showTab() {
    m_shownTab = currentTab();
    m_pendingProgress.clear();
    m_canvas->clearRunState();
    const TabRuns runs = m_tabs.value(m_shownTab);
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it)
        show(m_shownTab, it.key());
    emit runningNodesChanged();
    emit errorsChanged();
    scheduleCheck(false);
}

void CanvasRuns::flushProgress() {
    for (auto it = m_pendingProgress.cbegin(); it != m_pendingProgress.cend(); ++it)
        m_canvas->setRunProgress(it.key(), it.value());
    m_pendingProgress.clear();
    emit progressChanged();
}

void CanvasRuns::onEvent(const QString &run, const QJsonObject &event) {
    const QString tab = m_tabOf.value(run);
    TabRuns &runs = m_tabs[tab];
    const QString kind = event.value("event").toString();
    const QString path = event.value("node").toString();
    const QString top = topOf(path);
    const bool inner = path.contains('/');

    if (kind == QLatin1String("run.started")) {
        m_active = run;
        emit changed();
        return;
    }
    if (kind == QLatin1String("run.done")) {
        const QString status = event.value("status").toString();
        for (const QJsonValue &value : event.value("errors").toArray()) {
            const Error error = Error::fromJson(value.toObject());
            const QString shown = topOf(error.node);
            if (error.severity == Error::Severity::Warning) {
                if (!shown.isEmpty() && !runs.nodes[shown].warnings.contains(error.message)) {
                    runs.nodes[shown].warnings << error.message;
                    show(tab, shown);
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
            if (!shown.isEmpty()) {
                NodeView &view = runs.nodes[shown];
                view.state = QStringLiteral("error");
                view.error = error.node.contains('/') ? error.node + QStringLiteral(": ") + error.message : error.message;
                show(tab, shown);
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
            emit runningNodesChanged();
        }
        emit finished(run, status);
        return;
    }
    if (path.isEmpty())
        return;

    NodeView &view = runs.nodes[top];
    auto setState = [&](const QString &state) {
        if (inner)
            view.inner.insert(path, state);
        else
            view.state = state;
    };
    if (kind == QLatin1String("node.running")) {
        setState(QStringLiteral("running"));
        view.progress = -1;
        m_activeNodePath = path;
        m_activeNode = nodeTitle(tab, path);
        m_activeProgress = -1;
        emit progressChanged();
        if (tab == m_shownTab)
            emit runningNodesChanged();
    } else if (kind == QLatin1String("node.progress")) {
        view.progress = event.value("value").toDouble();
        if (path == m_activeNodePath)
            m_activeProgress = view.progress;
        if (tab == m_shownTab)
            m_pendingProgress.insert(top, view.progress);
        if (!m_progressTimer.isActive())
            m_progressTimer.start();
        return; // the canvas gets it with the next flush
    } else if (kind == QLatin1String("node.preview")) {
        const QJsonObject thumbnail = event.value("thumbnails").toObject().value("preview").toObject();
        if (thumbnail.isEmpty())
            return;
        view.thumbnail = thumbnail.value("url").toString();
        view.thumbnailWidth = thumbnail.value("width").toInt();
        view.thumbnailHeight = thumbnail.value("height").toInt();
    } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
        setState(QStringLiteral("cached"));
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
        if (!thumbnails.isEmpty()) {
            const QJsonObject thumbnail = thumbnails.contains("image") ? thumbnails.value("image").toObject()
                                                                       : thumbnails.begin().value().toObject();
            view.thumbnail = thumbnail.value("url").toString();
            view.thumbnailWidth = thumbnail.value("width").toInt();
            view.thumbnailHeight = thumbnail.value("height").toInt();
        }
    } else if (kind == QLatin1String("node.skipped")) {
        // Cancelled before it started: it would still run.
        setState(event.value("reason") == QLatin1String("cancelled") ? QStringLiteral("stale") : QStringLiteral("skipped"));
        view.progress = -1;
    } else if (kind == QLatin1String("node.error")) {
        const Error error = Error::fromJson(event.value("error").toObject());
        setState(QStringLiteral("error"));
        view.progress = -1;
        view.error = inner ? path + QStringLiteral(": ") + error.message : error.message;
        runs.errors << QVariantMap{{"node", path}, {"shown", top}, {"message", error.message},
                                   {"code", error.code}, {"hint", error.hint}};
        if (tab == m_shownTab)
            emit errorsChanged();
    } else {
        return;
    }
    if (kind != QLatin1String("node.running") && tab == m_shownTab)
        emit runningNodesChanged();
    show(tab, top);
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
    m_ticketTab.insert(ticket, {tab, m_tabs[tab].epoch, m_checkFromEdit});
    m_checkFromEdit = false;
    RunRequest request;
    request.document = m_canvas->document();
    request.workflow = m_workflows->pathOf(m_workflows->current());
    m_service->check(request, ticket);
}

void CanvasRuns::onChecked(int ticket, const QJsonObject &nodes, const QJsonArray &problems) {
    const Ticket asked = m_ticketTab.take(ticket);
    if (ticket != m_ticket || asked.tab.isEmpty())
        return;
    TabRuns &runs = m_tabs[asked.tab];
    // A run since the check was asked knows better.
    if (runs.busy > 0 || runs.epoch != asked.epoch)
        return;
    QHash<QString, NodeView> next;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const QString top = topOf(it.key());
        if (!next.contains(top)) {
            NodeView view = runs.nodes.value(top);
            view.inner.clear();
            view.warnings.clear();
            // An error stays until the workflow is edited.
            if (asked.edit || view.state != QLatin1String("error")) {
                view.error.clear();
                view.state.clear();
            }
            next.insert(top, view);
        }
        NodeView &view = next[top];
        const QString state = it.value().toObject().value("state").toString();
        if (it.key().contains('/'))
            view.inner.insert(it.key(), state);
        else if (view.state != QLatin1String("error"))
            view.state = state;
    }
    for (const QJsonValue &value : problems) {
        const Error error = Error::fromJson(value.toObject());
        const QString top = topOf(error.node);
        if (error.severity == Error::Severity::Warning && next.contains(top))
            next[top].warnings << error.message;
    }
    // Nodes the check has no state for (not part of any output) keep their last values only.
    for (auto it = runs.nodes.cbegin(); it != runs.nodes.cend(); ++it) {
        if (next.contains(it.key()))
            continue;
        NodeView view = it.value();
        view.inner.clear();
        view.warnings.clear();
        if (asked.edit || view.state != QLatin1String("error")) {
            view.state.clear();
            view.error.clear();
        }
        next.insert(it.key(), view);
    }
    if (asked.edit)
        runs.errors.clear();
    runs.nodes = next;
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
        if (aggregate(it.value()) == QLatin1String("running"))
            list << it.key();
    return list;
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
            wires << QVariantMap{{"path", links->data(index, LinkList::Path)}, {"color", links->data(index, LinkList::Color)}};
    }
    return wires;
}

void CanvasRuns::showError(const QString &node) {
    if (node.isEmpty())
        return;
    m_canvas->select({node});
    // Its header in the middle of the view, at the zoom the view has.
    const QPointF at = m_canvas->position(node) + QPointF(30 * m_canvas->unit(), 10 * m_canvas->unit());
    m_canvas->setView(at.x(), at.y(), m_canvas->view().value("zoom", 1).toReal());
}

QVariantList CanvasRuns::errors() const { return m_tabs.value(m_shownTab).errors; }

QString CanvasRuns::lastStatus() const { return m_tabs.value(m_shownTab).lastStatus; }

} // namespace fugue
