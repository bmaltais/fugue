#pragma once

// A number a person types into a field (docs/ui-ergonomics.md "Typed numbers"): digits, an optional sign, a decimal point,
// an exponent. The point is always accepted; a comma is a decimal point only where the language writes it so
// (QLocale::decimalPoint() is ","), and is refused elsewhere, so "1,000" is never read as 1 in English. A thousands separator
// is never accepted. One rule for every typed number of the window: the group editor's and the App builder's ranges, a node's
// number field while it steps, a slider's typed value and the Canvas view's selection fields. QML reads it as the
// `TypedNumber` singleton, on the default locale (Qt.locale()).

#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <optional>

namespace fugue {

class TypedNumber : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // For a RegularExpressionValidator: empty or a number (a partial one, "-" or "1e", is Intermediate, so not acceptable).
    Q_PROPERTY(QRegularExpression pattern READ pattern CONSTANT)

public:
    explicit TypedNumber(QObject *parent = nullptr) : QObject(parent) {}

    // The value of `text` read with `locale`'s decimal point, or none when it is not a whole number by the rule above
    // (empty included).
    static std::optional<double> read(const QString &text, const QLocale &locale);
    // Empty or a number, as `read` takes it.
    static QRegularExpression patternFor(const QLocale &locale);
    // `text` written with a point (a comma read as the decimal point becomes one): what is stored, as String() shows a
    // number in every language. Empty stays empty; text that is not a number is returned as it is (the caller refuses it).
    static QString normalized(const QString &text, const QLocale &locale);

    QRegularExpression pattern() const { return patternFor(QLocale()); }
    // The value, or NaN when the text is not a number.
    Q_INVOKABLE double parse(const QString &text) const;
    Q_INVOKABLE QString normalize(const QString &text) const { return normalized(text, QLocale()); }
};

} // namespace fugue
