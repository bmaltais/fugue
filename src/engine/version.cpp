#include "version.h"

#include <QRegularExpression>

namespace fugue {

std::optional<Version> Version::parse(const QString &text) {
    static const QRegularExpression form(
        QStringLiteral("^(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})$"));
    const QRegularExpressionMatch match = form.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    return Version{match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()};
}

QString Version::toString() const {
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

} // namespace fugue
