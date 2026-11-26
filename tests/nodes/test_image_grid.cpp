// Image Grid (#552, spec 14.2): the registered kernel under QCoreApplication, as the CLI runs it.

#include <QtTest>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/resample.h"

using namespace fugue;
using nodes::Pixels;

namespace {

Value solid(int w, int h, float v) {
    const Pixels p(w, h, 3, v);
    const vips::VImage image =
        vips::VImage::new_from_memory_copy(p.data.data(), p.data.size() * sizeof(float), w, h, 3, VIPS_FORMAT_FLOAT);
    return Value(MediaHandle::fromImage(image).value());
}

} // namespace

class TestImageGrid : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> run(const QList<Value> &images, int columns = 0, const QString &labels = QString()) {
        NodeContext context;
        context.node = "n";
        context.inputs = Values{{"images", Value::list(images)}, {"columns", columns}, {"labels", Value(labels)},
                                {"column_labels", Value(QString())}, {"row_labels", Value(QString())}, {"gap", 8},
                                {"background", Value(QString("#1e1e1e"))}, {"label_size", 0}};
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return m_kernels.node("fugue/image-grid")->run(context);
    }

    static QSize sizeOf(const Result<Values> &r) {
        const MediaHandle h = r.value().value("image").media();
        return QSize(h.width(), h.height());
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void layoutFollowsCountColumnsAndLabels() {
        const QList<Value> six = {solid(100, 80, 0.5f), solid(60, 100, 0.6f), solid(100, 100, 0.7f),
                                  solid(40, 40, 0.8f),  solid(100, 90, 0.9f), solid(90, 100, 0.4f)};
        // Six pictures: three columns, two rows; the cell is the largest picture, 100 x 100, with an 8 px gap.
        const Result<Values> automatic = run(six);
        QVERIFY2(automatic.ok(), qPrintable(automatic.errors().value(0).message));
        QCOMPARE(sizeOf(automatic), QSize(8 + 3 * 100 + 2 * 8 + 8, 8 + 2 * 100 + 8 + 8));
        QCOMPARE(sizeOf(run(six, 2)), QSize(8 + 2 * 100 + 8 + 8, 8 + 3 * 100 + 2 * 8 + 8));
        QCOMPARE(sizeOf(run(six, 6)).height(), 8 + 100 + 8); // more columns than needed is fine; fewer rows
        // Labels add a band under each row and leave the width alone.
        const Result<Values> labelled = run(six, 0, "Front\nBack\nLeft\nRight\nTop\nClose");
        QVERIFY(labelled.ok());
        QCOMPARE(sizeOf(labelled).width(), sizeOf(automatic).width());
        QVERIFY(sizeOf(labelled).height() > sizeOf(automatic).height() + 20);
    }

    void aSingleImageIsFittedNotCropped() {
        const Result<Values> one = run({solid(50, 30, 1.0f)});
        QVERIFY(one.ok());
        QCOMPARE(sizeOf(one), QSize(66, 46));
    }

    void limitsAreInputErrors() {
        const Result<Values> none = run({});
        QVERIFY(!none.ok());
        QCOMPARE(none.errors().value(0).input, QString("images"));
        QList<Value> many;
        for (int i = 0; i < 65; ++i)
            many.append(solid(4, 4, 0.5f));
        const Result<Values> over = run(many);
        QVERIFY(!over.ok());
        QCOMPARE(over.errors().value(0).input, QString("images"));
        QVERIFY(over.errors().value(0).message.contains("65"));
        // Eight 5000 px wide pictures in one row pass 16384 px: the error names the size.
        const Result<Values> wide = run(QList<Value>(8, solid(5000, 8, 0.5f)), 8);
        QVERIFY(!wide.ok());
        QVERIFY(wide.errors().value(0).message.contains("40"));
    }
};

QTEST_GUILESS_MAIN(TestImageGrid)
#include "test_image_grid.moc"
