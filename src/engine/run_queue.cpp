#include "run_queue.h"

#include <QDateTime>
#include <QJsonArray>

namespace fugue {

RunQueue::RunQueue(const RunEnvironment &environment, QObject *parent) : QObject(parent), m_environment(environment) {
    qRegisterMetaType<fugue::RunOutcome>();
}

RunQueue::~RunQueue() = default;

void RunQueue::record(const RunFolder &folder, const QJsonObject &event) {
    folder.appendEvent(event);
    emit runEvent(folder.id(), event);
}

QString RunQueue::enqueue(const CompiledGraph &graph, const RunFolder &folder, const RunOptions &options, bool first) {
    folder.writeState({{"status", "queued"}});
    record(folder, {{"event", "run.queued"}});
    if (first)
        m_waiting.prepend(Waiting{graph, folder, options});
    else
        m_waiting.append(Waiting{graph, folder, options});
    if (!m_active)
        startNext();
    return folder.id();
}

bool RunQueue::moveToFront(const QString &id) {
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).folder.id() == id) {
            m_waiting.move(i, 0);
            return true;
        }
    }
    return false;
}

bool RunQueue::cancel(const QString &id) {
    if (m_active && m_active->id() == id) {
        m_active->cancel();
        return true;
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).folder.id() != id)
            continue;
        const Waiting removed = m_waiting.takeAt(i);
        const QDateTime now =
            m_environment.clock ? m_environment.clock().toUTC() : QDateTime::currentDateTimeUtc();
        removed.folder.writeState({{"status", "cancelled"}, {"ended", now.toString(Qt::ISODateWithMs)}});
        record(removed.folder, {{"event", "run.done"}, {"status", "cancelled"}, {"errors", QJsonArray()}});
        RunOutcome outcome;
        outcome.status = QStringLiteral("cancelled");
        emit runFinished(id, outcome);
        return true;
    }
    return false;
}

QString RunQueue::active() const { return m_active ? m_active->id() : QString(); }

QStringList RunQueue::waiting() const {
    QStringList ids;
    for (const Waiting &waiting : m_waiting)
        ids.append(waiting.folder.id());
    return ids;
}

void RunQueue::startNext() {
    if (m_active || m_waiting.isEmpty())
        return;
    const Waiting next = m_waiting.takeFirst();
    const QString id = next.folder.id();
    m_active = std::make_unique<Run>(next.graph, next.folder, m_environment, next.options);
    connect(m_active.get(), &Run::emitted, this, [this, id](const QJsonObject &event) { emit runEvent(id, event); });
    // Queued: the finished run is let go only after it has returned from
    // emitting, and the next one starts from the event loop.
    connect(
        m_active.get(), &Run::finished, this,
        [this, id](const RunOutcome &outcome) {
            m_active.reset();
            emit runFinished(id, outcome);
            startNext();
        },
        Qt::QueuedConnection);
    emit runStarted(id);
    m_active->start();
}

} // namespace fugue
