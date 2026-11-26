#pragma once

// The projects dialog's model (spec 1.4.2, epic #204 step 2 slice 6): the registered projects as the window
// shows them, making, adding, renaming and unregistering one, and switching to one.
//
// Switching starts a NEW process, `fugue --project <path>`, because the root must be set before Qt starts and
// helpers and packs keep their environment. The new window is started through a launcher the tests replace
// (a fake records the program, the arguments and the environment); the real one is QProcess::startDetached,
// with the ordinary session's environment back (project.h baseEnvironment()). Then this window closes through
// the normal flow; with unsaved work it stays open and says so (`unsavedWork`), and openInNewWindow() starts
// the other window without closing this one. Nothing here ever deletes a folder.
//
// QML only asks: every call checks what it is given and returns why not as a sentence, empty when done.

#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

namespace fugue {

class CommandRegistry;
class Workflows;

// What a launcher starts.
struct LaunchSpec {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};
// Starts a program detached; false when it could not be started.
using ProjectLauncher = std::function<bool(const LaunchSpec &)>;
// The real one: QProcess::startDetached.
ProjectLauncher detachedLauncher();

class ProjectModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // One map per project, the Default first: root (empty for the Default), name, path (~ for the home
    // folder), isDefault, current, state (ok, missing or invalid), stateText (what a person reads for a
    // state that is not ok), problems (lines), inherits (words), registered.
    Q_PROPERTY(QVariantList projects READ projects NOTIFY changed)
    // The project this window runs in: its name, empty for the Default project; its root, empty for the
    // Default; and what it inherits.
    Q_PROPERTY(QString currentName READ currentName NOTIFY changed)
    Q_PROPERTY(QString currentRoot READ currentRoot NOTIFY changed)
    Q_PROPERTY(bool currentIsDefault READ currentIsDefault NOTIFY changed)
    Q_PROPERTY(QStringList currentInherits READ currentInherits NOTIFY changed)
    // The project chip: its text ("Default project" or the name) and whether it is shown. The Default shows
    // it only once there is another project to mix it up with.
    Q_PROPERTY(QString indicator READ indicator NOTIFY changed)
    Q_PROPERTY(bool indicatorShown READ indicatorShown NOTIFY changed)
    // What a project can inherit: {id, title, description}, in the spec's order, and the ones a new project has.
    Q_PROPERTY(QVariantList inheritChoices READ inheritChoices CONSTANT)
    Q_PROPERTY(QStringList defaultInherits READ defaultInherits CONSTANT)
    // Why projects.json cannot be read; empty when fine.
    Q_PROPERTY(QString registryProblem READ registryProblem NOTIFY changed)
    // True while a switch waits for the unsaved work of this window to be saved or sent to a new window.
    Q_PROPERTY(bool unsavedWork READ unsavedWork NOTIFY changed)

public:
    explicit ProjectModel(ProjectLauncher launcher = detachedLauncher(), QObject *parent = nullptr);

    // What this window can say about itself: `unsavedWork` is true while any workflow has unsaved changes,
    // `closeWindow` closes the window through the normal flow (the quit command).
    void setWindow(std::function<bool()> unsavedWork, std::function<void()> closeWindow);
    // The window title names the project (`<workflow> — <project> — Fugue`, the Default unnamed) and follows a rename.
    void bindTitle(Workflows &workflows);
    // file.projects opens the dialog (the window's, Main.qml).
    void registerCommands(CommandRegistry &registry);

    QVariantList projects() const { return m_projects; }
    QString currentName() const { return m_currentName; }
    QString currentRoot() const { return m_currentRoot; }
    bool currentIsDefault() const { return m_currentRoot.isEmpty(); }
    QStringList currentInherits() const { return m_currentInherits; }
    QString indicator() const;
    bool indicatorShown() const;
    QVariantList inheritChoices() const;
    QStringList defaultInherits() const;
    QString registryProblem() const { return m_registryProblem; }
    bool unsavedWork() const { return m_unsaved ? m_unsaved() : false; }

    // Reads the registry and the current project again.
    Q_INVOKABLE void refresh();
    // Where a project of this name goes when no folder is chosen, ~ for the home folder.
    Q_INVOKABLE QString defaultLocation(const QString &name) const;
    // The folder a new project of this name gets inside `parentFolder` (a path or a folder picker's URL; empty:
    // the default place): <parent>/<slug>, with -2, -3 added while that exists. Nothing is made; create() makes
    // it, with its parents. ~ for the home folder.
    Q_INVOKABLE QString projectPath(const QString &name, const QVariant &parentFolder) const;
    // Makes and registers a project. `folder` is its root (a path or a folder picker's URL), empty for the
    // default place; `inherits` the words it takes from the Default. {error, root}: error is empty when done.
    Q_INVOKABLE QVariantMap create(const QString &name, const QVariant &folder, const QStringList &inherits);
    // Registers the project in a folder (a path or a folder picker's URL).
    Q_INVOKABLE QString addFolder(const QVariant &folder);
    // Takes a project out of the list; its folder and files stay. The Default is not in the registry.
    Q_INVOKABLE QString remove(const QString &root);
    // The current project's name and what it inherits. The Default has no settings of its own.
    Q_INVOKABLE QString setCurrent(const QString &name, const QStringList &inherits);
    // Starts the window of the project at `root` (empty: the Default) and closes this one. With unsaved work
    // nothing starts: the dialog asks (`switchBlocked`), and openInNewWindow() is the other way.
    Q_INVOKABLE QString switchTo(const QString &root);
    // Starts the window and leaves this one open.
    Q_INVOKABLE QString openInNewWindow(const QString &root);

signals:
    void changed();
    // A switch to `root` (named `name`) was asked while this window has unsaved work.
    void switchBlocked(const QString &root, const QString &name);

private:
    QString launch(const QString &root);
    QString nameOfRoot(const QString &root) const;

    ProjectLauncher m_launcher;
    std::function<bool()> m_unsaved;
    std::function<void()> m_close;
    QVariantList m_projects;
    QString m_currentName;
    QString m_currentRoot;
    QStringList m_currentInherits;
    QString m_registryProblem;
};

} // namespace fugue
