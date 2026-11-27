// The Harmonize templates must hand the VAE Encode a picture and a seam strength map of one size.
//
// Seam: the real harmonize-image workflow, cut off before the model nodes (they need the helper and a GPU), run through
// the real scheduler on CPU nodes; the picture Sampler/VAE Encode would get is `seamed.image` and its mask is
// `strength_map.mask`. Pictures of other shapes and sizes than the sample, and masks painted at another size.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <vips/vips8>

#include "../test_paths.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/registry.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

QString savePng(const QString &file, int width, int height, int bands, int value, int hole = 0) {
    vips::VImage image = (vips::VImage::black(width, height, vips::VImage::option()->set("bands", bands)) + value).cast(VIPS_FORMAT_UCHAR);
    if (hole > 0) // a white square in the middle: the pasted part
        image.draw_rect({255.0}, width / 2 - hole, height / 2 - hole, 2 * hole, 2 * hole, vips::VImage::option()->set("fill", true));
    image.write_to_file(file.toUtf8().constData());
    return file;
}

QSize shownSize(const QJsonObject &event) {
    const QString path = event.value("shown").toObject().value("value").toObject().value("path").toString();
    const vips::VImage image = vips::VImage::new_from_file(path.toUtf8().constData());
    return {image.width(), image.height()};
}

} // namespace

class TestHarmonizeSizes : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    std::unique_ptr<FileHashes> m_hashes;
    std::unique_ptr<ResultCache> m_cache;

    // The template's graph up to the seam map, loading `file` and `mask`; the sizes of the two results.
    void sizesFor(const QString &template_, const QString &file, const QString &mask, QSize &picture, QSize &map) {
        QFile in((fugueSourceDir() + QStringLiteral("/templates/%1/workflow.fugue")).arg(template_));
        QVERIFY(in.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
        QJsonObject graph = root.value("graph").toObject();
        QJsonObject nodes = graph.value("nodes").toObject();
        for (const char *gone : {"models", "encode", "vae_encode", "sample", "decode", "back", "match", "save", "preview", "prompt_join"})
            nodes.remove(QString::fromLatin1(gone));
        QJsonObject load = nodes.value("load").toObject();
        QJsonObject inputs = load.value("inputs").toObject();
        inputs.insert("file", file);
        inputs.insert("mask", mask);
        load.insert("inputs", inputs);
        nodes.insert("load", load);
        // The same logic at a small working size (the template's 1 MP only makes the test slow).
        QJsonObject work = nodes.value("work").toObject();
        QJsonObject workInputs = work.value("inputs").toObject();
        workInputs.insert("megapixels", 0.05);
        work.insert("inputs", workInputs);
        nodes.insert("work", work);
        nodes.insert("p_image", QJsonObject{{"type", "fugue/preview"}, {"version", "1.0.0"},
                                            {"inputs", QJsonObject{{"value", QJsonObject{{"from", "seamed.image"}}}}}});
        nodes.insert("p_mask", QJsonObject{{"type", "fugue/preview"}, {"version", "1.0.0"},
                                           {"inputs", QJsonObject{{"value", QJsonObject{{"from", "strength_map.mask"}}}}}});
        graph.insert("nodes", nodes);
        root.insert("graph", graph);
        root.remove("layout");
        root.remove("requires");
        const Result<Document> document = Document::fromJson(root);
        QVERIFY2(document.ok(), "the template document does not load");
        const Compilation compiled = compile(*document, m_registry);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().value(0).message));
        const auto made = RunFolder::create(m_dir.filePath("runs"));
        QVERIFY(made.ok());
        RunEnvironment environment;
        environment.registry = &m_registry;
        environment.kernels = &m_kernels;
        environment.cache = m_cache.get();
        environment.hashes = m_hashes.get();
        const RunOutcome outcome = runGraph(*compiled, *made, environment, RunOptions());
        for (const Error &error : outcome.errors)
            QVERIFY2(error.severity != Error::Severity::Error, qPrintable(error.code + ": " + error.message));
        for (const QJsonObject &event : made->events()) {
            if (event.value("event").toString() != "node.done" && event.value("event").toString() != "node.cached")
                continue;
            if (event.value("node").toString() == "p_image")
                picture = shownSize(event);
            if (event.value("node").toString() == "p_mask")
                map = shownSize(event);
        }
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(m_dir.filePath("hashes.json"));
        m_cache = std::make_unique<ResultCache>(m_dir.filePath("results"));
    }

    void theSeamMapIsThePicturesSize_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("maskWidth");
        QTest::addColumn<int>("maskHeight");
        QTest::newRow("portrait 500x750 (to about 192x288), a smaller mask") << 500 << 750 << 150 << 225;
        QTest::newRow("tiny and not a multiple of 8") << 61 << 97 << 61 << 97;
    }

    void theSeamMapIsThePicturesSize() {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(int, maskWidth);
        QFETCH(int, maskHeight);
        const QString file = savePng(m_dir.filePath("picture.png"), width, height, 3, 120);
        const QString mask = savePng(m_dir.filePath("mask.png"), maskWidth, maskHeight, 1, 0, std::min(maskWidth, maskHeight) / 6);
        // The picture that reaches the model is the working size and a multiple of 16, the map as large.
        for (const char *name : {"harmonize-image"}) {
            QSize picture, map;
            sizesFor(QString::fromLatin1(name), file, mask, picture, map);
            QVERIFY2(picture.isValid() && map.isValid(), "no preview came out");
            QCOMPARE(map, picture);
            QCOMPARE(picture.width() % 16, 0);
            QCOMPARE(picture.height() % 16, 0);
        }
    }
};

QTEST_MAIN(TestHarmonizeSizes)
#include "test_harmonize_sizes.moc"
