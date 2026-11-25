// The well-known services of Settings > Providers > "Add a service…" (spec 8.2): one table, here and nowhere else. A
// preset is only a starting point for an instance (name, type, address); the person adds the key. Choosing one writes
// the same providers.json entry as typing it in would. Nothing here talks to a network: `keyUrl` is only opened by the
// person's click on "Get a key".
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace fugue {

struct ProviderPreset {
    QString id;          // stable, lowercase: "fal", "openai"
    QString name;        // the instance's first name; a second one is "<name> 2"
    QString group;       // "cloud" (Cloud services) or "own" (Your own machines)
    QString type;        // a provider module id
    QString address;     // empty: the type's own default, or one the person must fill (needsAddress)
    bool needsKey = false;
    bool needsAddress = false; // the address is the person's own, so the card opens with it focused
    QString keyUrl;      // where the service hands out a key; empty for none
    QString description; // one line, translated
    QString privacy;     // what leaves the machine, translated; empty for a service on the person's own network
    QStringList keywords; // model families the search finds it by (spec 8.7's default lists), never translated
    bool freeTier = false; // the service has a free allowance; the search finds it by "free tier"
};

// The word for where a service runs: "cloud", "network" or anything else (this machine), translated.
QString providerPlacementText(const QString &placement);
// The rows in the order the menu shows them (Cloud services, then Your own machines).
QList<ProviderPreset> knownProviderPresets();
// The row with this id; an empty id in the result when there is none.
ProviderPreset providerPresetById(const QString &id);
// The rows as QML reads them: the fields above plus `groupTitle` and `placementText` (translated; where it runs: Cloud,
// This machine or Your network).
QVariantList providerPresetsVariant();

} // namespace fugue
