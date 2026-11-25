#include "apps/app_launcher.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QVariantMap>

#include "apps/app_view.h"
#include "workflow/workflows.h"

namespace fugue {

AppLauncher::AppLauncher(AppView *view, Workflows *workflows, QList<TemplateRoot> roots, const Registry *registry, LauncherEntries entries,
                         Spawn spawn, QObject *parent)
    : QObject(parent), m_view(view), m_workflows(workflows), m_roots(std::move(roots)), m_registry(registry),
      m_entries(std::move(entries)), m_spawn(std::move(spawn)) {
    if (!m_spawn)
        m_spawn = [](const QStringList &arguments) {
            const QString appImage = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPIMAGE"));
            return QProcess::startDetached(appImage.isEmpty() ? QCoreApplication::applicationFilePath() : appImage, arguments);
        };
    connect(view, &AppView::appChanged, this, &AppLauncher::refresh);
    connect(workflows, &Workflows::tabsChanged, this, &AppLauncher::refresh);
    refresh();
}

void AppLauncher::setApp(const AppRef &app) {
    m_fixed = app;
    m_app = app;
    emit changed();
}

void AppLauncher::refresh() {
    if (m_fixed)
        return;
    // What names the shown app: its file, else the template folder it came from. Resolving reads the catalog, so only when
    // that changes (a tab's title or its unsaved mark change tabs() often).
    QString candidate;
    if (m_view->isApp()) {
        const QVariantList tabs = m_workflows->tabs();
        const int index = m_workflows->current();
        if (index >= 0 && index < tabs.size()) {
            const QVariantMap tab = tabs.at(index).toMap();
            candidate = tab.value("path").toString();
            if (candidate.isEmpty())
                candidate = tab.value("template").toString();
        }
    }
    if (candidate == m_candidate)
        return;
    m_candidate = candidate;
    m_app = candidate.isEmpty() ? std::nullopt : resolveApp(candidate, m_roots, m_registry);
    emit changed();
}

void AppLauncher::add() {
    if (!m_app)
        return;
    LauncherApp entry;
    entry.ref = m_app->ref;
    entry.name = m_view->title();
    entry.comment = m_view->description();
    entry.icon = m_app->preview;
    const QString why = m_entries.add(entry);
    emit changed();
    emit notice(why.isEmpty() ? tr("%1 is in your launcher.").arg(entry.name) : why);
}

void AppLauncher::remove() {
    if (!m_app)
        return;
    const QString why = m_entries.remove(m_app->ref);
    emit changed();
    emit notice(why.isEmpty() ? tr("%1 is out of your launcher.").arg(m_view->title()) : why);
}

void AppLauncher::openInFugue() {
    if (m_app && !m_spawn({QStringLiteral("--edit"), m_app->ref}))
        emit notice(tr("Could not start Fugue."));
}

} // namespace fugue
