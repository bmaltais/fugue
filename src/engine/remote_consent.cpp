#include "engine/remote_consent.h"

#include "engine/atomic_file.h"
#include "engine/compiler.h"
#include "engine/cloudflare_provider.h"
#include "engine/fal_provider.h"
#include "engine/gemini_provider.h"
#include "engine/replicate_provider.h"
#include "engine/remote_request.h"
#include "engine/json.h"
#include "engine/node_type.h"
#include "engine/paths.h"
#include "engine/provider_module.h"

#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QUrl>

namespace fugue {

namespace {

// The address as a URL, taking a bare "host:port" for http.
QUrl urlOf(const QString &address) {
    const QString text = address.trimmed();
    return QUrl(text.contains(QLatin1String("://")) ? text : QStringLiteral("http://") + text);
}

bool isLoopbackHost(const QString &host) {
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive))
        return true;
    QHostAddress address;
    return address.setAddress(host) && (address.isLoopback() || address.isNull() || address == QHostAddress::Any || address == QHostAddress::AnyIPv4);
}

QMutex &fileLock() {
    static QMutex mutex;
    return mutex;
}

QJsonObject readInstances(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("instances").toObject();
}

Errors writeInstances(const QString &path, const QJsonObject &instances) {
    return replaceFile(path, writeJson(QJsonObject{{"instances", instances}}), QFileInfo(path).fileName(), FileMode::OwnerOnly);
}

} // namespace

QString effectiveAddress(const ProviderInstance &instance) {
    if (!instance.address.trimmed().isEmpty())
        return instance.address;
    // A provider module's own default address is used when the instance has none (providers.cpp moduleProvider): that is where
    // the data goes. A default with {port} is a loopback port the module starts for itself.
    // fal.ai's queue is the default of a `fal` instance: a key alone must not skip the question (#407).
    if (instance.type == QLatin1String("fal"))
        return falQueueAddress(instance);
    // Google's API is the default of a `gemini` instance: a key alone must not skip the question (#407).
    if (instance.type == QLatin1String("gemini"))
        return geminiAddress(instance);
    if (instance.type == QLatin1String("replicate"))
        return replicateAddress(instance);
    // Cloudflare's API is the default of a `cloudflare` instance, for the same reason.
    if (instance.type == QLatin1String("cloudflare"))
        return cloudflareAddress(instance);
    static const QStringList builtin{QStringLiteral("omarchy"), QStringLiteral("http"), QStringLiteral("fugue-helper")};
    if (builtin.contains(instance.type))
        return {};
    const Result<QList<ProviderModule>> modules = providerModules();
    if (!modules.ok())
        return {};
    for (const ProviderModule &module : modules.value())
        if (module.id == instance.type && module.transport == QLatin1String("http") && !module.address.contains(QStringLiteral("{port}")))
            return module.address;
    return {};
}

QString hostOf(const ProviderInstance &instance) {
    const QString address = effectiveAddress(instance).trimmed();
    return address.isEmpty() ? QString() : urlOf(address).host();
}

namespace {
Placement placementOfHost(const ProviderInstance &instance, const QString &host);
}

Placement placementOf(const ProviderInstance &instance) {
    const Placement placement = placementOfHost(instance, hostOf(instance));
    // A fal instance's key and pictures also go to its storage host: the more remote of the two decides.
    if (instance.type == QLatin1String("fal"))
        return std::max(placement, placementOfHost(instance, urlOf(falRestAddress(instance)).host()));
    return placement;
}

namespace {
Placement placementOfHost(const ProviderInstance &instance, const QString &host) {
    if (host.isEmpty())
        return Placement::ThisMachine; // no address: this machine (a module that runs as a program here, or the model servers of 8.7)
    if (isLoopbackHost(host))
        return Placement::ThisMachine;
    // An omarchy instance is a host omarchy-local-ai paired with (8.7); a fugue-helper has no pairing step, so it is judged by its
    // address like any other service. The person's own network includes their tailnet's *.ts.net names.
    if (instance.type == QLatin1String("omarchy") || isLocalHost(host) || host.endsWith(QLatin1String(".ts.net"), Qt::CaseInsensitive))
        return Placement::OwnHost;
    return Placement::OffMachine;
}
} // namespace

QString consentFingerprint(const ProviderInstance &instance) {
    QString address = effectiveAddress(instance).trimmed().toLower();
    while (address.endsWith(QLatin1Char('/')))
        address.chop(1);
    // The storage host of a fal instance is part of what was agreed to.
    if (instance.type == QLatin1String("fal"))
        address += QLatin1Char('|') + falRestAddress(instance).toLower();
    return instance.type + QLatin1Char('|') + address;
}

QStringList sendsOf(const NodeType &type, const CompiledNode &node) {
    bool pictures = false, masks = false, audio = false, video = false, prompts = false;
    for (const InputSpec &spec : type.inputs) {
        const QString kind = spec.type.type.id;
        const InputValue written = node.inputs.value(spec.name);
        const bool linked = !written.isEmpty() && (written.kind() == InputValue::Kind::Link || written.kind() == InputValue::Kind::List);
        if (kind == QLatin1String("text")) {
            if (spec.choices)
                continue; // a choice is a setting, not something written
            const bool text = !written.isEmpty() && (linked || !written.literal().toString().trimmed().isEmpty());
            const bool defaulted = written.isEmpty() && spec.defaultValue.isString() && !spec.defaultValue.toString().trimmed().isEmpty();
            prompts = prompts || text || defaulted;
        } else if (!written.isEmpty() && kind == QLatin1String("image")) {
            (spec.role == QLatin1String("mask") ? masks : pictures) = true;
        } else if (!written.isEmpty() && kind == QLatin1String("audio")) {
            audio = true;
        } else if (!written.isEmpty() && kind == QLatin1String("video")) {
            video = true;
        }
    }
    QStringList sends;
    if (pictures)
        sends << QStringLiteral("pictures");
    if (masks)
        sends << QStringLiteral("masks");
    if (audio)
        sends << QStringLiteral("audio");
    if (video)
        sends << QStringLiteral("video");
    if (prompts)
        sends << QStringLiteral("prompts");
    return sends;
}

QList<RemoteUse> remoteUses(const QMap<QString, NodeRoute> &routes, const QSet<QString> &runs, const QList<ProviderInstance> &instances) {
    QList<RemoteUse> uses;
    for (const ProviderInstance &instance : instances) {
        if (!instance.enabled || placementOf(instance) != Placement::OffMachine)
            continue;
        RemoteUse use{instance.name, hostOf(instance), consentFingerprint(instance), {}, {}};
        for (auto it = routes.cbegin(); it != routes.cend(); ++it) {
            if (it->instance != instance.name || !runs.contains(it.key()))
                continue;
            use.nodes << it.key();
            for (const QString &kind : it->sends)
                if (!use.sends.contains(kind))
                    use.sends << kind;
        }
        if (!use.nodes.isEmpty())
            uses << use;
    }
    return uses;
}

QList<RemoteUse> notAllowed(const QList<RemoteUse> &uses, const QSet<QString> &allowed) {
    QList<RemoteUse> left;
    for (const RemoteUse &use : uses)
        if (!allowed.contains(use.instance))
            left << use;
    return left;
}

bool anyOffMachine(const QList<ProviderInstance> &instances) {
    for (const ProviderInstance &instance : instances)
        if (instance.enabled && placementOf(instance) == Placement::OffMachine)
            return true;
    return false;
}

QString sendsText(const QStringList &sends) {
    if (sends.isEmpty())
        return QStringLiteral("its inputs");
    if (sends.size() == 1)
        return sends.first();
    return sends.mid(0, sends.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + sends.last();
}

Error consentNeededError(const QList<RemoteUse> &uses) {
    QStringList names, parts;
    for (const RemoteUse &use : uses) {
        names << use.instance;
        parts << QStringLiteral("%1 (%2): %3").arg(use.instance, use.host, sendsText(use.sends));
    }
    Error error = makeError(QStringLiteral("consent_needed"),
                            QStringLiteral("This run would send data off this machine, and you have not allowed that: %1.").arg(parts.join(QStringLiteral("; "))),
                            QStringLiteral("Answer yes when the window asks, or pass --allow-remote <instance> to `fugue run` or `fugue app run` to allow it for that run."));
    for (const QString &name : names)
        error.valid.append(name);
    return error;
}

QString RemoteConsent::defaultPath() { return stateDir() + QStringLiteral("/remote-consent.json"); }

RemoteConsent::RemoteConsent(QString path) : m_path(std::move(path)) {}

bool RemoteConsent::isAllowed(const ProviderInstance &instance) const {
    return readInstances(m_path).value(instance.name).toString() == consentFingerprint(instance);
}

QSet<QString> RemoteConsent::allowedNames(const QList<ProviderInstance> &instances) const {
    return matching(readInstances(m_path), instances);
}

Errors RemoteConsent::allow(const ProviderInstance &instance) { return allow(instance.name, consentFingerprint(instance)); }

Errors RemoteConsent::allow(const QString &name, const QString &fingerprint) {
    QMutexLocker lock(&fileLock());
    QJsonObject kept = readInstances(m_path);
    kept.insert(name, fingerprint);
    return writeInstances(m_path, kept);
}

QSet<QString> RemoteConsent::matching(const QJsonObject &kept, const QList<ProviderInstance> &instances) {
    QSet<QString> names;
    for (const ProviderInstance &instance : instances)
        if (kept.value(instance.name).toString() == consentFingerprint(instance))
            names.insert(instance.name);
    return names;
}

Errors RemoteConsent::prune(const QList<ProviderInstance> &instances) {
    QMutexLocker lock(&fileLock());
    const QJsonObject kept = readInstances(m_path);
    QJsonObject left;
    for (auto it = kept.begin(); it != kept.end(); ++it)
        for (const ProviderInstance &instance : instances)
            if (instance.name == it.key() && consentFingerprint(instance) == it.value().toString())
                left.insert(it.key(), it.value());
    if (left.size() == kept.size())
        return {};
    return writeInstances(m_path, left);
}

} // namespace fugue
