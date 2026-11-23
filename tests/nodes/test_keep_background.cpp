// The "Keep the background" part of the pose-from-description template's edit group: the result is pasted over
// picture 1 inside the grown, feathered union of the person's mask in both, so the setting stays picture 1's.
//
// Seam: the real template's nodes (union, grow, soft, paste, keep) of the edit group, run through the real
// scheduler on small synthetic pictures. The two Remove Background Mask nodes (a pack, an ONNX model) are
// replaced by masks read from PNG files; everything after them is the template's own.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <vips/vips8>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

constexpr int W = 96;
constexpr int H = 64;

// A grey picture, with a white square (the person) at x0, or none when x0 is negative.
QString savePng(const QString &file, int grey, int x0) {
    vips::VImage image = (vips::VImage::black(W, H, vips::VImage::option()->set("bands", 3)) + grey).cast(VIPS_FORMAT_UCHAR);
    if (x0 >= 0)
        image.draw_rect({255.0, 255.0, 255.0}, x0, 26, 12, 12, vips::VImage::option()->set("fill", true));
    image.write_to_file(file.toUtf8().constData());
    return file;
}

QJsonObject load(const QString &file) {
    return QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"file", file}}}};
}

QJsonObject mask(const QString &from) {
    return QJsonObject{{"type", "fugue/image-to-mask"}, {"version", "1.0.1"},
                       {"inputs", QJsonObject{{"image", QJsonObject{{"from", from}}}, {"channel", "red"}}}};
}

} // namespace

class TestKeepBackground : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    std::unique_ptr<FileHashes> m_hashes;
    std::unique_ptr<ResultCache> m_cache;
    QString m_picture, m_result, m_maskPicture, m_maskResult;

    // The template's chain with `keep` as the Advanced switch is set; the final picture's pixels.
    vips::VImage chain(double keep) {
        QFile in(QStringLiteral(FUGUE_SOURCE_DIR "/templates/pose-from-description-keep-background/workflow.fugue"));
        if (!in.open(QIODevice::ReadOnly))
            QTest::qFail("the template does not open", __FILE__, __LINE__);
        const QJsonObject edit = QJsonDocument::fromJson(in.readAll()).object().value("groups").toObject()
                                     .value("local/edit-with-klein-9b").toObject().value("nodes").toObject();
        QJsonObject nodes;
        nodes.insert("picture", load(m_picture));
        nodes.insert("result", load(m_result));
        nodes.insert("m1_file", load(m_maskPicture));
        nodes.insert("m2_file", load(m_maskResult));
        nodes.insert("bg1", mask("m1_file.image"));
        nodes.insert("bg2", mask("m2_file.image"));
        for (const char *name : {"union", "grow", "soft", "paste", "keep"}) {
            QJsonObject node = edit.value(QString::fromLatin1(name)).toObject();
            QJsonObject inputs = node.value("inputs").toObject();
            for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                // `color` (Color Match) is outside this seam: its output is the result as loaded.
                if (it.value().toObject().value("from").toString() == "color.image")
                    it.value() = QJsonObject{{"from", "result.image"}};
            }
            if (QString::fromLatin1(name) == "paste")
                inputs.insert("destination", QJsonObject{{"from", "picture.image"}});
            if (QString::fromLatin1(name) == "keep")
                inputs.insert("opacity", keep);
            node.insert("inputs", inputs);
            nodes.insert(QString::fromLatin1(name), node);
        }
        nodes.insert("shown", QJsonObject{{"type", "fugue/preview"}, {"version", "1.0.0"},
                                          {"inputs", QJsonObject{{"value", QJsonObject{{"from", "keep.image"}}}}}});
        const Result<Document> document = Document::fromJson(
            QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}});
        if (!document.ok())
            QTest::qFail("the chain does not load", __FILE__, __LINE__);
        const Compilation compiled = compile(*document, m_registry);
        if (!compiled.ok())
            QTest::qFail(qPrintable(compiled.errors().value(0).message), __FILE__, __LINE__);
        const auto made = RunFolder::create(m_dir.filePath("runs"));
        RunEnvironment environment;
        environment.registry = &m_registry;
        environment.kernels = &m_kernels;
        environment.cache = m_cache.get();
        environment.hashes = m_hashes.get();
        const RunOutcome outcome = runGraph(*compiled, *made, environment, RunOptions());
        for (const Error &error : outcome.errors)
            if (error.severity == Error::Severity::Error)
                QTest::qFail(qPrintable(error.code + ": " + error.message), __FILE__, __LINE__);
        for (const QJsonObject &event : made->events()) {
            if (event.value("node").toString() != "shown" || !event.contains("shown"))
                continue;
            const QString path = event.value("shown").toObject().value("value").toObject().value("path").toString();
            return vips::VImage::new_from_file(path.toUtf8().constData()).extract_band(0).copy_memory();
        }
        QTest::qFail("no picture came out", __FILE__, __LINE__);
        return {};
    }

    // 8-bit steps, whether the picture was kept in 8 or 16 bits.
    static int at(const vips::VImage &image, int x, int y) {
        const double v = image.getpoint(x, y).at(0);
        return int(std::lround(image.format() == VIPS_FORMAT_USHORT ? v / 257.0 : v));
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(m_dir.filePath("hashes.json"));
        m_cache = std::make_unique<ResultCache>(m_dir.filePath("results"));
        // Picture 1: grey 100, the person on the left; the result: another grey, the person on the right.
        m_picture = savePng(m_dir.filePath("picture.png"), 100, 8);
        m_result = savePng(m_dir.filePath("result.png"), 200, 76);
        m_maskPicture = savePng(m_dir.filePath("mask1.png"), 0, 8);
        m_maskResult = savePng(m_dir.filePath("mask2.png"), 0, 76);
    }

    void withTheSwitchOnTheSettingIsPictureOnesOutsideThePerson() {
        const vips::VImage out = chain(1);
        QCOMPARE(at(out, 48, 6), 100);  // far from both people: picture 1 exactly
        QCOMPARE(at(out, 48, 58), 100);
        QCOMPARE(at(out, 82, 32), 255); // the person of the result
        QCOMPARE(at(out, 14, 32), 200); // where picture 1's person stood: the result's setting fills it
        QVERIFY(at(out, 64, 32) > 100 && at(out, 64, 32) < 200); // the feathered band between
    }

    // The whole template, with the pack it carries loaded, compiles.
    void theWholeTemplateCompilesWithItsCarriedPack() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const LoadedPack pack = loadPack(registry, QStringLiteral(FUGUE_SOURCE_DIR "/templates/pose-from-description-keep-background/packs/io.github.bmaltais.fugue-remove-background"));
        QVERIFY2(pack.enabled && pack.disabled.isEmpty(), "the carried pack does not load");
        QFile in(QStringLiteral(FUGUE_SOURCE_DIR "/templates/pose-from-description-keep-background/workflow.fugue"));
        QVERIFY(in.open(QIODevice::ReadOnly));
        const Result<Document> document = Document::fromJson(QJsonDocument::fromJson(in.readAll()).object());
        QVERIFY(document.ok());
        const Compilation compiled = compile(*document, registry);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().value(0).message));
    }

    void withTheSwitchOffTheResultComesBackUntouched() {
        const vips::VImage out = chain(0);
        QCOMPARE(at(out, 48, 6), 200);
        QCOMPARE(at(out, 14, 32), 200);
        QCOMPARE(at(out, 82, 32), 255);
    }
};

QTEST_MAIN(TestKeepBackground)
#include "test_keep_background.moc"
