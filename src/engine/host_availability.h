#pragma once

// Whether the models a document is missing here are available on a paired host (#735).

#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

namespace fugue {

struct HostAvailability {
    QString host; // a paired host whose fresh, error-free list has every one of the models: they are available there
    QString why;  // else, when a host has some of them: why that does not count ("gx10 has it, but ..."); empty when none does
};
// Asked with the handles that are not in this machine's folders, all of them at once.
using HostAvailabilityFn = std::function<HostAvailability(const QList<QJsonObject> &missing)>;

} // namespace fugue
