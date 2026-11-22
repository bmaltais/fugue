#include "paint_file.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QXmlStreamWriter>

#include <archive.h>
#include <archive_entry.h>

#include "engine/atomic_file.h"

namespace fugue::mask {

namespace {

constexpr char mimeText[] = "image/openraster";
constexpr char fugueNamespace[] = "urn:fugue:ora";
constexpr qint64 kMaxEntry = qint64(1) << 28;     // one entry, unpacked
constexpr qint64 kMaxXml = qint64(1) << 20;       // stack.xml
constexpr int kMaxEntries = 200;
constexpr int kMaxName = 200;                     // a layer's name, in characters
constexpr time_t kFixedTime = 315532800;          // 1980-01-01 00:00:00 UTC, the zip format's first moment

Error unreadable(const QString &why) {
    return makeError(QStringLiteral("media_unreadable"), QStringLiteral("This is not a paint file Fugue can read: %1.").arg(why),
                     QStringLiteral("Paint the picture again."));
}

QString kindName(LayerKind kind) {
    return kind == LayerKind::Mask ? QStringLiteral("mask") : kind == LayerKind::Picture ? QStringLiteral("picture")
                                                                                         : QStringLiteral("paint");
}

QString numberText(double value) { return QString::number(value, 'g', QLocale::FloatingPointShortest); }

// A color layer with its alpha multiplied by a one-channel mask of its size (rounded to the byte).
MaskPaint maskedLayer(const MaskPaint &pixels, const MaskPaint &mask) {
    std::vector<uint8_t> out = pixels.pixels();
    const std::vector<uint8_t> &m = mask.pixels();
    for (size_t i = 0; i < m.size() && i * 4 + 3 < out.size(); ++i)
        out[i * 4 + 3] = uint8_t((int(out[i * 4 + 3]) * int(m[i]) + 127) / 255);
    return MaskPaint(pixels.width(), pixels.height(), out, 4);
}

// ---- writing ---------------------------------------------------------------------------------

struct Sink {
    QByteArray bytes;
};

la_ssize_t sinkWrite(struct archive *, void *data, const void *buffer, size_t length) {
    static_cast<Sink *>(data)->bytes.append(static_cast<const char *>(buffer), qsizetype(length));
    return la_ssize_t(length);
}

bool addEntry(struct archive *out, const QByteArray &name, const QByteArray &bytes) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name.constData());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), la_int64_t(bytes.size()));
    archive_entry_set_mtime(entry.get(), kFixedTime, 0);
    archive_entry_set_uid(entry.get(), 0);
    archive_entry_set_gid(entry.get(), 0);
    if (archive_write_header(out, entry.get()) != ARCHIVE_OK)
        return false;
    qsizetype done = 0;
    while (done < bytes.size()) {
        const la_ssize_t wrote = archive_write_data(out, bytes.constData() + done, size_t(bytes.size() - done));
        if (wrote <= 0)
            return false;
        done += qsizetype(wrote);
    }
    return true;
}

// ---- reading ---------------------------------------------------------------------------------

bool safeName(const QString &name) {
    if (name.isEmpty() || name.size() > 128 || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name.contains(QStringLiteral("..")) || name.contains(QStringLiteral("//")) || name.contains(QLatin1Char(':')))
        return false;
    return std::none_of(name.begin(), name.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}

// What a file may hold: the mimetype, the stack, and PNGs (data/, the merged picture and the
// thumbnail other programs add). Anything else is refused rather than skipped, since skipping an
// entry still unpacks it.
bool knownName(const QString &name) {
    if (name == QLatin1String("mimetype") || name == QLatin1String("stack.xml") || name == QLatin1String("mergedimage.png") ||
        name == QLatin1String("Thumbnails/thumbnail.png"))
        return true;
    return name.startsWith(QStringLiteral("data/")) && name.size() > 5 && !name.endsWith(QLatin1Char('/'));
}

struct Unpacked {
    QByteArray stack;
    std::map<QString, QByteArray> pngs;
};

// The pixel size a PNG's header gives, read before anything decodes it.
bool pngSize(const QByteArray &png, qint64 *width, qint64 *height) {
    if (png.size() < 33 || !png.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)) || png.mid(12, 4) != "IHDR")
        return false;
    const auto be = [&](int at) {
        return (qint64(uchar(png[at])) << 24) | (qint64(uchar(png[at + 1])) << 16) | (qint64(uchar(png[at + 2])) << 8) | qint64(uchar(png[at + 3]));
    };
    *width = be(16);
    *height = be(20);
    return true;
}

Result<Unpacked> unpack(const QByteArray &bytes) {
    if (bytes.size() > paintFileMaxBytes)
        return unreadable(QStringLiteral("it is over %1 MB").arg(paintFileMaxBytes >> 20));
    std::unique_ptr<archive, decltype(&archive_read_free)> in(archive_read_new(), archive_read_free);
    if (archive_read_support_format_zip_seekable(in.get()) != ARCHIVE_OK ||
        archive_read_open_memory(in.get(), bytes.constData(), size_t(bytes.size())) != ARCHIVE_OK)
        return unreadable(QStringLiteral("it is not a zip file"));
    Unpacked out;
    qint64 total = 0;
    int count = 0;
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(in.get(), &entry)) == ARCHIVE_OK) {
        if (++count > kMaxEntries)
            return unreadable(QStringLiteral("it has too many entries"));
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (!safeName(name))
            return unreadable(QStringLiteral("an entry is named %1").arg(name.left(40)));
        if (count == 1 && name != QLatin1String("mimetype"))
            return unreadable(QStringLiteral("the first entry is not the mimetype"));
        // A folder entry (data/, Thumbnails/) that other programs add is only a name: nothing is
        // unpacked to disk, so it has no effect and is skipped. Anything else that is not a plain file is refused.
        if (archive_entry_filetype(entry) == AE_IFDIR && (name == QLatin1String("data/") || name == QLatin1String("Thumbnails/")))
            continue;
        if (!knownName(name) || archive_entry_filetype(entry) != AE_IFREG)
            return unreadable(QStringLiteral("it holds %1, which a paint file does not").arg(name.left(40)));
        const qint64 limit = name == QLatin1String("mimetype") ? 64 : name == QLatin1String("stack.xml") ? kMaxXml : kMaxEntry;
        QByteArray data;
        char buffer[1 << 16];
        for (;;) {
            const la_ssize_t got = archive_read_data(in.get(), buffer, sizeof buffer);
            if (got < 0)
                return unreadable(QStringLiteral("the zip is damaged"));
            if (got == 0)
                break;
            data.append(buffer, qsizetype(got));
            total += got;
            if (data.size() > limit || total > paintFileMaxLayerBytes + kMaxXml + 64)
                return unreadable(QStringLiteral("it unpacks to more than a paint file may"));
        }
        if (name == QLatin1String("mimetype")) {
            if (data != mimeText)
                return unreadable(QStringLiteral("its mimetype is not image/openraster"));
        } else if (name == QLatin1String("stack.xml")) {
            out.stack = data;
        } else if (!out.pngs.emplace(name, std::move(data)).second) {
            return unreadable(QStringLiteral("an entry is there twice"));
        }
    }
    if (status != ARCHIVE_EOF)
        return unreadable(QStringLiteral("the zip is damaged"));
    if (count == 0)
        return unreadable(QStringLiteral("it is empty"));
    if (out.stack.isEmpty())
        return unreadable(QStringLiteral("it has no stack.xml"));
    return out;
}

struct LayerSpec {
    QString name;
    QString src;
    LayerKind kind = LayerKind::Paint;
    bool visible = true;
    double opacity = 1;
    QString compositeOp; // as written; "" for none or svg:src-over
    QString raw;         // fugue:raw, the layer's own pixels when `src` has its mask baked in
    QString mask;        // fugue:mask, the layer mask's gray PNG
    bool maskEnabled = true;
    QString producer, created;
};

// A composite-op Fugue keeps: an OpenRaster name such as svg:multiply, or another program's own
// (krita:...), printable and short. Anything else is refused rather than carried.
bool plausibleOp(const QString &op) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9:._-]{0,63}$"));
    return pattern.match(op).hasMatch();
}

struct Stack {
    int width = 0;
    int height = 0;
    std::vector<LayerSpec> layers; // bottom first
};

bool wholeNumber(const QString &text, int min, int max, int *out) {
    bool ok = false;
    const int v = text.toInt(&ok);
    if (!ok || v < min || v > max)
        return false;
    *out = v;
    return true;
}

Result<Stack> parseStack(const QByteArray &xml) {
    QXmlStreamReader reader(xml);
    Stack stack;
    bool haveImage = false, inStack = false, doneStack = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isDTD() || reader.isEntityReference())
            return unreadable(QStringLiteral("stack.xml has a DTD"));
        if (!reader.isStartElement())
            continue;
        const QString tag = reader.name().toString();
        const QXmlStreamAttributes a = reader.attributes();
        if (!haveImage) {
            if (tag != QLatin1String("image") || !wholeNumber(a.value("w").toString(), 1, paintFileMaxSide, &stack.width) ||
                !wholeNumber(a.value("h").toString(), 1, paintFileMaxSide, &stack.height))
                return unreadable(QStringLiteral("stack.xml does not give a size"));
            if (qint64(stack.width) * stack.height > paintFileMaxPixels)
                return unreadable(QStringLiteral("the canvas is %1 x %2, over the %3 million pixels allowed")
                                      .arg(stack.width).arg(stack.height).arg(paintFileMaxPixels / 1000000));
            haveImage = true;
        } else if (tag == QLatin1String("stack")) {
            if (inStack || doneStack)
                return unreadable(QStringLiteral("stack.xml has a group of layers, which Fugue does not read"));
            inStack = true;
        } else if (tag == QLatin1String("layer") && inStack) {
            if (int(stack.layers.size()) >= paintFileMaxLayers)
                return unreadable(QStringLiteral("it has more than %1 layers").arg(paintFileMaxLayers));
            LayerSpec layer;
            layer.name = a.value("name").toString();
            if (layer.name.size() > kMaxName)
                return unreadable(QStringLiteral("a layer's name is over %1 characters").arg(kMaxName));
            layer.src = a.value("src").toString();
            const QString kind = a.value(QLatin1String(fugueNamespace), "kind").toString();
            if (kind == QLatin1String("mask"))
                layer.kind = LayerKind::Mask;
            else if (kind == QLatin1String("picture"))
                layer.kind = LayerKind::Picture;
            else if (!kind.isEmpty() && kind != QLatin1String("paint"))
                return unreadable(QStringLiteral("a layer is of the kind %1").arg(kind.left(20)));
            layer.visible = a.value("visibility").toString() != QLatin1String("hidden");
            bool ok = true;
            layer.opacity = a.hasAttribute("opacity") ? a.value("opacity").toDouble(&ok) : 1.0;
            if (!ok || !(layer.opacity >= 0 && layer.opacity <= 1))
                return unreadable(QStringLiteral("a layer's opacity is not 0 to 1"));
            if (a.hasAttribute("x") && a.value("x").toString() != QLatin1String("0"))
                return unreadable(QStringLiteral("a layer is offset, which Fugue does not read"));
            if (a.hasAttribute("y") && a.value("y").toString() != QLatin1String("0"))
                return unreadable(QStringLiteral("a layer is offset, which Fugue does not read"));
            if (layer.kind != LayerKind::Picture && !knownName(layer.src))
                return unreadable(QStringLiteral("a layer names a file that is not there"));
            if (a.hasAttribute("composite-op")) {
                layer.compositeOp = a.value("composite-op").toString();
                if (!plausibleOp(layer.compositeOp))
                    return unreadable(QStringLiteral("a layer's composite-op is not a name"));
                if (layer.compositeOp == QLatin1String("svg:src-over"))
                    layer.compositeOp.clear();
            }
            layer.raw = a.value(QLatin1String(fugueNamespace), "raw").toString();
            layer.mask = a.value(QLatin1String(fugueNamespace), "mask").toString();
            layer.maskEnabled = a.value(QLatin1String(fugueNamespace), "mask-enabled").toString() != QLatin1String("false");
            layer.producer = a.value(QLatin1String(fugueNamespace), "producer").toString().left(kMaxName);
            layer.created = a.value(QLatin1String(fugueNamespace), "created").toString().left(64);
            if ((!layer.raw.isEmpty() && !knownName(layer.raw)) || (!layer.mask.isEmpty() && !knownName(layer.mask)))
                return unreadable(QStringLiteral("a layer names a file that is not there"));
            if (layer.kind == LayerKind::Mask && !layer.mask.isEmpty())
                return unreadable(QStringLiteral("a mask layer has a layer mask"));
            stack.layers.insert(stack.layers.begin(), layer); // the stack lists the top first
        } else if (inStack) {
            return unreadable(QStringLiteral("stack.xml has <%1>, which Fugue does not read").arg(tag.left(20)));
        }
        if (reader.isEndElement() && tag == QLatin1String("stack"))
            doneStack = true;
    }
    if (reader.hasError() || !haveImage || !inStack)
        return unreadable(QStringLiteral("stack.xml is not valid"));
    return stack;
}

} // namespace

Result<QByteArray> paintToOra(const PaintDocument &document) {
    if (document.width() > paintFileMaxSide || document.height() > paintFileMaxSide ||
        qint64(document.width()) * document.height() > paintFileMaxPixels || document.layerCount() > paintFileMaxLayers)
        return makeError(QStringLiteral("value_out_of_range"),
                         QStringLiteral("The canvas is %1 x %2 with %3 layers; a paint file holds at most %4 x %4 pixels in %5 million and %6 layers.")
                             .arg(document.width()).arg(document.height()).arg(document.layerCount())
                             .arg(paintFileMaxSide).arg(paintFileMaxPixels / 1000000).arg(paintFileMaxLayers));
    // The picture layers' one placeholder pixel.
    const Result<QByteArray> placeholder = MaskPaint(1, 1, 4).toPng();
    if (!placeholder)
        return placeholder.errors();

    QByteArray xml;
    QXmlStreamWriter writer(&xml);
    writer.setAutoFormatting(false);
    writer.writeStartDocument(QStringLiteral("1.0"));
    writer.writeStartElement(QStringLiteral("image"));
    writer.writeNamespace(QString::fromLatin1(fugueNamespace), QStringLiteral("fugue"));
    writer.writeAttribute(QStringLiteral("version"), QStringLiteral("0.0.3"));
    writer.writeAttribute(QStringLiteral("w"), QString::number(document.width()));
    writer.writeAttribute(QStringLiteral("h"), QString::number(document.height()));
    writer.writeStartElement(QStringLiteral("stack"));
    std::vector<std::pair<QByteArray, QByteArray>> files; // name -> bytes, bottom first: each layer's src, then its extras
    std::vector<QByteArray> srcOf(size_t(document.layerCount())), rawOf(srcOf.size()), maskOf(srcOf.size());
    for (int position = 0; position < document.layerCount(); ++position) {
        const LayerInfo info = document.layer(position);
        const MaskPaint *pixels = document.pixels(info.id);
        const MaskPaint *mask = document.maskPixels(info.id);
        const QByteArray stem = QByteArrayLiteral("data/layer-") + QByteArray::number(position);
        QByteArray png;
        if (info.kind == LayerKind::Picture) {
            png = placeholder.value();
        } else if (mask && info.maskEnabled) {
            // Other programs have no layer mask: `src` is the layer as it looks (its alpha times the
            // mask), and the layer's own pixels and the mask are Fugue's extras, which they ignore.
            const Result<QByteArray> encoded = maskedLayer(*pixels, *mask).toPng();
            const Result<QByteArray> raw = pixels->toPng();
            if (!encoded || !raw)
                return !encoded ? encoded.errors() : raw.errors();
            png = encoded.value();
            rawOf[size_t(position)] = stem + "-raw.png";
            files.emplace_back(stem + ".png", png);
            files.emplace_back(rawOf[size_t(position)], raw.value());
        } else {
            const Result<QByteArray> encoded = pixels->toPng();
            if (!encoded)
                return encoded.errors();
            png = encoded.value();
        }
        srcOf[size_t(position)] = stem + ".png";
        if (rawOf[size_t(position)].isEmpty())
            files.emplace_back(srcOf[size_t(position)], png);
        if (mask) {
            const Result<QByteArray> encoded = mask->toPng();
            if (!encoded)
                return encoded.errors();
            maskOf[size_t(position)] = stem + "-mask.png";
            files.emplace_back(maskOf[size_t(position)], encoded.value());
        }
    }
    // The stack lists the top layer first.
    for (int position = document.layerCount() - 1; position >= 0; --position) {
        const LayerInfo info = document.layer(position);
        writer.writeStartElement(QStringLiteral("layer"));
        writer.writeAttribute(QStringLiteral("name"), info.name);
        writer.writeAttribute(QStringLiteral("src"), QString::fromLatin1(srcOf[size_t(position)]));
        writer.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
        writer.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
        writer.writeAttribute(QStringLiteral("opacity"), numberText(info.opacity));
        writer.writeAttribute(QStringLiteral("visibility"), info.visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
        writer.writeAttribute(QStringLiteral("composite-op"), info.compositeOp.isEmpty() ? QStringLiteral("svg:src-over") : info.compositeOp);
        writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("kind"), kindName(info.kind));
        if (!rawOf[size_t(position)].isEmpty())
            writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("raw"), QString::fromLatin1(rawOf[size_t(position)]));
        if (!maskOf[size_t(position)].isEmpty()) {
            writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("mask"), QString::fromLatin1(maskOf[size_t(position)]));
            writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("mask-enabled"), info.maskEnabled ? QStringLiteral("true") : QStringLiteral("false"));
        }
        if (!info.producer.isEmpty())
            writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("producer"), info.producer);
        if (!info.created.isEmpty())
            writer.writeAttribute(QString::fromLatin1(fugueNamespace), QStringLiteral("created"), info.created);
        writer.writeEndElement();
    }
    writer.writeEndElement(); // stack
    writer.writeEndElement(); // image
    writer.writeEndDocument();

    Sink sink;
    std::unique_ptr<archive, decltype(&archive_write_free)> out(archive_write_new(), archive_write_free);
    if (archive_write_set_format_zip(out.get()) != ARCHIVE_OK || archive_write_zip_set_compression_store(out.get()) != ARCHIVE_OK ||
        archive_write_set_bytes_in_last_block(out.get(), 1) != ARCHIVE_OK ||
        archive_write_open(out.get(), &sink, nullptr, sinkWrite, nullptr) != ARCHIVE_OK)
        return makeError(QStringLiteral("internal"), QStringLiteral("Fugue could not start a zip file."));
    bool ok = addEntry(out.get(), "mimetype", QByteArray(mimeText)) && addEntry(out.get(), "stack.xml", xml);
    for (const auto &file : files)
        ok = ok && addEntry(out.get(), file.first, file.second);
    if (!ok || archive_write_close(out.get()) != ARCHIVE_OK)
        return makeError(QStringLiteral("internal"), QStringLiteral("Fugue could not write the paint file: %1.")
                                                         .arg(QString::fromLocal8Bit(archive_error_string(out.get()))));
    return sink.bytes;
}

Result<PaintDocument> paintFromOra(const QByteArray &bytes) {
    const Result<Unpacked> unpacked = unpack(bytes);
    if (!unpacked)
        return unpacked.errors();
    const Result<Stack> stack = parseStack(unpacked->stack);
    if (!stack)
        return stack.errors();
    qint64 budget = paintFileMaxLayerBytes;
    for (const LayerSpec &layer : stack->layers) {
        const qint64 area = qint64(stack->width) * stack->height;
        if (layer.kind != LayerKind::Picture)
            budget -= area * (layer.kind == LayerKind::Mask ? 1 : 4);
        if (!layer.mask.isEmpty())
            budget -= area;
        if (budget < 0)
            return unreadable(QStringLiteral("its layers would take more than %1 MB").arg(paintFileMaxLayerBytes >> 20));
    }
    // A PNG of the file at the canvas's size, decoded as a mask (one channel) or a color.
    const auto decode = [&](const QString &name, bool asMask) -> Result<MaskPaint> {
        const auto found = unpacked->pngs.find(name);
        if (found == unpacked->pngs.end())
            return unreadable(QStringLiteral("a layer's picture is not in the file"));
        qint64 width = 0, height = 0;
        if (!pngSize(found->second, &width, &height) || width != stack->width || height != stack->height)
            return unreadable(QStringLiteral("a layer's picture is not the size of the canvas"));
        Result<MaskPaint> pixels = asMask ? MaskPaint::fromPng(found->second) : MaskPaint::fromPicture(found->second);
        if (!pixels)
            return unreadable(QStringLiteral("a layer's picture does not decode"));
        return pixels;
    };
    PaintDocument document(stack->width, stack->height);
    for (const LayerSpec &layer : stack->layers) {
        LayerInfo info;
        info.name = layer.name;
        info.kind = layer.kind;
        info.visible = layer.visible;
        info.opacity = layer.opacity;
        info.compositeOp = layer.compositeOp;
        info.maskEnabled = layer.maskEnabled;
        info.producer = layer.producer;
        info.created = layer.created;
        std::optional<MaskPaint> mask;
        if (!layer.mask.isEmpty()) {
            Result<MaskPaint> read = decode(layer.mask, true);
            if (!read)
                return read.errors();
            mask = std::move(read.value());
        }
        if (layer.kind == LayerKind::Picture) {
            document.loadLayer(info, MaskPaint(1, 1, 4), mask ? &*mask : nullptr);
            continue;
        }
        // With its mask baked into `src` (what other programs see), the layer's own pixels are `raw`.
        const Result<MaskPaint> pixels = decode(!layer.raw.isEmpty() && mask ? layer.raw : layer.src, layer.kind == LayerKind::Mask);
        if (!pixels)
            return pixels.errors();
        document.loadLayer(info, pixels.value(), mask ? &*mask : nullptr);
    }
    document.forgetHistory();
    return document;
}

bool looksLikeOra(QByteArrayView bytes) {
    // A zip's first local header (30 bytes, then the name and an extra field), the name mimetype, and
    // the mimetype's own text stored right after.
    if (bytes.size() < 30 || !bytes.startsWith(QByteArrayView("PK\x03\x04", 4)))
        return false;
    const auto le16 = [&](int at) { return int(uchar(bytes[at])) | (int(uchar(bytes[at + 1])) << 8); };
    const int name = le16(26), extra = le16(28);
    const qsizetype text = 30 + name + extra;
    return name == 8 && bytes.mid(30, 8) == "mimetype" && bytes.size() >= text + 16 && bytes.mid(text, 16) == "image/openraster";
}

Errors writePaintFile(const PaintDocument &document, const QString &path) {
    const Result<QByteArray> bytes = paintToOra(document);
    if (!bytes)
        return bytes.errors();
    return replaceFile(path, bytes.value(), QStringLiteral("the paint file"));
}

namespace {

bool startsWith(const QByteArray &bytes, const char *magic, int size) { return bytes.size() >= size && std::memcmp(bytes.constData(), magic, size_t(size)) == 0; }

} // namespace

bool looksLikePicture(QByteArrayView bytes) {
    const QByteArray head = bytes.left(12).toByteArray();
    return startsWith(head, "\x89PNG\r\n\x1a\n", 8) || startsWith(head, "\xff\xd8\xff", 3) || (startsWith(head, "RIFF", 4) && head.mid(8, 4) == "WEBP");
}

Result<PaintDocument> paintFromPicture(const QByteArray &bytes, const QString &layerName) {
    if (bytes.size() > paintFileMaxBytes)
        return unreadable(QStringLiteral("it is over %1 MB").arg(paintFileMaxBytes >> 20));
    Result<MaskPaint> picture = MaskPaint::fromPicture(bytes);
    if (!picture)
        return picture.errors();
    if (picture->width() > paintFileMaxSide || picture->height() > paintFileMaxSide || qint64(picture->width()) * picture->height() > paintFileMaxPixels)
        return makeError(QStringLiteral("value_out_of_range"),
                         QStringLiteral("The picture is %1 x %2; a canvas is at most %3 x %3 pixels in %4 million.")
                             .arg(picture->width()).arg(picture->height()).arg(paintFileMaxSide).arg(paintFileMaxPixels / 1000000));
    PaintDocument document(picture->width(), picture->height());
    document.loadLayer(LayerKind::Paint, layerName, true, 1.0, std::move(picture.value()));
    document.forgetHistory();
    return document;
}

Result<PaintDocument> readCanvasFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("The file %1 is not there.").arg(info.fileName()));
    if (info.size() > paintFileMaxBytes)
        return unreadable(QStringLiteral("it is over %1 MB").arg(paintFileMaxBytes >> 20));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("%1 cannot be read: %2.").arg(info.fileName(), file.errorString()));
    const QByteArray bytes = file.read(paintFileMaxBytes + 1);
    if (looksLikeOra(bytes) || bytes.startsWith("PK\x03\x04")) // any zip: paintFromOra says what is wrong with it
        return paintFromOra(bytes);
    if (looksLikePicture(bytes))
        return paintFromPicture(bytes, info.completeBaseName().isEmpty() ? QStringLiteral("Layer 1") : info.completeBaseName().left(64));
    return makeError(QStringLiteral("media_unreadable"),
                     QStringLiteral("%1 is not a file a canvas can be made from: Fugue reads an OpenRaster (.ora) file or a PNG, JPEG or WebP picture.").arg(info.fileName()),
                     QStringLiteral("Convert the picture to PNG, JPEG or WebP, or export an .ora."));
}

Result<PaintDocument> readPaintFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("The paint file %1 is not there.").arg(info.fileName()));
    if (info.size() > paintFileMaxBytes)
        return unreadable(QStringLiteral("it is over %1 MB").arg(paintFileMaxBytes >> 20));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("%1 cannot be read: %2.").arg(info.fileName(), file.errorString()));
    return paintFromOra(file.read(paintFileMaxBytes + 1));
}

} // namespace fugue::mask
