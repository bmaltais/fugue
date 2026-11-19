#pragma once

// Time as the engine takes it: from a clock a test can set, else the system's.

#include <QDateTime>
#include <QString>

#include <functional>

namespace fugue {

using Clock = std::function<QDateTime()>;

// The clock's time in UTC; the system clock's when it is unset.
inline QDateTime utcNow(const Clock &clock = {}) {
    return clock ? clock().toUTC() : QDateTime::currentDateTimeUtc();
}

// A time as run.json and the events write it: ISO 8601, UTC, with milliseconds.
inline QString isoTime(const QDateTime &time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}

} // namespace fugue
