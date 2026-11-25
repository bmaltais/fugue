#include "provider_module.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "engine/json.h"
#include "engine/paths.h"

namespace fugue {

namespace {

Error invalid(const QString &file, const QString &message) {
    return makeError(QStringLiteral("invalid_document"), QStringLiteral("%1: %2").arg(file, message),
                     QStringLiteral("Fix the provider.json (spec 8.8), or remove the folder."));
}

bool validId(const QString &id) {
    static const QRegularExpression word(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return word.match(id).hasMatch() && !id.contains(QStringLiteral(".."));
}

} // namespace

QString ProviderModule::program() const {
    if (command.isEmpty())
        return {};
    const QString first = command.first();
    // ./server and sub/server are the module's own files; a bare name is looked for on PATH.
    if (!folder.isEmpty() && !QDir::isAbsolutePath(first) && first.contains(QLatin1Char('/')))
        return QDir::cleanPath(QDir(folder).absoluteFilePath(first));
    return first;
}

QStringList ProviderModule::arguments() const { return command.mid(1); }

QString providerModulesDir() { return dataDir() + QStringLiteral("/providers"); }

Result<ProviderModule> parseProviderModule(const QJsonObject &manifest, const QString &folder) {
    const QString file = QDir(folder).filePath(QStringLiteral("provider.json"));
    if (manifest.value("manifest").toInt() != 1)
        return invalid(file, QStringLiteral("\"manifest\" must be 1."));
    ProviderModule module;
    module.folder = QFileInfo(folder).absoluteFilePath();
    module.id = manifest.value("id").toString();
    if (!validId(module.id))
        return invalid(file, QStringLiteral("\"id\" must be letters, digits and . - _ (got \"%1\").").arg(module.id));
    if (QFileInfo(module.folder).fileName() != module.id)
        return invalid(file, QStringLiteral("the folder must be named after the id, %1.").arg(module.id));
    module.version = manifest.value("version").toString();
    if (module.version.isEmpty())
        return invalid(file, QStringLiteral("\"version\" is missing."));
    module.name = manifest.value("name").toString(module.id);
    module.transport = manifest.value("transport").toString();
    if (module.transport != QLatin1String("http") && module.transport != QLatin1String("stdio"))
        return invalid(file, QStringLiteral("\"transport\" must be \"http\" or \"stdio\"."));

    const QJsonObject start = manifest.value("start").toObject();
    for (const QJsonValue &part : start.value("command").toArray())
        module.command.append(part.toString());
    if (start.contains("command") && (!start.value("command").isArray() || module.command.isEmpty() ||
                                      module.command.first().isEmpty()))
        return invalid(file, QStringLiteral("start.command must be a list that begins with the program."));
    module.when = start.value("when").toString(module.command.isEmpty() ? QStringLiteral("never")
                                                                        : QStringLiteral("on_demand"));
    if (module.when != QLatin1String("on_demand") && module.when != QLatin1String("never"))
        return invalid(file, QStringLiteral("start.when must be \"on_demand\" or \"never\"."));
    if (module.when == QLatin1String("never"))
        module.command.clear();
    module.idleTimeoutS = start.value("idle_timeout_s").toInt(module.idleTimeoutS);
    module.startTimeoutS = start.value("start_timeout_s").toInt(module.startTimeoutS);
    // A stdio provider is only ever started by Fugue: its pipes are the transport.
    if (module.transport == QLatin1String("stdio") && module.command.isEmpty())
        return invalid(file, QStringLiteral("a stdio provider needs start.command."));

    module.address = manifest.value("address").toString();
    if (module.transport == QLatin1String("http") && module.address.isEmpty() && module.command.isEmpty())
        return invalid(file, QStringLiteral("an http provider needs an \"address\" or a start.command."));
    if (module.address.contains(QStringLiteral("{port}")) && module.command.isEmpty())
        return invalid(file, QStringLiteral("\"address\" uses {port} but there is no start.command to give it one."));
    for (const QJsonValue &capability : manifest.value("capabilities").toArray())
        module.capabilities.append(capability.toString());
    module.settings = manifest.value("settings").toArray();
    module.needs = manifest.value("needs").toObject();
    return module;
}

QList<ProviderModule> builtinProviderModules() {
    ProviderModule omarchy;
    omarchy.id = QStringLiteral("omarchy");
    omarchy.version = QStringLiteral("1");
    omarchy.name = QStringLiteral("Omarchy model servers");
    omarchy.transport = QStringLiteral("builtin");
    omarchy.when = QStringLiteral("never");
    omarchy.capabilities = {QStringLiteral("image.generate"), QStringLiteral("image.edit"),
                            QStringLiteral("video.generate"), QStringLiteral("audio.generate"), QStringLiteral("audio.adapter"),
                            QStringLiteral("text.generate")};
    ProviderModule http;
    http.id = QStringLiteral("http");
    http.version = QStringLiteral("1");
    http.name = QStringLiteral("HTTP endpoint");
    http.transport = QStringLiteral("builtin");
    http.when = QStringLiteral("never");
    http.capabilities = {QStringLiteral("image.generate"), QStringLiteral("image.edit"), QStringLiteral("text.generate")};
    ProviderModule helper;
    helper.id = QStringLiteral("fugue-helper");
    helper.version = QStringLiteral("1");
    helper.name = QStringLiteral("Fugue helper");
    helper.transport = QStringLiteral("builtin"); // its program is found by helperProgram()
    helper.capabilities = {QStringLiteral("text.encode"), QStringLiteral("latent.sample"),
                           QStringLiteral("latent.decode"), QStringLiteral("latent.encode"),
                           QStringLiteral("image.upscale"), QStringLiteral("image.restore")};
    ProviderModule fal;
    fal.id = QStringLiteral("fal");
    fal.version = QStringLiteral("1");
    fal.name = QStringLiteral("fal.ai");
    fal.transport = QStringLiteral("builtin");
    fal.when = QStringLiteral("never");
    fal.capabilities = {QStringLiteral("image.generate"), QStringLiteral("image.edit")};
    ProviderModule cloudflare;
    cloudflare.id = QStringLiteral("cloudflare");
    cloudflare.version = QStringLiteral("1");
    cloudflare.name = QStringLiteral("Cloudflare Workers AI");
    cloudflare.transport = QStringLiteral("builtin");
    cloudflare.when = QStringLiteral("never");
    cloudflare.capabilities = {QStringLiteral("image.generate"), QStringLiteral("image.edit")};
    return {omarchy, http, helper, fal, cloudflare};
}

Result<QList<ProviderModule>> loadProviderModules(const QString &root) {
    QList<ProviderModule> modules;
    Errors warnings;
    const QStringList reserved = [] {
        QStringList ids;
        for (const ProviderModule &module : builtinProviderModules())
            ids.append(module.id);
        return ids;
    }();
    for (const QFileInfo &entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString path = QDir(entry.absoluteFilePath()).filePath(QStringLiteral("provider.json"));
        if (!QFileInfo::exists(path))
            continue;
        const Result<QJsonObject> manifest = readJsonObject(path);
        Result<ProviderModule> module = manifest ? parseProviderModule(manifest.value(), entry.absoluteFilePath())
                                                 : Result<ProviderModule>(manifest.errors());
        if (module && reserved.contains(module->id)) {
            module = invalid(path, QStringLiteral("%1 is the id of a provider that comes with Fugue.").arg(module->id));
        }
        if (!module) {
            for (Error error : module.errors()) {
                error.severity = Error::Severity::Warning;
                warnings.append(error);
            }
            continue;
        }
        modules.append(module.value());
    }
    return Result<QList<ProviderModule>>(modules, warnings);
}

Result<QList<ProviderModule>> providerModules(const QString &root) {
    const Result<QList<ProviderModule>> installed = loadProviderModules(root);
    QList<ProviderModule> all = builtinProviderModules();
    all.append(installed.value());
    return Result<QList<ProviderModule>>(all, installed.warnings());
}

Result<QList<ProviderModule>> providerModules() { return providerModules(providerModulesDir()); }

} // namespace fugue
