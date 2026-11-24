#include "engine/canvas_act_outcome.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <utility>

namespace fugue::canvas {

Error failureOf(const Errors &errors, const QString &actionName) {
    const auto hard = std::find_if(errors.cbegin(), errors.cend(), [](const Error &e) { return e.severity == Error::Severity::Error; });
    const Error first = hard != errors.cend()
                            ? *hard
                            : makeError(QStringLiteral("node_failed"), QCoreApplication::translate("CanvasActions", "%1 did not finish.").arg(actionName));
    const QString words = plainWords(first, actionName);
    return makeError(first.code, words, first.message != words ? first.message : QString());
}

ActOutcome::ActOutcome(const ActionGraph &graph, const ActPlan &plan, const QString &actionName)
    : m_writeNodes(graph.writes), m_newNodes(graph.news), m_actionName(actionName) {
    m_growth.canvas = plan.canvas;
    m_growth.grew = plan.grew;
    m_growth.growRevision = plan.growRevision;
}

QString ActOutcome::note(const QString &run, const QJsonObject &event) {
    const QString kind = event.value("event").toString();
    if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
        const QString node = event.value("node").toString();
        const bool cached = kind == QLatin1String("node.cached");
        const QJsonObject meta = event.value("outputs").toObject().value("canvas").toObject().value("meta").toObject();
        const QString canvas = meta.value("canvas").toString();
        if (!cached && m_newNodes.contains(node) && !canvas.isEmpty())
            m_made[run] << canvas;
        if (m_writeNodes.contains(node)) {
            const QString revision = meta.value("revision").toString();
            m_writes[run] << Written{cached, canvas, revision};
            return revision;
        }
    } else if (kind == QLatin1String("node.error")) {
        m_errors[run] << Error::fromJson(event.value("error").toObject());
    } else if (kind == QLatin1String("run.done")) {
        for (const QJsonValue &value : event.value("errors").toArray())
            m_errors[run] << Error::fromJson(value.toObject());
    }
    return {};
}

ActOutcome::RunEnd ActOutcome::endRun(const QString &run, const QString &status, const Errors &errors) {
    const QList<Written> writes = m_writes.take(run);
    const QStringList made = m_made.take(run);
    const Errors seen = m_errors.take(run) + errors;
    RunEnd end;
    if (status != QLatin1String("ok")) {
        if (m_firstNotOk.isEmpty())
            m_firstNotOk = status;
        if (status != QLatin1String("cancelled"))
            end.error = failureOf(seen, m_actionName);
        return end;
    }
    end.made = made;
    for (const Written &write : writes) {
        if (!write.cached || write.revision.isEmpty())
            continue;
        const Result<PlacedAgain> placed = placeWriteAgain(write.canvas, write.revision);
        if (!placed)
            end.error = failureOf(placed.errors(), m_actionName);
        else if (!placed->moved && !placed->layer.isEmpty())
            end.notice = QCoreApplication::translate("CanvasActions", "Same prompt, seed and selection as “%1”. Change the seed to get another.").arg(placed->layer);
    }
    return end;
}

QString ActOutcome::finish() {
    const QString ended = std::exchange(m_firstNotOk, QString());
    const ActPlan growth = std::exchange(m_growth, ActPlan{});
    if (ended.isEmpty() || !takeBackGrowth(growth))
        return {};
    return growthTakenBack(m_actionName, ended == QLatin1String("cancelled"));
}

} // namespace fugue::canvas
