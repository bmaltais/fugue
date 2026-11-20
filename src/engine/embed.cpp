#include "embed.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <array>

#include <glib.h>

#include "engine/json.h"
#include "engine/vips.h"

namespace fugue {

namespace {

const QByteArray pngSignature("\x89PNG\r\n\x1a\n", 8);
const QByteArray xmpOpen("<fugue:workflow>");
const QByteArray xmpClose("</fugue:workflow>");

quint32 crc32(const QByteArray &bytes) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> made{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int bit = 0; bit < 8; ++bit)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            made[n] = c;
        }
        return made;
    }();
    quint32 crc = 0xffffffffu;
    for (const char byte : bytes)
        crc = table[(crc ^ quint8(byte)) & 0xff] ^ (crc >> 8);
    return ~crc;
}

QByteArray pngChunk(const QByteArray &type, const QByteArray &data) {
    QByteArray chunk(4, 0);
    qToBigEndian<quint32>(quint32(data.size()), chunk.data());
    const QByteArray body = type + data;
    QByteArray crc(4, 0);
    qToBigEndian<quint32>(crc32(body), crc.data());
    return chunk + body + crc;
}

// keyword NUL, compression flag 0, method 0, language NUL, translated keyword NUL, text.
QByteArray itxtData(const QByteArray &text) {
    return QByteArray("fugue\0\0\0\0\0", 10) + text;
}

bool isFugueItxt(const QByteArray &data) {
    return data.startsWith(QByteArray("fugue\0", 6));
}

QString xmlEscape(const QString &text) {
    QString out = text;
    out.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    return out;
}

QString xmlUnescape(QString text) {
    text.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'").replace("&amp;", "&");
    return text;
}

QByteArray xmpPacket(const QByteArray &workflowJson) {
    return QByteArray("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
                      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                      "<rdf:Description rdf:about=\"\" xmlns:fugue=\"https://fugue.app/ns/1.0/\">") +
           xmpOpen + xmlEscape(QString::fromUtf8(workflowJson)).toUtf8() + xmpClose +
           QByteArray("</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>");
}

// The text of the first uncompressed `fugue` iTXt chunk in a PNG file, or null. Only chunk
// headers are read on the way, never the image data.
QByteArray pngFugueText(QFile &file) {
    qint64 at = 8;
    while (file.seek(at)) {
        const QByteArray header = file.read(8);
        if (header.size() < 8)
            break;
        const quint32 length = qFromBigEndian<quint32>(header.constData());
        const QByteArray type = header.mid(4, 4);
        if (type == "IEND")
            break;
        if (type == "iTXt" && length <= quint32(maxEmbeddedWorkflowBytes)) {
            const QByteArray data = file.read(length);
            if (data.size() < qsizetype(length))
                break;
            // Only the uncompressed form is written; one compressed by another tool is not read.
            if (isFugueItxt(data) && data.size() >= 10 && data.at(6) == 0)
                return data.mid(10);
        }
        at += 12 + qint64(length);
    }
    return {};
}

// The XMP chunk of a WebP file, or null; the other chunks are skipped unread.
QByteArray webpXmp(QFile &file) {
    qint64 at = 12;
    while (file.seek(at)) {
        const QByteArray header = file.read(8);
        if (header.size() < 8)
            break;
        const quint32 length = qFromLittleEndian<quint32>(header.constData() + 4);
        if (header.startsWith("XMP "))
            return length <= quint32(maxEmbeddedWorkflowBytes) ? file.read(length) : QByteArray();
        at += 8 + qint64(length) + (length & 1);
    }
    return {};
}

} // namespace

Result<QByteArray> pngWithWorkflow(const QByteArray &png, const QByteArray &workflowJson) {
    if (!png.startsWith(pngSignature) || png.size() < 8 + 12 + 13)
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("The bytes are not a PNG file."));
    QByteArray out = pngSignature;
    qsizetype at = 8;
    bool placed = false;
    while (at + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + at);
        const qsizetype end = at + 12 + qsizetype(length);
        if (end > png.size())
            return makeError(QStringLiteral("media_unreadable"), QStringLiteral("The PNG file is cut short."));
        const QByteArray type = png.mid(at + 4, 4);
        const bool old = type == "iTXt" && isFugueItxt(png.mid(at + 8, length));
        if (!old)
            out += png.mid(at, end - at);
        if (type == "IHDR" && !placed) {
            out += pngChunk("iTXt", itxtData(workflowJson));
            placed = true;
        }
        at = end;
    }
    if (!placed)
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("The PNG file has no header chunk."));
    return out;
}

Result<QByteArray> webpWithWorkflow(const vips::VImage &image, const QByteArray &workflowJson) {
    if (!initVips())
        return vipsUnavailable();
    try {
        vips::VImage pixels = image;
        if (pixels.format() == VIPS_FORMAT_USHORT)
            pixels = (pixels / 257.0).rint().cast(VIPS_FORMAT_UCHAR);
        else if (pixels.format() != VIPS_FORMAT_UCHAR)
            pixels = pixels.linear(255.0, 0.0)
                         .clamp(vips::VImage::option()->set("min", 0.0)->set("max", 255.0))
                         .rint()
                         .cast(VIPS_FORMAT_UCHAR);
        pixels = pixels.copy(); // its own header, so setting metadata leaves the caller's image alone
        if (!workflowJson.isEmpty()) {
            const QByteArray packet = xmpPacket(workflowJson);
            void *copy = g_memdup2(packet.constData(), gsize(packet.size()));
            pixels.set("xmp-data", [](void *data, void *) -> int { g_free(data); return 0; }, copy, size_t(packet.size()));
        }
        VipsBlob *blob = pixels.webpsave_buffer(
            vips::VImage::option()
                ->set("lossless", true)
                ->set("keep", workflowJson.isEmpty() ? VIPS_FOREIGN_KEEP_NONE : VIPS_FOREIGN_KEEP_XMP));
        size_t size = 0;
        const void *data = vips_blob_get(blob, &size);
        const QByteArray bytes(static_cast<const char *>(data), qsizetype(size));
        vips_area_unref(VIPS_AREA(blob));
        return bytes;
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
}

Result<QJsonObject> readEmbeddedWorkflow(const QString &path) {
    QFile file(path);
    // A pipe or a device is never opened: opening a pipe waits for a writer.
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly))
        return makeError(file.exists() ? QStringLiteral("permission_denied") : QStringLiteral("file_not_found"),
                         QStringLiteral("Cannot read %1.").arg(path));
    const QByteArray head = file.read(12);
    QByteArray text;
    if (head.startsWith(pngSignature)) {
        text = pngFugueText(file);
    } else if (head.size() == 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
        const QString xmp = QString::fromUtf8(webpXmp(file));
        const qsizetype open = xmp.indexOf(QString::fromLatin1(xmpOpen));
        const qsizetype close = xmp.indexOf(QString::fromLatin1(xmpClose));
        if (open >= 0 && close > open)
            text = xmlUnescape(xmp.mid(open + xmpOpen.size(), close - open - xmpOpen.size())).toUtf8();
    } else {
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("%1 is neither a PNG nor a WebP file.").arg(path));
    }
    if (text.isEmpty())
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("%1 holds no Fugue workflow.").arg(path));
    const Result<QJsonDocument> document = parseJson(text, QStringLiteral("The workflow in %1").arg(path));
    if (!document)
        return makeError(QStringLiteral("invalid_document"), document.error().message);
    if (!document->isObject())
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("The workflow in %1 is not a JSON object.").arg(path));
    return document->object();
}

} // namespace fugue
