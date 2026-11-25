// The store seam of frosted controls' contrast (src/picture_luma.h): how light the part of a picture under a control is.
// A white picture reads pale (dark ink), a black one dark (white ink), a split picture gives each half its own answer, and
// the answer follows a file's normalized rectangle and is cached.

#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include "picture_luma.h"

using namespace fugue;

class TestPictureLuma : public QObject {
    Q_OBJECT

    static QImage solid(const QColor &color, int w = 200, int h = 100) {
        QImage image(w, h, QImage::Format_RGB32);
        image.fill(color);
        return image;
    }
    static QImage split() {
        QImage image = solid(Qt::black);
        QPainter(&image).fillRect(QRect(100, 0, 100, 100), Qt::white);
        return image;
    }

private slots:
    void aWhitePictureIsPaleAndABlackOneIsDark() {
        QVERIFY(luma::mean(solid(Qt::white), {0, 0, 1, 1}) > luma::paleAbove);
        QVERIFY(luma::mean(solid(Qt::black), {0, 0, 1, 1}) < 0.05);
        QVERIFY(luma::mean(solid(QColor("#2a2533")), {0, 0, 1, 1}) < luma::paleAbove); // the dusk base: white ink
        QCOMPARE(luma::mean(QImage(), {0, 0, 1, 1}), -1.0);
        QCOMPARE(luma::mean(solid(Qt::white), {0, 0, 0, 0}), -1.0);
    }

    void aSplitPictureAnswersPerPatch() {
        const QImage image = split();
        QVERIFY(luma::mean(image, {0.05, 0.1, 0.3, 0.3}) < 0.05);       // under the black half
        QVERIFY(luma::mean(image, {0.65, 0.1, 0.3, 0.3}) > luma::paleAbove); // under the white half
        const double both = luma::mean(image, {0, 0, 1, 1});
        QVERIFY(both > 0.4 && both < 0.6); // a patch over the seam is a mix
    }

    void aFileIsReadSmallOnceAndAskedByNormalizedRectangle() {
        QTemporaryDir dir;
        const QString path = dir.filePath("split.png");
        QVERIFY(split().save(path));
        PictureLuma luma;
        const QUrl url = QUrl::fromLocalFile(path);
        QVERIFY(luma.meanOfFile(url, 0.05, 0.1, 0.3, 0.3) < 0.05);
        QVERIFY(luma.meanOfFile(url, 0.65, 0.1, 0.3, 0.3) > luma::paleAbove);
        QCOMPARE(luma.meanOfFile(QUrl::fromLocalFile(dir.filePath("missing.png")), 0, 0, 1, 1), -1.0);
        QVERIFY(luma.meanOfImage(QVariant::fromValue(split()), 0.65, 0.1, 0.3, 0.3) > luma::paleAbove);
    }
};

QTEST_MAIN(TestPictureLuma)
#include "test_picture_luma.moc"
