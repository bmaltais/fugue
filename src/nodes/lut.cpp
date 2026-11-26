#include "nodes/lut.h"

// Ticket #548: Apply LUT (spec 14.2). The `.cube` format is Adobe's published specification
// (Cube LUT Specification 1.0): keywords, then size^3 (or size) rows of three floats.

#include <QRegularExpression>
#include <QStringList>
#include <QStringView>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <memory>

#include "nodes/kernels.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

namespace {

constexpr int kMaxSize1d = 65536;
constexpr int kMaxSize3d = 256;

Error lineError(int line, const QString &what) {
    return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("file"),
                          QStringLiteral("The LUT file is not a valid .cube file: line %1: %2").arg(line).arg(what),
                          QStringLiteral("Pick a .cube file with LUT_1D_SIZE or LUT_3D_SIZE and one row of three numbers per entry."));
}

// Three numbers from the words after a keyword, or from a data row.
bool triple(const QStringList &words, int first, float out[3]) {
    if (words.size() != first + 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const double v = words[first + i].toDouble(&ok);
        if (!ok || !std::isfinite(v))
            return false;
        out[i] = float(v);
    }
    return true;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// The entry's channel `c` at (r, g, b) of a 3D table.
float at3(const Lut &lut, int r, int g, int b, int c) {
    return lut.table[((size_t(b) * lut.size + g) * lut.size + r) * 3 + c];
}

} // namespace

Result<Lut> parseCube(const QString &text, bool headerOnly) {
    Lut lut;
    bool haveSize = false;
    size_t wanted = 0;
    int lineNo = 0;
    int lastLine = 0;
    bool stopped = false;
    for (const QStringView raw : QStringView(text).split(QLatin1Char('\n'))) {
        ++lineNo;
        QString line = raw.toString();
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        lastLine = lineNo;
        const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString word = words.first();
        const bool keyword = word.at(0).isLetter();
        if (!keyword) {
            if (!haveSize)
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("a data row comes before LUT_1D_SIZE or LUT_3D_SIZE"))});
            if (headerOnly) {
                stopped = true; // the keywords are all above the first row: the rest is not read
                break;
            }
            float row[3];
            if (!triple(words, 0, row))
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("expected three numbers, found \"%1\"").arg(line))});
            if (lut.table.size() >= wanted)
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("more rows than the %1 the size asks for").arg(wanted / 3))});
            lut.table.insert(lut.table.end(), row, row + 3);
            continue;
        }
        if (word == QLatin1String("TITLE"))
            continue;
        if (word == QLatin1String("LUT_1D_SIZE") || word == QLatin1String("LUT_3D_SIZE")) {
            if (haveSize)
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("a second size keyword (one LUT per file)"))});
            const bool is3d = word == QLatin1String("LUT_3D_SIZE");
            bool ok = false;
            const int size = words.size() == 2 ? words[1].toInt(&ok) : 0;
            if (!ok || size < 2 || size > (is3d ? kMaxSize3d : kMaxSize1d))
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("%1 needs a whole number from 2 to %2").arg(word).arg(is3d ? kMaxSize3d : kMaxSize1d))});
            lut.is3d = is3d;
            lut.size = size;
            haveSize = true;
            wanted = (is3d ? size_t(size) * size * size : size_t(size)) * 3;
            lut.table.reserve(wanted);
            continue;
        }
        if (word == QLatin1String("DOMAIN_MIN") || word == QLatin1String("DOMAIN_MAX")) {
            float v[3];
            if (!triple(words, 1, v))
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("%1 needs three numbers").arg(word))});
            std::copy(v, v + 3, word == QLatin1String("DOMAIN_MIN") ? lut.domainMin : lut.domainMax);
            continue;
        }
        if (word == QLatin1String("LUT_1D_INPUT_RANGE") || word == QLatin1String("LUT_3D_INPUT_RANGE")) {
            // DaVinci Resolve's form of the domain: one min and one max for all three channels.
            bool okMin = false, okMax = false;
            const double lo = words.size() == 3 ? words[1].toDouble(&okMin) : 0.0;
            const double hi = words.size() == 3 ? words[2].toDouble(&okMax) : 0.0;
            if (!okMin || !okMax || !std::isfinite(lo) || !std::isfinite(hi))
                return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("%1 needs two numbers").arg(word))});
            std::fill(lut.domainMin, lut.domainMin + 3, float(lo));
            std::fill(lut.domainMax, lut.domainMax + 3, float(hi));
            continue;
        }
        return Result<Lut>(Errors{lineError(lineNo, QStringLiteral("unknown keyword \"%1\"").arg(word))});
    }
    if (headerOnly && !stopped)
        return lut; // the text ended where the file was cut: nothing more is known
    if (!haveSize)
        return Result<Lut>(Errors{lineError(std::max(lastLine, 1), QStringLiteral("no LUT_1D_SIZE or LUT_3D_SIZE"))});
    if (!headerOnly && lut.table.size() != wanted)
        return Result<Lut>(Errors{lineError(std::max(lastLine, 1), QStringLiteral("the file ends after %1 of the %2 rows the size asks for")
                                                                        .arg(lut.table.size() / 3).arg(wanted / 3))});
    for (int c = 0; c < 3; ++c)
        if (!(lut.domainMax[c] > lut.domainMin[c]))
            return Result<Lut>(Errors{lineError(std::max(lastLine, 1), QStringLiteral("DOMAIN_MAX is not above DOMAIN_MIN"))});
    return lut;
}

Pixels applyLut(const Pixels &image, const Lut &lut, float strength) {
    if (lut.size < 2 || image.channels < 3 || !(strength > 0.0f))
        return image;
    const float amount = std::min(strength, 1.0f);
    const int last = lut.size - 1;
    Pixels out = image;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            // Each channel's position in the table, 0..size-1.
            float pos[3];
            for (int c = 0; c < 3; ++c) {
                const float t = (image.at(x, y, c) - lut.domainMin[c]) / (lut.domainMax[c] - lut.domainMin[c]);
                pos[c] = std::clamp(t, 0.0f, 1.0f) * float(last);
            }
            float result[3];
            if (!lut.is3d) {
                for (int c = 0; c < 3; ++c) {
                    const int i0 = std::min(int(pos[c]), last - 1);
                    const float f = pos[c] - float(i0);
                    result[c] = lerp(lut.table[size_t(i0) * 3 + c], lut.table[size_t(i0 + 1) * 3 + c], f);
                }
            } else {
                int i0[3];
                float f[3];
                for (int c = 0; c < 3; ++c) {
                    i0[c] = std::min(int(pos[c]), last - 1);
                    f[c] = pos[c] - float(i0[c]);
                }
                for (int c = 0; c < 3; ++c) {
                    const auto v = [&](int dr, int dg, int db) { return at3(lut, i0[0] + dr, i0[1] + dg, i0[2] + db, c); };
                    const float c00 = lerp(v(0, 0, 0), v(1, 0, 0), f[0]);
                    const float c10 = lerp(v(0, 1, 0), v(1, 1, 0), f[0]);
                    const float c01 = lerp(v(0, 0, 1), v(1, 0, 1), f[0]);
                    const float c11 = lerp(v(0, 1, 1), v(1, 1, 1), f[0]);
                    result[c] = lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]);
                }
            }
            for (int c = 0; c < 3; ++c)
                out.at(x, y, c) = lerp(image.at(x, y, c), result[c], amount);
        }
    return out;
}

namespace {

// How much of the file the check reads: the keywords are in the first lines, a data row ends them.
constexpr qint64 kCheckBytes = 8192;

// The LUT file at `path` as an error would be told, or nothing: that it is there and can be opened, and when
// `parsed` is not null the whole file is read and parsed into it, else only its first lines (or all of a short file).
std::optional<Errors> lutProblems(const QString &path, Lut *parsed) {
    QFile file(path);
    if (!QFileInfo(path).isFile())
        return Errors{makeInputError(QStringLiteral("file_not_found"), QStringLiteral("file"),
                                     QStringLiteral("No LUT file at %1.").arg(path), QStringLiteral("Pick the .cube file again in the node."))};
    if (!file.open(QIODevice::ReadOnly))
        return Errors{makeInputError(QStringLiteral("file_not_found"), QStringLiteral("file"),
                                     QStringLiteral("The LUT file %1 cannot be read: %2").arg(path, file.errorString()))};
    bool headerOnly = false;
    QByteArray bytes;
    if (parsed) {
        bytes = file.readAll();
    } else {
        bytes = file.read(kCheckBytes);
        if (!file.atEnd()) { // a cut file: only the whole lines, and only what they say
            headerOnly = true;
            bytes.truncate(bytes.lastIndexOf('\n') + 1);
        }
    }
    const Result<Lut> lut = parseCube(QString::fromUtf8(bytes), headerOnly);
    if (!lut)
        return lut.errors();
    if (parsed)
        *parsed = lut.value();
    return std::nullopt;
}

class ApplyLutKernel : public NodeKernel {
public:
    // Under validate: the file is there, and its first lines are a good .cube header (at most a few KB are read).
    Errors check(const NodeContext &context) const override {
        const Value file = context.input(QStringLiteral("file"));
        if (file.isEmpty() || file.text().isEmpty())
            return {};
        return lutProblems(file.text(), nullptr).value_or(Errors{});
    }

    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const double strength = context.input("strength").number();
        if (!(strength >= 0.0 && strength <= 1.0))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("strength"),
                                  QStringLiteral("A strength of %1 is outside 0 to 1.").arg(strength));
        // As Load Image reads its file: the path was expanded and, with roots, checked.
        QString path = context.input("file").text();
        if (context.roots) {
            const Result<QString> readable = context.roots->checkRead(path, QStringLiteral("file"));
            if (!readable)
                return readable.errors();
            path = readable.value();
        }
        Lut lut;
        if (const std::optional<Errors> problems = lutProblems(path, &lut))
            return *problems;
        return imageOutput(applyLut(image.value(), lut, float(strength)));
    }
};

} // namespace

void addLutKernels(Kernels &kernels) {
    kernels.add("fugue/apply-lut", std::make_shared<ApplyLutKernel>());
}

} // namespace fugue::nodes
