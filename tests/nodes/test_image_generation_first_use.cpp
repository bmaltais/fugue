// libvips is set up the engine's way (vips.h: concurrency 1, no operation cache) even when
// an image node is the first code in the process to touch it, as in a run worker whose input
// image comes from the result cache and is never probed.
//
// Its own executable, because libvips is set up once per process: nothing here may call
// initVips() or libvips before the node does. The fixture PNG is written byte by byte.
//
// Seam: Generate Image's ProviderKernel::request with a start image handle read from JSON.

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <vips/vips8>

#include "engine/hash.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"

using namespace fugue;

namespace {

quint32 crc32(const QByteArray &bytes) {
    quint32 crc = 0xffffffffu;
    for (const char byte : bytes) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

QByteArray chunk(const QByteArray &type, const QByteArray &data) {
    QByteArray out(4, 0);
    qToBigEndian<quint32>(quint32(data.size()), out.data());
    QByteArray crc(4, 0);
    qToBigEndian<quint32>(crc32(type + data), crc.data());
    return out + type + data + crc;
}

// A gray 8-bit RGB PNG, without libvips: qCompress gives a zlib stream after its 4-byte length.
QByteArray grayPng(int width, int height) {
    QByteArray header(13, 0);
    qToBigEndian<quint32>(quint32(width), header.data());
    qToBigEndian<quint32>(quint32(height), header.data() + 4);
    header[8] = 8; // bit depth
    header[9] = 2; // RGB
    QByteArray rows;
    const QByteArray row = QByteArray(1, 0) + QByteArray(width * 3, char(128));
    for (int y = 0; y < height; ++y)
        rows += row;
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", qCompress(rows).mid(4)) +
           chunk("IEND", {});
}

} // namespace

class TestImageGenerationFirstUse : public QObject {
    Q_OBJECT

private slots:
    void theNodeSetsLibvipsUpItself() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath("start.png");
        const QByteArray png = grayPng(1024, 768);
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly) && out.write(png) == png.size());
        out.close();
        // A PNG in steps of 64 near a megapixel goes to the model as it is.
        const Result<MediaHandle> start = MediaHandle::fromJson(QJsonObject{
            {"type", "image"}, {"path", file}, {"hash", sha256(png)}, {"format", "png"},
            {"width", 1024}, {"height", 768}, {"channels", 3}});
        QVERIFY(start.ok());

        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        Kernels kernels;
        nodes::addImageGenerationKernels(kernels);
        FileHashes hashes(dir.filePath("hashes.json"));
        NodeContext context;
        context.node = "g";
        context.type = registry.nodeType("fugue/generate-image");
        context.inputs = {{"prompt", Value(QString("a cat"))}, {"image", Value(start.value())}, {"strength", Value(0.7)}};
        context.workDir = dir.path();
        context.hashes = &hashes;
        Selection selection;
        selection.model.handle = QJsonObject{{"resource", "fugue/image-model"}, {"name", "z-image"}};

        const Result<JobRequest> request = kernels.provider("fugue/generate-image")->request(context, selection);
        QVERIFY2(request.ok(), qPrintable(request.error().message));
        QCOMPARE(request->inputs.value("image").toObject().value("path").toString(), file);
        QCOMPARE(vips_cache_get_max(), 0);
        QCOMPARE(vips_concurrency_get(), 1);
    }
};

QTEST_GUILESS_MAIN(TestImageGenerationFirstUse)
#include "test_image_generation_first_use.moc"
