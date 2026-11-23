#include "engine/canvas_layer_source.h"

#include <QJsonArray>

#include <cmath>

#include "engine/canonical.h"
#include "engine/paint_file.h"

namespace fugue::canvas {

namespace {

Error corrupt(const QString &what) {
    return makeError(QStringLiteral("canvas_corrupt"), QStringLiteral("The canvas store is damaged: %1.").arg(what),
                     QStringLiteral("The files under the store folder are written once and never changed; restore them from a backup, or export the canvas from another copy."));
}

double rounded(double value) { return std::round(value * 1000.0) / 1000.0; }

QJsonObject tilesToJson(const TileMap &tiles) {
    QJsonObject out;
    for (const auto &[key, hash] : tiles)
        out.insert(key.text(), hash);
    return out;
}

bool isHash(const QString &text) {
    if (!text.startsWith(QLatin1String("sha256:")) || text.size() != 7 + 64)
        return false;
    for (int i = 7; i < text.size(); ++i)
        if (!QStringLiteral("0123456789abcdef").contains(text[i]))
            return false;
    return true;
}

bool tilesFromJson(const QJsonObject &from, int width, int height, TileMap *to) {
    for (auto it = from.begin(); it != from.end(); ++it) {
        TileKey key;
        if (!TileKey::parse(it.key(), &key) || key.x >= tilesAcross(width) || key.y >= tilesDown(height) || !isHash(it.value().toString()))
            return false;
        to->emplace(key, it.value().toString());
    }
    return true;
}

} // namespace

QString filterName(ResampleFilter filter) {
    return filter == ResampleFilter::Nearest ? QStringLiteral("nearest") : filter == ResampleFilter::Bicubic ? QStringLiteral("bicubic") : QStringLiteral("lanczos");
}

bool filterFromName(const QString &name, ResampleFilter *filter) {
    if (name == QLatin1String("lanczos"))
        *filter = ResampleFilter::Lanczos;
    else if (name == QLatin1String("bicubic"))
        *filter = ResampleFilter::Bicubic;
    else if (name == QLatin1String("nearest"))
        *filter = ResampleFilter::Nearest;
    else
        return false;
    return true;
}

LayerTransform LayerTransform::normalized() const {
    LayerTransform out = *this;
    out.cx = rounded(cx);
    out.cy = rounded(cy);
    out.width = rounded(width);
    out.height = rounded(height);
    double a = std::fmod(angle, 360.0);
    if (a > 180.0)
        a -= 360.0;
    else if (a <= -180.0)
        a += 360.0;
    out.angle = rounded(a);
    return out;
}

bool LayerTransform::isPlacement(int sourceWidth, int sourceHeight) const {
    return angle == 0 && !flipX && !flipY && width == sourceWidth && height == sourceHeight;
}

QString renderedKey(const TileMap &tiles, bool hasMask, const TileMap &maskTiles) {
    QJsonObject json{{"tiles", tilesToJson(tiles)}};
    if (hasMask)
        json.insert("mask", tilesToJson(maskTiles));
    return canonicalHash(json);
}

QJsonObject sourceToJson(const LayerSource &source) {
    QJsonObject out{{"width", source.width}, {"height", source.height}, {"tiles", tilesToJson(source.tiles)}, {"rendered", source.rendered}};
    if (source.hasMask)
        out.insert("mask", QJsonObject{{"tiles", tilesToJson(source.maskTiles)}});
    const LayerTransform &t = source.transform;
    QJsonObject placement{{"cx", t.cx}, {"cy", t.cy}, {"width", t.width}, {"height", t.height}, {"angle", t.angle}, {"filter", filterName(t.filter)}};
    if (t.flipX)
        placement.insert("flipX", true);
    if (t.flipY)
        placement.insert("flipY", true);
    out.insert("transform", placement);
    return out;
}

Result<LayerSource> sourceFromJson(const QJsonObject &object) {
    LayerSource source;
    source.width = object.value("width").toInt();
    source.height = object.value("height").toInt();
    if (source.width < 1 || source.height < 1 || source.width > mask::paintFileMaxSide || source.height > mask::paintFileMaxSide ||
        qint64(source.width) * source.height > mask::paintFileMaxPixels)
        return corrupt(QStringLiteral("a layer's original has a size that cannot be"));
    if (!tilesFromJson(object.value("tiles").toObject(), source.width, source.height, &source.tiles))
        return corrupt(QStringLiteral("a tile of a layer's original is not well formed"));
    if (object.contains("mask")) {
        source.hasMask = true;
        if (!tilesFromJson(object.value("mask").toObject().value("tiles").toObject(), source.width, source.height, &source.maskTiles))
            return corrupt(QStringLiteral("a layer's original mask is not well formed"));
    }
    source.rendered = object.value("rendered").toString();
    if (!isHash(source.rendered))
        return corrupt(QStringLiteral("a layer's original has no render key"));
    const QJsonObject t = object.value("transform").toObject();
    LayerTransform &place = source.transform;
    for (const char *name : {"cx", "cy", "width", "height", "angle"})
        if (!t.value(QLatin1String(name)).isDouble() || !std::isfinite(t.value(QLatin1String(name)).toDouble()))
            return corrupt(QStringLiteral("a layer's placement is not well formed"));
    place.cx = t.value("cx").toDouble();
    place.cy = t.value("cy").toDouble();
    place.width = t.value("width").toDouble();
    place.height = t.value("height").toDouble();
    place.angle = t.value("angle").toDouble();
    place.flipX = t.value("flipX").toBool();
    place.flipY = t.value("flipY").toBool();
    if (!filterFromName(t.value("filter").toString(), &place.filter) || place.width < 0.5 || place.height < 0.5 || place.width > maxTransformSide ||
        place.height > maxTransformSide || place.width * place.height > maxTransformArea || std::abs(place.cx) > 1e7 || std::abs(place.cy) > 1e7 ||
        std::abs(place.angle) > 360.0)
        return corrupt(QStringLiteral("a layer's placement is out of range"));
    return source;
}

} // namespace fugue::canvas
