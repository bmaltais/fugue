#include "engine/canvas_guides.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fugue::canvas {

namespace {

Result<GuideSet> bad(const QString &what) {
    return Result<GuideSet>(makeError(QStringLiteral("canvas_corrupt"), QStringLiteral("The guides file of this canvas is not valid: %1.").arg(what),
                                       QStringLiteral("Guides are not part of the canvas's history. Delete guides.json in the canvas's folder, or put the guides back from the Canvas view.")));
}

// An integer in [low, high] from a JSON number; false for anything else (a string, 1.5, NaN).
bool intIn(const QJsonValue &value, int low, int high, int *out) {
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) || d < low || d > high)
        return false;
    *out = int(d);
    return true;
}

} // namespace

QByteArray guidesToJson(const GuideSet &set) {
    std::vector<Guide> sorted = set.guides;
    std::sort(sorted.begin(), sorted.end(), [](const Guide &a, const Guide &b) { return a.vertical != b.vertical ? a.vertical : a.at < b.at; });
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    QJsonArray lines;
    for (const Guide &guide : sorted)
        lines.append(QJsonObject{{"axis", guide.vertical ? "x" : "y"}, {"at", guide.at}});
    return QJsonDocument(QJsonObject{{"format", guidesFormat}, {"grid", std::clamp(set.grid, minGridSpacing, maxGridSpacing)}, {"guides", lines}})
        .toJson(QJsonDocument::Indented);
}

Result<GuideSet> guidesFromJson(const QByteArray &bytes) {
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        return bad(QStringLiteral("it is not JSON"));
    const QJsonObject root = doc.object();
    int format = 0;
    if (!intIn(root.value("format"), 1, 1000000, &format))
        return bad(QStringLiteral("it has no format number"));
    if (format != guidesFormat)
        return bad(QStringLiteral("format %1 is not one this version reads").arg(format));
    GuideSet set;
    if (root.contains("grid") && !intIn(root.value("grid"), minGridSpacing, maxGridSpacing, &set.grid))
        return bad(QStringLiteral("the grid spacing must be a whole number from %1 to %2").arg(minGridSpacing).arg(maxGridSpacing));
    const QJsonValue lines = root.value("guides");
    if (!lines.isUndefined() && !lines.isArray())
        return bad(QStringLiteral("\"guides\" is not a list"));
    if (lines.toArray().size() > maxGuides)
        return bad(QStringLiteral("more than %1 guides").arg(maxGuides));
    for (const QJsonValue &entry : lines.toArray()) {
        const QString axis = entry.toObject().value("axis").toString();
        Guide guide;
        if (!entry.isObject() || (axis != QLatin1String("x") && axis != QLatin1String("y")) || !intIn(entry.toObject().value("at"), -maxGuidePosition, maxGuidePosition, &guide.at))
            return bad(QStringLiteral("a guide needs an axis (\"x\" or \"y\") and a whole-number position"));
        guide.vertical = axis == QLatin1String("x");
        if (std::find(set.guides.begin(), set.guides.end(), guide) == set.guides.end())
            set.guides.push_back(guide);
    }
    return set;
}

} // namespace fugue::canvas
