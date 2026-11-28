#include "engine/host_models.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include "engine/paths.h"
#include "engine/remote_helper_provider.h"

namespace fugue {

namespace {
bool isHex64(const QString &text) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(text).hasMatch();
}
} // namespace

QJsonObject HostModel::toJson() const {
    return QJsonObject{{"name", name}, {"size", double(size)}, {"sha256", sha256.isEmpty() ? QJsonValue() : QJsonValue(sha256)},
                       {"folder", folder}, {"family", family.isEmpty() ? QJsonValue() : QJsonValue(family)}};
}

HostModel HostModel::fromJson(const QJsonObject &object) {
    HostModel model;
    model.name = object.value("name").toString();
    model.size = qint64(object.value("size").toDouble());
    const QString sha = object.value("sha256").toString().toLower();
    model.sha256 = isHex64(sha) ? sha : QString();
    model.folder = object.value("folder").toString();
    model.family = object.value("family").toString();
    return model;
}

const HostModel *HostModelList::byHash(const QString &hash) const {
    QString want = hash.toLower();
    if (want.startsWith(QLatin1String("sha256:")))
        want = want.mid(7);
    if (!isHex64(want))
        return nullptr;
    for (const HostModel &model : models)
        if (model.sha256 == want)
            return &model;
    return nullptr;
}

QJsonObject HostModelList::toJson() const {
    QJsonArray array;
    for (const HostModel &model : models)
        array.append(model.toJson());
    return QJsonObject{{"host", host},
                       {"supported", supported},
                       {"models", array},
                       {"fetched_at", fetchedAt.isValid() ? QJsonValue(fetchedAt.toString(Qt::ISODate)) : QJsonValue()},
                       {"error", error},
                       {"truncated", truncated},
                       {"unhashed", unhashed},
                       {"hashing", hashing}};
}

HostModelList HostModelList::fromJson(const QJsonObject &object) {
    HostModelList list;
    list.host = object.value("host").toString();
    list.supported = object.value("supported").toBool(true);
    for (const QJsonValue &value : object.value("models").toArray())
        if (const HostModel model = HostModel::fromJson(value.toObject()); !model.name.isEmpty())
            list.models.append(model);
    list.fetchedAt = QDateTime::fromString(object.value("fetched_at").toString(), Qt::ISODate);
    list.error = object.value("error").toString();
    list.truncated = object.value("truncated").toBool();
    list.unhashed = object.value("unhashed").toInt();
    list.hashing = object.value("hashing").toInt();
    return list;
}

HostModelList parseHostModels(const QString &host, const QJsonObject &reply, const QDateTime &now) {
    HostModelList list = HostModelList::fromJson(reply);
    list.host = host;
    list.supported = true;
    list.fetchedAt = now;
    list.error.clear();
    return list;
}

HostModelCache::HostModelCache(QString folder) : m_folder(std::move(folder)) {}

HostModelCache &HostModelCache::shared() {
    static HostModelCache cache;
    return cache;
}

QString HostModelCache::pathOf(const ProviderInstance &instance) const {
    const QString folder = m_folder.isEmpty() ? stateDir() + QStringLiteral("/host_models") : m_folder;
    const QByteArray id = QCryptographicHash::hash((instance.name + QLatin1Char('\n') + instance.address).toUtf8(), QCryptographicHash::Sha256);
    return folder + QLatin1Char('/') + QString::fromLatin1(id.toHex().left(24)) + QStringLiteral(".json");
}

HostModelList HostModelCache::cached(const ProviderInstance &instance) const {
    HostModelList list;
    list.host = instance.name;
    QFile file(pathOf(instance));
    if (!file.open(QIODevice::ReadOnly))
        return list;
    list = HostModelList::fromJson(QJsonDocument::fromJson(file.readAll()).object());
    list.host = instance.name;
    return list;
}

bool HostModelCache::stale(const HostModelList &list, int ttlSeconds, const QDateTime &now) {
    return !list.fetchedAt.isValid() || list.fetchedAt.secsTo(now) >= ttlSeconds;
}

void HostModelCache::keep(const ProviderInstance &instance, const HostModelList &list) const {
    const QString path = pathOf(instance);
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(list.toJson()).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

void HostModelCache::refresh(const ProviderInstance &instance, bool hash, const QStringList &names, Done done, const Make &make) {
    const QString key = pathOf(instance);
    if (!isPairedHelper(instance) || !instance.enabled) {
        QTimer::singleShot(0, [done = std::move(done), list = cached(instance)] { done(list); });
        return;
    }
    if (done)
        m_waiting[key].append(std::move(done));
    if (m_busy.contains(key))
        return;
    m_busy.insert(key);
    Result<std::shared_ptr<Provider>> made = make ? make(instance) : createProvider(instance);
    const auto finish = [this, key](const HostModelList &list) {
        m_busy.remove(key);
        const QList<Done> waiting = m_waiting.take(key);
        for (const Done &wait : waiting)
            wait(list);
    };
    const std::shared_ptr<Provider> provider = made ? made.value() : nullptr;
    auto *remote = dynamic_cast<RemoteHelperProvider *>(provider.get());
    if (!remote) {
        HostModelList list = cached(instance);
        list.error = made ? QStringLiteral("not a helper on a paired host") : made.error().message;
        QTimer::singleShot(0, [finish, list] { finish(list); });
        return;
    }
    remote->listModels(hash, names, [this, instance, provider, finish](const Result<QJsonObject> &answer) {
        HostModelList list = cached(instance);
        if (answer) {
            list = parseHostModels(instance.name, answer.value(), QDateTime::currentDateTimeUtc());
        } else if (answer.error().code == QLatin1String("not_found")) {
            // A helper from before the operation: its models are unknown, which is not the same as none.
            list = HostModelList();
            list.host = instance.name;
            list.supported = false;
            list.fetchedAt = QDateTime::currentDateTimeUtc();
        } else {
            list.error = answer.error().message; // the last good list stays, with when it was fetched
        }
        keep(instance, list);
        // Not from inside the provider's own reply.
        QTimer::singleShot(0, [finish, list, provider] { finish(list); });
    });
}

QStringList HostModelCache::hostsHavingAll(const QList<ProviderInstance> &instances, const QStringList &hashes) const {
    QStringList hosts;
    if (hashes.isEmpty())
        return hosts;
    for (const ProviderInstance &instance : instances) {
        if (!instance.enabled || !isPairedHelper(instance))
            continue;
        const HostModelList list = cached(instance);
        if (list.supported && std::all_of(hashes.cbegin(), hashes.cend(), [&list](const QString &hash) { return list.byHash(hash) != nullptr; }))
            hosts << instance.name;
    }
    return hosts;
}

QString HostModelCache::hostHaving(const QList<ProviderInstance> &instances, const QJsonObject &handle) const {
    const QString hash = handle.value("hash").toString();
    if (hash.isEmpty())
        return {};
    const QStringList hosts = hostsHavingAll(instances, {hash});
    return hosts.isEmpty() ? QString() : hosts.first();
}

std::function<QString(const QJsonObject &)> HostModelCache::hostHavingFunction() const {
    QList<ProviderInstance> paired;
    if (const Result<QList<ProviderInstance>> loaded = loadProviderInstances(providersPath()); loaded.ok())
        for (const ProviderInstance &instance : loaded.value())
            if (instance.enabled && isPairedHelper(instance))
                paired.append(instance);
    if (paired.isEmpty())
        return {};
    return [this, paired](const QJsonObject &handle) { return hostHaving(paired, handle); };
}

void HostModelCache::refreshStale(const QList<ProviderInstance> &instances, int ttlSeconds, const Done &changed, const Make &make) {
    for (const ProviderInstance &instance : instances)
        if (instance.enabled && isPairedHelper(instance) && stale(cached(instance), ttlSeconds))
            refresh(instance, false, {}, changed, make);
}

} // namespace fugue
