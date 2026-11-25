#pragma once

// What `fugue --app <ref>` (and `--edit <ref>`) names (#398, epic #397): a template's id, a workflow file, a template folder or
// a `.fuguepack`. Resolving is only reading the disk and the template catalog; opening is the window's Workflows (an app is a
// tab like any other, shown as the App view). The same words as `fugue app info <id|file>` (src/cli/app.cpp): a path that
// exists wins over an id.

#include <QList>
#include <QString>
#include <optional>

#include "engine/templates.h"

namespace fugue {

class Registry;
class Workflows;

struct AppRef {
    enum class Kind { Template, Workflow, Package };
    Kind kind = Kind::Workflow;
    // What the launcher entry and a second window say to name it: the template's id when the catalog has that folder, else the
    // absolute path.
    QString ref;
    QString file;    // the workflow or package file; for a template its workflow
    QString preview; // a picture of the app, empty when there is none (preview.webp|png|jpg beside the workflow)
    std::optional<Template> templ; // Kind::Template
};

// The app `ref` names, or nothing with `*why` set. `roots` are where templates are (TemplateLibrary::standardRoots()).
std::optional<AppRef> resolveApp(const QString &ref, const QList<TemplateRoot> &roots, const Registry *registry, QString *why = nullptr);

// `--app <ref>` or `--app=<ref>` (also `--edit`, by `option`) in the arguments: nothing when the option is absent, an empty
// string when it has no value.
std::optional<QString> appRefFromArgs(int argc, char **argv, const char *option = "--app");

// Opens it as a tab of `workflows` (a template as a new unnamed tab, as the Templates window does; a file as that file).
// False with the notice the window shows.
bool openApp(Workflows &workflows, const AppRef &app);

} // namespace fugue
