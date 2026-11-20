// Model downloads (spec 8.9, #89): a missing model fetched from its `source` into the model
// folder downloads go in, kept only when its content hash is the handle's.
//
// Seams: planDownload() (where it goes, which sources and names are taken) over real folders, and
// ModelDownload / DownloadQueue against a loopback HTTP server (fake_http.h): breaks, ranges,
// a wrong file, cancel.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/registry.h"
#include "fake_http.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpRequest;
using fugue::test::HttpResponse;

namespace {

PortType type(const QString &id) { return *PortType::parse(id); }

void declare(Registry &registry, const QString &id, const QStringList &folders, const QString &files = {}) {
    DeclaredPortType declared;
    declared.type = type(id);
    declared.kind = PortKind::Resource;
    declared.name = QStringLiteral("A model");
    declared.extensions = {QStringLiteral(".safetensors"), QStringLiteral(".gguf")};
    declared.folders = folders;
    declared.files = files;
    QVERIFY(registry.add(declared).isEmpty());
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Bytes enough to arrive in several pieces.
QByteArray modelBytes() {
    QByteArray bytes;
    for (int i = 0; i < 200000; ++i)
        bytes.append(char(i * 7 % 251));
    return bytes;
}

MissingModel missingModel(const QString &resource, const QString &name, const QByteArray &bytes, const QString &source,
                          const QString &folder = {}) {
    return {QJsonObject{{"resource", resource}, {"hash", sha256(bytes)}, {"name", name}}, source, folder, -1};
}

// The bytes from "bytes=N-" on, as a 206, or the whole body when the request asks no range.
HttpResponse ranged(const HttpRequest &request, const QByteArray &bytes) {
    const QByteArray range = request.headers.value("range");
    if (!range.startsWith("bytes="))
        return {200, bytes, "application/octet-stream"};
    const qsizetype from = range.mid(6, range.indexOf('-') - 6).toLongLong();
    if (from >= bytes.size())
        return {416, {}, "text/plain"};
    HttpResponse response{206, bytes.mid(from), "application/octet-stream"};
    response.headers << "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(bytes.size() - 1)
                            + '/' + QByteArray::number(bytes.size());
    return response;
}

Errors runToEnd(ModelDownload &download) {
    QSignalSpy finished(&download, &ModelDownload::finished);
    download.start();
    if (finished.isEmpty())
        finished.wait(10000);
    return finished.isEmpty() ? Errors{makeError("internal", "no end")} : finished.first().first().value<Errors>();
}

} // namespace

class TestModelDownload : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root, m_state;
    Registry m_registry;
    std::unique_ptr<FileHashes> m_hashes;
    QNetworkAccessManager m_network;
    std::unique_ptr<FakeHttpServer> m_server;

    QString url(const QByteArray &path) const { return QString::fromLatin1(m_server->origin() + path); }
    DownloadTarget target(const QByteArray &bytes, const QByteArray &path = "/m.safetensors", qint64 size = -1) {
        return {QJsonObject{{"resource", "io.test.pack/lora"}, {"hash", sha256(bytes)}, {"name", "m.safetensors"}},
                QUrl(url(path)), m_root.filePath("loras/m.safetensors"), size};
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<fugue::Errors>();
        declare(m_registry, "io.test.pack/lora", {"loras"}, "lora");
        declare(m_registry, "io.test.pack/encoder", {"text_encoders", "clip"});
    }
    void init() {
        QVERIFY(m_root.isValid() && m_state.isValid());
        QDir(m_root.path()).removeRecursively();
        QDir().mkpath(m_root.path());
        QFile::remove(m_state.filePath("hashes.json"));
        m_hashes = std::make_unique<FileHashes>(m_state.filePath("hashes.json"));
        m_server = std::make_unique<FakeHttpServer>();
        QVERIFY(m_server->listen());
    }

    // ---- where it goes, and what is taken ----

    void aDownloadGoesInTheTypesFolderOfTheDownloadRoot() {
        QTemporaryDir comfy, loras;
        QDir().mkpath(comfy.filePath("clip")); // ComfyUI's older name is the one there
        writeFile(loras.filePath("a.safetensors"), "x");
        ModelLibrary library({m_root.path(), comfy.path(), loras.path()}, &m_registry,
                             {{loras.path(), QStringLiteral("io.test.pack/lora")}});
        QCOMPARE(library.downloadRoot(), m_root.path());
        QCOMPARE(library.downloadFolder(type("io.test.pack/encoder")), m_root.filePath("text_encoders"));

        library.setDownloadRoot(comfy.path());
        QCOMPARE(library.downloadFolder(type("io.test.pack/encoder")), comfy.filePath("clip"));
        QCOMPARE(library.downloadFolder(type("io.test.pack/lora")), comfy.filePath("loras"));

        // A LoRA folder takes LoRAs; anything else goes in Fugue's own folder.
        library.setDownloadRoot(loras.path());
        QCOMPARE(library.downloadFolder(type("io.test.pack/lora")), loras.path());
        QCOMPARE(library.downloadFolder(type("io.test.pack/encoder")), m_root.filePath("text_encoders"));

        // A root no longer among the model folders is not used.
        library.setDownloadRoot(QStringLiteral("/nowhere"));
        QCOMPARE(library.downloadRoot(), m_root.path());

        QCOMPARE(library.downloadRoots({type("io.test.pack/lora")}),
                 (QStringList{m_root.path(), comfy.path(), loras.path()}));
        QCOMPARE(library.downloadRoots({type("io.test.pack/lora"), type("io.test.pack/encoder")}),
                 (QStringList{m_root.path(), comfy.path()}));

        // One download is not split: a LoRA folder takes a batch of LoRAs only.
        library.setDownloadRoot(loras.path());
        library.fitDownloadRoot({type("io.test.pack/lora")});
        QCOMPARE(library.downloadRoot(), loras.path());
        library.fitDownloadRoot({type("io.test.pack/lora"), type("io.test.pack/encoder")});
        QCOMPARE(library.downloadRoot(), m_root.path());
        QCOMPARE(library.downloadFolder(type("io.test.pack/lora")), m_root.filePath("loras"));
    }

    void onlyHttpsOrThisMachineIsASource() {
        QVERIFY(isAllowedSource(QUrl("https://huggingface.co/a/b/resolve/main/m.gguf")));
        QVERIFY(isAllowedSource(QUrl("http://127.0.0.1:8080/m.gguf")));
        QVERIFY(isAllowedSource(QUrl("http://[::1]/m.gguf")));
        QVERIFY(isAllowedSource(QUrl("http://localhost/m.gguf")));
        QVERIFY(!isAllowedSource(QUrl("http://example.com/m.gguf")));
        QVERIFY(!isAllowedSource(QUrl("ftp://example.com/m.gguf")));
        QVERIFY(!isAllowedSource(QUrl("file:///etc/passwd")));
        QVERIFY(!isAllowedSource(QUrl("https:///m.gguf")));
    }

    void aPlanNamesTheFileAndRefusesWhatItCannotTake() {
        const ModelLibrary library({m_root.path()}, &m_registry);
        const QByteArray bytes = "abc";
        const Result<DownloadTarget> plan =
            planDownload(library, missingModel("io.test.pack/lora", "sub/m.safetensors", bytes, "https://h.example/m"));
        QVERIFY2(plan, qPrintable(plan ? QString() : plan.error().message));
        QCOMPARE(plan->path, m_root.filePath("loras/sub/m.safetensors"));
        QCOMPARE(plan->source, QUrl("https://h.example/m"));

        for (const auto &[name, source] : {std::pair{"m.safetensors", ""}, std::pair{"m.safetensors", "http://h.example/m"},
                                           std::pair{"../m.safetensors", "https://h.example/m"},
                                           std::pair{"/abs/m.safetensors", "https://h.example/m"},
                                           std::pair{".hidden/m.safetensors", "https://h.example/m"},
                                           std::pair{"m.exe", "https://h.example/m"}}) {
            const Result<DownloadTarget> refused =
                planDownload(library, missingModel("io.test.pack/lora", QString::fromLatin1(name), bytes,
                                                   QString::fromLatin1(source)));
            QVERIFY2(!refused, name);
            QCOMPARE(refused.error().code, QStringLiteral("download_failed"));
        }

        // Never over a file that is there.
        writeFile(m_root.filePath("loras/m.safetensors"), "another");
        const Result<DownloadTarget> taken =
            planDownload(library, missingModel("io.test.pack/lora", "m.safetensors", bytes, "https://h.example/m"));
        QVERIFY(!taken);
        QVERIFY(taken.error().message.contains("another file"));
    }

    // ---- fetching ----

    void aModelArrivesHashedAndIsFoundByItsHash() {
        const QByteArray bytes = modelBytes();
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, bytes, "application/octet-stream"};
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        QSignalSpy progress(&download, &ModelDownload::progress);
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        const QString path = m_root.filePath("loras/m.safetensors");
        QCOMPARE(readFile(path), bytes);
        QVERIFY(!QFileInfo::exists(path + ".part"));
        QCOMPARE(m_hashes->known(path), sha256(bytes)); // remembered: found at once
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toLongLong(), qint64(bytes.size()));
        QCOMPARE(progress.last().at(1).toLongLong(), qint64(bytes.size()));
        QCOMPARE(m_server->requests().first().headers.value("accept-encoding"), QByteArray("identity"));

        // The library now finds it (and the store on disk has it, for other processes).
        const ModelLibrary library({m_root.path()}, &m_registry);
        FileHashes again(m_state.filePath("hashes.json"));
        QVERIFY(library.resolve(target(bytes).handle, again, ModelLibrary::Hashing::RememberedOnly));
    }

    void aWrongFileIsDeletedAndSaysSo() {
        const QByteArray bytes = modelBytes();
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, "a newer build", "application/octet-stream"};
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_mismatch"));
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors")));
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors.part")));
    }

    void aSizeTheWorkflowRecordsStopsAnotherFileEarly() {
        const QByteArray bytes = modelBytes();
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, bytes + "more", "application/octet-stream"};
        });
        ModelDownload download(target(bytes, "/m.safetensors", bytes.size()), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_mismatch"));
        QVERIFY(errors.first().message.contains(QString::number(bytes.size())));
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors.part")));
    }

    void aBreakGoesOnWithARangeRequest() {
        const QByteArray bytes = modelBytes();
        int asked = 0;
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) {
            HttpResponse response = ranged(request, bytes);
            if (++asked == 1)
                response.cutAfter = 70000; // the connection drops part way
            return response;
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        download.setRetries(3, 0);
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);
        QCOMPARE(m_server->requests().size(), 2);
        QCOMPARE(m_server->requests().at(1).headers.value("range"), QByteArray("bytes=70000-"));
    }

    void aPartLeftByAnEarlierFugueIsHashedAndResumed() {
        const QByteArray bytes = modelBytes();
        writeFile(m_root.filePath("loras/m.safetensors.part"), bytes.left(123456));
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) { return ranged(request, bytes); });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);
        QCOMPARE(m_server->requests().size(), 1);
        QCOMPARE(m_server->requests().first().headers.value("range"), QByteArray("bytes=123456-"));
    }

    void aWholePartIsCheckedWithoutFetchingAgain() {
        const QByteArray bytes = modelBytes();
        writeFile(m_root.filePath("loras/m.safetensors.part"), bytes);
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) { return ranged(request, bytes); });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);
    }

    void aServerThatIgnoresTheRangeStartsThePartOver() {
        const QByteArray bytes = modelBytes();
        writeFile(m_root.filePath("loras/m.safetensors.part"), "stale bytes of an older file");
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, bytes, "application/octet-stream"};
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);
    }

    void aRedirectIsFollowedOnlyToWhatASourceMayBe() {
        const QByteArray bytes = modelBytes();
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            HttpResponse response{302, {}, "text/plain"};
            response.headers << "Location: " + m_server->origin() + "/store/m.safetensors";
            return response;
        });
        m_server->on("GET", "/store/m.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, bytes, "application/octet-stream"};
        });
        m_server->on("GET", "/away.safetensors", [&](const HttpRequest &) {
            HttpResponse response{302, {}, "text/plain"};
            response.headers << "Location: http://example.invalid/m.safetensors"; // off this machine, in clear
            return response;
        });
        ModelDownload followed(target(bytes), &m_network, m_hashes.get());
        const Errors none = runToEnd(followed);
        QVERIFY2(none.isEmpty(), qPrintable(none.isEmpty() ? QString() : none.first().message));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);

        DownloadTarget away = target(bytes, "/away.safetensors");
        away.path = m_root.filePath("loras/away.safetensors");
        ModelDownload refused(away, &m_network, m_hashes.get());
        const Errors errors = runToEnd(refused);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_failed"));
        QVERIFY(errors.first().message.contains("example.invalid"));
        QVERIFY(!QFileInfo::exists(away.path));
    }

    void aRangeWithNoTotalEndsWithTheStream() {
        const QByteArray bytes = modelBytes();
        writeFile(m_root.filePath("loras/m.safetensors.part"), bytes.left(1000));
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) {
            HttpResponse response = ranged(request, bytes);
            response.headers = {"Content-Range: bytes 1000-" + QByteArray::number(bytes.size() - 1) + "/*"};
            return response;
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        download.setRetries(3, 0);
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(m_server->requests().size(), 1);
    }

    void aLinkAtThePartsNameIsNotWrittenThrough() {
        const QByteArray bytes = modelBytes();
        QTemporaryDir elsewhere;
        writeFile(elsewhere.filePath("precious"), "keep me");
        QDir().mkpath(m_root.filePath("loras"));
        QVERIFY(QFile::link(elsewhere.filePath("precious"), m_root.filePath("loras/m.safetensors.part")));
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) { return ranged(request, bytes); });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        const Errors errors = runToEnd(download);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().message));
        QCOMPARE(readFile(elsewhere.filePath("precious")), QByteArray("keep me"));
        QCOMPARE(readFile(m_root.filePath("loras/m.safetensors")), bytes);
        QVERIFY(!QFileInfo(m_root.filePath("loras/m.safetensors")).isSymLink());
    }

    void oneFileHasOneWriter() {
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        ModelDownload first(target("abc"), &m_network, m_hashes.get());
        first.start();
        ModelDownload second(target("abc"), &m_network, m_hashes.get());
        const Errors errors = runToEnd(second);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().message.contains("another Fugue"));
        QVERIFY(QFileInfo::exists(m_root.filePath("loras/m.safetensors.part"))); // the first one's, kept
        first.cancel();
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors.part.lock")));
    }

    void anErrorStatusFailsAtOnceAndKeepsNothing() {
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) { return HttpResponse{403, "gated", "text/plain"}; });
        ModelDownload download(target("abc"), &m_network, m_hashes.get());
        download.setRetries(3, 0);
        const Errors errors = runToEnd(download);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_failed"));
        QVERIFY(errors.first().message.contains("403"));
        QVERIFY(errors.first().hint.contains("sign-in"));
        QCOMPARE(m_server->requests().size(), 1); // not retried
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors")));
    }

    void aServerThatKeepsBreakingIsGivenUpOnAndThePartKept() {
        const QByteArray bytes = modelBytes();
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &request) {
            HttpResponse response = ranged(request, bytes);
            response.cutAfter = 0;
            return response;
        });
        ModelDownload download(target(bytes), &m_network, m_hashes.get());
        download.setRetries(2, 0);
        const Errors errors = runToEnd(download);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_failed"));
        QCOMPARE(m_server->requests().size(), 3);
        QVERIFY(QFileInfo::exists(m_root.filePath("loras/m.safetensors.part")));
    }

    void cancelStopsAndDeletesThePart() {
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        ModelDownload download(target("abc"), &m_network, m_hashes.get());
        QSignalSpy finished(&download, &ModelDownload::finished);
        download.start();
        QTRY_COMPARE(m_server->requests().size(), 1);
        QVERIFY(QFileInfo::exists(m_root.filePath("loras/m.safetensors.part")));
        download.cancel();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().value<Errors>().first().code, QStringLiteral("cancelled"));
        QVERIFY(!QFileInfo::exists(m_root.filePath("loras/m.safetensors.part")));
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1); // once
    }

    // ---- several ----

    void aQueueFetchesOneAfterAnotherAndGoesOnPastAFailure() {
        const QByteArray one = modelBytes(), two = "second model";
        m_server->on("GET", "/one.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, one, "application/octet-stream"};
        });
        m_server->on("GET", "/two.safetensors", [&](const HttpRequest &) {
            return HttpResponse{200, two, "application/octet-stream"};
        });
        DownloadTarget missing = target("gone", "/gone.safetensors");
        missing.path = m_root.filePath("loras/gone.safetensors");
        DownloadTarget first = target(one, "/one.safetensors"), second = target(two, "/two.safetensors");
        first.path = m_root.filePath("loras/one.safetensors");
        second.path = m_root.filePath("loras/two.safetensors");

        DownloadQueue queue(&m_network, m_hashes.get());
        queue.setRetries(0, 0);
        QSignalSpy started(&queue, &DownloadQueue::started), done(&queue, &DownloadQueue::finished);
        queue.add({first, missing});
        queue.add({second, first}); // `first` is queued already
        QVERIFY(queue.busy());
        QVERIFY(done.wait(10000));
        QCOMPARE(started.size(), 3);
        const Errors errors = done.first().first().value<Errors>();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("download_failed")); // the 404
        QCOMPARE(readFile(first.path), one);
        QCOMPARE(readFile(second.path), two);
        QVERIFY(!queue.busy());
        // One request at a time, in order.
        QCOMPARE(m_server->requests().at(0).path, QByteArray("/one.safetensors"));
        QCOMPARE(m_server->requests().at(1).path, QByteArray("/gone.safetensors"));
        QCOMPARE(m_server->requests().at(2).path, QByteArray("/two.safetensors"));
    }

    void cancellingAQueueDropsTheRest() {
        m_server->on("GET", "/m.safetensors", [&](const HttpRequest &) {
            HttpResponse response;
            response.hold = true;
            return response;
        });
        DownloadTarget other = target("x", "/other.safetensors");
        other.path = m_root.filePath("loras/other.safetensors");
        DownloadQueue queue(&m_network, m_hashes.get());
        QSignalSpy done(&queue, &DownloadQueue::finished);
        queue.add({target("abc"), other});
        QTRY_COMPARE(m_server->requests().size(), 1);
        queue.cancel();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first().first().value<Errors>().first().code, QStringLiteral("cancelled"));
        QCOMPARE(m_server->requests().size(), 1);
    }

    // ---- what the workflow records ----

    void aWorkflowKeepsTheSizeItRecordsOfAModel() {
        const QString hash = QStringLiteral("sha256:") + QString(64, u'1');
        const QJsonObject lora{{"resource", "io.test.pack/lora"}, {"hash", hash}, {"name", "m.safetensors"}};
        QJsonObject recorded = lora;
        recorded.insert("source", "https://h.example/m");
        recorded.insert("size", 10200000000.0);
        const QJsonObject json{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"a", QJsonObject{{"type", "fugue/text"},
                                                                         {"version", "1.0.0"},
                                                                         {"inputs", QJsonObject{{"x", lora}}}}}}}}},
            {"requires", QJsonObject{{"models", QJsonArray{recorded}}}}};
        const Result<Document> document = Document::fromJson(json);
        QVERIFY(document);
        QCOMPARE(document->models(), QJsonArray{recorded});
        const QList<MissingModel> missing = ModelLibrary({m_root.path()}, &m_registry).missing(*document, *m_hashes);
        QCOMPARE(missing.size(), 1);
        QCOMPARE(missing.first().size, qint64(10200000000));
        QVERIFY(!missing.first().handle.contains("size"));
    }
};

QTEST_GUILESS_MAIN(TestModelDownload)
#include "test_model_download.moc"
