// `fugue settings list` (spec 13.1, 1.4.2): every setting in effect in this session and where its value
// comes from: the session's own settings.json, the Default project's (inherited), or the built-in default.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "cli/commands.h"
#include "engine/project.h"
#include "engine/settings.h"

namespace fugue::cli {

namespace {

QString shown(const QJsonValue &value) {
    if (value.isUndefined() || value.isNull())
        return QStringLiteral("(not set)");
    if (value.isString())
        return value.toString();
    if (value.isArray()) {
        QStringList items;
        for (const QJsonValue &item : value.toArray())
            items << item.toString();
        return items.isEmpty() ? QStringLiteral("[]") : items.join(QStringLiteral(", "));
    }
    return value.isObject() ? QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                            : QVariant(value.toVariant()).toString();
}

} // namespace

Reply settingsList(const Command &, Context &) {
    const EffectiveSettings effective = effectiveSettings();
    QJsonArray listed;
    QStringList lines;
    for (const QString &key : settingKeys()) {
        const QJsonValue value = settingValue(effective.settings, key);
        const SettingOrigin origin = effective.origins.value(key);
        QJsonObject entry{{"key", key}, {"origin", settingOriginName(origin)}};
        entry.insert("value", value.isUndefined() ? QJsonValue(QJsonValue::Null) : value);
        QString text = QStringLiteral("%1 = %2  (%3").arg(key, shown(value), settingOriginName(origin));
        if (key == QLatin1String("model_paths") && !effective.settings.inheritedModelPaths.isEmpty()) {
            entry.insert("inherited_entries", QJsonArray::fromStringList(effective.settings.inheritedModelPaths));
            text += QStringLiteral("; inherited from Default: %1").arg(effective.settings.inheritedModelPaths.join(QStringLiteral(", ")));
        }
        listed.append(entry);
        lines << text + QLatin1Char(')');
    }
    Reply reply = Reply::success(QJsonObject{{"settings", listed}, {"path", settingsPath()}, {"inherits", QJsonArray::fromStringList(sessionInherits())}},
                                 lines.join('\n'));
    for (const QString &problem : effective.problems) {
        Error warning = makeError("usage", problem);
        warning.severity = Error::Severity::Warning;
        reply.errors << warning;
    }
    return reply;
}

} // namespace fugue::cli
