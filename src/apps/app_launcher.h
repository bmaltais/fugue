#pragma once

// How the App view's app is started from outside the window (#398, epic #397, spec 12.18): "Add to launcher" and "Remove from
// launcher" (a desktop entry, apps/launcher_entries.h) and "Open in Fugue" (the full window, in a process of its own). One
// object for the page's two buttons, in the normal window's App view and in the app's own window; QML only asks.
//
// The app it works on is the shown tab's: its file when it has one, else the template folder it was opened from (resolveApp
// names it by the template's id when the catalog has it). In an app's own window the app is the one `--app` named, which also
// covers a package. A tab with neither (a new unsaved workflow) is not available: there is nothing a launcher could start.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>
#include <optional>

#include "apps/app_ref.h"
#include "apps/launcher_entries.h"

namespace fugue {

class AppView;
class Workflows;

class AppLauncher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // The shown app can have an entry and be opened in the full window.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // The app has an entry now.
    Q_PROPERTY(bool listed READ listed NOTIFY changed)
    // The shown app is a package (a `.fugueapp` from a file) that is ready to run and not kept yet: Keep this app is offered (#394).
    Q_PROPERTY(bool keepable READ keepable NOTIFY changed)

public:
    using Spawn = std::function<bool(const QStringList &arguments)>;

    // `entries` is where the entries go (a test gives a scratch folder); `spawn` starts a new Fugue with these arguments (the
    // default starts this program, detached).
    AppLauncher(AppView *view, Workflows *workflows, QList<TemplateRoot> roots, const Registry *registry, LauncherEntries entries = LauncherEntries(),
                Spawn spawn = {}, QObject *parent = nullptr);

    bool available() const { return m_app.has_value(); }
    bool listed() const { return m_app && m_entries.has(refOf()); }
    bool keepable() const;

    // The window of an app opened by `--app`: that app, whatever the shown tab says.
    void setApp(const AppRef &app);

    // "Keep this app" (#394): installs the package the shown app was opened from into the person's apps (the apps folder, as a
    // template) and adds its launcher entry, so it starts by name from then on. Offered once the app is ready (the one Get ready
    // sheet has run). The packs it carries were installed by that sheet; the folder keeps them too.
    Q_INVOKABLE void keep();
    Q_INVOKABLE void add();
    Q_INVOKABLE void remove();
    // The full window, with the app's workflow in a tab (`fugue --edit <ref>`).
    Q_INVOKABLE void openInFugue();

signals:
    void changed();
    // One quiet line: a failure, or the entry made or removed.
    void notice(const QString &text);

private:
    void refresh();
    // What the entry is for: the kept app's id once kept, else the app's own ref.
    QString refOf() const { return m_kept.isEmpty() ? m_app->ref : m_kept; }

    AppView *m_view;
    Workflows *m_workflows;
    QList<TemplateRoot> m_roots;
    const Registry *m_registry;
    LauncherEntries m_entries;
    Spawn m_spawn;
    std::optional<AppRef> m_app;
    std::optional<AppRef> m_fixed;
    QString m_candidate;
    QString m_kept; // the template id of the shown package once kept
};

} // namespace fugue
