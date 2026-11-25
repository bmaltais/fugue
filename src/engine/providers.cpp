#include "providers.h"

#include "engine/helper_provider.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/queue.h"
#include "engine/remote_helper_provider.h"
#include "omarchy_provider.h"
#include "engine/provider_module.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace fugue {

namespace {


Error invalid(const QString &message) {
    return makeError(QStringLiteral("invalid_document"), message,
                         QStringLiteral("Fix %1.").arg(providersFilePath()));
}

QString modelName(const QJsonObject &handle) {
    const QString name = handle.value("name").toString();
    return name.isEmpty() ? handle.value("hash").toString() : name;
}

bool hasProperties(const OfferedModel &model, const QJsonObject &required) {
    for (auto it = required.begin(); it != required.end(); ++it) {
        const QJsonValue offered = model.properties.value(it.key());
        if (it.value().isArray()) {
            const QJsonArray have = offered.toArray();
            for (const QJsonValue &item : it.value().toArray())
                if (!have.contains(item))
                    return false;
        } else if (offered != it.value()) {
            return false;
        }
    }
    return true;
}

struct Match {
    const Candidate *candidate;
    const OfferedModel *model;
};

// Joins names as "A", "A or B", "A, B or C".
QString either(const QStringList &names) {
    if (names.size() < 2)
        return names.join(QString());
    return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" or ") +
           names.last();
}

void appendUnique(QStringList &list, const QString &item) {
    if (!item.isEmpty() && !list.contains(item))
        list.append(item);
}

Result<Selection> chosen(const Match &match) {
    Selection selection;
    selection.instance = match.candidate->instance.name;
    selection.engine = match.candidate->description->engine;
    selection.model = *match.model;
    selection.sharesFilesystem = match.candidate->description->sharesFilesystem;
    return Result<Selection>(selection);
}

Result<Selection> failed(const Error &error) { return Result<Selection>(error); }

// Selection among instances that are enabled and described. `pinned` names the one pinned
// instance, for the wording; `disabled` and `unreachable` are only for the hint.
Result<Selection> choose(const QList<const Candidate *> &usable, const Requirement &requirement,
                          const QString &pinned, const QStringList &disabled,
                          const QStringList &unreachable) {
    QList<Match> fitting; // models of the capability that have the required properties
    for (const Candidate *candidate : usable) {
        const CapabilityOffer *offer = candidate->description->offer(requirement.capability);
        if (!offer)
            continue;
        for (const OfferedModel &model : offer->models)
            if (hasProperties(model, requirement.properties))
                fitting.append(Match{candidate, &model});
    }
    const auto isReady = [](const Match &match) {
        return match.model->status == ModelStatus::Ready;
    };
    // The ready models that have what the node would like (Requirement::preferred), by "instance:
    // model", which the node names when the one chosen lacks it.
    QStringList preferredOffers;
    for (const Match &match : fitting)
        if (isReady(match) && !requirement.preferred.isEmpty() && hasProperties(*match.model, requirement.preferred))
            appendUnique(preferredOffers, QStringLiteral("%1: %2").arg(match.candidate->instance.name,
                                                                     match.model->title.isEmpty() ? modelName(match.model->handle)
                                                                                                  : match.model->title));
    const auto pick = [&preferredOffers](const Match &match) {
        Result<Selection> selection = chosen(match);
        selection->preferredOffers = preferredOffers;
        return selection;
    };
    QJsonArray readyNames;
    for (const Match &match : fitting)
        if (isReady(match) && !readyNames.contains(modelName(match.model->handle)))
            readyNames.append(modelName(match.model->handle));

    if (!requirement.model.isEmpty()) {
        const QString wanted = modelName(requirement.model);
        QList<Match> same;
        for (const Match &match : fitting)
            if (sameModel(requirement.model, match.model->handle))
                same.append(match);
        for (const Match &match : same)
            if (isReady(match))
                return pick(match);
        if (!same.isEmpty()) {
            const Match &first = same.first();
            QString message = QStringLiteral("%1 is not running on %2.")
                                  .arg(wanted, first.candidate->instance.name);
            if (!first.model->reason.isEmpty())
                message += QLatin1Char(' ') + first.model->reason;
            Error error = makeError(
                QStringLiteral("model_not_running"), message,
                first.model->hint.isEmpty()
                    ? QStringLiteral("Start %1 on %2.").arg(wanted, first.candidate->instance.name)
                    : first.model->hint);
            error.valid = readyNames;
            return failed(error);
        }
        Error error = makeError(
            QStringLiteral("model_missing"),
            pinned.isEmpty()
                ? QStringLiteral("No provider has %1 for %2.").arg(wanted, requirement.capability)
                : QStringLiteral("%1 does not have %2 for %3.")
                      .arg(pinned, wanted, requirement.capability),
            QStringLiteral("Set up %1 on a provider, or pick a model that is there.").arg(wanted));
        error.valid = readyNames;
        return failed(error);
    }

    // Any ready model: one that has what the node prefers, else the first.
    for (const Match &match : fitting)
        if (isReady(match) && hasProperties(*match.model, requirement.preferred))
            return pick(match);
    for (const Match &match : fitting)
        if (isReady(match))
            return pick(match);

    // Nothing ready: say what would fix it, the most specific fix first.
    QStringList fixes;
    for (const Match &match : fitting)
        appendUnique(fixes, match.model->hint);
    QString message;
    if (pinned.isEmpty()) {
        message = QStringLiteral("Nothing can serve %1 right now.").arg(requirement.capability);
        appendUnique(fixes, QStringLiteral("Start a model that offers %1, or add a provider to %2.")
                                .arg(requirement.capability, providersFilePath()));
        if (!disabled.isEmpty())
            fixes.append(QStringLiteral("Enable %1 in %2.").arg(either(disabled), providersFilePath()));
        for (const QString &line : unreachable)
            fixes.append(line);
    } else if (fitting.isEmpty()) {
        message = QStringLiteral("The pinned provider %1 does not offer %2.")
                      .arg(pinned, requirement.capability);
        fixes.append(QStringLiteral("Remove the pin, or pin a provider that offers %1.")
                         .arg(requirement.capability));
    } else {
        message = QStringLiteral("The pinned provider %1 cannot serve %2 right now.")
                      .arg(pinned, requirement.capability);
        // A model that is not ready may give no hint of its own; the error still says
        // how to fix it (8.5).
        if (fixes.isEmpty()) {
            QStringList names;
            for (const Match &match : fitting)
                appendUnique(names, modelName(match.model->handle));
            fixes.append(QStringLiteral("Start %1 on %2, or remove the pin.").arg(either(names), pinned));
        }
    }
    for (const Match &match : fitting)
        if (!match.model->reason.isEmpty() && !message.contains(match.model->reason))
            message += QLatin1Char(' ') + match.model->reason;
    return failed(makeError(QStringLiteral("capability_unavailable"), message,
                                fixes.join(QLatin1Char(' '))));
}

} // namespace

QString providersPath() {
    // $XDG_CONFIG_HOME is honored (1.4), as for every other path of Fugue's own.
    return configDir() + QStringLiteral("/providers.json");
}

Result<QList<ProviderInstance>> parseProviderInstances(const QJsonObject &document) {
    if (!document.value("providers").isArray())
        return Result<QList<ProviderInstance>>(invalid(QStringLiteral("providers.json has no \"providers\" list.")));
    QList<ProviderInstance> instances;
    Errors errors;
    QSet<QString> names;
    int index = 0;
    for (const QJsonValue &entry : document.value("providers").toArray()) {
        ++index;
        const QJsonObject object = entry.toObject();
        ProviderInstance instance;
        instance.name = object.value("name").toString();
        instance.type = object.value("type").toString();
        instance.address = object.value("address").toString();
        instance.key = object.value("key").toString();
        instance.enabled = object.value("enabled").toBool(true);
        for (auto it = object.begin(); it != object.end(); ++it)
            if (!QStringList{"name", "type", "address", "key", "enabled"}.contains(it.key()))
                instance.settings.insert(it.key(), it.value());
        if (instance.name.isEmpty()) {
            errors.append(invalid(QStringLiteral("Provider %1 has no name.").arg(index)));
            continue;
        }
        if (instance.type.isEmpty()) {
            errors.append(invalid(QStringLiteral("Provider %1 has no type.").arg(instance.name)));
            continue;
        }
        if (names.contains(instance.name)) {
            errors.append(
                invalid(QStringLiteral("Two providers are named %1.").arg(instance.name)));
            continue;
        }
        names.insert(instance.name);
        instances.append(instance);
    }
    if (!errors.isEmpty())
        return Result<QList<ProviderInstance>>(errors);
    return Result<QList<ProviderInstance>>(instances);
}

Result<QList<ProviderInstance>> loadProviderInstances(const QString &path) {
    return loadProviderInstances(path, OmarchyPaths::standard());
}

Result<QList<ProviderInstance>> loadProviderInstances(const QString &path, const OmarchyPaths &omarchy) {
    const Result<QJsonObject> document = readJsonObject(path);
    if (document)
        return parseProviderInstances(document.value());
    if (document.error().code == QLatin1String("file_not_found") && !QFileInfo::exists(path))
        return Result<QList<ProviderInstance>>(defaultProviderInstances(omarchy));
    // Unreadable or corrupt is never taken for missing: the user's instances and keys
    // are in there.
    Error error = document.error();
    error.hint = QStringLiteral("Fix the file, or remove it to use this machine only.");
    return Result<QList<ProviderInstance>>(error);
}

QList<ProviderInstance> defaultProviderInstances() { return defaultProviderInstances(OmarchyPaths::standard()); }

QString pairedHostsProblem() { return pairedHostsProblem(OmarchyPaths::standard()); }

QString pairedHostsProblem(const OmarchyPaths &omarchy) {
    const QString path = QDir(omarchy.stateDir).filePath("network.json");
    const Result<QJsonObject> network = readJsonObject(path);
    if (network || (network.error().code == QLatin1String("file_not_found") && !QFileInfo::exists(path)))
        return {};
    return network.error().message;
}

namespace {

// This machine's and each paired host's omarchy instance (8.2).
QList<ProviderInstance> omarchyDefaults(const OmarchyPaths &omarchy) {
    ProviderInstance local;
    local.name = QStringLiteral("This machine");
    local.type = QStringLiteral("omarchy");
    QList<ProviderInstance> instances{local};

    // network.json (omarchy-local-ai agent_local.py survey_network): `hosts` by name, each
    // with `dns` and `paired`. Its models carry the same fields, for a file without hosts.
    const Result<QJsonObject> network = readJsonObject(QDir(omarchy.stateDir).filePath("network.json"));
    if (!network)
        return instances;
    QMap<QString, QString> paired; // host -> dns, in name order
    const auto take = [&paired](const QJsonObject &row) {
        const QString dns = row.value("dns").toString();
        const QString host = row.value("host").toString(dns.section(QLatin1Char('.'), 0, 0));
        if (row.value("paired").toBool() && !host.isEmpty() && !dns.isEmpty() && !paired.contains(host))
            paired.insert(host, dns);
    };
    const QJsonObject hosts = network->value("hosts").toObject();
    for (auto it = hosts.begin(); it != hosts.end(); ++it)
        take(it.value().toObject());
    if (hosts.isEmpty()) {
        const QJsonObject models = network->value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it)
            take(it.value().toObject());
    }
    for (auto it = paired.cbegin(); it != paired.cend(); ++it) {
        if (it.key() == local.name)
            continue;
        ProviderInstance host;
        host.name = it.key();
        host.type = QStringLiteral("omarchy");
        host.address = QStringLiteral("https://") + it.value();
        instances.append(host);
    }
    return instances;
}

} // namespace

QList<ProviderInstance> defaultProviderInstances(const OmarchyPaths &omarchy) {
    QList<ProviderInstance> instances = omarchyDefaults(omarchy);
    // Last: "any ready model" stays the model servers'; only the helper runs models on disk.
    if (!helperProgram(hasNvidiaGpu()).isEmpty()) {
        ProviderInstance helper;
        helper.name = QStringLiteral("Fugue helper");
        helper.type = QStringLiteral("fugue-helper");
        instances.append(helper);
    }
    return instances;
}

Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance) {
    return createProvider(instance, providerModulesDir());
}

namespace {

// What a local helper asks while it idles (spec 1.4.2): does a run of another root wait for the GPU?
std::function<bool()> yieldsToAnotherRoot() {
    return [] { return Lock::otherRootWaiting(gpuLockPath()); };
}

// The provider of an installed module (8.8): the instance's own address wins over the module's
// default; a module Fugue starts is started by its first describe or submit.
std::shared_ptr<Provider> moduleProvider(const ProviderInstance &given, const ProviderModule &module) {
    ProviderInstance instance = given;
    if (!instance.settings.contains("idle_timeout_s"))
        instance.settings.insert("idle_timeout_s", module.idleTimeoutS);
    if (module.transport == QLatin1String("stdio")) {
        StdioLaunch launch;
        launch.program = module.program();
        launch.arguments = module.arguments();
        launch.workingDirectory = module.folder;
        launch.label = module.name;
        launch.hint = QStringLiteral("Check the files of the provider in %1.").arg(module.folder);
        launch.describeOverProtocol = true;
        launch.engine = module.id;
        auto provider = std::make_shared<HelperProvider>(instance, launch);
        provider->setYieldProbe(yieldsToAnotherRoot());
        return provider;
    }
    RemoteProviderOptions options;
    options.where = module.name;
    options.defaultEngine = module.id;
    if (instance.address.isEmpty() && !module.address.contains(QStringLiteral("{port}")))
        instance.address = module.address;
    if (instance.address.isEmpty()) { // the module starts it, on a port of its own
        options.program = module.program();
        options.arguments = module.arguments();
        options.workingDirectory = module.folder;
        options.addressTemplate = module.address;
        options.idleMs = module.idleTimeoutS > 0 ? module.idleTimeoutS * 1000 : 0;
        options.startTimeoutMs = module.startTimeoutS * 1000;
    }
    return std::make_shared<RemoteHelperProvider>(instance, options);
}

} // namespace

Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance, const QString &modulesRoot) {
    if (instance.type == QLatin1String("omarchy"))
        return Result<std::shared_ptr<Provider>>(
            std::make_shared<OmarchyProvider>(instance, OmarchyPaths::standard()));
    if (instance.type == QLatin1String("fugue-helper") && !instance.address.isEmpty())
        return Result<std::shared_ptr<Provider>>(std::make_shared<RemoteHelperProvider>(instance));
    if (instance.type == QLatin1String("fugue-helper")) {
        auto helper = std::make_shared<HelperProvider>(instance, helperProgram(hasNvidiaGpu()));
        helper->setYieldProbe(yieldsToAnotherRoot());
        return Result<std::shared_ptr<Provider>>(helper);
    }
    const Result<QList<ProviderModule>> modules = providerModules(modulesRoot);
    QStringList names;
    for (const ProviderModule &module : modules.value()) {
        if (module.id == instance.type && !module.isBuiltin())
            return Result<std::shared_ptr<Provider>>(moduleProvider(instance, module));
        names.append(module.id);
    }
    names.sort();
    QJsonArray ids;
    for (const QString &name : std::as_const(names))
        ids.append(name);
    QString message = QStringLiteral("%1 has the provider type %2, which this Fugue does not have.").arg(instance.name, instance.type);
    // A module that is there but does not read is why its type is unknown: say so.
    for (const Error &problem : modules.warnings())
        if (problem.message.contains(QStringLiteral("/%1/").arg(instance.type)))
            message += QLatin1Char(' ') + problem.message;
    Error error = makeError(
        QStringLiteral("unknown_provider"), message,
        QStringLiteral("Change its type in %1, or install the module in %2.").arg(providersFilePath(), modulesRoot));
    error.valid = ids;
    return Result<std::shared_ptr<Provider>>(error);
}

bool sameModel(const QJsonObject &requested, const QJsonObject &offered) {
    const QString hash = requested.value("hash").toString();
    if (!hash.isEmpty())
        return offered.value("hash").toString() == hash;
    const QString resource = requested.value("resource").toString();
    const QString offeredResource = offered.value("resource").toString();
    if (!resource.isEmpty() && !offeredResource.isEmpty() && resource != offeredResource)
        return false;
    const QString name = requested.value("name").toString();
    return !name.isEmpty() && offered.value("name").toString() == name;
}

QString pairedHelperName(const QList<ProviderInstance> &instances) {
    for (const ProviderInstance &instance : instances) {
        if (!instance.enabled || instance.address.isEmpty() || instance.type == QLatin1String("omarchy"))
            continue;
        const QString host = QUrl(instance.address).host();
        if (host == QLatin1String("localhost") || host == QLatin1String("::1") || host.startsWith(QLatin1String("127.")))
            continue;
        return instance.name;
    }
    return {};
}

void probePairedHelper(const QList<ProviderInstance> &instances, std::function<void(const QString &host)> done,
                       std::function<Result<std::shared_ptr<Provider>>(const ProviderInstance &)> make) {
    const QString name = pairedHelperName(instances);
    const auto instance = std::find_if(instances.cbegin(), instances.cend(), [&](const ProviderInstance &i) { return i.name == name; });
    if (name.isEmpty() || instance == instances.cend()) {
        QTimer::singleShot(0, [done = std::move(done)] { done({}); });
        return;
    }
    const Result<std::shared_ptr<Provider>> provider = make ? make(*instance) : createProvider(*instance);
    if (!provider) {
        QTimer::singleShot(0, [done = std::move(done)] { done({}); });
        return;
    }
    const std::shared_ptr<Provider> held = provider.value(); // alive until it has answered
    held->describe([held, name, done = std::move(done)](const Result<Description> &described) { done(described.ok() ? name : QString()); });
}

Result<Selection> selectProvider(const QList<Candidate> &candidates, const Requirement &requirement) {
    const QString pin = requirement.nodePin.isEmpty() ? requirement.workflowPin : requirement.nodePin;
    if (!pin.isEmpty()) {
        const Candidate *pinned = nullptr;
        QJsonArray names;
        for (const Candidate &candidate : candidates) {
            names.append(candidate.instance.name);
            if (candidate.instance.name == pin)
                pinned = &candidate;
        }
        if (!pinned) {
            Error error = makeError(
                QStringLiteral("unknown_provider"),
                QStringLiteral("No provider is named %1.").arg(pin),
                QStringLiteral("Pin one of the configured providers, or add %1 to %2.").arg(pin, providersFilePath()));
            error.valid = names;
            return failed(error);
        }
        if (!pinned->instance.enabled)
            return failed(makeError(
                QStringLiteral("capability_unavailable"),
                QStringLiteral("The pinned provider %1 is disabled.").arg(pin),
                QStringLiteral("Enable %1 in %2, or remove the pin.").arg(pin, providersFilePath())));
        if (!pinned->description.ok())
            return Result<Selection>(pinned->description.errors());
        return choose({pinned}, requirement, pin, {}, {});
    }

    QList<const Candidate *> usable;
    QStringList disabled;
    QStringList unreachable;
    for (const Candidate &candidate : candidates) {
        if (!candidate.instance.enabled) {
            disabled.append(candidate.instance.name);
        } else if (!candidate.description.ok()) {
            const QString why = candidate.description.errors().isEmpty()
                                    ? QString()
                                    : candidate.description.errors().first().message;
            unreachable.append(QStringLiteral("%1 is unavailable: %2").arg(candidate.instance.name, why));
        } else {
            usable.append(&candidate);
        }
    }
    if (requirement.modelsElsewhere) {
        QList<const Candidate *> remote;
        for (const Candidate *candidate : std::as_const(usable))
            if (!candidate->description->sharesFilesystem)
                remote.append(candidate);
        if (!remote.isEmpty()) {
            usable = remote;
        } else {
            // A paired helper that does not answer: say so, not the model_missing of a machine that never had the files.
            for (const Candidate &candidate : candidates) {
                if (!candidate.instance.enabled || candidate.description.ok() || candidate.instance.address.isEmpty()
                    || candidate.instance.type == QLatin1String("omarchy"))
                    continue;
                const QString why = candidate.description.errors().isEmpty() ? QString()
                                                                             : candidate.description.errors().first().message;
                return failed(makeError(
                    QStringLiteral("provider_unreachable"),
                    QStringLiteral("The model files are not on this machine, and the paired host %1 is not answering. %2")
                        .arg(candidate.instance.name, why).trimmed(),
                    QStringLiteral("Start the helper on %1, or get the model files onto this machine (Get ready).")
                        .arg(candidate.instance.name)));
            }
        }
    }
    return choose(usable, requirement, {}, disabled, unreachable);
}

} // namespace fugue
