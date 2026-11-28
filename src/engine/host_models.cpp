#include "engine/host_models.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutexLocker>
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

namespace {
// What a host says is data from another machine: a name that could step out of a folder, or carry control characters into a terminal or a
// tooltip, is dropped; a text is cut to a length.
bool safeName(const QString &name) {
    if (name.isEmpty() || name.size() > 512 || name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1Char('\\')) || name.contains(QLatin1Char('\\')))
        return false;
    for (const QChar c : name)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return false;
    for (const QString &part : name.split(QLatin1Char('/')))
        if (part == QLatin1String("..") || part == QLatin1String("."))
            return false;
    return true;
}
QString plain(QString text, int most) {
    QString out;
    for (const QChar c : text.left(most))
        out += c.unicode() < 0x20 || c.unicode() == 0x7f ? QLatin1Char(' ') : c;
    return out;
}
constexpr int kMostModels = 4000;
constexpr qint64 kMostListBytes = 8 << 20;
} // namespace

HostModel HostModel::fromJson(const QJsonObject &object) {
    HostModel model;
    model.name = safeName(object.value("name").toString()) ? object.value("name").toString() : QString();
    model.size = qint64(object.value("size").toDouble());
    const QString sha = object.value("sha256").toString().toLower();
    model.sha256 = isHex64(sha) ? sha : QString();
    model.folder = model.name.contains(QLatin1Char('/')) ? model.name.section(QLatin1Char('/'), 0, 0) : QString(); // from the name, not the host's say
    model.family = plain(object.value("family").toString(), 80);
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
                       {"attempted_at", attemptedAt.isValid() ? QJsonValue(attemptedAt.toString(Qt::ISODate)) : QJsonValue()},
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
    for (const QJsonValue &value : object.value("models").toArray()) {
        if (list.models.size() >= kMostModels) {
            list.truncated = true;
            break;
        }
        if (const HostModel model = HostModel::fromJson(value.toObject()); !model.name.isEmpty())
            list.models.append(model);
    }
    list.fetchedAt = QDateTime::fromString(object.value("fetched_at").toString(), Qt::ISODate);
    list.attemptedAt = QDateTime::fromString(object.value("attempted_at").toString(), Qt::ISODate);
    list.error = plain(object.value("error").toString(), 300);
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
    list.attemptedAt = now;
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
    const QString path = pathOf(instance);
    const QFileInfo info(path);
    QMutexLocker lock(&m_parsedMutex);
    const auto kept = m_parsed.constFind(path); // the GUI asks often: the file is parsed again only when it changed
    if (kept != m_parsed.constEnd() && kept->mtime == info.lastModified() && kept->size == info.size())
        return kept->list;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return list;
    list = HostModelList::fromJson(QJsonDocument::fromJson(file.read(kMostListBytes)).object());
    list.host = instance.name;
    m_parsed.insert(path, {info.lastModified(), info.size(), list});
    return list;
}

bool HostModelCache::stale(const HostModelList &list, int ttlSeconds, const QDateTime &now) {
    const QDateTime last = list.attemptedAt.isValid() ? list.attemptedAt : list.fetchedAt;
    return !last.isValid() || last.secsTo(now) >= ttlSeconds;
}

void HostModelCache::keep(const ProviderInstance &instance, const HostModelList &list) const {
    const QString path = pathOf(instance);
    {
        QMutexLocker lock(&m_parsedMutex);
        m_parsed.remove(path);
    }
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
        QTimer::singleShot(0, [done = std::move(done), list = cached(instance)] {
            if (done)
                done(list);
        });
        return;
    }
    if (m_busy.contains(key)) {
        if (hash) { // a request to hash comes after the refresh that is out, not instead of it
            Later &later = m_later[key];
            for (const QString &name : names)
                if (!later.names.contains(name))
                    later.names << name;
            if (done)
                later.dones.append(std::move(done));
            if (later.names.isEmpty())
                later.names << QString(); // hash with no names: the host's own order
        } else if (done) {
            m_waiting[key].append(std::move(done));
        }
        return;
    }
    if (done)
        m_waiting[key].append(std::move(done));
    m_busy.insert(key);
    Result<std::shared_ptr<Provider>> made = make ? make(instance) : createProvider(instance);
    const auto finish = [this, key, instance, make](const HostModelList &list) {
        m_busy.remove(key);
        const QList<Done> waiting = m_waiting.take(key);
        for (const Done &wait : waiting)
            wait(list);
        if (m_later.contains(key)) {
            Later later = m_later.take(key);
            later.names.removeAll(QString());
            const QList<Done> dones = later.dones;
            refresh(instance, true, later.names, [dones](const HostModelList &again) {
                for (const Done &wait : dones)
                    wait(again);
            }, make);
        }
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
        list.attemptedAt = QDateTime::currentDateTimeUtc();
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

HostAvailability HostModelCache::availability(const QList<ProviderInstance> &instances, const QList<QJsonObject> &missing, const QDateTime &now) const {
    QStringList hashes;
    for (const QJsonObject &handle : missing)
        hashes << handle.value("hash").toString();
    HostAvailability answer;
    if (hashes.isEmpty() || hashes.contains(QString()))
        return answer; // a handle with no hash names no file a host could be asked for
    for (const ProviderInstance &instance : instances) {
        if (!instance.enabled || !isPairedHelper(instance))
            continue;
        const HostModelList list = cached(instance);
        if (!list.supported || !list.known())
            continue;
        int have = 0;
        for (const QString &hash : hashes)
            have += list.byHash(hash) ? 1 : 0;
        if (have == 0)
            continue;
        const QString why = !list.error.isEmpty()
                                ? QObject::tr("%1 has it, but its list could not be refreshed (%2).").arg(instance.name, list.error)
                            : list.fetchedAt.secsTo(now) >= kUsableSeconds
                                ? QObject::tr("%1 has it, but its list is older than 10 minutes.").arg(instance.name)
                            : have < hashes.size()
                                ? QObject::tr("%1 has it, but not the other models these nodes need.").arg(instance.name)
                                : QString();
        if (why.isEmpty()) {
            answer.host = instance.name;
            answer.why.clear();
            return answer;
        }
        if (answer.why.isEmpty())
            answer.why = why;
    }
    return answer;
}

HostAvailabilityFn HostModelCache::availabilityFunction() const {
    QList<ProviderInstance> paired;
    if (const Result<QList<ProviderInstance>> loaded = loadProviderInstances(providersPath()); loaded.ok())
        for (const ProviderInstance &instance : loaded.value())
            if (instance.enabled && isPairedHelper(instance))
                paired.append(instance);
    if (paired.isEmpty())
        return {};
    return [this, paired](const QList<QJsonObject> &missing) { return availability(paired, missing); };
}

void HostModelCache::refreshStale(const QList<ProviderInstance> &instances, int ttlSeconds, const Done &changed, const Make &make) {
    for (const ProviderInstance &instance : instances)
        if (instance.enabled && isPairedHelper(instance) && stale(cached(instance), ttlSeconds))
            refresh(instance, false, {}, changed, make);
}

} // namespace fugue
