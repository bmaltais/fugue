#pragma once

// Where a model is available from (#164): this machine, or a paired host. Pure: it takes what
// was found (a file on disk, the models the providers offered) and says where each model a
// template names can be had, and whether the template can run now. Never a guess: a place is
// shown only when the disk or a provider reported the model (a handle with a hash is the same
// model by hash, else by resource and name, sameModel()).
//
// "A cloud service" is a model an enabled provider instance that runs off this machine offers (placementOf, never a
// guess from the name or a network call); it is not "this machine" even when the instance has no address.
//
// "This machine" is a file in the model folders, or a model a server on this machine serves;
// "shared host" is a model a paired host serves (ListedModel::host). A host that does not answer
// for its models (not ready) keeps them listed but counts as offline: it never makes a model
// available.

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "engine/model_offers.h"

namespace fugue {

// Where one model is found.
struct ModelPlaces {
    bool local = false;
    QStringList hosts;        // paired hosts that serve it and are answering, in the order offered
    QStringList offlineHosts; // paired hosts that list it but are not answering
    // Services off this machine (a configured provider instance whose placementOf is OffMachine, such as fal.ai) that
    // offer it, whether or not they are ready; `cloudReady` says one answers (has its key).
    QStringList clouds;
    bool cloudReady = false;

    // Can be used right now.
    bool available() const { return local || !hosts.isEmpty() || cloudReady; }
};

// Where the model `handle` is: `onDisk` says a file of it is in the model folders; `offered` are
// the providers' models (listOfferedModels) and the disk's.
ModelPlaces placesOf(const QJsonObject &handle, bool onDisk, const QList<ListedModel> &offered);

// What a template's models add up to.
struct TemplatePlaces {
    bool ready = true;        // every model is available somewhere (true with no models)
    bool local = false;       // some model is on this machine
    QStringList hosts;        // the answering hosts that serve some model, each once
    QStringList offlineHosts; // hosts that list a model not available elsewhere, each once
    QStringList clouds;       // the off-machine services that offer some model, each once
    int missing = 0;          // models available nowhere
};

TemplatePlaces summarizePlaces(const QList<ModelPlaces> &models);

// The glyph names a card or row shows for `places`, in order: "monitor" for this machine,
// "server" for a shared host, "cloud" for an off-machine service (Lucide, share/fonts/lucide-fugue.txt).
QStringList placeGlyphs(bool local, bool host, bool cloud = false);

} // namespace fugue
