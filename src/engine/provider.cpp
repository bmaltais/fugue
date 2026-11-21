#include "provider.h"

#include <QTimer>

namespace fugue {

QString modelStatusName(ModelStatus status) {
    switch (status) {
    case ModelStatus::Ready:
        return QStringLiteral("ready");
    case ModelStatus::Loading:
        return QStringLiteral("loading");
    case ModelStatus::Unavailable:
        break;
    }
    return QStringLiteral("unavailable");
}

ModelStatus parseModelStatus(const QString &text) {
    for (const ModelStatus status : {ModelStatus::Ready, ModelStatus::Loading})
        if (text == modelStatusName(status))
            return status;
    return ModelStatus::Unavailable;
}

ModelStatus CapabilityOffer::status() const {
    bool loading = false;
    for (const OfferedModel &model : models) {
        if (model.status == ModelStatus::Ready)
            return ModelStatus::Ready;
        loading = loading || model.status == ModelStatus::Loading;
    }
    return loading ? ModelStatus::Loading : ModelStatus::Unavailable;
}

const CapabilityOffer *Description::offer(const QString &capability) const {
    for (const CapabilityOffer &offer : capabilities)
        if (offer.capability == capability)
            return &offer;
    return nullptr;
}

namespace {

QJsonObject mergedObject(const QJsonObject &base, const QJsonObject &over) {
    QJsonObject merged = base;
    for (auto it = over.begin(); it != over.end(); ++it)
        merged.insert(it.key(), it.value());
    return merged;
}

OfferedModel parseModel(const QJsonValue &entry, const QJsonObject &capability, const QString &filesTitle) {
    QJsonObject object = entry.isString() ? QJsonObject{{"name", entry.toString()}} : entry.toObject();
    OfferedModel model;
    model.title = object.value("title").toString(filesTitle);
    if (object.value("handle").isObject()) {
        model.handle = object.value("handle").toObject();
    } else {
        for (const char *field : {"name", "hash", "resource"})
            if (!object.value(QLatin1String(field)).toString().isEmpty())
                model.handle.insert(QLatin1String(field), object.value(QLatin1String(field)));
        if (object.value("size").isDouble())
            model.handle.insert("size", object.value("size"));
    }
    if (model.title.isEmpty())
        model.title = model.handle.value("name").toString();
    model.status = parseModelStatus(object.value("status").toString(capability.value("status").toString(QStringLiteral("ready"))));
    model.reason = object.value("reason").toString(capability.value("reason").toString());
    model.hint = object.value("hint").toString(capability.value("hint").toString());
    model.properties = mergedObject(capability.value("properties").toObject(), object.value("properties").toObject());
    model.limits = mergedObject(capability.value("limits").toObject(), object.value("limits").toObject());
    model.defaults = mergedObject(capability.value("defaults").toObject(), object.value("defaults").toObject());
    return model;
}

} // namespace

Description parseDescription(const QJsonObject &reply, const QString &defaultEngine, const QString &filesTitle) {
    Description description;
    description.protocol = reply.value("protocol").toInt(1);
    description.engine = reply.value("engine").toString(defaultEngine);
    // Per host: results are deterministic per machine and build (spec 8.10), so a run on one host
    // never answers from, or for, an entry made by another machine's engine (the cache key has the engine).
    if (const QString host = reply.value("host").toString(); !host.isEmpty())
        description.engine += QLatin1Char('@') + host;
    description.sharesFilesystem = reply.value("shares_filesystem").toBool(false);
    if (reply.value("free_memory_bytes").isDouble())
        description.freeMemoryBytes = qint64(reply.value("free_memory_bytes").toDouble());
    const QJsonValue capabilities = reply.value("capabilities");
    if (capabilities.isArray()) {
        for (const QJsonValue &name : capabilities.toArray()) {
            OfferedModel files;
            files.title = filesTitle;
            files.status = ModelStatus::Ready;
            description.capabilities.append(CapabilityOffer{name.toString(), {files}});
        }
    } else {
        const QJsonObject offered = capabilities.toObject();
        for (auto it = offered.begin(); it != offered.end(); ++it) {
            const QJsonObject capability = it.value().toObject();
            CapabilityOffer offer;
            offer.capability = it.key();
            if (capability.value("models").isArray()) {
                for (const QJsonValue &entry : capability.value("models").toArray())
                    offer.models.append(parseModel(entry, capability, {}));
            } else {
                offer.models.append(parseModel(QJsonObject{}, capability, filesTitle));
            }
            description.capabilities.append(offer);
        }
    }
    return description;
}

bool JobStatus::isTerminal() const {
    return state == QLatin1String("done") || state == QLatin1String("error") ||
           state == QLatin1String("cancelled");
}

Provider::~Provider() = default;

void Provider::shutdown(Reply<Nothing> reply) { later(std::move(reply), Result<Nothing>(Nothing{})); }

void Provider::deliverLater(std::function<void()> deliver) {
    QTimer::singleShot(0, this, std::move(deliver));
}

} // namespace fugue
