#include "typed_number.h"

#include <QList>

#include <cmath>
#include <limits>

namespace fugue {

namespace {

QList<TypedNumber *> &instances() {
    static QList<TypedNumber *> all;
    return all;
}

bool commaIsDecimal(const QLocale &locale) { return locale.decimalPoint() == QStringLiteral(","); }

QString numberPattern(const QLocale &locale) {
    const QString point = commaIsDecimal(locale) ? QStringLiteral("[.,]") : QStringLiteral("\\.");
    return QStringLiteral("[-+]?(\\d+(%1\\d*)?|%1\\d+)([eE][-+]?\\d+)?").arg(point);
}

} // namespace

TypedNumber::TypedNumber(QObject *parent) : QObject(parent) { instances().append(this); }

TypedNumber::~TypedNumber() { instances().removeOne(this); }

void TypedNumber::localeChanged() {
    for (TypedNumber *each : std::as_const(instances()))
        emit each->patternChanged();
}

QRegularExpression TypedNumber::patternFor(const QLocale &locale) {
    return QRegularExpression(QStringLiteral("^(|%1)$").arg(numberPattern(locale)));
}

std::optional<double> TypedNumber::read(const QString &text, const QLocale &locale) {
    const QString trimmed = text.trimmed();
    const QRegularExpression whole(QStringLiteral("^%1$").arg(numberPattern(locale)));
    if (!whole.match(trimmed).hasMatch())
        return std::nullopt;
    // C-locale reading of the point form: false (and no number) on overflow and underflow too.
    bool ok = false;
    const double value = QString(trimmed).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    return ok && std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
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
