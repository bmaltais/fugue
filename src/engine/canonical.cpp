#include "canonical.h"

#include "hash.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace fugue {

namespace {

// ECMAScript Number::toString (ECMA-262 7.1.12.1) from the shortest digits
// that round-trip, which Qt's 'e' form gives as "d.ddde+N".
QByteArray number(double value) {
    if (value == 0)
        return "0"; // also minus zero
    QByteArray out;
    if (value < 0) {
        out += '-';
        value = -value;
    }
    const QByteArray scientific = QByteArray::number(value, 'e', QLocale::FloatingPointShortest);
    const qsizetype e = scientific.indexOf('e');
    QByteArray digits = scientific.left(e);
    digits.replace(".", "");
    const int k = int(digits.size());
    const int n = scientific.mid(e + 1).toInt() + 1; // value = 0.digits * 10^n

    if (k <= n && n <= 21) {
        out += digits + QByteArray(n - k, '0');
    } else if (0 < n && n <= 21) {
        out += digits.left(n) + '.' + digits.mid(n);
    } else if (-6 < n && n <= 0) {
        out += "0." + QByteArray(-n, '0') + digits;
    } else {
        out += digits.left(1);
        if (k > 1)
            out += '.' + digits.mid(1);
        out += 'e';
        out += n - 1 >= 0 ? '+' : '-';
        out += QByteArray::number(std::abs(n - 1));
    }
    return out;
}

bool hasLoneSurrogate(const QString &text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            ++i;
        else if (c.isSurrogate())
            return true;
    }
    return false;
}

bool string(QByteArray &out, const QString &text) {
    if (hasLoneSurrogate(text))
        return false;
    static const char hex[] = "0123456789abcdef";
    QString escaped;
    escaped.reserve(text.size() + 2);
    escaped += u'"';
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"': escaped += u"\\\""; break;
        case '\\': escaped += u"\\\\"; break;
        case '\b': escaped += u"\\b"; break;
        case '\t': escaped += u"\\t"; break;
        case '\n': escaped += u"\\n"; break;
        case '\f': escaped += u"\\f"; break;
        case '\r': escaped += u"\\r"; break;
        default:
            if (c.unicode() < 0x20) {
                escaped += u"\\u00";
                escaped += QLatin1Char(hex[c.unicode() >> 4]);
                escaped += QLatin1Char(hex[c.unicode() & 0xf]);
            } else {
                escaped += c;
            }
        }
    }
    escaped += u'"';
    out += escaped.toUtf8();
    return true;
}

bool write(QByteArray &out, const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        out += "null";
        return true;
    case QJsonValue::Bool:
        out += value.toBool() ? "true" : "false";
        return true;
    case QJsonValue::Double: {
        const double d = value.toDouble();
        if (!std::isfinite(d))
            return false;
        out += number(d);
        return true;
    }
    case QJsonValue::String:
        return string(out, value.toString());
    case QJsonValue::Array: {
        out += '[';
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i)
                out += ',';
            if (!write(out, array.at(i)))
                return false;
        }
        out += ']';
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end()); // QString < compares UTF-16 code units
        out += '{';
        bool first = true;
        for (const QString &key : keys) {
            if (!first)
                out += ',';
            first = false;
            if (!string(out, key))
                return false;
            out += ':';
            if (!write(out, object.value(key)))
                return false;
        }
        out += '}';
        return true;
    }
    }
    return false;
}

} // namespace

std::optional<QByteArray> canonicalJson(const QJsonValue &value) {
    QByteArray out;
    if (!write(out, value))
        return std::nullopt;
    return out;
}

QString canonicalHash(const QJsonValue &value) {
    const std::optional<QByteArray> bytes = canonicalJson(value);
    return bytes ? sha256(*bytes) : QString();
}

} // namespace fugue
