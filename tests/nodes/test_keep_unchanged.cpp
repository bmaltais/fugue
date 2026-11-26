// The group "Keep unchanged areas" the fugue pack ships (spec 14.4, #621): the registry entry and the
// definition, and the shipped `fugue` binary running it on synthetic pairs (CPU only, no helper).
//
// The pair: a smooth picture with a little grain, and an edit of it with one region redrawn, a small
// global brightness and colour shift, and a slight blur, as an edit model returns. The checks are
// the group's promise: the mask covers the changed region, it is 0 far from it, and where the mask is
// exactly 0 the composite is the original's pixel for pixel. A second edit comes back at another size
// and aspect ratio and must still land on the original's pixels.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include <vips/vips8>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/group_library.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/pixel_io.h"

using namespace fugue;
using namespace fugue::nodes;

namespace {

constexpr int W = 320;
constexpr int H = 240;
// The redrawn region, and how far from it the mask must be 0 (margin 16 + feather 16 + blur 4, with room).
constexpr int RX = 120, RY = 80, RW = 80, RH = 80;
constexpr int FAR = 70;

Pixels picture() {
    Pixels p(W, H, 3);
    unsigned s = 12345u;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                s = s * 1664525u + 1013904223u;
                p.at(x, y, c) = 0.25f + 0.3f * (float(x) / W * (c + 1) / 3 + float(y) / H * 0.3f) +
                                0.05f * std::sin(x * 0.05f + c) * std::cos(y * 0.07f) + 0.01f * float((s >> 24) & 0xFF) / 255.0f;
            }
    return p;
}

float bilinear(const Pixels &p, float x, float y, int c) {
    x = std::clamp(x, 0.0f, float(p.width - 1));
    y = std::clamp(y, 0.0f, float(p.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, p.width - 1), y1 = std::min(y0 + 1, p.height - 1);
    const float fx = x - x0, fy = y - y0;
    return (p.at(x0, y0, c) * (1 - fx) + p.at(x1, y0, c) * fx) * (1 - fy) + (p.at(x0, y1, c) * (1 - fx) + p.at(x1, y1, c) * fx) * fy;
}

// What an edit model returns: a region redrawn, a global shift, a slight blur.
Pixels edit(const Pixels &original) {
    Pixels e = original;
    for (int y = RY; y < RY + RH; ++y)
        for (int x = RX; x < RX + RW; ++x)
            for (int c = 0; c < 3; ++c)
                e.at(x, y, c) = (c == 0 ? 0.95f : c == 1 ? 0.8f : 0.1f) + 0.04f * std::sin((x + y) * 0.2f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            e.at(x, y, 0) += 0.03f; // brighter, a little warmer
            e.at(x, y, 1) += 0.02f;
            e.at(x, y, 2) -= 0.01f;
        }
    Pixels blurred = e;
    for (int y = 1; y < H - 1; ++y)
        for (int x = 1; x < W - 1; ++x)
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        sum += e.at(x + i, y + j, c);
                blurred.at(x, y, c) = sum / 9;
            }
    return blurred;
}

// The edit scaled by 1.05 and cut to 330 wide: another size and another aspect ratio, the same picture.
Pixels reshaped(const Pixels &edited) {
    const float scale = 1.05f;
    Pixels r(330, int(std::lround(H * scale)), 3);
    const float offsetX = (W * scale - r.width) / 2;
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x)
            for (int c = 0; c < 3; ++c)
                r.at(x, y, c) = bilinear(edited, (x + offsetX + 0.5f) / scale - 0.5f, (y + 0.5f) / scale - 0.5f, c);
    return r;
}

void save(const QString &path, const Pixels &pixels) {
    initVips();
    std::vector<unsigned char> bytes(pixels.data.size());
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = (unsigned char)std::lround(std::clamp(pixels.data[i], 0.0f, 1.0f) * 255.0f);
    vips::VImage::new_from_memory_copy(bytes.data(), bytes.size(), pixels.width, pixels.height, pixels.channels, VIPS_FORMAT_UCHAR)
        .write_to_file(path.toUtf8().constData());
}

Pixels read(const QJsonObject &output) {
    const Result<MediaHandle> handle = MediaHandle::fromJson(output);
    if (!handle)
        qFatal("no picture in the outputs");
    const Result<Pixels> p = pixelsOf(Value(handle.value()), "image");
    if (!p)
        qFatal("cannot read %s", qPrintable(handle->path()));
    return p.value();
}

bool far(int x, int y) {
    return x < RX - FAR || x >= RX + RW + FAR || y < RY - FAR || y >= RY + RH + FAR;
}

class TestKeepUnchanged : public QObject {
    Q_OBJECT

    // The shipped binary running the group on `edited`; the outputs of its image and mask.
    struct Run {
        Pixels original, image, mask; // the original as its 8-bit file reads
    };

    Run run(const QTemporaryDir &dir, const Pixels &original, const Pixels &edited) {
        QDir().mkpath(dir.filePath("inputs"));
        save(dir.filePath("inputs/original.png"), original);
        save(dir.filePath("inputs/edited.png"), edited);
        const auto load = [&](const QString &file) {
            return QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"},
                               {"inputs", QJsonObject{{"file", dir.filePath("inputs/" + file)}}}};
        };
        const auto from = [](const QString &output) { return QJsonObject{{"from", output}}; };
        const QJsonObject nodes{
            {"original", load("original.png")},
            {"edited", load("edited.png")},
            {"keep", QJsonObject{{"type", "fugue/keep-unchanged"}, {"version", "1.0.0"},
                                 {"inputs", QJsonObject{{"original", from("original.image")}, {"edited", from("edited.image")}}}}}};
        const QString path = dir.filePath("flow.fugue");
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("cannot write the flow");
        f.write(QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}}).toJson(QJsonDocument::Compact));
        f.close();
        Registry registry;
        addBuiltinNodeTypes(registry);
        Result<Document> document = Document::loadFile(path);
        if (!document)
            qFatal("%s", qPrintable(document.errors().value(0).message));
        embedGroupTypes(document.value(), registry); // as the canvas does when a group is added
        if (hasErrors(document->saveFile(path)))
            qFatal("cannot save the flow");

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        env.insert("XDG_CACHE_HOME", dir.filePath("cache"));
        env.insert("XDG_STATE_HOME", dir.filePath("state"));
        env.insert("FUGUE_HELPER", QStringLiteral(FUGUE_FAKE_HELPER));
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(dir.path());
        process.start(QStringLiteral(FUGUE_BINARY), {"run", path, "--to", "keep/paste", "--wait", "--json"});
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            qFatal("fugue timed out");
        }
        const QByteArray out = process.readAllStandardOutput();
        if (process.exitCode() != 0)
            qWarning("fugue: %s %s", out.constData(), process.readAllStandardError().constData());
        Run result;
        QJsonObject image, mask;
        for (const QByteArray &line : out.trimmed().split('\n')) {
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            if (event.value("event").toString() != QLatin1String("node.done"))
                continue;
            if (event.value("node").toString() == QLatin1String("keep/paste"))
                image = event.value("outputs").toObject().value("image").toObject();
            if (event.value("node").toString() == QLatin1String("keep/feather"))
                mask = event.value("outputs").toObject().value("mask").toObject();
        }
        if (image.isEmpty() || mask.isEmpty())
            qFatal("the run did not finish: %s", out.constData());
        FileHashes hashes(dir.filePath("hashes.json"));
        const Result<MediaHandle> file = MediaHandle::probe(MediaType::Image, dir.filePath("inputs/original.png"), hashes);
        if (!file)
            qFatal("cannot probe the original");
        result.original = read(file->toJson());
        result.image = read(image);
        result.mask = read(mask);
        return result;
    }

    // The promise, checked on one run.
    static void check(const Run &r, const char *what) {
        const Pixels &original = r.original;
        QCOMPARE(r.image.width, W);
        QCOMPARE(r.image.height, H);
        QCOMPARE(r.mask.width, W);
        QCOMPARE(r.mask.height, H);
        double inside = 0;
        for (int y = RY; y < RY + RH; ++y)
            for (int x = RX; x < RX + RW; ++x)
                inside += r.mask.at(x, y, 0);
        const double mean = inside / (RW * RH);
        QVERIFY2(mean >= 0.9, qPrintable(QStringLiteral("%1: mean mask in the region %2").arg(what).arg(mean)));
        int zero = 0, differing = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (far(x, y))
                    QVERIFY2(r.mask.at(x, y, 0) == 0.0f, qPrintable(QStringLiteral("%1: mask %2 at %3,%4").arg(what).arg(r.mask.at(x, y, 0)).arg(x).arg(y)));
                if (r.mask.at(x, y, 0) != 0.0f)
                    continue;
                ++zero;
                for (int c = 0; c < 3; ++c)
                    differing += r.image.at(x, y, c) != original.at(x, y, c);
            }
        QVERIFY(zero > W * H / 2);
        QCOMPARE(differing, 0); // where the mask is 0, the output is the original
        QVERIFY(std::fabs(r.image.at(RX + RW / 2, RY + RH / 2, 0) - original.at(RX + RW / 2, RY + RH / 2, 0)) > 0.05f); // the edit came through
        qInfo("%s: mean mask in the region %.3f, %d pixels with mask 0, %d of them differing", what, mean, zero, differing);
    }

private slots:
    void thePackShipsTheGroup() {
        Registry registry;
        addBuiltinNodeTypes(registry);
        const NodeType *group = registry.nodeType("fugue/keep-unchanged");
        QVERIFY(group);
        QCOMPARE(group->runtime.value("kind").toString(), QStringLiteral("group"));
        QCOMPARE(group->name, QStringLiteral("Keep unchanged areas"));
        QCOMPARE(group->category, QStringLiteral("Image/Mask"));
        QVERIFY(group->input("original") && group->input("edited"));
        const std::pair<const char *, double> knobs[] = {{"blur", 4}, {"threshold", 0.04}, {"speck", 12}, {"close", 24}, {"margin", 16}, {"feather", 16}};
        for (const auto &[name, value] : knobs) {
            QVERIFY2(group->input(QLatin1String(name)), name);
            QCOMPARE(group->input(QLatin1String(name))->defaultValue.toDouble(), value);
        }
        QVERIFY(group->output("image"));
        QVERIFY(group->output("mask"));
        QCOMPARE(group->output("mask")->role, QStringLiteral("mask"));

        const Result<Document> file = loadLibraryGroup(*group);
        QVERIFY2(file.ok(), qPrintable(file.errors().value(0).message));
        const GroupDefinition *definition = file->group("fugue/keep-unchanged");
        QVERIFY(definition);
        // Every exposed input and output names a node of the definition.
        for (const auto &input : definition->inputs)
            for (const PortRef &target : input.targets)
                QVERIFY2(definition->nodes.contains(target.node), qPrintable(target.toString()));
        Document doc;
        Node instance;
        instance.type = group->id;
        instance.version = group->version;
        doc.graph.nodes.insert("keep", instance);
        QCOMPARE(embedGroupTypes(doc, registry), QStringList{"fugue/keep-unchanged"});
        for (const Error &error : compile(doc, registry).errors())
            QVERIFY2(error.severity != Error::Severity::Error || error.code == QLatin1String("missing_input"),
                     qPrintable(error.code + ": " + error.message));
    }

    void theMaskCoversTheChangeAndTheRestIsTheOriginal() {
        QTemporaryDir dir;
        const Pixels original = picture();
        check(run(dir, original, edit(original)), "same size");
    }

    void anEditOfAnotherSizeAndAspectLandsOnTheOriginal() {
        QTemporaryDir dir;
        const Pixels original = picture();
        check(run(dir, original, reshaped(edit(original))), "another size");
    }
};

} // namespace

QTEST_GUILESS_MAIN(TestKeepUnchanged)
#include "test_keep_unchanged.moc"
