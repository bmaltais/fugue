#include "providers.h"

#include "omarchy_provider.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStandardPaths>

namespace fugue {

namespace {

const QString kProvidersFile = QStringLiteral("~/.config/fugue/providers.json");

Error invalid(const QString &message) {
    return makeError(QStringLiteral("invalid_document"), message,
                         QStringLiteral("Fix %1.").arg(kProvidersFile));
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
    return Result<Selection>(Selection{match.candidate->instance.name,
                                       match.candidate->description->engine, *match.model});
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
        return match.model->status == QLatin1String("ready");
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
                return chosen(match);
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

    for (const Match &match : fitting)
        if (isReady(match))
            return chosen(match);

    // Nothing ready: say what would fix it, the most specific fix first.
    QStringList fixes;
    for (const Match &match : fitting)
        appendUnique(fixes, match.model->hint);
    QString message;
    if (pinned.isEmpty()) {
        message = QStringLiteral("Nothing can serve %1 right now.").arg(requirement.capability);
        appendUnique(fixes, QStringLiteral("Start a model that offers %1, or add a provider to %2.")
                                .arg(requirement.capability, kProvidersFile));
        if (!disabled.isEmpty())
            fixes.append(QStringLiteral("Enable %1 in %2.").arg(either(disabled), kProvidersFile));
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
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/fugue/providers.json");
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
    QFile file(path);
    if (!file.exists())
        return Result<QList<ProviderInstance>>(defaultProviderInstances());
    if (!file.open(QIODevice::ReadOnly))
        return Result<QList<ProviderInstance>>(
            makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(path)));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return Result<QList<ProviderInstance>>(
            makeError(QStringLiteral("invalid_json"),
                      QStringLiteral("%1 is not valid JSON: %2.").arg(path, parseError.errorString()),
                      QStringLiteral("Fix the file, or remove it to use this machine only.")));
    return parseProviderInstances(document.object());
}

QList<ProviderInstance> defaultProviderInstances() {
    ProviderInstance local;
    local.name = QStringLiteral("This machine");
    local.type = QStringLiteral("omarchy");
    return {local};
}

Result<std::shared_ptr<Provider>> createProvider(const ProviderInstance &instance) {
    if (instance.type == QLatin1String("omarchy"))
        return Result<std::shared_ptr<Provider>>(
            std::make_shared<OmarchyProvider>(instance, OmarchyPaths::standard()));
    Error error = makeError(
        QStringLiteral("unknown_provider"),
        QStringLiteral("%1 has the provider type %2, which this Fugue does not have.").arg(instance.name, instance.type),
        QStringLiteral("Change its type in %1.").arg(kProvidersFile));
    error.valid = QJsonArray{"omarchy"};
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
                QStringLiteral("Pin one of the configured providers, or add %1 to %2.").arg(pin, kProvidersFile));
            error.valid = names;
            return failed(error);
        }
        if (!pinned->instance.enabled)
            return failed(makeError(
                QStringLiteral("capability_unavailable"),
                QStringLiteral("The pinned provider %1 is disabled.").arg(pin),
                QStringLiteral("Enable %1 in %2, or remove the pin.").arg(pin, kProvidersFile)));
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
    return choose(usable, requirement, {}, disabled, unreachable);
}

} // namespace fugue
