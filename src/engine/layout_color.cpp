#include "engine/layout_color.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace fugue {

QString normalizeHex(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral("^#?([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return {};
    QString digits = match.captured(1).toLower();
    if (digits.size() == 3) {
        QString wide;
        for (const QChar c : digits)
            wide += QString(2, c);
        digits = wide;
    }
    return QLatin1Char('#') + digits;
}

bool isHexColor(const QString &text) {
    return text.startsWith(QLatin1Char('#')) && text.size() == 7 && normalizeHex(text) == text.toLower();
}

Hsv hsvFromHex(const QString &hex) {
    const QString normal = normalizeHex(hex);
    if (normal.isEmpty())
        return {};
    const auto channel = [&normal](int at) { return normal.mid(at, 2).toInt(nullptr, 16) / 255.0; };
    const double r = channel(1), g = channel(3), b = channel(5);
    const double top = std::max({r, g, b}), bottom = std::min({r, g, b}), span = top - bottom;
    Hsv out;
    out.v = top;
    out.s = top > 0 ? span / top : 0;
    if (span > 0) {
        if (top == r)
            out.h = 60 * std::fmod((g - b) / span, 6.0);
        else if (top == g)
            out.h = 60 * ((b - r) / span + 2);
        else
            out.h = 60 * ((r - g) / span + 4);
        if (out.h < 0)
            out.h += 360;
    }
    return out;
}

QString hexFromHsv(const Hsv &hsv) {
    const double s = std::clamp(hsv.s, 0.0, 1.0), v = std::clamp(hsv.v, 0.0, 1.0);
    double h = std::fmod(hsv.h, 360.0);
    if (h < 0)
        h += 360;
    const double chroma = v * s;
    const double x = chroma * (1 - std::abs(std::fmod(h / 60, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    switch (int(h / 60)) {
    case 0: r = chroma; g = x; break;
    case 1: r = x; g = chroma; break;
    case 2: g = chroma; b = x; break;
    case 3: g = x; b = chroma; break;
    case 4: r = x; b = chroma; break;
    default: r = chroma; b = x; break;
    }
    const double m = v - chroma;
    const auto byte = [m](double c) { return int(std::lround((c + m) * 255)); };
    return QStringLiteral("#%1%2%3")
        .arg(byte(r), 2, 16, QLatin1Char('0'))
        .arg(byte(g), 2, 16, QLatin1Char('0'))
        .arg(byte(b), 2, 16, QLatin1Char('0'));
}

RecentColors::RecentColors(QString path, int limit) : m_path(std::move(path)), m_limit(limit) {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QString color = value.isString() ? normalizeHex(value.toString()) : QString();
        if (!color.isEmpty() && !m_colors.contains(color) && m_colors.size() < m_limit)
            m_colors.append(color);
    }
}

void RecentColors::use(const QString &color) {
    const QString normal = normalizeHex(color);
    if (normal.isEmpty())
        return;
    m_colors.removeAll(normal);
    m_colors.prepend(normal);
    while (m_colors.size() > m_limit)
        m_colors.removeLast();
    QDir().mkpath(QFileInfo(m_path).path());
    QSaveFile file(m_path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonArray::fromStringList(m_colors)).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

} // namespace fugue
