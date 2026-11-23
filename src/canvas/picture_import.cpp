#include "canvas/picture_import.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <QObject>
#include <QUrl>

#include <cstring>

#include "engine/canvas_transform.h"

namespace fugue {

namespace {

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}

ImportedPicture refused(const QString &why) {
    ImportedPicture out;
    out.problem = why;
    return out;
}

QString sizeProblem(int width, int height) {
    if (const QString why = canvas::placeableProblem(width, height); !why.isEmpty())
        return QObject::tr("%1 Make the picture smaller first.").arg(why);
    return {};
}

} // namespace

ImportedPicture pictureFromImage(const QImage &image) {
    if (image.isNull())
        return refused(QObject::tr("There is no picture to place."));
    if (const QString why = sizeProblem(image.width(), image.height()); !why.isEmpty())
        return refused(why);
    const QImage straight = image.convertedTo(QImage::Format_RGBA8888); // straight (not premultiplied) bytes, as a layer holds them
    if (straight.isNull())
        return refused(QObject::tr("The picture could not be read."));
    std::vector<uint8_t> pixels(size_t(straight.width()) * size_t(straight.height()) * 4);
    for (int y = 0; y < straight.height(); ++y)
        std::memcpy(pixels.data() + size_t(y) * size_t(straight.width()) * 4, straight.constScanLine(y), size_t(straight.width()) * 4);
    ImportedPicture out;
    out.pixels = mask::MaskPaint(straight.width(), straight.height(), pixels, 4);
    return out;
}

ImportedPicture pictureFromClipboard() {
    QClipboard *board = QGuiApplication::instance() ? QGuiApplication::clipboard() : nullptr;
    const QMimeData *data = board ? board->mimeData() : nullptr;
    if (!data || !data->hasImage())
        return refused(QObject::tr("The clipboard has no picture."));
    return pictureFromImage(board->image());
}

bool isPictureFile(const QString &pathOrUrl) {
    const QString suffix = QFileInfo(localPath(pathOrUrl)).suffix().toLower();
    return suffix == QLatin1String("png") || suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("webp") ||
           suffix == QLatin1String("gif") || suffix == QLatin1String("bmp") || suffix == QLatin1String("tif") || suffix == QLatin1String("tiff");
}

ImportedPicture pictureFromFile(const QString &pathOrUrl) {
    const QString path = localPath(pathOrUrl);
    const QFileInfo info(path);
    if (!isPictureFile(path))
        return refused(QObject::tr("%1 is not a picture file: drop a PNG, JPEG or WebP.").arg(info.fileName()));
    if (!info.isFile())
        return refused(QObject::tr("%1 is not there.").arg(info.fileName()));
    if (info.size() > maxPictureFileBytes)
        return refused(QObject::tr("%1 is larger than %2 MB.").arg(info.fileName()).arg(maxPictureFileBytes >> 20));
    // The size is read from the header first, so a picture that would not fit is never decoded.
    QImageReader reader(path);
    if (const QSize size = reader.size(); size.isValid())
        if (const QString why = sizeProblem(size.width(), size.height()); !why.isEmpty())
            return refused(why);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return refused(QObject::tr("%1 could not be opened.").arg(info.fileName()));
    const Result<mask::MaskPaint> decoded = mask::MaskPaint::fromPicture(file.readAll());
    if (!decoded)
        return refused(QObject::tr("%1 could not be read as a picture.").arg(info.fileName()));
    if (const QString why = sizeProblem(decoded->width(), decoded->height()); !why.isEmpty())
        return refused(why);
    ImportedPicture out;
    out.pixels = decoded.value();
    return out;
}

} // namespace fugue
