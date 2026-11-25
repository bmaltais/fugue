#include "picture_luma.h"

#include <QFileInfo>
#include <QImageReader>
#include <cmath>

namespace fugue {
namespace luma {

namespace {
double linear(double channel) { return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4); }
} // namespace

double relative(const QColor &color) { return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF()); }

double mean(const QImage &image, const QRectF &normalized) {
    if (image.isNull())
        return -1;
    QImage small = image;
    if (qMax(small.width(), small.height()) > 64)
        small = small.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    small = small.convertToFormat(QImage::Format_ARGB32);
    const int w = small.width(), h = small.height();
    const int x0 = qBound(0, int(std::floor(normalized.left() * w)), w - 1);
    const int y0 = qBound(0, int(std::floor(normalized.top() * h)), h - 1);
    const int x1 = qBound(x0 + 1, int(std::ceil(normalized.right() * w)), w);
    const int y1 = qBound(y0 + 1, int(std::ceil(normalized.bottom() * h)), h);
    if (normalized.width() <= 0 || normalized.height() <= 0)
        return -1;
    double sum = 0;
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x, ++count)
            sum += relative(QColor::fromRgb(small.pixel(x, y)));
    return count > 0 ? sum / count : -1;
}

} // namespace luma

double PictureLuma::meanOfFile(const QUrl &source, double x, double y, double width, double height) {
    const QString path = source.isLocalFile() ? source.toLocalFile() : source.toString();
    const QFileInfo info(path);
    if (!info.isFile())
        return -1;
    const QString key = path + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    auto it = m_cache.find(key);
    if (it == m_cache.end()) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && qMax(size.width(), size.height()) > 64)
            reader.setScaledSize(size.scaled(64, 64, Qt::KeepAspectRatio));
        it = m_cache.insert(key, reader.read());
    }
    return luma::mean(it.value(), QRectF(x, y, width, height));
}

double PictureLuma::meanOfImage(const QVariant &image, double x, double y, double width, double height) const {
    return luma::mean(image.value<QImage>(), QRectF(x, y, width, height));
}

} // namespace fugue
