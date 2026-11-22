#include "workflow/tab_session.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "engine/atomic_file.h"
#include "engine/json.h"

namespace fugue {

Errors saveTabSession(const QString &path, const TabSession &session) {
    const QJsonObject object{{"format", 1},
                             {"tabs", QJsonArray::fromStringList(session.paths)},
                             {"active", session.active}};
    return replaceFile(path, QJsonDocument(object).toJson(QJsonDocument::Indented), QStringLiteral("the open tabs"));
}

TabSession loadTabSession(const QString &path) {
    const Result<QJsonObject> read = readJsonObject(path);
    if (!read || read.value().value("format").toInt() != 1)
        return {};
    TabSession session;
    for (const QJsonValue &value : read.value().value("tabs").toArray())
        if (value.isString() && !value.toString().isEmpty())
            session.paths << value.toString();
    const QJsonValue active = read.value().value("active");
    if (active.isString())
        session.active = active.toString();
    return session;
}

TabRestore planRestore(const TabSession &session) {
    TabRestore plan;
    for (const QString &path : session.paths) {
        if (plan.paths.contains(path) || plan.skipped.contains(path))
            continue;
        if (QFileInfo(path).isAbsolute() && QFileInfo(path).isFile())
            plan.paths << path;
        else
            plan.skipped << path;
    }
    plan.active = plan.paths.contains(session.active) ? session.active : (plan.paths.isEmpty() ? QString() : plan.paths.last());
    return plan;
}

} // namespace fugue
