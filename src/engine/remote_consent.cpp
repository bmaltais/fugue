#include "engine/remote_consent.h"

#include "engine/atomic_file.h"
#include "engine/compiler.h"
#include "engine/http_provider.h"
#include "engine/json.h"
#include "engine/node_type.h"
#include "engine/paths.h"

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

QString hostOf(const ProviderInstance &instance) { return instance.address.trimmed().isEmpty() ? QString() : urlOf(instance.address).host(); }

Placement placementOf(const ProviderInstance &instance) {
    const QString host = hostOf(instance);
    const bool paired = instance.type == QLatin1String("omarchy") || instance.type == QLatin1String("fugue-helper");
    if (host.isEmpty())
        return Placement::ThisMachine; // no address: this machine (a module that runs as a program here, or the model servers of 8.7)
    if (isLoopbackHost(host))
        return Placement::ThisMachine;
    if (paired || isLocalHost(host))
        return Placement::OwnHost;
    return Placement::OffMachine;
}

QString consentFingerprint(const ProviderInstance &instance) {
    QString address = instance.address.trimmed().toLower();
    while (address.endsWith(QLatin1Char('/')))
        address.chop(1);
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
        RemoteUse use{instance.name, hostOf(instance), {}, {}};
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
                            QStringLiteral("Run again with --allow-remote <instance> to allow it for this run, or answer yes when the window asks."));
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
    const QJsonObject kept = readInstances(m_path);
    QSet<QString> names;
    for (const ProviderInstance &instance : instances)
        if (kept.value(instance.name).toString() == consentFingerprint(instance))
            names.insert(instance.name);
    return names;
}

Errors RemoteConsent::allow(const ProviderInstance &instance) {
    QMutexLocker lock(&fileLock());
    QJsonObject kept = readInstances(m_path);
    kept.insert(instance.name, consentFingerprint(instance));
    return writeInstances(m_path, kept);
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
