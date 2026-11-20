#include "hash.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>

namespace fugue {

namespace {

QString prefixed(const QByteArray &digest) {
    return QStringLiteral("sha256:") + QString::fromLatin1(digest.toHex());
}

} // namespace

QString sha256(QByteArrayView bytes) {
    return prefixed(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
}

QString sha256File(const QString &path) {
    return sha256File(path, [](qint64, qint64) { return true; });
}

QString sha256File(const QString &path, const HashProgress &progress) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const qint64 total = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(qsizetype(1) << 20, Qt::Uninitialized);
    qint64 done = 0;
    if (progress && !progress(0, total))
        return {};
    for (;;) {
        const qint64 read = file.read(chunk.data(), chunk.size());
        if (read < 0)
            return {};
        if (read == 0)
            break;
        hash.addData(QByteArrayView(chunk.constData(), read));
        done += read;
        if (progress && !progress(done, total))
            return {};
    }
    return prefixed(hash.result());
}

bool isSha256(const QString &text) {
    static const QRegularExpression form(QStringLiteral("^sha256:[0-9a-f]{64}$"));
    return form.match(text).hasMatch();
}

} // namespace fugue
