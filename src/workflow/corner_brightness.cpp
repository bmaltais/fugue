#include "workflow/corner_brightness.h"

#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QUrl>

#include <cmath>

namespace fugue {

namespace {

double linear(int channel) {
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double measure(const QString &path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && size.width() > 192)
        reader.setScaledSize(QSize(192, qMax(1, size.height() * 192 / size.width())));
    const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull())
        return -1;
    // The part the card shows: 16:10, centred (Image.PreserveAspectCrop).
    QRect shown = image.rect();
    if (shown.width() * 10 > shown.height() * 16) {
        const int w = shown.height() * 16 / 10;
        shown = QRect((shown.width() - w) / 2, 0, w, shown.height());
    } else {
        const int h = shown.width() * 10 / 16;
        shown = QRect(0, (shown.height() - h) / 2, shown.width(), h);
    }
    // The button's square: about a fifth of the width, at the corner.
    const int side = qMax(1, shown.width() / 5);
    const QRect corner(shown.right() - side + 1, shown.bottom() - side + 1, side, side);
    double sum = 0;
    for (int y = corner.top(); y <= corner.bottom(); ++y)
        for (int x = corner.left(); x <= corner.right(); ++x) {
            const QRgb p = image.pixel(x, y);
            const double a = qAlpha(p) / 255.0; // a see-through pixel shows the card's well, taken as dark
            sum += a * (0.2126 * linear(qRed(p)) + 0.7152 * linear(qGreen(p)) + 0.0722 * linear(qBlue(p)));
        }
    return sum / (double(corner.width()) * corner.height());
}

}

double cornerLuminance(const QString &pathOrUrl) {
    const QString path = pathOrUrl.startsWith(QLatin1String("file:")) ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return -1;
    static QMutex lock;
    static QHash<QString, double> cache;
    const QString key = path + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
                        + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker guard(&lock);
        if (const auto hit = cache.constFind(key); hit != cache.cend())
            return hit.value();
    }
    const double value = measure(path);
    QMutexLocker guard(&lock);
    cache.insert(key, value);
    return value;
}

}
