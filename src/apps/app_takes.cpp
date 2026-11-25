#include "apps/app_takes.h"

#include <QDir>
#include <QFileInfo>

#include "engine/json.h"
#include "engine/run_folder.h"

namespace fugue::app {

QJsonObject takeRecord(const QString &app, const QJsonObject &values, const QJsonObject &modes) {
    return {{"app", app}, {"values", values}, {"modes", modes}};
}

namespace {
QString clearedFile(const QString &runsRoot) { return QFileInfo(runsRoot).absolutePath() + QStringLiteral("/app-takes.json"); }
} // namespace

QString clearedThrough(const QString &runsRoot, const QString &key) {
    if (runsRoot.isEmpty() || key.isEmpty())
        return {};
    const Result<QJsonObject> read = readJsonObject(clearedFile(runsRoot));
    return read ? read->value("cleared").toObject().value(key).toString() : QString();
}

void setClearedThrough(const QString &runsRoot, const QString &key, const QString &run) {
    if (runsRoot.isEmpty() || key.isEmpty() || run.isEmpty())
        return;
    const Result<QJsonObject> read = readJsonObject(clearedFile(runsRoot));
    QJsonObject all = read ? read.value() : QJsonObject();
    QJsonObject cleared = all.value("cleared").toObject();
    if (cleared.value(key).toString() >= run)
        return; // an older clear never undoes a newer one
    cleared.insert(key, run);
    all.insert("cleared", cleared);
    writeJsonFile(clearedFile(runsRoot), all);
}

QList<StoredTake> storedTakes(const QString &runsRoot, const QString &key, int max, int scan, const QString &clearedThrough) {
    QList<StoredTake> found;
    if (runsRoot.isEmpty() || key.isEmpty())
        return found;
    // Run ids sort by start time, so the newest folders are the last names.
    const QStringList names = QDir(runsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    int looked = 0;
    for (const QString &name : names) {
        if (found.size() >= max || ++looked > scan || (!clearedThrough.isEmpty() && name <= clearedThrough))
            break; // newest first: every name after a cleared one is older still
        const Result<RunFolder> folder = RunFolder::open(runsRoot, name);
        if (!folder)
            continue;
        const RunStatus status = folder->status();
        if (status != RunStatus::Ok && status != RunStatus::Failed)
            continue; // going or waiting is the session's; a cancelled run made nothing to look at
        const QJsonObject record = folder->workflow().value(takeKey).toObject();
        if (record.value("app").toString() != key)
            continue;
        found.append({name, runStatusName(status), record.value("values").toObject(), record.value("modes").toObject()});
    }
    return found;
}

} // namespace fugue::app
