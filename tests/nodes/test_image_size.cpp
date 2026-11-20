// Image Size (#72): an aspect ratio and a number of megapixels make a width and a height, in
// steps of a multiple (64), as Generate Image sizes a start image (one shared function).
//
// Seam: the built-in node type and its kernel, called as the scheduler calls it.

#include <QtTest>

#include "engine/node_kernel.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "nodes/image_size.h"

using namespace fugue;

class TestImageSize : public QObject {
    Q_OBJECT

    Registry m_registry;
    Kernels m_kernels;

    QSize run(const QString &aspect, double megapixels, int multiple = 64) {
        NodeContext context;
        context.node = "size";
        context.type = m_registry.nodeType("fugue/image-size");
        context.inputs = {{"aspect", Value(aspect)}, {"megapixels", Value(megapixels)}, {"multiple", Value(multiple)}};
        const Result<Values> made = m_kernels.node(QStringLiteral("fugue/image-size"))->run(context);
        if (!made.ok())
            qFatal("image size failed: %s", qPrintable(made.error().message));
        return {int(made->value("width").number()), int(made->value("height").number())};
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
    }

    void theNodeTypeOffersTheUsualAspectsAndOneMegapixel() {
        const NodeType *type = m_registry.nodeType("fugue/image-size");
        QVERIFY(type);
        const InputSpec *aspect = type->input("aspect");
        QVERIFY(aspect && aspect->choices);
        for (const char *ratio : {"1:1", "16:9", "9:16", "3:2", "2:3", "4:3", "3:4", "5:4", "4:5", "6:5", "5:6", "21:9", "9:21"})
            QVERIFY2(aspect->choices->contains(QLatin1String(ratio)), ratio);
        QCOMPARE(aspect->defaultValue.toString(), QStringLiteral("1:1"));
        QCOMPARE(type->input("megapixels")->defaultValue.toDouble(), 1.0);
        QVERIFY(type->input("multiple")->advanced);
        QVERIFY(type->output("width") && type->output("height"));
    }

    void anAspectAndMegapixelsMakeASizeInStepsOf64_data() {
        QTest::addColumn<QString>("aspect");
        QTest::addColumn<double>("megapixels");
        QTest::addColumn<QSize>("size");
        QTest::newRow("1:1, 1 MP") << "1:1" << 1.0 << QSize(1024, 1024);
        QTest::newRow("16:9, 1 MP") << "16:9" << 1.0 << QSize(1344, 768);
        QTest::newRow("9:16, 1 MP") << "9:16" << 1.0 << QSize(768, 1344);
        QTest::newRow("2:3, 1 MP") << "2:3" << 1.0 << QSize(832, 1280);
        QTest::newRow("1:1, 0.25 MP") << "1:1" << 0.25 << QSize(512, 512);
        QTest::newRow("1:1, 4 MP") << "1:1" << 4.0 << QSize(2048, 2048);
    }

    void anAspectAndMegapixelsMakeASizeInStepsOf64() {
        QFETCH(QString, aspect);
        QFETCH(double, megapixels);
        QFETCH(QSize, size);
        QCOMPARE(run(aspect, megapixels), size);
    }

    // Each side is a multiple of `multiple`, never below one step, and the area stays near the
    // megapixels asked for.
    void everySizeIsAMultipleNearTheAreaAsked() {
        for (const char *aspect : {"1:1", "16:9", "9:21", "5:4", "6:5"})
            for (const double megapixels : {0.1, 0.5, 1.0, 2.0})
                for (const int multiple : {8, 16, 64}) {
                    const QSize size = run(QString::fromLatin1(aspect), megapixels, multiple);
                    QVERIFY(size.width() % multiple == 0 && size.height() % multiple == 0);
                    QVERIFY(size.width() >= multiple && size.height() >= multiple);
                    const double area = double(size.width()) * size.height() / (1024.0 * 1024.0);
                    QVERIFY2(qAbs(area - megapixels) / megapixels < 0.2,
                             qPrintable(QStringLiteral("%1 %2 %3: %4").arg(aspect).arg(megapixels).arg(multiple).arg(area)));
                }
    }

    // Generate Image sizes a start image with the same function.
    void generateImagesFitIsTheSameFunction() {
        QCOMPARE(nodes::sizeForMegapixels(16.0 / 9.0, 1.0, 64), QSize(1344, 768));
    }
};

QTEST_MAIN(TestImageSize)
#include "test_image_size.moc"
