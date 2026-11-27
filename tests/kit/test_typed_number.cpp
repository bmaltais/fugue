// Typed numbers (typed_number.h, docs/ui-ergonomics.md "Typed numbers"): a point always, a comma only where the language
// writes the decimal point as one, never a thousands separator.
#include <QRegularExpressionValidator>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include "typed_number.h"

using fugue::TypedNumber;

class TestTypedNumber : public QObject {
    Q_OBJECT

private slots:
    void aCommaIsADecimalPointOnlyWhereTheLanguageWritesOne() {
        const QLocale english(QLocale::English, QLocale::UnitedStates);
        const QLocale danish(QLocale::Danish, QLocale::Denmark);
        QCOMPARE(TypedNumber::read("0.5", english), std::optional<double>(0.5));
        QCOMPARE(TypedNumber::read("1,000", english), std::nullopt); // never 1, never a thousand
        QCOMPARE(TypedNumber::read("0,5", english), std::nullopt);
        QCOMPARE(TypedNumber::read("0,5", danish), std::optional<double>(0.5));
        QCOMPARE(TypedNumber::read("-0,5", danish), std::optional<double>(-0.5));
        QCOMPARE(TypedNumber::read("0.5", danish), std::optional<double>(0.5)); // the point is always one
        QCOMPARE(TypedNumber::read("1.000,5", danish), std::nullopt);           // no thousands separator
        QCOMPARE(TypedNumber::read(" 2e3 ", english), std::optional<double>(2000));
        QCOMPARE(TypedNumber::read("1e999", english), std::nullopt); // too large for a double
        QCOMPARE(TypedNumber::read("1e-400", danish), std::nullopt); // too small
        for (const char *partial : {"", "-", "1e", ".", ","})
            QVERIFY2(!TypedNumber::read(partial, danish), partial);
        QCOMPARE(TypedNumber::normalized("-0,5", danish), QStringLiteral("-0.5"));
        QCOMPARE(TypedNumber::normalized("1,000", english), QStringLiteral("1,000")); // refused: left for the caller to refuse
    }

    // The QML singleton reads the default locale (QLocale::setDefault, which Qt.locale() follows).
    void theSingletonFollowsTheDefaultLocale() {
        const QLocale before;
        const auto restore = qScopeGuard([&] { QLocale::setDefault(before); });
        TypedNumber number;
        QSignalSpy changed(&number, &TypedNumber::patternChanged);
        TypedNumber::localeChanged(); // what the Translator says after a language change
        QCOMPARE(changed.count(), 1);
        QLocale::setDefault(QLocale(QLocale::Danish, QLocale::Denmark));
        QCOMPARE(number.parse("0,5"), 0.5);
        QCOMPARE(number.normalize("-0,5"), QStringLiteral("-0.5"));
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QVERIFY(qIsNaN(number.parse("1,000")));
        QCOMPARE(number.parse("0.5"), 0.5);
    }

    // The validator's pattern: empty and whole numbers are acceptable, a partial number is only intermediate.
    void theFieldsPatternTakesEmptyAndWholeNumbers() {
        const QLocale english(QLocale::English, QLocale::UnitedStates);
        QRegularExpressionValidator validator(TypedNumber::patternFor(english));
        const auto state = [&](QString text) {
            int at = 0;
            return validator.validate(text, at);
        };
        QCOMPARE(state(""), QValidator::Acceptable);
        QCOMPARE(state("-1.5e3"), QValidator::Acceptable);
        QCOMPARE(state("-"), QValidator::Intermediate);
        QCOMPARE(state("1e"), QValidator::Intermediate);
        QCOMPARE(state("1,000"), QValidator::Invalid);
    }
};

QTEST_GUILESS_MAIN(TestTypedNumber)
#include "test_typed_number.moc"
