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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return prefixed(hash.result());
}

bool isSha256(const QString &text) {
    static const QRegularExpression form(QStringLiteral("^sha256:[0-9a-f]{64}$"));
    return form.match(text).hasMatch();
}

} // namespace fugue
