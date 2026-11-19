#pragma once

// "Did you mean ...?" for error hints (spec 13.3).

#include <QString>
#include <QStringList>

namespace fugue {

// The option closest to a near miss (at most two edits, ignoring case), or an
// empty string when none is close.
QString nearest(const QString &value, const QStringList &options);

} // namespace fugue
