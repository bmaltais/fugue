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
// `local` are the document's other hashed models, which are on this machine: the host must have them too, since the nodes that go there read them there.
using HostAvailabilityFn = std::function<HostAvailability(const QList<QJsonObject> &missing, const QList<QJsonObject> &local)>;

} // namespace fugue
