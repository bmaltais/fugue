// Media handles as values (spec 2.4): the serialized form and pending video edits.
//
// Seams: MediaHandle::fromJson / toJson, and the edit methods trimmed() and cropped().
// File access, probing and decoding are covered by test_media_image and test_media_video.

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/media.h"

using namespace fugue;

namespace {

const char *hashA = "sha256:3f9a000000000000000000000000000000000000000000000000000000000001";

QJsonObject parse(const char *text) {
    return QJsonDocument::fromJson(text).object();
}

} // namespace

class TestMediaHandle : public QObject {
    Q_OBJECT

private slots:
    void imageHandleRoundTripsTheSpecExample() {
        const QJsonObject spec = parse(R"({"type": "image", "path": "/c/runs/r1/nodes/gen/image.png",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "width": 1024, "height": 1024, "channels": 3, "format": "png"})");
        const Result<MediaHandle> handle = MediaHandle::fromJson(spec);
        QVERIFY2(handle.ok(), qPrintable(handle.error().message));
        QCOMPARE(handle->type(), MediaType::Image);
        QCOMPARE(handle->path(), QStringLiteral("/c/runs/r1/nodes/gen/image.png"));
        QCOMPARE(handle->hash(), QString::fromLatin1(hashA));
        QCOMPARE(handle->width(), 1024);
        QCOMPARE(handle->height(), 1024);
        QCOMPARE(handle->channels(), 3);
        QCOMPARE(handle->format(), QStringLiteral("png"));
        QCOMPARE(handle->toJson(), spec);
    }

    void videoHandleKeepsItsFieldsAndEdits() {
        const QJsonObject spec = parse(R"({"type": "video", "path": "/c/clip.mp4",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "format": "mp4", "width": 1280, "height": 720, "fps": 25, "frames": 250,
            "duration": 10, "has_audio": true,
            "edits": {"trim": {"start": 1.5, "end": 4}, "crop": {"x": 10, "y": 20, "width": 640, "height": 360}}})");
        const Result<MediaHandle> handle = MediaHandle::fromJson(spec);
        QVERIFY2(handle.ok(), qPrintable(handle.error().message));
        QCOMPARE(handle->type(), MediaType::Video);
        QCOMPARE(handle->fps(), 25.0);
        QCOMPARE(handle->frames(), 250);
        QCOMPARE(handle->duration(), 10.0);
        QVERIFY(handle->hasAudio());
        QVERIFY(handle->edits().trim.has_value());
        QCOMPARE(handle->edits().trim->start, 1.5);
        QCOMPARE(handle->edits().crop->width, 640);
        QCOMPARE(handle->toJson(), spec);
    }

    void audioHandleHasOnlyAudioFields() {
        const QJsonObject spec = parse(R"({"type": "audio", "path": "/c/a.wav",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "format": "wav", "channels": 2, "sample_rate": 48000, "duration": 1.25})");
        const Result<MediaHandle> handle = MediaHandle::fromJson(spec);
        QVERIFY2(handle.ok(), qPrintable(handle.error().message));
        QCOMPARE(handle->sampleRate(), 48000);
        QCOMPARE(handle->toJson(), spec);
    }

    void fieldsOfOtherTypesAreIgnored() {
        const Result<MediaHandle> handle = MediaHandle::fromJson(parse(R"({"type": "audio",
            "path": "/c/a.wav", "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "format": "wav", "channels": 1, "sample_rate": 8000, "duration": 1, "width": 5, "fps": 3})"));
        QVERIFY(handle.ok());
        QVERIFY(!handle->toJson().contains("width"));
        QVERIFY(!handle->toJson().contains("fps"));
    }

    void badHandlesAreRejected() {
        const char *cases[] = {
            R"({"type": "picture", "path": "/a.png", "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001"})",
            R"({"type": "image", "path": "relative.png", "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001"})",
            R"({"type": "image", "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001"})",
            R"({"type": "image", "path": "/a.png", "hash": "md5:1"})",
            R"({"type": "image", "path": "/a.png", "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001", "edits": {}})",
        };
        for (const char *text : cases) {
            const Result<MediaHandle> handle = MediaHandle::fromJson(parse(text));
            QVERIFY2(!handle.ok(), text);
            QCOMPARE(handle.error().code, QStringLiteral("invalid_document"));
        }
    }

    void trimsComposeOnTheTrimmedTimeline() {
        Result<MediaHandle> video = MediaHandle::fromJson(parse(R"({"type": "video", "path": "/c/v.mp4",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "format": "mp4", "width": 100, "height": 80, "fps": 10, "frames": 100, "duration": 10,
            "has_audio": false})"));
        QVERIFY(video.ok());
        QCOMPARE(video->effectiveDuration(), 10.0);
        const Result<MediaHandle> once = video->trimmed(2, 8);
        QVERIFY(once.ok());
        QCOMPARE(once->effectiveDuration(), 6.0);
        const Result<MediaHandle> twice = once->trimmed(1, 3);
        QVERIFY(twice.ok());
        QCOMPARE(twice->edits().trim->start, 3.0);
        QCOMPARE(twice->edits().trim->end, 5.0);
        // The file and its description do not change: edits are pending.
        QCOMPARE(twice->duration(), 10.0);
        QCOMPARE(twice->hash(), video->hash());

        QVERIFY(!once->trimmed(5, 7).ok());  // past the trimmed end (6 s)
        QVERIFY(!video->trimmed(3, 3).ok()); // empty
        QVERIFY(!video->trimmed(-1, 2).ok());
        QCOMPARE(video->trimmed(-1, 2).error().code, QStringLiteral("value_out_of_range"));
    }

    void cropsComposeInsideTheCroppedFrame() {
        Result<MediaHandle> video = MediaHandle::fromJson(parse(R"({"type": "video", "path": "/c/v.mp4",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "format": "mp4", "width": 100, "height": 80, "fps": 10, "frames": 100, "duration": 10,
            "has_audio": false})"));
        QVERIFY(video.ok());
        const Result<MediaHandle> once = video->cropped(10, 20, 50, 40);
        QVERIFY(once.ok());
        QCOMPARE(once->effectiveWidth(), 50);
        QCOMPARE(once->effectiveHeight(), 40);
        const Result<MediaHandle> twice = once->cropped(5, 5, 10, 10);
        QVERIFY(twice.ok());
        const Crop crop = *twice->edits().crop;
        QCOMPARE(crop.x, 15);
        QCOMPARE(crop.y, 25);
        QCOMPARE(crop.width, 10);
        QCOMPARE(crop.height, 10);
        QCOMPARE(twice->width(), 100);

        QVERIFY(!once->cropped(45, 0, 10, 10).ok()); // 45 + 10 > 50
        QVERIFY(!video->cropped(0, 0, 0, 10).ok());
        QVERIFY(!video->cropped(-1, 0, 10, 10).ok());
    }

    void onlyVideosTakeEdits() {
        const Result<MediaHandle> image = MediaHandle::fromJson(parse(R"({"type": "image", "path": "/a.png",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "width": 10, "height": 10, "channels": 3, "format": "png"})"));
        QVERIFY(image.ok());
        QVERIFY(!image->trimmed(0, 1).ok());
        QVERIFY(!image->cropped(0, 0, 1, 1).ok());
    }

    void atPathKeepsHashAndMetadata() {
        const Result<MediaHandle> image = MediaHandle::fromJson(parse(R"({"type": "image", "path": "/a.png",
            "hash": "sha256:3f9a000000000000000000000000000000000000000000000000000000000001",
            "width": 10, "height": 10, "channels": 3, "format": "png"})"));
        QVERIFY(image.ok());
        const MediaHandle moved = image->atPath("/store/blobs/3f9a.png");
        QCOMPARE(moved.path(), QStringLiteral("/store/blobs/3f9a.png"));
        QJsonObject expected = image->toJson();
        expected["path"] = "/store/blobs/3f9a.png";
        QCOMPARE(moved.toJson(), expected);
    }
};

QTEST_GUILESS_MAIN(TestMediaHandle)
#include "test_media_handle.moc"
