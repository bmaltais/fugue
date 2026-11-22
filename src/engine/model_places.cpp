#include "engine/model_places.h"

#include "engine/providers.h"

namespace fugue {

ModelPlaces placesOf(const QJsonObject &handle, bool onDisk, const QList<ListedModel> &offered) {
    ModelPlaces places;
    places.local = onDisk;
    for (const ListedModel &model : offered) {
        if (!sameModel(handle, model.handle))
            continue;
        if (model.host.isEmpty()) {
            // A local server counts when it answers (a file on disk is ready).
            if (model.status != QLatin1String("unavailable"))
                places.local = true;
            continue;
        }
        QStringList &list = model.hostOffline() ? places.offlineHosts : places.hosts;
        if (!list.contains(model.host))
            list << model.host;
    }
    return places;
}

TemplatePlaces summarizePlaces(const QList<ModelPlaces> &models) {
    TemplatePlaces out;
    for (const ModelPlaces &model : models) {
        if (!model.available())
            ++out.missing;
        out.local = out.local || model.local;
        for (const QString &host : model.hosts)
            if (!out.hosts.contains(host))
                out.hosts << host;
        if (!model.available())
            for (const QString &host : model.offlineHosts)
                if (!out.offlineHosts.contains(host))
                    out.offlineHosts << host;
    }
    out.ready = out.missing == 0;
    return out;
}

QStringList placeGlyphs(bool local, bool host) {
    QStringList glyphs;
    if (local)
        glyphs << QStringLiteral("monitor");
    if (host)
        glyphs << QStringLiteral("server");
    return glyphs;
}

} // namespace fugue
