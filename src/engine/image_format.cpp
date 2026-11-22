#include "image_format.h"

#include "engine/paint_file.h"

namespace fugue {

std::optional<ImageFormat> sniffImage(QByteArrayView b) {
    if (b.startsWith(QByteArrayView("\x89PNG\r\n\x1a\n", 8)))
        return ImageFormat{QStringLiteral("png"), QStringLiteral("image/png")};
    if (b.startsWith(QByteArrayView("\xff\xd8\xff", 3)))
        return ImageFormat{QStringLiteral("jpg"), QStringLiteral("image/jpeg")};
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP")
        return ImageFormat{QStringLiteral("webp"), QStringLiteral("image/webp")};
    if (b.startsWith("GIF87a") || b.startsWith("GIF89a"))
        return ImageFormat{QStringLiteral("gif"), QStringLiteral("image/gif")};
    if (b.startsWith(QByteArrayView("II*\0", 4)) || b.startsWith(QByteArrayView("MM\0*", 4)))
        return ImageFormat{QStringLiteral("tif"), QStringLiteral("image/tiff")};
    if (b.size() >= 14 && b.startsWith("BM") && b.mid(6, 4) == QByteArrayView("\0\0\0\0", 4))
        return ImageFormat{QStringLiteral("bmp"), QStringLiteral("image/bmp")};
    if (b.size() >= 12 && b.mid(4, 4) == "ftyp" && (b.mid(8, 4) == "avif" || b.mid(8, 4) == "avis"))
        return ImageFormat{QStringLiteral("avif"), QStringLiteral("image/avif")};
    return std::nullopt;
}

std::optional<ImageFormat> sniffInputFile(QByteArrayView b) {
    if (const std::optional<ImageFormat> image = sniffImage(b))
        return image;
    if (mask::looksLikeOra(b))
        return ImageFormat{QStringLiteral("ora"), QStringLiteral("image/openraster")};
    return std::nullopt;
}

} // namespace fugue
