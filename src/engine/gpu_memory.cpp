#include "gpu_memory.h"

#include <QFile>

namespace fugue {

std::optional<qint64> memAvailableMb(const QString &meminfoPath) {
    QFile file(meminfoPath);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    // "MemAvailable:   123456789 kB"
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        const QList<QByteArray> fields = line.mid(int(sizeof("MemAvailable:")) - 1).simplified().split(' ');
        bool ok = false;
        const qint64 kilobytes = fields.value(0).toLongLong(&ok);
        if (!ok || kilobytes < 0 || fields.value(1) != "kB")
            return std::nullopt;
        return kilobytes / 1024;
    }
    return std::nullopt;
}

} // namespace fugue
