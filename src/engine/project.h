#pragma once

// Projects (spec 1.4.2, epic #204): a project is a FUGUE_HOME root with a `project.json` in it. A
// per-user registry in the BASE config folder (the ordinary session's, even from inside a project)
// lists the projects the user knows; it is a list of paths, not a scan, so a project may sit on
// another drive. This is the data and the operations on it; the selection (which root a session
// starts in) is engine/fugue_home.h and the commands are cli/project.cpp. What a project inherits is
// only recorded here: the layering itself is a later step.

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue {

// What a project may inherit from the Default project, in this order. A new project inherits the
// first three of `models`, `packs`, `preferences` (kDefaultInherits).
QStringList inheritableItems();
QStringList defaultInherits();
// What can never be inherited (providers and keys, recent files, results, outputs, ...): listing one is
// ignored and reported, like an unknown word.
QStringList neverInheritableItems();

struct ProjectManifest {
    QString name;
    QStringList inherits; // known, inheritable items only, each once, in file order
    QStringList ignored;  // the items of `inherits` that were left out (unknown or never inheritable)
    QJsonObject raw;      // the file as read: keys this version does not know are written back
};

// A name a project may have: 1 to 64 characters, no path separator, no dot, no control character, and not
// `default` (the ordinary session's, reserved). Empty when fine, else why not.
QString projectNameProblem(const QString &name);
// The folder name a project gets under the default location: lowercase letters and digits joined by `-`.
QString projectSlug(const QString &name);

// project.json as text. project_invalid for what cannot be used (not an object, a name that is not
// allowed, `inherits` not a list of text), unsupported_format_version for another format. An item of
// `inherits` that is unknown or never inheritable is a project_inherits_ignored WARNING, not a failure.
Result<ProjectManifest> parseProjectManifest(const QByteArray &json, const QString &what);
// `<root>/project.json`.
QString projectManifestPath(const QString &root);
Result<ProjectManifest> readProject(const QString &root);
Errors writeProject(const QString &root, const ProjectManifest &manifest);

// The registry `<base config>/projects.json`: {"format": 1, "last": <path>, "projects": [<abs path>...]}.
struct ProjectRegistry {
    QStringList projects; // absolute clean paths, each once
    QString last;         // where the window last ran, empty for Default or never
};
QString projectRegistryPath();
bool projectRegistryExists();
// No file is an empty registry; a file that cannot be read or is not a registry is an error (never
// replaced by an empty one).
Result<ProjectRegistry> readProjectRegistry();
Errors writeProjectRegistry(const ProjectRegistry &registry);

enum class ProjectState {
    Ok,
    Missing, // the folder or its project.json is gone (a drive that is not mounted)
    Invalid, // project.json cannot be used
};
QString projectStateName(ProjectState state);

struct ProjectEntry {
    QString root;
    ProjectState state = ProjectState::Ok;
    ProjectManifest manifest; // empty name when not Ok (name() then falls back to the folder name)
    bool registered = false;
    Errors problems; // why it is Invalid; the warnings of an Ok one
    QString name() const;
};

// The registered projects, in registry order.
Result<QList<ProjectEntry>> listProjects();
// The project at `root` whether registered or not.
ProjectEntry inspectProject(const QString &root);
// By name (case-insensitive) among the registered ones, or by path (anything with a `/`, or starting with
// `.` or `~`) registered or not. unknown_project; a name two projects share is a usage error naming both.
Result<ProjectEntry> findProject(const QString &nameOrPath);
// The root of a project that can be started in: found, present and valid.
Result<QString> resolveProjectRoot(const QString &nameOrPath);

// Makes a project and registers it. `inDir` is its root (made if needed; it must not already hold a
// project), or empty for `<base data>/projects/<slug>` (a number is added when that is taken). `inherits` is
// what to record. project_exists for a taken name, project_invalid for a bad one.
Result<ProjectEntry> createProject(const QString &name, const QString &inDir, const QStringList &inherits);
// Registers the project in `dir`; project_invalid when there is none; project_exists when its name is another
// registered project's. Registering one twice is fine.
Result<ProjectEntry> addProject(const QString &dir);
// Takes the project out of the registry. Its folder and files are never touched.
Result<ProjectEntry> removeProject(const QString &nameOrPath);
// Replaces the project's `inherits` (unknown and never-inheritable words are reported as warnings and not recorded).
Result<ProjectEntry> setProjectInherits(const QString &nameOrPath, const QStringList &items);

// What the session's own root inherits: the `inherits` of the project.json at fugueHome(); empty for the
// Default project, a root with no project.json (anonymous) and a project.json that cannot be used.
QStringList sessionInherits();

// The root the window last ran in, when it is still a usable project; else empty.
QString lastUsedProjectRoot();
// The window start's: remembers the current root (fugueHome()) as the last used if it is a project, or
// forgets it when the session is Default. Only when there is a registry to update or a project to remember;
// a failure to write is not worth stopping the window for.
void recordLastUsedProject();

} // namespace fugue
