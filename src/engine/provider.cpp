#include "provider.h"

#include <QTimer>

namespace fugue {

Error providerError(const QString &code, const QString &message, const QString &hint) {
    Error error;
    error.code = code;
    error.message = message;
    error.hint = hint;
    return error;
}

QString CapabilityOffer::status() const {
    bool loading = false;
    for (const OfferedModel &model : models) {
        if (model.status == QLatin1String("ready"))
            return QStringLiteral("ready");
        loading = loading || model.status == QLatin1String("loading");
    }
    return loading ? QStringLiteral("loading") : QStringLiteral("unavailable");
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

void Provider::shutdown(Reply<Nothing> reply) { later(std::move(reply), Outcome<Nothing>{}); }

void Provider::deliverLater(std::function<void()> deliver) {
    QTimer::singleShot(0, this, std::move(deliver));
}

} // namespace fugue
