#pragma once

// What one Generate of a canvas action came to (#231, #337): the end of what canvas_act.h starts, shared by the window's task bar
// (canvas/canvas_actions.h) and `fugue canvas act --wait` (cli/canvas_act.cpp), which only show the answer. Feed it the events of the
// Generate's runs, end each run, then finish the Generate:
//   - note() reads a run's Canvas Write and Canvas New results and its errors from the events;
//   - endRun() settles one run. An ok run reports the canvases it made and has a fully cached Write placed on the head again (design
//     decision 5). A failed or cancelled run lands nothing of its own;
//   - finish() takes an Expand's growth back when a run did not end ok and the canvas is still as the growth left it.
// Call endRun() as each run ends, so a variation that landed keeps the growth it was written on. No queue and no Qt GUI.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/canvas_act.h"
#include "engine/canvas_action.h"
#include "engine/error.h"

namespace fugue::canvas {

// The error a failed run is reported by: its first hard error, else "<action> did not finish." `message` is in plain words (design 4);
// `hint` holds the original sentence when the plain words replaced it.
Error failureOf(const Errors &errors, const QString &actionName);

class ActOutcome {
public:
    struct RunEnd {
        QStringList made;           // the canvases an ok run's Canvas New nodes made
        QString notice;             // a cached Write that is still on the head: why nothing new came
        std::optional<Error> error; // the run failed, or its cached Write could not be placed (see failureOf)
    };

    ActOutcome() = default;
    ActOutcome(const ActionGraph &graph, const ActPlan &plan, const QString &actionName);

    // One event of `run`, in order. The revision a Canvas Write of this event reported, else empty.
    QString note(const QString &run, const QJsonObject &event);
    // `run` ended with `status` ("ok", "failed" or "cancelled"). `errors`: any the events did not carry.
    RunEnd endRun(const QString &run, const QString &status, const Errors &errors = {});
    // Every run has ended (or was cancelled): the line saying the growth was taken back, else empty.
    QString finish();

private:
    struct Written {
        bool cached = false;
        QString canvas, revision;
    };

    QStringList m_writeNodes, m_newNodes;
    QString m_actionName;
    ActPlan m_growth; // only what takeBackGrowth reads
    QString m_firstNotOk;
    QHash<QString, QList<Written>> m_writes;
    QHash<QString, QStringList> m_made;
    QHash<QString, Errors> m_errors;
};

} // namespace fugue::canvas
