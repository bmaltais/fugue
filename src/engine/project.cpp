#include "project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>

#include <optional>

#include "engine/atomic_file.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

constexpr int kFormat = 1;

const QStringList &neverInheritable() {
    static const QStringList items = {"providers", "keys", "recent", "recovery", "hashes", "results", "runs", "thumbnails",
                                      "queue",     "outputs", "inputs", "workflows", "documents", "output_folder",
                                      "inputs_folder", "download_folder", "cache_limits"};
    return items;
}

QString absolute(const QString &path) {
    return QDir::cleanPath(QFileInfo(expandHome(path)).absoluteFilePath());
}

Error warningFor(const QString &item, bool never) {
    Error warning = makeError("project_inherits_ignored",
                              never ? QStringLiteral("\"%1\" can never be inherited, so it is ignored.").arg(item)
                                    : QStringLiteral("\"%1\" is not something a project inherits, so it is ignored.").arg(item),
                              QStringLiteral("A project may inherit: %1.").arg(inheritableItems().join(QStringLiteral(", "))));
    warning.severity = Error::Severity::Warning;
    for (const QString &valid : inheritableItems())
        warning.valid.append(valid);
    return warning;
}

// The words of `items` a project may record, each once; the others go to `ignored` with a warning.
void sortItems(const QStringList &items, QStringList *kept, QStringList *ignored, Errors *warnings) {
    for (const QString &raw : items) {
        const QString item = raw.trimmed();
        if (item.isEmpty())
            continue;
        if (inheritableItems().contains(item)) {
            if (!kept->contains(item))
                kept->append(item);
        } else if (!ignored->contains(item)) {
            ignored->append(item);
            warnings->append(warningFor(item, neverInheritable().contains(item)));
        }
    }
}

QJsonObject manifestJson(const ProjectManifest &manifest) {
    QJsonObject object = manifest.raw;
    object.insert("format", kFormat);
    object.insert("name", manifest.name);
    object.insert("inherits", QJsonArray::fromStringList(manifest.inherits));
    return object;
}

Error invalid(const QString &what, const QString &why) {
    return makeError("project_invalid", QStringLiteral("%1 is not a usable project: %2.").arg(what, why),
                     QStringLiteral("project.json is {\"format\": 1, \"name\": \"Acme\", \"inherits\": [\"models\"]}."));
}

bool looksLikePath(const QString &text) {
    return text.contains(QLatin1Char('/')) || text.startsWith(QLatin1Char('.')) || text.startsWith(QLatin1Char('~'));
}

Error unknownProject(const QString &what) {
    return makeError("unknown_project", QStringLiteral("There is no project \"%1\".").arg(what),
                     QStringLiteral("`fugue project list` shows the registered ones; `fugue project add <folder>` registers one."));
}

bool samePath(const QString &a, const QString &b) { return QDir::cleanPath(a) == QDir::cleanPath(b); }

} // namespace

QStringList inheritableItems() { return {"models", "packs", "templates", "prompts", "preferences"}; }
QStringList defaultInherits() { return {"models", "packs", "preferences"}; }
QStringList neverInheritableItems() { return neverInheritable(); }

QString projectNameProblem(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("a project needs a name");
    if (trimmed != name)
        return QStringLiteral("a name does not start or end with a space");
    if (name.size() > 64)
        return QStringLiteral("a name is at most 64 characters");
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char('.'))
            return QStringLiteral("a name has no \"/\", \"\\\" or \".\", so it can never be taken for a path");
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return QStringLiteral("a name has no control characters");
    }
    if (name.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("\"default\" is the ordinary session's name and is reserved");
    return {};
}

QString projectSlug(const QString &name) {
    QString slug;
    bool dash = false;
    for (const QChar c : name.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            if (dash && !slug.isEmpty())
                slug += QLatin1Char('-');
            dash = false;
            slug += c;
        } else {
            dash = true;
        }
    }
    return slug.isEmpty() ? QStringLiteral("project") : slug;
}

Result<ProjectManifest> parseProjectManifest(const QByteArray &json, const QString &what) {
    const Result<QJsonDocument> parsed = parseJson(json, what);
    if (!parsed)
        return parsed.errors();
    if (!parsed->isObject())
        return invalid(what, QStringLiteral("it is not a JSON object"));
    const QJsonObject object = parsed->object();
    if (!object.value("format").isDouble() || object.value("format").toInt() != kFormat) {
        if (object.value("format").isDouble())
            return makeError("unsupported_format_version",
                             QStringLiteral("%1 has format %2; this Fugue reads format %3.").arg(what).arg(object.value("format").toInt()).arg(kFormat),
                             QStringLiteral("Update Fugue, or fix the file."));
        return invalid(what, QStringLiteral("\"format\" must be 1"));
    }
    if (!object.value("name").isString())
        return invalid(what, QStringLiteral("\"name\" must be text"));
    ProjectManifest manifest;
    manifest.name = object.value("name").toString();
    if (const QString problem = projectNameProblem(manifest.name); !problem.isEmpty())
        return invalid(what, problem);
    QStringList wanted;
    if (object.contains("inherits")) {
        if (!object.value("inherits").isArray())
            return invalid(what, QStringLiteral("\"inherits\" must be a list of text"));
        for (const QJsonValue &item : object.value("inherits").toArray()) {
            if (!item.isString())
                return invalid(what, QStringLiteral("\"inherits\" must be a list of text"));
            wanted << item.toString();
        }
    }
    Errors warnings;
    sortItems(wanted, &manifest.inherits, &manifest.ignored, &warnings);
    manifest.raw = object;
    return Result<ProjectManifest>(manifest, warnings);
}

QString projectManifestPath(const QString &root) { return root + QStringLiteral("/project.json"); }

Result<ProjectManifest> readProject(const QString &root) {
    QFile file(projectManifestPath(root));
    if (!file.exists())
        return makeError("file_not_found", QStringLiteral("%1 has no project.json.").arg(root),
                         QStringLiteral("`fugue project new <name> --in <folder>` makes a project."));
    if (!file.open(QIODevice::ReadOnly))
        return makeError("permission_denied", QStringLiteral("Cannot read %1.").arg(file.fileName()));
    return parseProjectManifest(file.readAll(), file.fileName());
}

Errors writeProject(const QString &root, const ProjectManifest &manifest) {
    return writeJsonFile(projectManifestPath(root), manifestJson(manifest));
}

QString projectRegistryPath() { return baseConfigDir() + QStringLiteral("/projects.json"); }
bool projectRegistryExists() { return QFileInfo::exists(projectRegistryPath()); }

Result<ProjectRegistry> readProjectRegistry() {
    const QString path = projectRegistryPath();
    if (!QFileInfo::exists(path))
        return ProjectRegistry();
    const Result<QJsonObject> object = readJsonObject(path);
    if (!object)
        return object.errors();
    if (object->value("format").toInt() != kFormat)
        return makeError("unsupported_format_version", QStringLiteral("%1 has format %2; this Fugue reads format %3.").arg(path).arg(object->value("format").toInt()).arg(kFormat));
    ProjectRegistry registry;
    for (const QJsonValue &item : object->value("projects").toArray())
        if (item.isString() && QDir::isAbsolutePath(item.toString())) {
            const QString clean = QDir::cleanPath(item.toString());
            if (!registry.projects.contains(clean))
                registry.projects.append(clean);
        }
    registry.last = object->value("last").isString() ? QDir::cleanPath(object->value("last").toString()) : QString();
    return registry;
}

Errors writeProjectRegistry(const ProjectRegistry &registry) {
    QJsonObject object{{"format", kFormat}, {"projects", QJsonArray::fromStringList(registry.projects)}};
    if (!registry.last.isEmpty())
        object.insert("last", registry.last);
    if (const Errors made = makeFolder(QFileInfo(projectRegistryPath()).path()); hasErrors(made))
        return made;
    return writeJsonFile(projectRegistryPath(), object);
}

QString projectStateName(ProjectState state) {
    switch (state) {
    case ProjectState::Ok:
        return QStringLiteral("ok");
    case ProjectState::Missing:
        return QStringLiteral("missing");
    case ProjectState::Invalid:
        return QStringLiteral("invalid");
    }
    return {};
}

QString ProjectEntry::name() const { return manifest.name.isEmpty() ? QFileInfo(root).fileName() : manifest.name; }

ProjectEntry inspectProject(const QString &given) {
    ProjectEntry entry;
    entry.root = absolute(given);
    if (!QFileInfo(projectManifestPath(entry.root)).isFile()) {
        entry.state = ProjectState::Missing;
        return entry;
    }
    const Result<ProjectManifest> manifest = readProject(entry.root);
    if (!manifest) {
        entry.state = ProjectState::Invalid;
        entry.problems = manifest.errors();
        return entry;
    }
    entry.manifest = *manifest;
    entry.problems = manifest.warnings();
    return entry;
}

Result<QList<ProjectEntry>> listProjects() {
    const Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry)
        return registry.errors();
    QList<ProjectEntry> entries;
    for (const QString &root : registry->projects) {
        ProjectEntry entry = inspectProject(root);
        entry.registered = true;
        entries.append(entry);
    }
    return entries;
}

Result<ProjectEntry> findProject(const QString &nameOrPath) {
    const Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry)
        return registry.errors();
    if (looksLikePath(nameOrPath)) {
        ProjectEntry entry = inspectProject(nameOrPath);
        entry.registered = registry->projects.contains(entry.root);
        if (entry.state == ProjectState::Missing && !entry.registered)
            return makeError("unknown_project", QStringLiteral("%1 is not a project (no project.json).").arg(entry.root),
                             QStringLiteral("`fugue project new <name> --in <folder>` makes one; `--home` runs in a folder that is not."));
        return entry;
    }
    QList<ProjectEntry> matches;
    for (const QString &root : registry->projects) {
        ProjectEntry entry = inspectProject(root);
        entry.registered = true;
        if (entry.name().compare(nameOrPath, Qt::CaseInsensitive) == 0)
            matches.append(entry);
    }
    if (matches.isEmpty())
        return unknownProject(nameOrPath);
    if (matches.size() > 1)
        return makeError("usage", QStringLiteral("More than one project is named \"%1\": %2 and %3.").arg(nameOrPath, matches.at(0).root, matches.at(1).root),
                         QStringLiteral("Name it by its folder, or `fugue project remove <folder>` one."));
    return matches.first();
}

Result<QString> resolveProjectRoot(const QString &nameOrPath) {
    const Result<ProjectEntry> found = findProject(nameOrPath);
    if (!found)
        return found.errors();
    if (found->state == ProjectState::Missing)
        return makeError("file_not_found", QStringLiteral("The project \"%1\" is missing: %2 has no project.json.").arg(found->name(), found->root),
                         QStringLiteral("Mount its drive, or `fugue project remove` it."));
    if (found->state == ProjectState::Invalid)
        return found->problems;
    return found->root;
}

namespace {

// Registers `root` (once) and writes the registry.
Errors registerRoot(const QString &root) {
    Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry)
        return registry.errors();
    if (!registry->projects.contains(root))
        registry->projects.append(root);
    return writeProjectRegistry(*registry);
}

// The registered project, if any, other than the one at `root` that has `name`.
Result<std::optional<QString>> nameTakenBy(const QString &name, const QString &root) {
    const Result<QList<ProjectEntry>> entries = listProjects();
    if (!entries)
        return entries.errors();
    for (const ProjectEntry &entry : *entries)
        if (!samePath(entry.root, root) && entry.name().compare(name, Qt::CaseInsensitive) == 0)
            return std::optional<QString>(entry.root);
    return std::optional<QString>();
}

Error nameTaken(const QString &name, const QString &root) {
    return makeError("project_exists", QStringLiteral("A project named \"%1\" is registered already: %2.").arg(name, root),
                     QStringLiteral("Pick another name, or `fugue project remove` the old one."));
}

} // namespace

Result<ProjectEntry> createProject(const QString &name, const QString &inDir, const QStringList &inherits) {
    if (const QString problem = projectNameProblem(name); !problem.isEmpty())
        return makeError("project_invalid", QStringLiteral("\"%1\" is not a name for a project: %2.").arg(name, problem));
    const Result<std::optional<QString>> taken = nameTakenBy(name, QString());
    if (!taken)
        return taken.errors();
    if (*taken)
        return nameTaken(name, **taken);
    QString root;
    if (!inDir.isEmpty()) {
        root = absolute(inDir);
        if (QFileInfo(projectManifestPath(root)).exists())
            return makeError("project_exists", QStringLiteral("%1 holds a project already.").arg(root),
                             QStringLiteral("`fugue project add %1` registers it.").arg(root));
        if (QFileInfo::exists(root) && !QFileInfo(root).isDir())
            return makeError("project_invalid", QStringLiteral("%1 is not a folder.").arg(root));
    } else {
        const QString base = baseDataDir() + QStringLiteral("/projects/") + projectSlug(name);
        root = base;
        for (int n = 2; QFileInfo::exists(root); ++n)
            root = base + QLatin1Char('-') + QString::number(n);
    }
    if (const Errors made = makeFolder(root); hasErrors(made))
        return made;
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (QString failed; !makeRootFolders(root, &failed))
        return makeError("project_invalid", failed + QStringLiteral("."));
    ProjectManifest manifest;
    manifest.name = name;
    Errors warnings;
    sortItems(inherits, &manifest.inherits, &manifest.ignored, &warnings);
    if (const Errors written = writeProject(root, manifest); hasErrors(written))
        return written;
    if (const Errors registered = registerRoot(root); hasErrors(registered))
        return registered;
    ProjectEntry entry = inspectProject(root);
    entry.registered = true;
    entry.manifest.ignored = manifest.ignored; // what was asked for and left out; the file holds only the rest
    entry.problems = warnings;
    return Result<ProjectEntry>(entry, warnings);
}

Result<ProjectEntry> addProject(const QString &dir) {
    ProjectEntry entry = inspectProject(dir);
    if (entry.state == ProjectState::Missing)
        return makeError("project_invalid", QStringLiteral("%1 is not a project: there is no project.json in it.").arg(entry.root),
                         QStringLiteral("`fugue project new <name> --in %1` makes one.").arg(entry.root));
    if (entry.state == ProjectState::Invalid)
        return entry.problems;
    const Result<std::optional<QString>> taken = nameTakenBy(entry.name(), entry.root);
    if (!taken)
        return taken.errors();
    if (*taken)
        return nameTaken(entry.name(), **taken);
    if (const Errors registered = registerRoot(entry.root); hasErrors(registered))
        return registered;
    entry.registered = true;
    return Result<ProjectEntry>(entry, entry.problems);
}

Result<ProjectEntry> removeProject(const QString &nameOrPath) {
    Result<ProjectEntry> found = findProject(nameOrPath);
    if (!found)
        return found.errors();
    Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry)
        return registry.errors();
    if (!registry->projects.contains(found->root))
        return makeError("unknown_project", QStringLiteral("%1 is not registered.").arg(found->root));
    registry->projects.removeAll(found->root);
    if (samePath(registry->last, found->root))
        registry->last.clear();
    if (const Errors written = writeProjectRegistry(*registry); hasErrors(written))
        return written;
    found->registered = false;
    return found;
}

Result<ProjectEntry> setProjectInherits(const QString &nameOrPath, const QStringList &items) {
    Result<ProjectEntry> found = findProject(nameOrPath);
    if (!found)
        return found.errors();
    if (found->state == ProjectState::Missing)
        return makeError("file_not_found", QStringLiteral("The project \"%1\" is missing: %2 has no project.json.").arg(found->name(), found->root));
    if (found->state == ProjectState::Invalid)
        return found->problems;
    ProjectManifest manifest = found->manifest;
    manifest.inherits.clear();
    manifest.ignored.clear();
    Errors warnings;
    sortItems(items, &manifest.inherits, &manifest.ignored, &warnings);
    if (const Errors written = writeProject(found->root, manifest); hasErrors(written))
        return written;
    ProjectEntry entry = inspectProject(found->root);
    entry.registered = found->registered;
    entry.manifest.ignored = manifest.ignored;
    entry.problems = warnings;
    return Result<ProjectEntry>(entry, warnings);
}

Result<ProjectEntry> renameProject(const QString &nameOrPath, const QString &newName) {
    Result<ProjectEntry> found = findProject(nameOrPath);
    if (!found)
        return found.errors();
    if (found->state == ProjectState::Missing)
        return makeError("file_not_found", QStringLiteral("The project \"%1\" is missing: %2 has no project.json.").arg(found->name(), found->root));
    if (found->state == ProjectState::Invalid)
        return found->problems;
    if (const QString problem = projectNameProblem(newName); !problem.isEmpty())
        return makeError("project_invalid", QStringLiteral("\"%1\" is not a name for a project: %2.").arg(newName, problem));
    const Result<std::optional<QString>> taken = nameTakenBy(newName, found->root);
    if (!taken)
        return taken.errors();
    if (*taken)
        return nameTaken(newName, **taken);
    ProjectManifest manifest = found->manifest;
    manifest.name = newName;
    if (const Errors written = writeProject(found->root, manifest); hasErrors(written))
        return written;
    ProjectEntry entry = inspectProject(found->root);
    entry.registered = found->registered;
    return entry;
}

QProcessEnvironment baseEnvironment(const QProcessEnvironment &current) {
    QProcessEnvironment env = current;
    const bool inRoot = env.contains(QStringLiteral("FUGUE_HOME"));
    for (const char *name : {"CONFIG_HOME", "DATA_HOME", "STATE_HOME", "CACHE_HOME", "RUNTIME_DIR"}) {
        const QString base = QStringLiteral("FUGUE_BASE_XDG_") + QLatin1String(name);
        const QString redirected = QStringLiteral("XDG_") + QLatin1String(name);
        if (env.contains(base))
            env.insert(redirected, env.value(base));
        env.remove(base);
    }
    if (inRoot)
        for (const char *name : {"TMPDIR", "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH"})
            env.remove(QString::fromLatin1(name));
    env.remove(QStringLiteral("FUGUE_HOME"));
    env.remove(QStringLiteral("FUGUE_PROJECT"));
    return env;
}

QStringList sessionInherits() {
    const QString root = fugueHome();
    if (root.isEmpty() || !QFileInfo::exists(projectManifestPath(root)))
        return {};
    const Result<ProjectManifest> manifest = readProject(root);
    return manifest ? manifest->inherits : QStringList();
}

QString lastUsedProjectRoot() {
    const Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry || registry->last.isEmpty())
        return {};
    return inspectProject(registry->last).state == ProjectState::Ok ? registry->last : QString();
}

void recordLastUsedProject() {
    const QString root = fugueHome();
    if (!root.isEmpty() && !rootIsChosenProject())
        return; // FUGUE_HOME or --home: a script's, a test's or a scratch root, never the person's last project
    const bool project = !root.isEmpty() && QFileInfo(projectManifestPath(root)).isFile();
    if (!project && (!root.isEmpty() || !projectRegistryExists()))
        return; // an anonymous --home, or Default with nothing to forget
    Result<ProjectRegistry> registry = readProjectRegistry();
    if (!registry || registry->last == (project ? root : QString()))
        return;
    registry->last = project ? root : QString();
    writeProjectRegistry(*registry);
}

} // namespace fugue
