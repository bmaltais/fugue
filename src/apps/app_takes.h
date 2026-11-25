#pragma once

// The takes of an app that outlive the window (#389, epic #397): the run history of one workflow file, read back from the run
// folders (spec 9.7) the History panel and `fugue runs` read. The app owns no outputs and no store of its own; a take is a run.
//
// What makes a run a take: its workflow.json (the bound copy the App view started it from) carries the `app_take` record,
// `{"app": <the app's key>, "values": {key: value}, "modes": {key: mode}}`. The key is the app's workflow file (the saved file,
// else the template's own workflow.fugue for an app opened from the catalog, which has no file of its own: History's runs of a
// template tab carry no workflow path), so takes follow the app across restarts, per file, over all projects. The record is the
// form as the run bound
// it, with the seed a Random seed used as a plain number in a "fixed" mode, so running those values again makes the same
// picture. The record sits in the document's `extra` (spec 4.1), outside the source hash, so it changes no cache key and a
// run of the graph never has one. Runs are kept as long as History keeps them (run folders go 7 days after the run ends, 10.5).
//
// Pure file reading, no Qt objects: tested alone (tests/apps/test_app_view.cpp).

#include <QJsonObject>
#include <QList>
#include <QString>

namespace fugue::app {

inline constexpr const char *takeKey = "app_take";

struct StoredTake {
    QString run;
    QString status; // "ok" or "failed"
    QJsonObject values;
    QJsonObject modes;
};

// The record a run's bound document carries.
QJsonObject takeRecord(const QString &app, const QJsonObject &values, const QJsonObject &modes);

// The ended runs of the app `key` (an absolute workflow path) that were started from the App view, newest first, at most `max`. Only the
// newest `scan` run folders are looked at, so a long history costs a bounded read.
QList<StoredTake> storedTakes(const QString &runsRoot, const QString &key, int max, int scan = 400);

} // namespace fugue::app
