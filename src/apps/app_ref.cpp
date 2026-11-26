#include "apps/app_ref.h"

#include <QDir>
#include <QFileInfo>

#include "engine/share_package.h"
#include "workflow/workflows.h"

namespace fugue {

namespace {

QString previewIn(const QString &folder) { return previewFilesIn(folder).still; }

std::optional<AppRef> fail(QString *why, const QString &text) {
    if (why)
        *why = text;
    return std::nullopt;
}

} // namespace

std::optional<AppRef> resolveApp(const QString &ref, const QList<TemplateRoot> &roots, const Registry *registry, QString *why) {
    const QFileInfo info(ref);
    if (info.isFile()) {
        AppRef out;
        // A package is told by its bytes (a `.fugueapp` renamed `.fuguepack`, or `.zip`, is the same one, 11.5, #394).
        out.kind = isSharePackage(info.absoluteFilePath()) ? AppRef::Kind::Package : AppRef::Kind::Workflow;
        out.ref = out.file = info.absoluteFilePath();
        out.preview = out.kind == AppRef::Kind::Package ? QString() : previewIn(info.absolutePath());
        return out;
    }
    std::optional<Template> found;
    if (info.isDir()) {
        const QString folder = QDir(info.absoluteFilePath()).absolutePath();
        found = TemplateLibrary({{QFileInfo(folder).absolutePath(), false}}, registry).find(QFileInfo(folder).fileName());
        if (!found)
            return fail(why, QStringLiteral("%1 is not a template folder (it needs a template.json and a workflow).").arg(ref));
        // The catalog's own copy of it is named by its id.
        const std::optional<Template> listed = TemplateLibrary(roots, registry).find(found->id);
        AppRef out;
        out.kind = AppRef::Kind::Template;
        out.ref = listed && QFileInfo(listed->folder).canonicalFilePath() == QFileInfo(folder).canonicalFilePath() ? listed->id : folder;
        out.file = found->workflow;
        out.preview = found->preview;
        out.templ = found;
        return out;
    }
    found = TemplateLibrary(roots, registry).find(ref);
    if (!found)
        return fail(why, QStringLiteral("There is no app %1. Give a template id, a workflow file or a template folder.").arg(ref));
    AppRef out;
    out.kind = AppRef::Kind::Template;
    out.ref = found->id;
    out.file = found->workflow;
    out.preview = found->preview;
    out.templ = found;
    return out;
}

std::optional<QString> appRefFromArgs(int argc, char **argv, const char *option) {
    const QString flag = QString::fromLatin1(option);
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == flag)
            return i + 1 < argc ? QString::fromLocal8Bit(argv[i + 1]) : QString(); // none after it: named, but nothing
        if (arg.startsWith(flag + QLatin1Char('=')))
            return arg.mid(flag.size() + 1);
    }
    return std::nullopt;
}

bool openApp(Workflows &workflows, const AppRef &app) {
    if (app.templ)
        return workflows.openTemplateWithFiles(*app.templ);
    return workflows.openPath(app.file);
}

} // namespace fugue
