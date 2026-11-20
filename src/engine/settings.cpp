#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

const QStringList kKeys{"navigation",     "language",       "output_folder",
                        "inputs_folder",  "model_paths",    "model_folder_kinds",
                        "pack_paths",     "new_seed",       "embed_workflow",
                        "corner_radius"};

QString oneOf(const QJsonValue &value, const QStringList &choices, const QString &key) {
    if (value.isString() && choices.contains(value.toString()))
        return {};
    return QStringLiteral("%1 must be one of %2.").arg(key, choices.join(QStringLiteral(", ")));
}

QString checkFolder(const QJsonValue &value, const QString &what) {
    if (!value.isString() || value.toString().trimmed().isEmpty())
        return QStringLiteral("%1 must be a folder path.").arg(what);
    const QString path = value.toString();
    if (path != path.trimmed())
        return QStringLiteral("%1 has spaces at an end: %2.").arg(what, path);
    if (!path.startsWith(QLatin1Char('/')) && path != QLatin1String("~") && !path.startsWith(QLatin1String("~/")))
        return QStringLiteral("%1 must start with / or ~/: %2.").arg(what, path);
    const QFileInfo info(expandHome(path));
    if (info.exists() && !info.isDir())
        return QStringLiteral("%1 is a file, not a folder: %2.").arg(what, path);
    return {};
}

QString checkFolders(const QJsonValue &value, const QString &key) {
    if (!value.isArray())
        return QStringLiteral("%1 must be a list of folder paths.").arg(key);
    QSet<QString> seen;
    for (const QJsonValue &item : value.toArray()) {
        if (QString reason = checkFolder(item, key); !reason.isEmpty())
            return reason;
        const QString cleaned = QDir::cleanPath(expandHome(item.toString()));
        if (seen.contains(cleaned))
            return QStringLiteral("%1 lists %2 twice.").arg(key, item.toString());
        seen.insert(cleaned);
    }
    return {};
}

QStringList strings(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

void apply(Settings &settings, const QString &key, const QJsonValue &value) {
    if (key == QLatin1String("navigation"))
        settings.navigation = value.toString();
    else if (key == QLatin1String("language"))
        settings.language = value.toString();
    else if (key == QLatin1String("output_folder"))
        settings.outputFolder = value.toString();
    else if (key == QLatin1String("inputs_folder"))
        settings.inputsFolder = value.toString();
    else if (key == QLatin1String("model_paths"))
        settings.modelPaths = strings(value);
    else if (key == QLatin1String("model_folder_kinds")) {
        const QJsonObject kinds = value.toObject();
        for (auto it = kinds.begin(); it != kinds.end(); ++it)
            settings.modelFolderKinds.insert(it.key(), it.value().toString());
    } else if (key == QLatin1String("pack_paths"))
        settings.packPaths = strings(value);
    else if (key == QLatin1String("new_seed"))
        settings.newSeed = value.toString();
    else if (key == QLatin1String("embed_workflow"))
        settings.embedWorkflow = value.toString();
    else if (key == QLatin1String("corner_radius"))
        settings.cornerRadius = value.isString() ? Settings::followOmarchy : value.toInt();
}

} // namespace

QString settingsPath() { return configDir() + QStringLiteral("/settings.json"); }

QStringList settingKeys() { return kKeys; }

QJsonValue settingDefault(const QString &key) {
    if (key == QLatin1String("navigation"))
        return QStringLiteral("legacy");
    if (key == QLatin1String("new_seed"))
        return QStringLiteral("random");
    if (key == QLatin1String("embed_workflow"))
        return QStringLiteral("full");
    if (key == QLatin1String("corner_radius"))
        return 8;
    if (key == QLatin1String("model_paths") || key == QLatin1String("pack_paths"))
        return QJsonArray();
    if (key == QLatin1String("model_folder_kinds"))
        return QJsonObject();
    return QJsonValue(QJsonValue::Undefined); // language and folders: absent
}

QString checkSetting(const QString &key, const QJsonValue &value) {
    if (key == QLatin1String("navigation"))
        return oneOf(value, {"legacy", "standard"}, key);
    if (key == QLatin1String("new_seed"))
        return oneOf(value, {"random", "fixed"}, key);
    if (key == QLatin1String("embed_workflow"))
        return oneOf(value, {"full", "no-prompts", "none"}, key);
    if (key == QLatin1String("corner_radius")) {
        const double px = value.toDouble(-1);
        if (value == QJsonValue(QStringLiteral("omarchy"))
            || (value.isDouble() && px == std::floor(px) && px >= 0 && px <= Settings::maxCornerRadius))
            return {};
        return QStringLiteral("corner_radius must be a whole number of px from 0 to %1, or omarchy.")
            .arg(Settings::maxCornerRadius);
    }
    if (key == QLatin1String("language")) {
        static const QRegularExpression code(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2})?$"));
        if (value.isString() && code.match(value.toString()).hasMatch())
            return {};
        return QStringLiteral("language must be a language code such as da or pt-BR.");
    }
    if (key == QLatin1String("output_folder") || key == QLatin1String("inputs_folder"))
        return checkFolder(value, key);
    if (key == QLatin1String("model_paths") || key == QLatin1String("pack_paths"))
        return checkFolders(value, key);
    if (key == QLatin1String("model_folder_kinds")) {
        static const QRegularExpression typeId(QStringLiteral("^[a-z0-9.-]+/[a-z0-9.-]+$"));
        if (!value.isObject())
            return QStringLiteral("model_folder_kinds must map model paths to what they hold.");
        const QJsonObject kinds = value.toObject();
        for (auto it = kinds.begin(); it != kinds.end(); ++it)
            if (!it.value().isString() || (it.value().toString() != QLatin1String("layout")
                                           && !typeId.match(it.value().toString()).hasMatch()))
                return QStringLiteral("model_folder_kinds: %1 must hold layout or a model type such as fugue/lora.")
                    .arg(it.key());
        return {};
    }
    return QStringLiteral("%1 is not a setting.").arg(key);
}

SettingsFile readSettings(const QString &path) {
    SettingsFile file;
    if (!QFileInfo::exists(path))
        return file;
    const Result<QJsonObject> read = readJsonObject(path);
    if (!read) {
        file.writable = false;
        file.problems << read.error().message;
        return file;
    }
    file.document = read.value();
    for (const QString &key : kKeys) {
        if (!file.document.contains(key))
            continue;
        const QJsonValue value = file.document.value(key);
        if (const QString reason = checkSetting(key, value); !reason.isEmpty())
            file.problems << reason;
        else
            apply(file.settings, key, value);
    }
    return file;
}

Errors changeSetting(const QString &path, const QString &key, const QJsonValue &value) {
    if (!kKeys.contains(key))
        return {makeError(QStringLiteral("usage"), checkSetting(key, value))};
    if (const QString reason = value.isUndefined() ? QString() : checkSetting(key, value); !reason.isEmpty())
        return {makeError(QStringLiteral("usage"), reason)};
    QJsonObject document;
    if (QFileInfo::exists(path)) {
        const Result<QJsonObject> read = readJsonObject(path);
        if (!read)
            return read.errors();
        document = read.value();
    }
    if (value.isUndefined() || value == settingDefault(key)) {
        if (!document.contains(key))
            return {}; // already the default: nothing to write
        document.remove(key);
    } else if (document.value(key) == value) {
        return {};
    }
    if (value.isUndefined() || value == settingDefault(key))
        document.remove(key);
    else
        document.insert(key, value);
    return replaceFile(path, writeJson(document), path);
}

Settings currentSettings() { return readSettings(settingsPath()).settings; }

} // namespace fugue
