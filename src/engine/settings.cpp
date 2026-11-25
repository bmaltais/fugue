#include "settings.h"

#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

#include "engine/atomic_file.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/project.h"

namespace fugue {

namespace {

const QStringList kKeys{"navigation",     "language",       "output_folder",
                        "inputs_folder",  "model_paths",    "model_folder_kinds",
                        "download_folder", "pack_paths",    "new_seed",
                        "embed_workflow", "look_corners_omarchy",  "hover_magnifier",
                        "restore_tabs",   "picture_mode", "step_preview", "cache_results_gb", "cache_thumbnails_gb", "runs_kept", "frost", "look_preset", "look_colour", "look_glow", "look_depth", "look_corners", "look_spacing", "node_style"};

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
    } else if (key == QLatin1String("download_folder"))
        settings.downloadFolder = value.toString();
    else if (key == QLatin1String("pack_paths"))
        settings.packPaths = strings(value);
    else if (key == QLatin1String("new_seed"))
        settings.newSeed = value.toString();
    else if (key == QLatin1String("embed_workflow"))
        settings.embedWorkflow = value.toString();
    else if (key == QLatin1String("look_corners_omarchy"))
        settings.lookCornersOmarchy = value.toBool();
    else if (key == QLatin1String("picture_mode"))
        settings.pictureMode = value.toString();
    else if (key == QLatin1String("step_preview"))
        settings.stepPreview = value.toString();
    else if (key == QLatin1String("look_preset"))
        settings.lookPreset = value.toString();
    else if (key == QLatin1String("node_style"))
        settings.nodeStyle = value.toString();
    else if (key == QLatin1String("look_colour"))
        settings.lookColour = std::clamp(value.toDouble(0.4), 0.0, 1.0);
    else if (key == QLatin1String("look_glow"))
        settings.lookGlow = std::clamp(value.toDouble(0.5), 0.0, 1.0);
    else if (key == QLatin1String("look_depth"))
        settings.lookDepth = std::clamp(value.toDouble(0.5), 0.0, 1.0);
    else if (key == QLatin1String("look_corners"))
        settings.lookCorners = std::clamp(value.toDouble(0.5), 0.0, 1.0);
    else if (key == QLatin1String("look_spacing"))
        settings.lookSpacing = std::clamp(value.toDouble(0.5), 0.0, 1.0);
    else if (key == QLatin1String("hover_magnifier"))
        settings.hoverMagnifier = value.toBool();
    else if (key == QLatin1String("frost"))
        settings.frost = std::clamp(value.toDouble(0.6), 0.0, 1.0);
    else if (key == QLatin1String("restore_tabs"))
        settings.restoreTabs = value.toBool();
    else if (key == QLatin1String("cache_results_gb"))
        settings.cacheResultsGb = value.toDouble();
    else if (key == QLatin1String("cache_thumbnails_gb"))
        settings.cacheThumbnailsGb = value.toDouble();
    else if (key == QLatin1String("runs_kept"))
        settings.runsKept = value.toInt();
}

// The Default project's valid value of `key`, undefined when it has none or the class is not inherited.
QJsonValue inheritedValue(const QString &key) {
    if (settingClass(key) == SettingClass::Own)
        return QJsonValue(QJsonValue::Undefined);
    if (!sessionInherits().contains(settingClassName(settingClass(key))))
        return QJsonValue(QJsonValue::Undefined);
    const SettingsFile base = readSettings(baseSettingsPath());
    const QJsonValue value = base.document.value(key);
    return !value.isUndefined() && checkSetting(key, value).isEmpty() ? value : QJsonValue(QJsonValue::Undefined);
}

// True when writing the built-in value of `key` into the session's own file must be kept: the Default's
// value for it is another one (lists and maps only add, so they never shadow).
bool shadowsInherited(const QString &path, const QString &key, const QJsonValue &value) {
    if (path != settingsPath() || fugueHome().isEmpty() || value.isArray() || value.isObject())
        return false;
    const QJsonValue inherited = inheritedValue(key);
    return !inherited.isUndefined() && inherited != value;
}

} // namespace

QString settingOriginName(SettingOrigin origin) {
    switch (origin) {
    case SettingOrigin::BuiltIn:
        return QStringLiteral("built-in");
    case SettingOrigin::Own:
        return QStringLiteral("own");
    case SettingOrigin::Default:
        return QStringLiteral("inherited from Default");
    }
    return {};
}

SettingClass settingClass(const QString &key) {
    static const QStringList preferences{"navigation", "language", "look_corners_omarchy", "hover_magnifier", "frost", "look_preset", "look_colour", "look_glow", "look_depth", "look_corners", "look_spacing", "node_style", "new_seed", "embed_workflow", "restore_tabs", "picture_mode", "step_preview"};
    if (preferences.contains(key))
        return SettingClass::Preferences;
    if (key == QLatin1String("model_paths") || key == QLatin1String("model_folder_kinds"))
        return SettingClass::Models;
    return SettingClass::Own;
}

QString settingClassName(SettingClass settingClass) {
    switch (settingClass) {
    case SettingClass::Preferences:
        return QStringLiteral("preferences");
    case SettingClass::Models:
        return QStringLiteral("models");
    case SettingClass::Own:
        break;
    }
    return {};
}

QString settingsPath() { return configDir() + QStringLiteral("/settings.json"); }

QStringList settingKeys() { return kKeys; }

QJsonValue settingDefault(const QString &key) {
    if (key == QLatin1String("navigation"))
        return QStringLiteral("legacy");
    if (key == QLatin1String("new_seed"))
        return QStringLiteral("random");
    if (key == QLatin1String("embed_workflow"))
        return QStringLiteral("full");
    if (key == QLatin1String("picture_mode"))
        return QStringLiteral("smart");
    if (key == QLatin1String("step_preview"))
        return QStringLiteral("fast");
    if (key == QLatin1String("look_corners_omarchy"))
        return false;
    if (key == QLatin1String("hover_magnifier"))
        return false;
    if (key == QLatin1String("frost"))
        return 0.6;
    if (key == QLatin1String("look_preset"))
        return QStringLiteral("calm");
    if (key == QLatin1String("node_style"))
        return QStringLiteral("compact");
    if (key == QLatin1String("look_colour"))
        return 0.4;
    if (key == QLatin1String("look_glow") || key == QLatin1String("look_depth") || key == QLatin1String("look_corners")
        || key == QLatin1String("look_spacing"))
        return 0.5;
    if (key == QLatin1String("restore_tabs"))
        return true;
    if (key == QLatin1String("cache_results_gb"))
        return 10;
    if (key == QLatin1String("cache_thumbnails_gb"))
        return 1;
    if (key == QLatin1String("runs_kept"))
        return 20;
    if (key == QLatin1String("model_paths") || key == QLatin1String("pack_paths"))
        return QJsonArray();
    if (key == QLatin1String("model_folder_kinds"))
        return QJsonObject();
    return QJsonValue(QJsonValue::Undefined); // language and folders: absent
}

QJsonValue settingUnsetValue(const QString &key) {
    if (key == QLatin1String("language"))
        return QString();
    return QJsonValue(QJsonValue::Undefined);
}

QString checkSetting(const QString &key, const QJsonValue &value) {
    if (key == QLatin1String("navigation"))
        return oneOf(value, {"legacy", "standard"}, key);
    if (key == QLatin1String("new_seed"))
        return oneOf(value, {"random", "fixed"}, key);
    if (key == QLatin1String("embed_workflow"))
        return oneOf(value, {"full", "no-prompts", "none"}, key);
    if (key == QLatin1String("picture_mode"))
        return oneOf(value, {"smart", "sinks", "all"}, key);
    if (key == QLatin1String("step_preview"))
        return oneOf(value, {"off", "fast", "tae", "vae"}, key);
    if (key == QLatin1String("look_corners_omarchy"))
        return value.isBool() ? QString() : QStringLiteral("look_corners_omarchy must be true or false.");
    if (key == QLatin1String("frost"))
        return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 1 ? QString() : QStringLiteral("frost must be a number from 0 to 1.");
    if (key == QLatin1String("look_preset"))
        return value.isString() && QRegularExpression(QStringLiteral("^[a-z0-9_-]{1,32}$")).match(value.toString()).hasMatch()
                   ? QString() : QStringLiteral("look_preset must be a preset's id: lower-case letters, digits, - and _.");
    if (key == QLatin1String("node_style"))
        return oneOf(value, {"compact", "flat"}, key);
    if (key == QLatin1String("look_colour") || key == QLatin1String("look_glow") || key == QLatin1String("look_depth")
        || key == QLatin1String("look_corners") || key == QLatin1String("look_spacing"))
        return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 1 ? QString() : QStringLiteral("%1 must be a number from 0 to 1.").arg(key);
    if (key == QLatin1String("hover_magnifier"))
        return value.isBool() ? QString() : QStringLiteral("hover_magnifier must be true or false.");
    if (key == QLatin1String("restore_tabs"))
        return value.isBool() ? QString() : QStringLiteral("restore_tabs must be true or false.");
    if (key == QLatin1String("cache_results_gb") || key == QLatin1String("cache_thumbnails_gb")) {
        if (value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= Settings::maxCacheGb)
            return {};
        return QStringLiteral("%1 must be a number of GB from 0 (no limit) to %2.").arg(key).arg(Settings::maxCacheGb);
    }
    if (key == QLatin1String("runs_kept")) {
        const double count = value.toDouble(-1);
        if (value.isDouble() && count == std::floor(count) && count >= 0 && count <= Settings::maxRunsKept)
            return {};
        return QStringLiteral("runs_kept must be a whole number from 0 to %1.").arg(Settings::maxRunsKept);
    }
    if (key == QLatin1String("language")) {
        static const QRegularExpression code(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2})?$"));
        if (value.isString() && (value.toString().isEmpty() || code.match(value.toString()).hasMatch()))
            return {};
        return QStringLiteral("language must be a language code such as da or pt-BR, or empty to follow Omarchy.");
    }
    if (key == QLatin1String("output_folder") || key == QLatin1String("inputs_folder")
        || key == QLatin1String("download_folder"))
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

// corner_radius (a whole px, 0 to 16, or "omarchy") was replaced by the Look's Corners slider and its "follow Omarchy" option (#467).
// The old value becomes the slider position that gives the same radius (8 px at 0.5, a factor 0.1 + 1.8 x position) or the option, unless
// the file already says its own; the old key goes. True when the document changed.
bool migrateCornerRadius(QJsonObject &document) {
    if (!document.contains(QLatin1String("corner_radius")))
        return false;
    const QJsonValue old = document.take(QLatin1String("corner_radius"));
    if (old.isString() && old.toString() == QLatin1String("omarchy")) {
        if (!document.contains(QLatin1String("look_corners_omarchy")))
            document.insert(QLatin1String("look_corners_omarchy"), true);
    } else if (old.isDouble() && !document.contains(QLatin1String("look_corners"))) {
        const double px = std::clamp(old.toDouble(), 0.0, 16.0);
        const double position = std::clamp(std::round((px / 8.0 - 0.1) / 1.8 * 1000.0) / 1000.0, 0.0, 1.0);
        if (std::abs(position - 0.5) > 0.0005)
            document.insert(QLatin1String("look_corners"), position);
    }
    return true;
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
    migrateCornerRadius(file.document);
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
        migrateCornerRadius(document); // written back with this change: the old key goes
    }
    // The default is not written, except where the Default project's value would otherwise show through:
    // a project that sets a preference back to the built-in value must say so.
    // The unset sentinel ("" for language) is the same: written only over an inherited value.
    const bool unsetValue = !value.isUndefined() && value == settingUnsetValue(key);
    const bool isDefault = value.isUndefined() || ((value == settingDefault(key) || unsetValue) && !shadowsInherited(path, key, value));
    if (isDefault) {
        if (!document.contains(key))
            return {}; // already the default: nothing to write
        document.remove(key);
    } else if (document.value(key) == value) {
        return {};
    }
    if (isDefault)
        document.remove(key);
    else
        document.insert(key, value);
    return replaceFile(path, writeJson(document), path);
}

QString baseSettingsPath() { return baseConfigDir() + QStringLiteral("/settings.json"); }

EffectiveSettings layerSettings(const SettingsFile &own, const SettingsFile &base, const QStringList &inherits,
                                const QString &defaultModelsDir) {
    EffectiveSettings out;
    out.settings = own.settings;
    out.problems = own.problems;
    const bool models = inherits.contains(settingClassName(SettingClass::Models));
    const bool preferences = inherits.contains(settingClassName(SettingClass::Preferences));
    if (models || preferences)
        for (const QString &problem : base.problems)
            out.problems << QStringLiteral("Default project: %1").arg(problem);
    out.settings.inheritsModels = models && !defaultModelsDir.isEmpty();
    for (const QString &key : kKeys) {
        const SettingClass kind = settingClass(key);
        const bool inheritable = (kind == SettingClass::Preferences && preferences) || (kind == SettingClass::Models && models);
        const bool ownHas = own.document.contains(key);
        out.origins.insert(key, ownHas ? SettingOrigin::Own : SettingOrigin::BuiltIn);
        if (!inheritable)
            continue;
        const QJsonValue value = base.document.value(key);
        if (value.isUndefined() || !checkSetting(key, value).isEmpty())
            continue;
        if (key == QLatin1String("model_paths")) {
            QSet<QString> seen;
            for (const QString &path : out.settings.modelPaths)
                seen.insert(QDir::cleanPath(expandHome(path)));
            for (const QString &path : strings(value))
                if (!seen.contains(QDir::cleanPath(expandHome(path)))) {
                    seen.insert(QDir::cleanPath(expandHome(path)));
                    out.settings.modelPaths << path;
                    out.settings.inheritedModelPaths << path;
                }
            if (!out.settings.inheritedModelPaths.isEmpty() && !ownHas)
                out.origins[key] = SettingOrigin::Default;
        } else if (key == QLatin1String("model_folder_kinds")) {
            const QJsonObject kinds = value.toObject();
            bool added = false;
            for (auto it = kinds.begin(); it != kinds.end(); ++it)
                if (!out.settings.modelFolderKinds.contains(it.key())) {
                    out.settings.modelFolderKinds.insert(it.key(), it.value().toString());
                    added = true;
                }
            if (added && !ownHas)
                out.origins[key] = SettingOrigin::Default;
        } else if (!ownHas) {
            apply(out.settings, key, value);
            out.origins[key] = SettingOrigin::Default;
        }
    }
    return out;
}

EffectiveSettings effectiveSettings() {
    const SettingsFile own = readSettings(settingsPath());
    const QStringList inherits = sessionInherits();
    const bool layered = inherits.contains(settingClassName(SettingClass::Models))
                         || inherits.contains(settingClassName(SettingClass::Preferences));
    return layerSettings(own, layered ? readSettings(baseSettingsPath()) : SettingsFile(), inherits,
                         baseDataDir() + QStringLiteral("/models"));
}

QJsonValue settingValue(const Settings &settings, const QString &key) {
    if (key == QLatin1String("navigation"))
        return settings.navigation;
    if (key == QLatin1String("language"))
        return settings.language.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(settings.language);
    if (key == QLatin1String("output_folder"))
        return settings.outputFolder.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(settings.outputFolder);
    if (key == QLatin1String("inputs_folder"))
        return settings.inputsFolder.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(settings.inputsFolder);
    if (key == QLatin1String("model_paths"))
        return QJsonArray::fromStringList(settings.modelPaths);
    if (key == QLatin1String("model_folder_kinds")) {
        QJsonObject kinds;
        for (auto it = settings.modelFolderKinds.begin(); it != settings.modelFolderKinds.end(); ++it)
            kinds.insert(it.key(), it.value());
        return kinds;
    }
    if (key == QLatin1String("download_folder"))
        return settings.downloadFolder.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(settings.downloadFolder);
    if (key == QLatin1String("pack_paths"))
        return QJsonArray::fromStringList(settings.packPaths);
    if (key == QLatin1String("new_seed"))
        return settings.newSeed;
    if (key == QLatin1String("embed_workflow"))
        return settings.embedWorkflow;
    if (key == QLatin1String("look_corners_omarchy"))
        return settings.lookCornersOmarchy;
    if (key == QLatin1String("picture_mode"))
        return settings.pictureMode;
    if (key == QLatin1String("step_preview"))
        return settings.stepPreview;
    if (key == QLatin1String("hover_magnifier"))
        return settings.hoverMagnifier;
    if (key == QLatin1String("frost"))
        return settings.frost;
    if (key == QLatin1String("look_preset"))
        return settings.lookPreset;
    if (key == QLatin1String("node_style"))
        return settings.nodeStyle;
    if (key == QLatin1String("look_colour"))
        return settings.lookColour;
    if (key == QLatin1String("look_glow"))
        return settings.lookGlow;
    if (key == QLatin1String("look_depth"))
        return settings.lookDepth;
    if (key == QLatin1String("look_corners"))
        return settings.lookCorners;
    if (key == QLatin1String("look_spacing"))
        return settings.lookSpacing;
    if (key == QLatin1String("restore_tabs"))
        return settings.restoreTabs;
    if (key == QLatin1String("cache_results_gb"))
        return settings.cacheResultsGb;
    if (key == QLatin1String("cache_thumbnails_gb"))
        return settings.cacheThumbnailsGb;
    if (key == QLatin1String("runs_kept"))
        return settings.runsKept;
    return QJsonValue(QJsonValue::Undefined);
}

Settings currentSettings() { return effectiveSettings().settings; }

QString effectiveKeymapPath() {
    const QString own = configDir() + QStringLiteral("/keybindings.json");
    if (QFileInfo::exists(own) || !sessionInherits().contains(settingClassName(SettingClass::Preferences)))
        return own;
    const QString base = baseConfigDir() + QStringLiteral("/keybindings.json");
    return QFileInfo::exists(base) ? base : own;
}

} // namespace fugue
