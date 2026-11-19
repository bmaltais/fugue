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
