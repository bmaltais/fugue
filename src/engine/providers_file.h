#pragma once

// Writing providers.json (spec 8.2), the other half of loadProviderInstances (providers.h).
// Only the settings panel writes it, and only when the user changes something.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include "engine/providers.h"

namespace fugue {

// The "providers" list as providers.json holds it: name and type always, address and key
// when set, enabled only when false, then the module's own settings.
QJsonArray providerInstancesJson(const QList<ProviderInstance> &instances);

// Writes `instances` as the providers list of `path`, mode 0600 from the first byte and
// atomically (engine/atomic_file), keeping the file's other top-level keys. Nothing is
// written when the list would not load back (parseProviderInstances' errors) or when the
// file there is not a JSON object (its read error): the user's keys are in it.
Errors saveProviderInstances(const QString &path, const QList<ProviderInstance> &instances);

} // namespace fugue
