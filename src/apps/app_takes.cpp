#include "apps/app_takes.h"

#include <QDir>

#include "engine/run_folder.h"

namespace fugue::app {

QJsonObject takeRecord(const QString &app, const QJsonObject &values, const QJsonObject &modes) {
    return {{"app", app}, {"values", values}, {"modes", modes}};
}

QList<StoredTake> storedTakes(const QString &runsRoot, const QString &key, int max, int scan) {
    QList<StoredTake> found;
    if (runsRoot.isEmpty() || key.isEmpty())
        return found;
    // Run ids sort by start time, so the newest folders are the last names.
    const QStringList names = QDir(runsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    int looked = 0;
    for (const QString &name : names) {
        if (found.size() >= max || ++looked > scan)
            break;
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
