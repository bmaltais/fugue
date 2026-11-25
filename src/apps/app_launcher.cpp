#include "apps/app_launcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QVariantMap>

#include "apps/app_view.h"
#include "engine/share_package.h"
#include "engine/templates.h"
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
    connect(workflows, &Workflows::noticeChanged, this, &AppLauncher::changed); // ready or not: Keep this app follows it
    refresh();
}

void AppLauncher::setApp(const AppRef &app) {
    m_kept.clear();
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
            if (candidate.isEmpty())
                candidate = tab.value("package").toString(); // an app bundle opened from a file (#394)
        }
    }
    if (candidate == m_candidate)
        return;
    m_candidate = candidate;
    m_kept.clear();
    m_app = candidate.isEmpty() ? std::nullopt : resolveApp(candidate, m_roots, m_registry);
    emit changed();
}

void AppLauncher::add() {
    if (!m_app)
        return;
    LauncherApp entry;
    entry.ref = refOf();
    entry.name = m_view->title();
    entry.comment = m_view->description();
    entry.icon = m_app->preview;
    const QString why = m_entries.add(entry);
    emit changed();
    emit notice(why.isEmpty() ? tr("%1 is in your launcher.").arg(entry.name) : why);
}

bool AppLauncher::keepable() const {
    return m_app && m_app->kind == AppRef::Kind::Package && m_kept.isEmpty() && m_view->isApp() && !m_workflows->missingSomething();
}

void AppLauncher::keep() {
    if (!keepable())
        return;
    const QString root = TemplateLibrary::ownFolder();
    QDir().mkpath(root);
    // Unpacked beside its place, so naming it there is a rename and never a copy of half a folder.
    QTemporaryDir scratch(root + QStringLiteral("/.keep-XXXXXX"));
    if (!scratch.isValid()) {
        emit notice(tr("Could not keep %1: %2").arg(m_view->title(), scratch.errorString()));
        return;
    }
    const Result<QString> unpacked = unpackSharePackage(m_app->file, scratch.path());
    if (!unpacked) {
        emit notice(tr("Could not keep %1: %2").arg(m_view->title(), unpacked.errors().first().message));
        return;
    }
    const QString base = QFileInfo(unpacked.value()).fileName();
    QString id = base;
    for (int n = 2; QFileInfo::exists(QDir(root).filePath(id)); ++n)
        id = base + QLatin1Char('-') + QString::number(n);
    if (!QDir().rename(unpacked.value(), QDir(root).filePath(id))) {
        emit notice(tr("Could not keep %1: its folder could not be put in %2.").arg(m_view->title(), root));
        return;
    }
    const std::optional<Template> kept = TemplateLibrary({{root, true}}, m_registry).find(id);
    if (!kept) {
        QDir(QDir(root).filePath(id)).removeRecursively();
        emit notice(tr("Could not keep %1: it is not an app this Fugue can open.").arg(m_view->title()));
        return;
    }
    LauncherApp entry;
    entry.ref = id;
    entry.name = m_view->title();
    entry.comment = m_view->description();
    entry.icon = kept->preview;
    if (const QString why = m_entries.add(entry); !why.isEmpty()) {
        emit notice(why);
        return;
    }
    m_kept = id;
    emit changed();
    emit notice(tr("%1 is kept: it is in your apps and your launcher.").arg(entry.name));
}

void AppLauncher::remove() {
    if (!m_app)
        return;
    const QString why = m_entries.remove(refOf());
    emit changed();
    emit notice(why.isEmpty() ? tr("%1 is out of your launcher.").arg(m_view->title()) : why);
}

void AppLauncher::openInFugue() {
    if (m_app && !m_spawn({QStringLiteral("--edit"), m_app->ref}))
        emit notice(tr("Could not start Fugue."));
}

} // namespace fugue
