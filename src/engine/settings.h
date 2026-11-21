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
// | language         | a language code ("da", "pt-BR")          | absent: follow Omarchy   |
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
};

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

// The settings in effect now: settingsPath() read afresh, so a change reaches the next run
// and the CLI with no restart.
Settings currentSettings();

} // namespace fugue
