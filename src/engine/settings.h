#pragma once

// settings.json (spec 1.4): the user's settings, read by the window and the CLI alike.
//
// The file is one JSON object. Fugue knows the keys below; any other key (a newer Fugue's,
// a hand-written note) stays in the file untouched. A known key with a bad value is a
// problem: its default is used and the value stays in the file as written. A file that is
// there but is not a JSON object is never overwritten, so the user's text is not lost.
//
// | Key              | Value                                    | Default                  |
// | ---------------- | ---------------------------------------- | ------------------------ |
// | navigation       | "legacy" or "standard" (12.3)            | "legacy"                 |
// | language         | a language code ("da", "pt-BR"), or ""   | absent: follow Omarchy   |
// | output_folder    | an absolute or ~ path                    | ~/Pictures/Fugue         |
// | inputs_folder    | an absolute or ~ path                    | <output folder>/inputs   |
// | model_paths      | extra model search paths (8.9)           | []                       |
// | model_folder_kinds | what a model path holds, by the path as  | {}                       |
// |                  | written: "layout" or a resource type id;  |                          |
// |                  | unnamed: found by its files (8.9, #86)    |                          |
// | download_folder  | the model folder downloads go in, one of  | Fugue's own models folder|
// |                  | the folders above (8.9, #89)              |                          |
// | pack_paths       | extra pack search paths (6.5)            | []                       |
// | new_seed         | "random" or "fixed": new seed widgets    | "random"                 |
// | embed_workflow   | "full", "no-prompts" or "none" (11.7)    | "full"                   |
// | corner_radius    | 0 to 16 px, or "omarchy" (#63)           | 8                        |
// | hover_magnifier  | true or false: far out, a short hover    | false                    |
// |                  | shows a node at zoom 1 (12.6, #74, #123)  |                          |
// | restore_tabs     | true or false: start with the saved       | true                     |
// |                  | workflows that were open when the session |                          |
// |                  | ended (12.10, #212)                        |                          |
// | cache_results_gb | size limit of the result cache in GB;    | 10                       |
// |                  | 0: none (10.5, #141)                     |                          |
// | cache_thumbnails_gb | size limit of the picture previews in | 1                        |
// |                  | GB; 0: none (12.7, #141)                  |                          |
// | runs_kept        | ended runs kept whatever their age (9.7,  | 20                       |
// |                  | #141)                                     |                          |

#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue {

struct Settings {
    QString navigation = QStringLiteral("legacy");
    QString language;     // empty: follow Omarchy's language
    QString outputFolder; // as written (may start with ~); empty: the default
    QString inputsFolder; // as written; empty: the output folder's inputs
    QStringList modelPaths;
    QMap<QString, QString> modelFolderKinds; // by the path as written in modelPaths
    QString downloadFolder; // as written; empty: Fugue's own models folder
    QStringList packPaths;
    QString newSeed = QStringLiteral("random");
    QString embedWorkflow = QStringLiteral("full");
    // The canvas corners in px, or followOmarchy for Hyprland's rounding (look::corners).
    static constexpr int followOmarchy = -1;
    static constexpr int maxCornerRadius = 16;
    int cornerRadius = 8;
    bool hoverMagnifier = false; // the hover magnifier (#74), off unless set (#123)
    bool restoreTabs = true;     // reopen the last session's saved tabs at start (#212)
    // The cache limits (#141); the cache manager turns them into bytes. 0 is no limit.
    static constexpr double maxCacheGb = 100000;
    static constexpr int maxRunsKept = 100000;
    double cacheResultsGb = 10;
    double cacheThumbnailsGb = 1;
    int runsKept = 20;
    // Layering (spec 1.4.2). When the session is in a project that inherits `models`, modelPaths holds the
    // project's own entries and then Default's, and these say which of them came from Default and that
    // Default's own models folder is a root too. All read-only roots: Fugue never writes in them.
    QStringList inheritedModelPaths;
    bool inheritsModels = false;
};

// Where a setting's value comes from (spec 1.4.2).
enum class SettingOrigin {
    BuiltIn, // the default, nothing is set
    Own,     // this session's settings.json
    Default, // inherited from the Default project's settings.json
};
// "built-in", "own", "inherited from Default".
QString settingOriginName(SettingOrigin origin);

// Which layer a key belongs to: `preferences` and `models` can be inherited (project.json's `inherits`),
// every other key is `own`: a project never sees the Default's output folder, caches, pack paths or
// download folder.
enum class SettingClass { Preferences, Models, Own };
SettingClass settingClass(const QString &key);
// The word of project.json's `inherits` for a class: "preferences", "models"; empty for Own.
QString settingClassName(SettingClass settingClass);

// What a settings.json holds.
struct SettingsFile {
    Settings settings;     // the valid values, defaults for the rest
    QJsonObject document;  // the whole object as read, unknown keys included
    QStringList problems;  // one line each: a bad value, or why the file cannot be read
    bool writable = true;  // false when the file is there but is not a JSON object
};

// $XDG_CONFIG_HOME/fugue/settings.json (~/.config/fugue/settings.json).
QString settingsPath();

// The keys Fugue knows, in the table's order.
QStringList settingKeys();

// The default of a known key, as it would be written.
QJsonValue settingDefault(const QString &key);

// The value that means "unset" for a key whose built-in state is an absent key (language: "" follows
// Omarchy); undefined for every other key. A project that inherits the Default's value writes it, not
// nothing, so the unset state can shadow (changeSetting drops it when there is nothing to shadow).
QJsonValue settingUnsetValue(const QString &key);

// Reads a settings.json; a missing file is every default and no problem.
SettingsFile readSettings(const QString &path);

// Why `value` cannot be the value of `key`, in one sentence; empty when it can.
QString checkSetting(const QString &key, const QJsonValue &value);

// Sets one key in the file and writes it whole, atomically (engine/atomic_file), making
// the folder when needed. A value equal to the default, or an undefined value, removes the
// key; a change to nothing new writes nothing. Errors: usage for
// a bad value (nothing is written), the read error for a file that is not a JSON object
// (never overwritten), or the write's error.
Errors changeSetting(const QString &path, const QString &key, const QJsonValue &value);

// The settings of the Default project (the base config folder's settings.json); settingsPath() when the
// session is the Default project.
QString baseSettingsPath();

// What the layering of 1.4.2 makes of a session's own file and the Default's, for a project that
// inherits `inherits` (words of project.json). A scalar is the project's key, else the Default's, else the
// built-in; model_paths is the project's entries, then the Default's not already there; model_folder_kinds
// takes the Default's entries for paths the project does not name. A key whose class is not in `inherits`
// is only ever read from the project's file. `defaultModelsDir` is the Default's models folder.
struct EffectiveSettings {
    Settings settings;
    QMap<QString, SettingOrigin> origins; // for every known key
    QStringList problems;                 // the project's file's, then the Default's (prefixed)
};
EffectiveSettings layerSettings(const SettingsFile &own, const SettingsFile &base, const QStringList &inherits,
                                const QString &defaultModelsDir);

// layerSettings() of this session: its own file, and the Default's when project.json inherits a class.
EffectiveSettings effectiveSettings();

// The value of a known key in a Settings, as it would be written in the file.
QJsonValue settingValue(const Settings &settings, const QString &key);

// The settings in effect now: effectiveSettings() read afresh, so a change reaches the next run
// and the CLI with no restart.
Settings currentSettings();

// keybindings.json is one value: the project's file, else the Default's when `preferences` is inherited
// (a project's own file replaces it whole, the two do not merge). Empty when neither exists.
QString effectiveKeymapPath();

} // namespace fugue
