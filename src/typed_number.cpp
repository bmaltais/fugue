#include "typed_number.h"

#include <limits>

namespace fugue {

namespace {

bool commaIsDecimal(const QLocale &locale) { return locale.decimalPoint() == QStringLiteral(","); }

QString numberPattern(const QLocale &locale) {
    const QString point = commaIsDecimal(locale) ? QStringLiteral("[.,]") : QStringLiteral("\\.");
    return QStringLiteral("[-+]?(\\d+(%1\\d*)?|%1\\d+)([eE][-+]?\\d+)?").arg(point);
}

} // namespace

QRegularExpression TypedNumber::patternFor(const QLocale &locale) {
    return QRegularExpression(QStringLiteral("^(|%1)$").arg(numberPattern(locale)));
}

std::optional<double> TypedNumber::read(const QString &text, const QLocale &locale) {
    const QString trimmed = text.trimmed();
    const QRegularExpression whole(QStringLiteral("^%1$").arg(numberPattern(locale)));
    if (!whole.match(trimmed).hasMatch())
        return std::nullopt;
    bool ok = false;
    const double value = QString(trimmed).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

QString TypedNumber::normalized(const QString &text, const QLocale &locale) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !read(trimmed, locale))
        return trimmed;
    return QString(trimmed).replace(QLatin1Char(','), QLatin1Char('.'));
}

double TypedNumber::parse(const QString &text) const {
    return read(text, QLocale()).value_or(std::numeric_limits<double>::quiet_NaN());
}

} // namespace fugue
