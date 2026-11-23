#include "engine/psd_file.h"

#include <QSet>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

#include "engine/new_canvas.h"
#include "engine/paint_file.h"

namespace fugue::mask {

namespace {

// ---- blend keys: the Photoshop key for each composite-op (the OpenRaster names Fugue stores) ----------------
struct KeyOp {
    const char *key;
    const char *op;
};
constexpr KeyOp kKeyOps[] = {
    {"norm", "svg:src-over"}, {"mul ", "svg:multiply"},    {"scrn", "svg:screen"},    {"over", "svg:overlay"},
    {"sLit", "svg:soft-light"}, {"dark", "svg:darken"},    {"lite", "svg:lighten"},   {"lddg", "svg:plus"},
    {"idiv", "svg:color-burn"}, {"div ", "svg:color-dodge"}, {"hLit", "svg:hard-light"}, {"diff", "svg:difference"},
    {"smud", "svg:exclusion"}, {"hue ", "svg:hue"},        {"sat ", "svg:saturation"}, {"colr", "svg:color"},
    {"lum ", "svg:luminosity"},
};

// Readable names of the keys Fugue meets and does not map, for the dropped list.
QString keyName(const QString &key) {
    static const struct { const char *key, *name; } names[] = {
        {"diss", "Dissolve"}, {"dkCl", "Darker Color"}, {"lgCl", "Lighter Color"}, {"lbrn", "Linear Burn"}, {"vLit", "Vivid Light"},
        {"lLit", "Linear Light"}, {"pLit", "Pin Light"}, {"hMix", "Hard Mix"}, {"fsub", "Subtract"}, {"fdiv", "Divide"}, {"pass", "Pass Through"}};
    for (const auto &n : names)
        if (key == QLatin1String(n.key))
            return QString::fromLatin1(n.name);
    return key;
}

Error warning(const QString &message) {
    Error e = makeError(QStringLiteral("psd_dropped"), message);
    e.severity = Error::Severity::Warning;
    return e;
}

Error unreadable(const QString &why) {
    return makeError(QStringLiteral("media_unreadable"), QStringLiteral("This is not a Photoshop file Fugue can read: %1.").arg(why),
                     QStringLiteral("Save it again from Photoshop as a .psd, or export a PNG."));
}

Error cannotHold(const QString &why, const QString &hint) { return makeError(QStringLiteral("input_invalid"), why, hint); }

// ---- writing -----------------------------------------------------------------------------------------------
void put8(QByteArray &out, uint v) { out.append(char(v & 0xff)); }
void put16(QByteArray &out, uint v) { put8(out, v >> 8); put8(out, v); }
void put32(QByteArray &out, quint32 v) { put16(out, v >> 16); put16(out, v & 0xffff); }
void putKey(QByteArray &out, const char *key) { out.append(key, 4); }

void padTo(QByteArray &out, int multiple, qsizetype from = 0) {
    while ((out.size() - from) % multiple != 0)
        out.append('\0');
}

// PackBits of one row.
void packRow(const uint8_t *row, int width, QByteArray &out) {
    int i = 0;
    while (i < width) {
        int run = 1;
        while (i + run < width && run < 128 && row[i + run] == row[i])
            ++run;
        if (run >= 3) {
            put8(out, uint(257 - run)); // -(run-1)
            put8(out, row[i]);
            i += run;
            continue;
        }
        int j = i; // literal: until a run of 3 begins or 128 bytes
        while (j < width && j - i < 128) {
            if (j + 2 < width && row[j] == row[j + 1] && row[j] == row[j + 2])
                break;
            ++j;
        }
        if (j == i)
            ++j;
        put8(out, uint(j - i - 1));
        out.append(reinterpret_cast<const char *>(row + i), j - i);
        i = j;
    }
}

// One channel plane (width x height bytes) as a compression code 1 record: the code, a 2-byte length per row, the rows.
QByteArray rlePlane(const std::vector<uint8_t> &plane, int width, int height, bool withCode) {
    QByteArray rows, counts;
    for (int y = 0; y < height; ++y) {
        const qsizetype before = rows.size();
        packRow(plane.data() + size_t(y) * width, width, rows);
        put16(counts, uint(rows.size() - before));
    }
    QByteArray out;
    if (withCode)
        put16(out, 1);
    out += counts;
    out += rows;
    return out;
}

std::vector<uint8_t> planeOf(const std::vector<uint8_t> &rgba, int channel) {
    std::vector<uint8_t> plane(rgba.size() / 4);
    for (size_t p = 0; p < plane.size(); ++p)
        plane[p] = rgba[p * 4 + channel];
    return plane;
}

QString asciiName(const QString &name) {
    QString out;
    for (const QChar c : name)
        out += (c.unicode() >= 0x20 && c.unicode() < 0x7f) ? c : QChar('?');
    return out.left(255);
}

QByteArray pascalName(const QString &name) {
    const QByteArray text = asciiName(name).toLatin1();
    QByteArray out;
    put8(out, uint(text.size()));
    out += text;
    padTo(out, 4);
    return out;
}

QByteArray unicodeBlock(const QString &name) {
    QByteArray data;
    put32(data, quint32(name.size()));
    for (const QChar c : name)
        put16(data, c.unicode());
    put16(data, 0);
    return data;
}

void putBlock(QByteArray &out, const char *key, QByteArray data) {
    padTo(data, 4);
    putKey(out, "8BIM");
    putKey(out, key);
    put32(out, quint32(data.size()));
    out += data;
}

// ---- reading -----------------------------------------------------------------------------------------------
// A view of the file that never reads past its end: a read past it gives 0 and marks the reader bad, and every
// structural step checks `ok()` before it trusts what it read.
class Reader {
public:
    Reader(const uint8_t *data, qint64 size) : m_data(data), m_size(size) {}
    bool ok() const { return !m_bad; }
    qint64 pos() const { return m_pos; }
    qint64 left() const { return m_bad ? 0 : m_size - m_pos; }
    void fail() { m_bad = true; }
    uint8_t u8() { return need(1) ? m_data[m_pos++] : 0; }
    uint16_t u16() { const uint a = u8(); return uint16_t(a << 8 | u8()); }
    int16_t i16() { return int16_t(u16()); }
    uint32_t u32() { const uint32_t a = u16(); return a << 16 | u16(); }
    int32_t i32() { return int32_t(u32()); }
    void skip(qint64 n) {
        if (n < 0 || !need(n))
            return;
        m_pos += n;
    }
    // The next n bytes as their own reader (the parent moves past them).
    Reader take(qint64 n) {
        if (n < 0 || !need(n)) {
            m_bad = true;
            return Reader(m_data, 0);
        }
        Reader r(m_data + m_pos, n);
        m_pos += n;
        return r;
    }
    QByteArray bytes(qint64 n) {
        if (n < 0 || !need(n)) {
            m_bad = true;
            return {};
        }
        QByteArray b(reinterpret_cast<const char *>(m_data + m_pos), n);
        m_pos += n;
        return b;
    }
    const uint8_t *here() const { return m_data + m_pos; }

private:
    bool need(qint64 n) {
        if (m_bad || n > m_size - m_pos) {
            m_bad = true;
            return false;
        }
        return true;
    }
    const uint8_t *m_data;
    qint64 m_size;
    qint64 m_pos = 0;
    bool m_bad = false;
};

constexpr qint64 maxLayerPixels = paintFileMaxPixels * 2; // a layer bigger than the canvas is cut to it, but it is still decoded whole

// Decodes one channel of `width` x `height` bytes. `compression` is the 2-byte code that began the data; `r` is what
// follows it and holds nothing but this channel. Returns nothing for a damaged or unsupported channel.
std::optional<std::vector<uint8_t>> decodeChannel(int compression, Reader r, int width, int height) {
    const qint64 count = qint64(width) * height;
    std::vector<uint8_t> out;
    if (count == 0)
        return out;
    if (count > maxLayerPixels)
        return std::nullopt;
    if (compression == 0) {
        if (r.left() < count)
            return std::nullopt;
        out.assign(r.here(), r.here() + count);
        return out;
    }
    if (compression == 1) {
        std::vector<uint32_t> lengths(static_cast<size_t>(height), 0u);
        for (uint32_t &l : lengths)
            l = r.u16();
        if (!r.ok())
            return std::nullopt;
        out.reserve(size_t(count));
        for (int y = 0; y < height; ++y) {
            Reader row = r.take(lengths[size_t(y)]);
            if (!r.ok())
                return std::nullopt;
            int got = 0;
            while (got < width && row.left() > 0) {
                const int n = int8_t(row.u8());
                if (n >= 0) { // n + 1 literal bytes
                    const int run = n + 1;
                    if (row.left() < run || got + run > width)
                        return std::nullopt;
                    for (int k = 0; k < run; ++k)
                        out.push_back(row.u8());
                    got += run;
                } else if (n != -128) { // 1 - n copies of the next byte
                    const int run = 1 - n;
                    if (row.left() < 1 || got + run > width) // never more than the row holds: the expansion is bounded
                        return std::nullopt;
                    out.insert(out.end(), size_t(run), row.u8());
                    got += run;
                }
            }
            if (got != width)
                return std::nullopt;
        }
        return out;
    }
    if (compression == 2 || compression == 3) {
        // Inflate straight into exactly `count` bytes: a stream that holds more (a zlib bomb) or less is refused, and no
        // size the stream claims is trusted.
        const QByteArray packed = r.bytes(r.left());
        out.resize(size_t(count));
        z_stream stream{};
        if (inflateInit(&stream) != Z_OK)
            return std::nullopt;
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
        stream.avail_in = uInt(std::min<qsizetype>(packed.size(), std::numeric_limits<uInt>::max()));
        stream.next_out = out.data();
        stream.avail_out = uInt(count);
        const int status = inflate(&stream, Z_FINISH);
        const bool exact = status == Z_STREAM_END && stream.avail_out == 0;
        inflateEnd(&stream);
        if (!exact) // Z_BUF_ERROR with no room left is a stream longer than promised; a short one ends with room left
            return std::nullopt;
        if (compression == 3) // the prediction: each byte is the difference to the one before it in the row
            for (int y = 0; y < height; ++y)
                for (int x = 1; x < width; ++x)
                    out[size_t(y) * width + x] = uint8_t(out[size_t(y) * width + x] + out[size_t(y) * width + x - 1]);
        return out;
    }
    return std::nullopt;
}

struct Channel {
    int id = 0;
    quint32 length = 0;
};

struct Record {
    qint64 top = 0, left = 0, bottom = 0, right = 0; // 32-bit in the file; wide here so no sum of them overflows
    std::vector<Channel> channels;
    QString blendKey = QStringLiteral("norm");
    int opacity = 255;
    bool clipping = false;
    bool visible = true;
    bool hasMask = false, maskDisabled = false;
    qint64 maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    int maskDefault = 0;
    bool maskRelative = false;
    // The "real" user mask a layer with a vector mask too has (channel -3): its own rectangle, default and flags.
    bool hasRealMask = false, realMaskDisabled = false;
    qint64 realTop = 0, realLeft = 0, realBottom = 0, realRight = 0;
    int realDefault = 0;
    QString name;
    int divider = 0; // lsct type: 1 or 2 open/closed group, 3 its end
    bool adjustment = false, text = false, smart = false, effects = false, vectorMask = false;
    qint64 layerWidth() const { return right - left; }
    qint64 layerHeight() const { return bottom - top; }
};

const QSet<QString> &adjustmentKeys() {
    static const QSet<QString> keys{"brit", "levl", "curv", "hue2", "hue ", "blnc", "blwh", "mixr", "clrL", "phfl", "vibA", "nvrt", "post",
                                    "thrs", "grdm", "selc", "expA", "SoCo", "GdFl", "PtFl"};
    return keys;
}

} // namespace

QString psdBlendKeyOfOp(const QString &compositeOp, bool *exact) {
    if (exact)
        *exact = true;
    if (compositeOp.isEmpty())
        return QStringLiteral("norm");
    for (const KeyOp &k : kKeyOps)
        if (compositeOp == QLatin1String(k.op))
            return QString::fromLatin1(k.key);
    if (exact)
        *exact = false;
    return QStringLiteral("norm");
}

QString psdOpOfBlendKey(const QString &key, bool *known) {
    if (known)
        *known = true;
    if (key == QLatin1String("norm") || key == QLatin1String("pass"))
        return {};
    for (const KeyOp &k : kKeyOps)
        if (key == QLatin1String(k.key))
            return QString::fromLatin1(k.op);
    if (known)
        *known = false;
    return {};
}

bool looksLikePsd(QByteArrayView bytes) { return bytes.startsWith("8BPS"); }

// ---- export ------------------------------------------------------------------------------------------------
Result<PsdExported> paintToPsd(const PaintDocument &document) {
    PsdImage image;
    image.width = document.width();
    image.height = document.height();
    PsdExported skipped;
    for (int position = 0; position < document.layerCount(); ++position) {
        const LayerInfo info = document.layer(position);
        const MaskPaint *pixels = document.pixels(info.id);
        if (info.kind == LayerKind::Mask || !pixels) {
            skipped.notes << QStringLiteral("The mask layer \"%1\" is a selection, not a picture: it is not in the Photoshop file.").arg(info.name);
            continue;
        }
        PsdLayer layer;
        layer.info = info;
        layer.pixels = *pixels;
        if (info.hasMask && document.maskPixels(info.id))
            layer.mask = *document.maskPixels(info.id);
        image.layers.push_back(std::move(layer));
    }
    image.merged = document.flatten();
    Result<PsdExported> written = paintToPsd(image);
    if (!written)
        return written;
    PsdExported out = written.value();
    out.notes = skipped.notes + out.notes;
    return out;
}

Result<PsdExported> paintToPsd(const PsdImage &image) {
    const int width = image.width, height = image.height;
    if (width > psdMaxSide || height > psdMaxSide)
        return makeError(QStringLiteral("input_invalid"),
                         QStringLiteral("A Photoshop file holds at most %1 px a side; this canvas is %2 x %3.").arg(psdMaxSide).arg(width).arg(height),
                         QStringLiteral("Export an .ora or a .png instead, or make the canvas smaller."));
    PsdExported out;

    // The layer records and their channel data, bottom first. A group is a divider record, its layers, then its folder record.
    QByteArray records, channelData;
    int layerCount = 0;
    // One record. `pixels` null is a record with no picture (a group's divider or folder): an empty rectangle.
    const auto addRecord = [&](const QString &name, const QString &key, double opacity, bool visible, const MaskPaint *pixels, const MaskPaint *layerMask, bool maskEnabled, int section) {
        std::vector<std::pair<int, QByteArray>> channels; // id, compression code + data
        if (pixels) {
            for (const int c : {0, 1, 2, 3})
                channels.emplace_back(c == 3 ? -1 : c, rlePlane(planeOf(pixels->pixels(), c), width, height, true));
        } else {
            for (const int id : {-1, 0, 1, 2}) {
                QByteArray none;
                put16(none, 0); // raw, no bytes: the rectangle is empty
                channels.emplace_back(id, none);
            }
        }
        if (layerMask)
            channels.emplace_back(-2, rlePlane(layerMask->pixels(), width, height, true));

        QByteArray record;
        put32(record, 0), put32(record, 0), put32(record, pixels ? quint32(height) : 0), put32(record, pixels ? quint32(width) : 0); // top left bottom right
        put16(record, uint(channels.size()));
        for (const auto &[id, data] : channels) {
            put16(record, uint(id & 0xffff));
            put32(record, quint32(data.size()));
            channelData += data;
        }
        putKey(record, "8BIM");
        putKey(record, key.toLatin1().constData());
        put8(record, uint(std::lround(std::clamp(opacity, 0.0, 1.0) * 255)));
        put8(record, 0); // clipping: base
        put8(record, visible ? 0 : 2); // flags: bit 1 set = hidden
        put8(record, 0);

        QByteArray extra;
        if (layerMask) {
            put32(extra, 20);
            put32(extra, 0), put32(extra, 0), put32(extra, quint32(height)), put32(extra, quint32(width));
            put8(extra, 255);                  // what lies outside the mask's rectangle: shown (it covers the layer)
            put8(extra, maskEnabled ? 0 : 2); // flags: bit 1 = mask disabled
            put16(extra, 0);                   // padding of the 20-byte form
        } else {
            put32(extra, 0);
        }
        put32(extra, uint(8 + 8 * channels.size())); // blending ranges: the composite, then each channel, all the full range
        for (size_t i = 0; i <= channels.size(); ++i) {
            put16(extra, 0), put16(extra, 0xffff);
            put16(extra, 0), put16(extra, 0xffff);
        }
        extra += pascalName(name);
        putBlock(extra, "luni", unicodeBlock(name));
        if (section >= 0) { // lsct: the section type, then the blend signature and key (the same key as the record: a group composites as one, so Normal unless it has a mode)
            QByteArray data;
            put32(data, quint32(section));
            putKey(data, "8BIM");
            putKey(data, key.toLatin1().constData());
            putBlock(extra, "lsct", data);
        }
        put32(record, quint32(extra.size()));
        record += extra;
        records += record;
        ++layerCount;
    };
    const std::function<void(const std::vector<PsdLayer> &, int)> write = [&](const std::vector<PsdLayer> &layers, int depth) {
        for (const PsdLayer &layer : layers) {
            bool exact = true;
            const QString key = psdBlendKeyOfOp(layer.info.compositeOp, &exact);
            if (!exact)
                out.notes << QStringLiteral("The layer \"%1\" has a blend mode (%2) Photoshop does not know: it is written as Normal.").arg(layer.info.name, layer.info.compositeOp);
            if (!layer.group) {
                addRecord(layer.info.name, key, layer.info.opacity, layer.info.visible, &layer.pixels, layer.mask ? &*layer.mask : nullptr, layer.info.maskEnabled, -1);
                continue;
            }
            addRecord(QStringLiteral("</Layer group>"), QStringLiteral("norm"), 1.0, true, nullptr, nullptr, true, 3);
            write(layer.children, depth + 1);
            addRecord(layer.info.name, key, layer.info.opacity, layer.info.visible, nullptr, layer.mask ? &*layer.mask : nullptr, layer.info.maskEnabled, layer.open ? 1 : 2);
        }
    };
    write(image.layers, 0);
    if (layerCount == 0)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("There is no layer to write."), QStringLiteral("A mask layer is a selection, not a picture."));

    QByteArray layerInfo;
    put16(layerInfo, uint(-layerCount & 0xffff)); // negative: the merged image's first alpha channel is its transparency
    layerInfo += records;
    layerInfo += channelData;
    padTo(layerInfo, 2);
    QByteArray layerAndMask;
    put32(layerAndMask, quint32(layerInfo.size()));
    layerAndMask += layerInfo;
    put32(layerAndMask, 0); // global layer mask info: none

    // The header, then the sections: color mode data and image resources are empty.
    QByteArray file;
    putKey(file, "8BPS");
    put16(file, 1);
    file.append(6, '\0');
    put16(file, 4); // channels of the merged image: R G B and its transparency
    put32(file, quint32(height));
    put32(file, quint32(width));
    put16(file, 8);
    put16(file, 3); // RGB
    put32(file, 0);
    put32(file, 0);
    put32(file, quint32(layerAndMask.size()));
    file += layerAndMask;

    // The merged image: the flattened canvas, planes one after the other, one RLE record for all. Photoshop writes a
    // merged image with transparency blended onto white (readers take the white out again using the alpha plane),
    // so the colors are c * a + 255 * (1 - a).
    MaskPaint flat = image.merged;
    {
        std::vector<uint8_t> matted = flat.pixels();
        for (size_t p = 0; p + 3 < matted.size(); p += 4)
            for (int c = 0; c < 3; ++c)
                matted[p + c] = uint8_t((matted[p + c] * matted[p + 3] + 255 * (255 - matted[p + 3]) + 127) / 255);
        flat = MaskPaint(width, height, matted, 4);
    }
    QByteArray counts, rows;
    for (int c = 0; c < 4; ++c) {
        const std::vector<uint8_t> plane = planeOf(flat.pixels(), c);
        for (int y = 0; y < height; ++y) {
            const qsizetype before = rows.size();
            packRow(plane.data() + size_t(y) * width, width, rows);
            put16(counts, uint(rows.size() - before));
        }
    }
    put16(file, 1);
    file += counts;
    file += rows;
    out.bytes = file;
    return out;
}

// ---- import ------------------------------------------------------------------------------------------------
Result<PsdImported> paintFromPsdLayers(const QByteArray &bytes) {
    Reader r(reinterpret_cast<const uint8_t *>(bytes.constData()), bytes.size());
    if (!looksLikePsd(bytes))
        return unreadable(QStringLiteral("it does not begin with 8BPS"));
    r.skip(4);
    const int version = r.u16();
    r.skip(6);
    const int channelCount = r.u16();
    const quint32 height = r.u32(), width = r.u32();
    const int depth = r.u16();
    const int mode = r.u16();
    if (!r.ok())
        return unreadable(QStringLiteral("the header is cut short"));
    if (version == 2)
        return cannotHold(QStringLiteral("This is a Photoshop large document (.psb); Fugue reads .psd files."),
                          QStringLiteral("Save it as a .psd (at most %1 px a side) or export a PNG.").arg(psdMaxSide));
    if (version != 1)
        return unreadable(QStringLiteral("its version is %1").arg(version));
    if (channelCount < 1 || channelCount > 56 || width < 1 || height < 1 || width > quint32(psdMaxSide) || height > quint32(psdMaxSide))
        return unreadable(QStringLiteral("its size or channel count is not one the format allows"));
    if (mode != 3 || depth != 8)
        return cannotHold(QStringLiteral("This Photoshop file is %1 bits per channel, color mode %2: Fugue reads 8-bit RGB.")
                              .arg(depth)
                              .arg(mode == 3 ? QStringLiteral("RGB") : mode == 1 ? QStringLiteral("Grayscale") : mode == 4 ? QStringLiteral("CMYK") : mode == 2 ? QStringLiteral("Indexed") : mode == 9 ? QStringLiteral("Lab") : QString::number(mode)),
                          QStringLiteral("In Photoshop: Image > Mode > RGB Color and 8 Bits/Channel, then save again."));
    if (const QString why = canvasSizeProblem(int(width), int(height)); !why.isEmpty())
        return cannotHold(QStringLiteral("This Photoshop file is %1 x %2 px: %3").arg(width).arg(height).arg(why), QStringLiteral("Make the picture smaller in Photoshop, then save again."));
    const int cw = int(width), ch = int(height);

    r.skip(r.u32()); // color mode data
    r.skip(r.u32()); // image resources
    if (!r.ok())
        return unreadable(QStringLiteral("its sections are cut short"));
    const quint32 layerSectionLength = r.u32();
    Reader section = r.take(layerSectionLength);
    if (!r.ok())
        return unreadable(QStringLiteral("the layer section is cut short"));

    Errors dropped;
    std::vector<Record> records;
    // ---- the layer records
    int layerTotal = 0;
    bool mergedHasAlpha = false;
    Reader info(nullptr, 0);
    qint64 infoLength = 0;
    if (layerSectionLength > 0) {
        infoLength = section.u32();
        info = section.take(infoLength);
        if (!section.ok())
            return unreadable(QStringLiteral("the layer information is cut short"));
    }
    if (infoLength >= 2) {
        const int count = info.i16();
        mergedHasAlpha = count < 0; // only then is the merged image's first alpha channel its transparency
        layerTotal = std::abs(count);
        if (layerTotal > 4 * paintFileMaxLayers + 16)
            return cannotHold(QStringLiteral("This Photoshop file has %1 layers; a Fugue canvas holds at most %2.").arg(layerTotal).arg(paintFileMaxLayers),
                              QStringLiteral("Merge or delete layers in Photoshop, then save again."));
        for (int i = 0; i < layerTotal; ++i) {
            Record rec;
            rec.top = info.i32(), rec.left = info.i32(), rec.bottom = info.i32(), rec.right = info.i32();
            const int n = info.u16();
            if (!info.ok() || n > 56)
                return unreadable(QStringLiteral("layer %1 has a damaged record").arg(i + 1));
            for (int c = 0; c < n; ++c) {
                Channel channel;
                channel.id = info.i16();
                channel.length = info.u32();
                rec.channels.push_back(channel);
            }
            info.skip(4); // 8BIM
            const QByteArray key = info.bytes(4);
            rec.blendKey = QString::fromLatin1(key);
            rec.opacity = info.u8();
            rec.clipping = info.u8() != 0;
            const int flags = info.u8();
            rec.visible = !(flags & 2);
            info.skip(1);
            Reader extra = info.take(info.u32());
            if (!info.ok())
                return unreadable(QStringLiteral("layer %1 has a damaged record").arg(i + 1));
            const quint32 maskLength = extra.u32();
            Reader maskData = extra.take(maskLength);
            if (maskLength >= 18) {
                rec.maskTop = maskData.i32(), rec.maskLeft = maskData.i32(), rec.maskBottom = maskData.i32(), rec.maskRight = maskData.i32();
                rec.maskDefault = maskData.u8();
                const int maskFlags = maskData.u8();
                rec.maskDisabled = maskFlags & 2;
                rec.maskRelative = maskFlags & 1; // the rectangle is counted from the layer's top left
                if (maskFlags & 16) { // parameters: a density byte and/or a feather double, for the user and the vector mask
                    const int params = maskData.u8();
                    if (params & 1) maskData.skip(1);
                    if (params & 2) maskData.skip(8);
                    if (params & 4) maskData.skip(1);
                    if (params & 8) maskData.skip(8);
                }
                rec.hasMask = maskData.ok();
                if (rec.hasMask && maskData.left() >= 18) { // the form with a real user mask: flags, default, rectangle
                    const int realFlags = maskData.u8();
                    rec.realDefault = maskData.u8();
                    rec.realTop = maskData.i32(), rec.realLeft = maskData.i32(), rec.realBottom = maskData.i32(), rec.realRight = maskData.i32();
                    rec.realMaskDisabled = realFlags & 2;
                    rec.hasRealMask = maskData.ok() && (realFlags & 1) == 0;
                }
                if (rec.maskRelative) {
                    rec.maskTop += rec.top, rec.maskBottom += rec.top, rec.maskLeft += rec.left, rec.maskRight += rec.left;
                }
            }
            extra.skip(extra.u32()); // blending ranges
            const int nameLength = extra.u8();
            rec.name = QString::fromLatin1(extra.bytes(nameLength));
            extra.skip((4 - (1 + nameLength) % 4) % 4);
            if (!extra.ok())
                return unreadable(QStringLiteral("layer %1 has a damaged record").arg(i + 1));
            // Tagged blocks: 8BIM/8B64, a key, a length, the data.
            while (extra.left() >= 12) {
                const QByteArray sig = extra.bytes(4);
                if (sig != "8BIM" && sig != "8B64")
                    break;
                const QString tag = QString::fromLatin1(extra.bytes(4));
                const quint32 length = extra.u32();
                Reader data = extra.take(length);
                if (!extra.ok())
                    break;
                if (tag == QLatin1String("luni")) {
                    const quint32 chars = data.u32();
                    if (data.ok() && qint64(chars) * 2 <= data.left()) {
                        QString name;
                        for (quint32 k = 0; k < chars; ++k)
                            name += QChar(data.u16());
                        while (name.endsWith(QChar(0)))
                            name.chop(1);
                        if (!name.isEmpty()) {
                            rec.name = name;
                        }
                    }
                } else if (tag == QLatin1String("lsct")) {
                    rec.divider = int(data.u32());
                } else if (adjustmentKeys().contains(tag)) {
                    rec.adjustment = true;
                } else if (tag == QLatin1String("TySh") || tag == QLatin1String("tySh")) {
                    rec.text = true;
                } else if (tag == QLatin1String("SoLd") || tag == QLatin1String("PlLd") || tag == QLatin1String("SoLE") || tag == QLatin1String("lnkD") ||
                           tag == QLatin1String("lnk2") || tag == QLatin1String("lnk3")) {
                    rec.smart = true;
                } else if (tag == QLatin1String("lfx2") || tag == QLatin1String("lrFX")) {
                    rec.effects = true;
                } else if (tag == QLatin1String("vmsk") || tag == QLatin1String("vsms")) {
                    rec.vectorMask = true;
                }
                // a block's length is as the writer padded it; any stray pad bytes before the next signature are skipped
                while (extra.left() >= 12 && extra.here()[0] != '8')
                    extra.skip(1);
            }
            records.push_back(rec);
        }
    }

    // ---- what is kept, and the sizes of every record checked before any pixel is decoded
    int real = 0;
    for (const Record &rec : records)
        if (rec.divider == 0 && !rec.adjustment)
            ++real;
    if (real > paintFileMaxLayers)
        return cannotHold(QStringLiteral("This Photoshop file has %1 layers; a Fugue canvas holds at most %2.").arg(real).arg(paintFileMaxLayers),
                          QStringLiteral("Merge or delete layers in Photoshop, then save again."));
    for (const Record &rec : records) {
        if (rec.divider != 0 || rec.adjustment)
            continue;
        const auto fine = [](qint64 w, qint64 h) { return w >= 0 && h >= 0 && w <= psdMaxSide && h <= psdMaxSide; };
        if (!fine(rec.layerWidth(), rec.layerHeight()) || (rec.hasMask && !fine(rec.maskRight - rec.maskLeft, rec.maskBottom - rec.maskTop)) ||
            (rec.hasRealMask && !fine(rec.realRight - rec.realLeft, rec.realBottom - rec.realTop)))
            return unreadable(QStringLiteral("layer \"%1\" has a size the format does not allow").arg(rec.name));
    }

    // ---- the channel data of each record, in order; only what is kept is decoded, and the total is capped
    std::vector<std::vector<std::pair<int, std::vector<uint8_t>>>> planes(records.size()); // per record: channel id and its bytes
    qint64 decoded = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        Record &rec = records[i];
        QSet<int> seen;
        for (const Channel &channel : rec.channels) {
            Reader data = info.take(channel.length);
            if (!info.ok())
                return unreadable(QStringLiteral("the pixels of layer %1 are cut short").arg(i + 1));
            if (seen.contains(channel.id))
                return unreadable(QStringLiteral("layer %1 has the same channel twice").arg(i + 1));
            seen.insert(channel.id);
            if (channel.length < 2 || rec.divider != 0 || rec.adjustment)
                continue; // nothing kept to read
            const int compression = data.u16();
            qint64 w = rec.layerWidth(), h = rec.layerHeight();
            if (channel.id == -2) {
                if (!rec.hasMask)
                    continue;
                w = rec.maskRight - rec.maskLeft;
                h = rec.maskBottom - rec.maskTop;
            } else if (channel.id == -3) {
                if (!rec.hasRealMask)
                    continue;
                w = rec.realRight - rec.realLeft;
                h = rec.realBottom - rec.realTop;
            } else if (channel.id < -3 || channel.id > 2) {
                continue; // a spot or extra alpha channel of the layer: not read
            }
            decoded += w * h;
            if (decoded > paintFileMaxLayerBytes)
                return cannotHold(QStringLiteral("The layers of this Photoshop file hold more pixels than a canvas can keep."), QStringLiteral("Make the picture smaller or merge layers, then save again."));
            std::optional<std::vector<uint8_t>> plane = decodeChannel(compression, data, int(w), int(h));
            if (!plane)
                return unreadable(QStringLiteral("the pixels of layer \"%1\" are damaged or use a compression Fugue does not read").arg(rec.name));
            planes[i].emplace_back(channel.id, std::move(*plane));
        }
    }

    // ---- the layers
    if (qint64(real) * cw * ch * 4 > paintFileMaxLayerBytes)
        return cannotHold(QStringLiteral("The layers of this Photoshop file hold more pixels than a canvas can keep."), QStringLiteral("Make the picture smaller or merge layers, then save again."));

    // A divider opens a level; the folder record that follows its layers closes it into a group (the file runs bottom up).
    std::vector<std::vector<PsdLayer>> levels(1);
    std::vector<bool> levelLossy(1, false); // an adjustment layer or clipping was dropped inside the level
    int kept = 0;
    for (size_t k = 0; k < records.size(); ++k) {
        const Record &rec = records[k];
        if (rec.divider == 3) {
            levels.emplace_back();
            levelLossy.push_back(false);
            continue;
        }
        if (rec.divider == 1 || rec.divider == 2) {
            if (levels.size() == 1) {
                dropped << warning(QStringLiteral("The group \"%1\" has no end in the file: it was ignored.").arg(rec.name));
                continue;
            }
            PsdLayer g;
            g.group = true;
            g.open = rec.divider == 1;
            g.info.kind = LayerKind::Paint;
            g.info.name = rec.name.left(64);
            g.info.visible = rec.visible;
            g.info.opacity = rec.opacity / 255.0;
            bool known = true;
            g.info.compositeOp = psdOpOfBlendKey(rec.blendKey, &known);
            g.lossy = levelLossy.back() || rec.hasMask || rec.vectorMask || (!known && rec.blendKey != QLatin1String("pass"));
            g.children = std::move(levels.back());
            levels.pop_back();
            levelLossy.pop_back();
            levels.back().push_back(std::move(g));
            continue;
        }
        if (rec.adjustment) {
            dropped << warning(QStringLiteral("The adjustment layer \"%1\" was dropped: Fugue has no adjustment layers.").arg(rec.name));
            levelLossy.back() = true;
            continue;
        }
        const auto planeOfId = [&](int id) -> const std::vector<uint8_t> * {
            for (const auto &[channel, data] : planes[k])
                if (channel == id)
                    return &data;
            return nullptr;
        };
        const qint64 lw = std::max<qint64>(0, rec.layerWidth()), lh = std::max<qint64>(0, rec.layerHeight());
        const std::vector<uint8_t> *colors[3] = {planeOfId(0), planeOfId(1), planeOfId(2)};
        const std::vector<uint8_t> *alpha = planeOfId(-1);
        std::vector<uint8_t> rgba(size_t(cw) * ch * 4, 0);
        // Only the part of the layer on the canvas is visited (the rectangle is the file's, not to be trusted for a loop bound).
        const qint64 y0 = std::max<qint64>(0, -rec.top), y1 = std::min<qint64>(lh, ch - rec.top);
        const qint64 x0 = std::max<qint64>(0, -rec.left), x1 = std::min<qint64>(lw, cw - rec.left);
        for (qint64 y = y0; y < y1; ++y) {
            const qint64 cy = rec.top + y;
            for (qint64 x = x0; x < x1; ++x) {
                const qint64 cx = rec.left + x;
                const size_t from = size_t(y * lw + x), to = size_t((cy * cw + cx) * 4);
                for (int c = 0; c < 3; ++c)
                    rgba[to + c] = colors[c] && from < colors[c]->size() ? (*colors[c])[from] : 0;
                rgba[to + 3] = alpha ? (from < alpha->size() ? (*alpha)[from] : 0) : 255;
            }
        }
        if (lw > 0 && lh > 0 && (rec.left < 0 || rec.top < 0 || rec.right > cw || rec.bottom > ch))
            dropped << warning(QStringLiteral("The layer \"%1\" reaches past the canvas: the part outside was cut off.").arg(rec.name));

        LayerInfo li;
        li.kind = LayerKind::Paint;
        li.name = rec.name.left(64);
        li.visible = rec.visible;
        li.opacity = rec.opacity / 255.0;
        bool known = true;
        li.compositeOp = psdOpOfBlendKey(rec.blendKey, &known);
        if (!known)
            dropped << warning(QStringLiteral("The layer \"%1\" has the blend mode %2, which Fugue does not have: it shows as Normal.").arg(rec.name, keyName(rec.blendKey)));
        else if (!li.compositeOp.isEmpty()) {
            bool drawn = true;
            blendOfOp(li.compositeOp, &drawn);
            if (!drawn)
                dropped << warning(QStringLiteral("The layer \"%1\" has a blend mode Fugue does not draw yet (%2): it shows as Normal here and is kept for exporting.").arg(rec.name, li.compositeOp.mid(4)));
        }
        if (rec.text)
            dropped << warning(QStringLiteral("The text layer \"%1\" is now plain pixels: its text can no longer be edited.").arg(rec.name));
        if (rec.smart)
            dropped << warning(QStringLiteral("The smart object \"%1\" is now plain pixels: its linked or embedded source was dropped.").arg(rec.name));
        if (rec.effects)
            dropped << warning(QStringLiteral("The layer effects of \"%1\" (shadow, stroke, glow...) were dropped.").arg(rec.name));
        if (rec.vectorMask)
            dropped << warning(QStringLiteral("The vector mask of \"%1\" was dropped.").arg(rec.name));
        if (rec.clipping) {
            dropped << warning(QStringLiteral("The layer \"%1\" was clipped to the layer below: the clipping was dropped.").arg(rec.name));
            levelLossy.back() = true;
        }

        // A layer with a vector mask too carries its pixel mask as channel -3 (the "real" user mask): prefer that one.
        const std::vector<uint8_t> *maskPlane = rec.hasRealMask && planeOfId(-3) ? planeOfId(-3) : planeOfId(-2);
        const bool usesReal = maskPlane && maskPlane == planeOfId(-3);
        const qint64 mTop = usesReal ? rec.realTop : rec.maskTop, mLeft = usesReal ? rec.realLeft : rec.maskLeft;
        const qint64 mw = std::max<qint64>(0, usesReal ? rec.realRight - rec.realLeft : rec.maskRight - rec.maskLeft);
        const qint64 mh = std::max<qint64>(0, usesReal ? rec.realBottom - rec.realTop : rec.maskBottom - rec.maskTop);
        const int mDefault = usesReal ? rec.realDefault : rec.maskDefault;
        std::optional<MaskPaint> layerMask;
        if (rec.hasMask && (maskPlane || mw == 0 || mh == 0)) {
            std::vector<uint8_t> mask(size_t(cw) * ch, uint8_t(mDefault));
            if (maskPlane) {
                const qint64 my0 = std::max<qint64>(0, -mTop), my1 = std::min<qint64>(mh, ch - mTop);
                const qint64 mx0 = std::max<qint64>(0, -mLeft), mx1 = std::min<qint64>(mw, cw - mLeft);
                for (qint64 y = my0; y < my1; ++y)
                    for (qint64 x = mx0; x < mx1; ++x)
                        if (size_t(y * mw + x) < maskPlane->size())
                            mask[size_t((mTop + y) * cw + mLeft + x)] = (*maskPlane)[size_t(y * mw + x)];
            }
            layerMask = MaskPaint(cw, ch, mask, 1);
            li.hasMask = true;
            li.maskEnabled = !(usesReal ? rec.realMaskDisabled : rec.maskDisabled);
        }
        PsdLayer leaf;
        leaf.info = li;
        leaf.pixels = MaskPaint(cw, ch, rgba, 4);
        leaf.mask = layerMask;
        levels.back().push_back(std::move(leaf));
        ++kept;
    }
    while (levels.size() > 1) { // groups that were never closed: their layers join the level below
        dropped << warning(QStringLiteral("A group in the file has no header: its layers were put in the level below."));
        std::vector<PsdLayer> inner = std::move(levels.back());
        levels.pop_back();
        for (PsdLayer &l : inner)
            levels.back().push_back(std::move(l));
    }
    PsdImported imported;
    imported.width = cw;
    imported.height = ch;
    imported.layers = std::move(levels.front());

    // ---- no layers (a flat file, or every layer dropped): the merged image is the canvas
    if (kept == 0) {
        const quint32 compression = r.u16();
        const qint64 planeBytes = qint64(cw) * ch;
        if (!r.ok() || (compression != 0 && compression != 1))
            return unreadable(QStringLiteral("it holds no layers and its merged picture is missing or uses a compression Fugue does not read"));
        std::vector<std::vector<uint8_t>> merged;
        const bool withAlpha = mergedHasAlpha && channelCount >= 4; // a flat file's 4th channel may be a saved selection, not transparency
        const int usable = withAlpha ? 4 : 3;
        if (compression == 0) {
            for (int c = 0; c < usable; ++c) {
                std::optional<std::vector<uint8_t>> plane = decodeChannel(0, r.take(planeBytes), cw, ch);
                if (!r.ok() || !plane)
                    return unreadable(QStringLiteral("its merged picture is cut short"));
                merged.push_back(std::move(*plane));
            }
        } else {
            QList<QByteArray> counts;
            for (int c = 0; c < channelCount; ++c)
                counts << r.bytes(qint64(ch) * 2);
            if (!r.ok())
                return unreadable(QStringLiteral("its merged picture is cut short"));
            for (int c = 0; c < usable; ++c) {
                qint64 size = 0;
                Reader table(reinterpret_cast<const uint8_t *>(counts[c].constData()), counts[c].size());
                for (int y = 0; y < ch; ++y)
                    size += table.u16();
                QByteArray one = counts[c] + r.bytes(size);
                if (!r.ok())
                    return unreadable(QStringLiteral("its merged picture is cut short"));
                std::optional<std::vector<uint8_t>> plane = decodeChannel(1, Reader(reinterpret_cast<const uint8_t *>(one.constData()), one.size()), cw, ch);
                if (!plane)
                    return unreadable(QStringLiteral("its merged picture is damaged"));
                merged.push_back(std::move(*plane));
            }
        }
        if (merged.size() < 3)
            return unreadable(QStringLiteral("its merged picture has fewer than 3 color channels"));
        std::vector<uint8_t> rgba(size_t(cw) * ch * 4);
        for (size_t p = 0; p < size_t(cw) * ch; ++p) {
            for (int c = 0; c < 3; ++c)
                rgba[p * 4 + c] = merged[size_t(c)][p];
            rgba[p * 4 + 3] = withAlpha ? merged[3][p] : 255;
            if (withAlpha && rgba[p * 4 + 3] > 0 && rgba[p * 4 + 3] < 255) // the white Photoshop blended in: take it out
                for (int c = 0; c < 3; ++c) {
                    const int a = rgba[p * 4 + 3];
                    rgba[p * 4 + c] = uint8_t(std::clamp((rgba[p * 4 + c] * 255 - 255 * (255 - a) + a / 2) / a, 0, 255));
                }
        }
        imported.layers.clear(); // groups with nothing in them
        imported.merged = MaskPaint(cw, ch, rgba, 4);
        if (layerTotal > 0)
            dropped << warning(QStringLiteral("No layer could be kept, so the picture is the file's merged image on one layer."));
    }
    return Result<PsdImported>(std::move(imported), std::move(dropped));
}

namespace {

QString groupWarning(const PsdLayer &g, bool mask) {
    QString what = QStringLiteral("The group \"%1\" was flattened: its layers are kept in order, with the group's visibility and opacity applied to each.").arg(g.info.name);
    if (mask)
        what += QStringLiteral(" Its own mask or blend mode was dropped.");
    return what;
}

// The layers of `in` with every group flattened into `out` (its visibility and opacity folded into each layer).
void flattenGroups(const std::vector<PsdLayer> &in, bool visible, double opacity, std::vector<PsdLayer> &out, Errors &warnings, bool keepSimple, int depth) {
    for (const PsdLayer &l : in) {
        if (!l.group) {
            PsdLayer c = l;
            c.info.visible = l.info.visible && visible;
            c.info.opacity = l.info.opacity * opacity;
            out.push_back(std::move(c));
        } else if (keepSimple && !l.lossy && !l.children.empty() && depth < 8) {
            PsdLayer c = l;
            c.children.clear();
            c.info.visible = l.info.visible && visible;
            c.info.opacity = l.info.opacity * opacity;
            flattenGroups(l.children, true, 1.0, c.children, warnings, true, depth + 1);
            if (c.children.empty()) {
                warnings << warning(QStringLiteral("The group \"%1\" holds no layer: it was dropped.").arg(l.info.name));
                continue;
            }
            out.push_back(std::move(c));
        } else {
            warnings << warning(groupWarning(l, l.lossy || l.mask.has_value() || !l.info.compositeOp.isEmpty()));
            flattenGroups(l.children, visible && l.info.visible, opacity * l.info.opacity, out, warnings, keepSimple, depth + 1);
        }
    }
}

} // namespace

Result<PsdImported> paintFromPsdSettled(const QByteArray &bytes) {
    Result<PsdImported> read = paintFromPsdLayers(bytes);
    if (!read)
        return read;
    Errors warnings = read.warnings();
    PsdImported imported = std::move(read.value());
    std::vector<PsdLayer> settled;
    flattenGroups(imported.layers, true, 1.0, settled, warnings, true, 0);
    imported.layers = std::move(settled);
    return Result<PsdImported>(std::move(imported), std::move(warnings));
}

Result<PaintDocument> paintFromPsd(const QByteArray &bytes) {
    Result<PsdImported> read = paintFromPsdLayers(bytes);
    if (!read)
        return Result<PaintDocument>(read.errors());
    Errors warnings = read.warnings();
    std::vector<PsdLayer> flat;
    flattenGroups(read->layers, true, 1.0, flat, warnings, false, 0);
    PaintDocument document(read->width, read->height);
    for (const PsdLayer &l : flat)
        document.loadLayer(l.info, l.pixels, l.mask ? &*l.mask : nullptr);
    if (flat.empty() && read->merged)
        document.loadLayer(LayerKind::Paint, QStringLiteral("Background"), true, 1.0, *read->merged);
    document.forgetHistory();
    return Result<PaintDocument>(std::move(document), std::move(warnings));
}

} // namespace fugue::mask
