#include "settings/project_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

#include "commands/command_registry.h"
#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

// The message of the first error (the hints name command line steps, which a window has no use for).
QString firstProblem(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.message;
    return errors.isEmpty() ? QString() : errors.first().message;
}

// A folder from the dialog: typed text, or the folder picker's file:// URL.
QString folderText(const QVariant &value) {
    if (value.typeId() == QMetaType::QUrl)
        return value.toUrl().toLocalFile();
    const QString text = value.toString().trimmed();
    return text.startsWith(QLatin1String("file:")) ? QUrl(text).toLocalFile() : text;
}

} // namespace

ProjectLauncher detachedLauncher() {
    return [](const LaunchSpec &spec) {
        QProcess process;
        process.setProgram(spec.program);
        process.setArguments(spec.arguments);
        process.setProcessEnvironment(spec.environment);
        return process.startDetached();
    };
}

ProjectModel::ProjectModel(ProjectLauncher launcher, QObject *parent) : QObject(parent), m_launcher(std::move(launcher)) {
    refresh();
}

void ProjectModel::setWindow(std::function<bool()> unsavedWork, std::function<void()> closeWindow) {
    m_unsaved = std::move(unsavedWork);
    m_close = std::move(closeWindow);
}

void ProjectModel::bindTitle(Workflows &workflows) {
    const auto follow = [this, &workflows] { workflows.setProjectName(m_currentName); };
    connect(this, &ProjectModel::changed, &workflows, follow);
    follow();
}

void ProjectModel::registerCommands(CommandRegistry &registry) {
    registry.setHandler(QStringLiteral("file.projects"),
                        [&registry] { emit registry.windowRequested(QStringLiteral("file.projects")); });
}

QString ProjectModel::indicator() const { return m_currentRoot.isEmpty() ? tr("Default project") : m_currentName; }

bool ProjectModel::indicatorShown() const { return !m_currentRoot.isEmpty() || m_registered > 0; }

QVariantList ProjectModel::inheritChoices() const {
    // The words of project.json, with what each hands to the project (spec 1.4.2).
    const QMap<QString, std::pair<QString, QString>> text{
        {QStringLiteral("models"),
         {tr("Models"), tr("The Default project's model folders and model catalog, read-only. No second copy on disk.")}},
        {QStringLiteral("packs"),
         {tr("Packs"), tr("The Default project's node packs, with the trust you gave them, read-only. A pack still runs from the Default's folder.")}},
        {QStringLiteral("templates"), {tr("Templates"), tr("The templates you saved in the Default project, read-only.")}},
        {QStringLiteral("prompts"),
         {tr("Prompts"), tr("The Default project's prompt templates. They can hold text of other clients, so this is off unless you choose it.")}},
        {QStringLiteral("preferences"),
         {tr("Preferences"), tr("Canvas navigation, language, corner radius, hover magnifier, new seeds, embedded workflow, reopening tabs and your keys.")}},
    };
    QVariantList list;
    for (const QString &id : inheritableItems())
        list.append(QVariantMap{{"id", id}, {"title", text.value(id).first}, {"description", text.value(id).second}});
    return list;
}

QStringList ProjectModel::defaultInherits() const { return fugue::defaultInherits(); }

QString ProjectModel::nameOfRoot(const QString &root) const {
    return root.isEmpty() ? tr("Default project") : inspectProject(root).name();
}

void ProjectModel::refresh() {
    m_projects.clear();
    m_registryProblem.clear();
    m_registered = 0;
    const QString current = fugueHome();
    // The current project's own entry, whether registered or not (a project started by path).
    m_currentRoot.clear();
    m_currentName.clear();
    m_currentInherits.clear();
    if (!current.isEmpty()) {
        const ProjectEntry entry = inspectProject(current);
        m_currentRoot = entry.root;
        m_currentName = entry.name();
        m_currentInherits = entry.state == ProjectState::Ok ? entry.manifest.inherits : QStringList();
    }
    m_projects.append(QVariantMap{{"root", QString()}, {"name", tr("Default project")}, {"path", tilde(baseDataDir())},
                                  {"isDefault", true}, {"current", current.isEmpty()}, {"state", QStringLiteral("ok")},
                                  {"stateText", QString()}, {"problems", QStringList()}, {"inherits", QStringList()},
                                  {"registered", true}});
    bool currentListed = current.isEmpty();
    const Result<QList<ProjectEntry>> entries = listProjects();
    if (!entries) {
        m_registryProblem = firstProblem(entries.errors());
    } else {
        m_registered = int(entries->size());
        for (const ProjectEntry &entry : *entries) {
            const bool isCurrent = !current.isEmpty() && QDir::cleanPath(entry.root) == QDir::cleanPath(current);
            currentListed = currentListed || isCurrent;
            QStringList problems;
            for (const Error &error : entry.problems)
                problems << error.message;
            const QString state = projectStateName(entry.state);
            m_projects.append(QVariantMap{
                {"root", entry.root}, {"name", entry.name()}, {"path", tilde(entry.root)}, {"isDefault", false},
                {"current", isCurrent}, {"state", state},
                {"stateText", entry.state == ProjectState::Missing ? tr("Missing: its folder or project.json is not there (a drive that is not mounted?)")
                              : entry.state == ProjectState::Invalid ? tr("Cannot be used: %1").arg(problems.value(0))
                                                                     : QString()},
                {"problems", problems}, {"inherits", entry.manifest.inherits}, {"registered", true}});
        }
    }
    if (!currentListed) {
        const ProjectEntry entry = inspectProject(current);
        m_projects.append(QVariantMap{{"root", entry.root}, {"name", entry.name()}, {"path", tilde(entry.root)},
                                      {"isDefault", false}, {"current", true}, {"state", QStringLiteral("ok")},
                                      {"stateText", tr("Not in your list: started from its folder")}, {"problems", QStringList()},
                                      {"inherits", entry.manifest.inherits}, {"registered", false}});
    }
    emit changed();
}

QString ProjectModel::defaultLocation(const QString &name) const {
    return tilde(baseDataDir() + QStringLiteral("/projects/") + projectSlug(name.isEmpty() ? QStringLiteral("project") : name));
}

QVariantMap ProjectModel::create(const QString &name, const QVariant &folder, const QStringList &inherits) {
    const Result<ProjectEntry> made = createProject(name.trimmed(), expandHome(folderText(folder)), inherits);
    if (!made)
        return {{"error", firstProblem(made.errors())}, {"root", QString()}};
    refresh();
    return {{"error", QString()}, {"root", made->root}};
}

QString ProjectModel::addFolder(const QVariant &folder) {
    const QString text = folderText(folder);
    if (text.isEmpty())
        return tr("Choose the folder of the project.");
    const Result<ProjectEntry> added = addProject(expandHome(text));
    if (!added)
        return firstProblem(added.errors());
    refresh();
    return {};
}

QString ProjectModel::remove(const QString &root) {
    if (root.isEmpty())
        return tr("The Default project is not in the list; there is nothing to remove.");
    const Result<ProjectEntry> removed = removeProject(root);
    if (!removed)
        return firstProblem(removed.errors());
    refresh();
    return {};
}

QString ProjectModel::setCurrent(const QString &name, const QStringList &inherits) {
    if (m_currentRoot.isEmpty())
        return tr("The Default project is the ordinary Fugue: it has no name or inherits to change.");
    if (name.trimmed() != m_currentName) {
        const Result<ProjectEntry> renamed = renameProject(m_currentRoot, name.trimmed());
        if (!renamed)
            return firstProblem(renamed.errors());
    }
    if (inherits != m_currentInherits) {
        const Result<ProjectEntry> set = setProjectInherits(m_currentRoot, inherits);
        if (!set)
            return firstProblem(set.errors());
    }
    refresh();
    return {};
}

QString ProjectModel::launch(const QString &root) {
    if (!root.isEmpty()) {
        const Result<QString> usable = resolveProjectRoot(root);
        if (!usable)
            return firstProblem(usable.errors());
    }
    LaunchSpec spec;
    spec.program = QCoreApplication::applicationFilePath();
    // `default` names the ordinary session: without it the new window would open the last project it ran in.
    spec.arguments = {QStringLiteral("--project"), root.isEmpty() ? QStringLiteral("default") : root};
    spec.environment = baseEnvironment(QProcessEnvironment::systemEnvironment());
    if (!m_launcher || !m_launcher(spec))
        return tr("Fugue could not start a window for %1.").arg(nameOfRoot(root));
    return {};
}

QString ProjectModel::switchTo(const QString &root) {
    if (QDir::cleanPath(root) == QDir::cleanPath(m_currentRoot) && (root.isEmpty() == m_currentRoot.isEmpty()))
        return tr("This window is already in %1.").arg(nameOfRoot(root));
    if (unsavedWork()) {
        if (!root.isEmpty())
            if (const Result<QString> usable = resolveProjectRoot(root); !usable)
                return firstProblem(usable.errors());
        emit switchBlocked(root, nameOfRoot(root));
        return {};
    }
    if (const QString problem = launch(root); !problem.isEmpty())
        return problem;
    if (m_close)
        m_close();
    return {};
}

QString ProjectModel::openInNewWindow(const QString &root) { return launch(root); }

} // namespace fugue
