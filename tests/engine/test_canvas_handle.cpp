// The fugue/canvas handle (#208, spec 14.5): a descriptor, never pixels or a path, the same bytes for
// the same canvas and revision. Seam: engine/canvas_handle.h.

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_handle.h"

using namespace fugue;
using namespace fugue::canvas;

namespace {
const QString revisionA = "sha256:" + QString(64, 'a');
}

class TestCanvasHandle : public QObject {
    Q_OBJECT
private slots:
    void aHandleHoldsTheCanvasAndTheRevisionOnly() {
        const CanvasHandle handle{"canvas-0123456789ab", revisionA};
        QCOMPARE(canvasHandleMeta(handle).keys(), (QStringList{"canvas", "revision"}));
        const Result<CanvasHandle> back = canvasHandleFrom(canvasHandleMeta(handle));
        QVERIFY(back);
        QCOMPARE(back.value(), handle);
    }

    void theWrittenFileIsCanonicalAndCarriesNoPath() {
        QTemporaryDir dir;
        const CanvasHandle handle{"canvas-0123456789ab", revisionA};
        const Result<QJsonObject> value = writeCanvasHandle(dir.path(), handle);
        QVERIFY(value);
        QCOMPARE(value->value("data").toString(), QString("fugue/canvas"));
        QVERIFY(!QJsonDocument(value->value("meta").toObject()).toJson().contains("/home"));
        QFile file(value->value("path").toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray(R"({"canvas":"canvas-0123456789ab","revision":"sha256:)") + QByteArray(64, 'a') + "\"}");
        const Result<CanvasHandle> back = canvasHandleFrom(value.value());
        QVERIFY(back);
        QCOMPARE(back.value(), handle);
    }

    void badHandlesAreInputInvalid() {
        const QJsonObject good = canvasHandleMeta({"canvas-0123456789ab", revisionA});
        QJsonObject extra = good;
        extra.insert("path", "/tmp/x"); // a path is never part of the descriptor
        QJsonObject badId = good;
        badId.insert("canvas", "../etc");
        QJsonObject badHash = good;
        badHash.insert("revision", "head");
        QJsonObject wrongType{{"data", "fugue/stitch"}, {"path", "x"}, {"meta", good}};
        for (const QJsonObject &bad : {extra, badId, badHash, wrongType, QJsonObject()}) {
            const Result<CanvasHandle> r = canvasHandleFrom(bad);
            QVERIFY(!r);
            QCOMPARE(r.error().code, QString("input_invalid"));
        }
        QVERIFY(!canvasHandleFrom(QJsonValue(5)));
        QVERIFY(!writeCanvasHandle(QDir::tempPath(), {"../x", revisionA}));
    }
};

QTEST_GUILESS_MAIN(TestCanvasHandle)
#include "test_canvas_handle.moc"
