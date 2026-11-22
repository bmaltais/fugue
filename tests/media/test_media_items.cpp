// The core of the media browser (#133): which files a workflow's runs made, in which groups, what
// an image's embedded workflow says, and the thumbnail cache.
//
// Seam: media/media_items.h, media/thumbnail_cache.h and media/media_library.h, over run folders
// written by hand in a temporary folder, as the scheduler leaves them.

#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/embed.h"
#include "engine/run_folder.h"
#include "media/media_items.h"
#include "media/media_library.h"
#include "media/thumbnail_cache.h"

using namespace fugue;

namespace {

QString makePng(const QString &path, int width = 400, int height = 200, const QByteArray &workflow = {}) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    if (!workflow.isEmpty())
        bytes = pngWithWorkflow(bytes, workflow).value();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
    return path;
}

QJsonObject image(const QString &path, const QString &type = "image") { return {{"type", type}, {"path", path}}; }

// A workflow of  prompt -> gen -> save, plus an unrelated `load` that feeds gen.
QJsonObject graph() {
    return QJsonDocument::fromJson(R"({"fugue": 1, "graph": {"nodes": {
        "prompt": {"type": "fugue/text", "inputs": {"text": "a red fox"}},
        "load": {"type": "fugue/load-image", "inputs": {"file": "x.png"}},
        "gen": {"type": "fugue/generate-image", "inputs": {"prompt": {"from": "prompt.text"}, "image": {"from": "load.image"}}},
        "save": {"type": "fugue/save-image", "inputs": {"image": {"from": "gen.image"}}}
    }}})").object();
}

} // namespace

class TestMediaItems : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString root() const { return dir.path() + "/runs"; }

    // A run of `workflow` whose nodes hold `outputs` (node -> output map) in `state`.
    void makeRun(const QString &id, const QString &workflow, const QMap<QString, QJsonObject> &outputs,
                 const QString &state = "cached", bool withWorkflow = true) {
        const RunFolder folder = RunFolder::create(root(), id).value();
        QJsonObject nodes;
        for (auto it = outputs.begin(); it != outputs.end(); ++it)
            nodes.insert(it.key(), QJsonObject{{"state", state}, {"outputs", it.value()}});
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", workflow}}}, {"nodes", nodes}}));
        if (withWorkflow)
            QVERIFY(folder.writeWorkflow(graph()).isEmpty());
    }

    // The later tests list every run of the folder: a test that made its own runs takes them away.
    void forget(const QStringList &ids) {
        for (const QString &id : ids)
            QVERIFY(QDir(root() + "/" + id).removeRecursively());
    }

private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    void aRunIdSaysWhenItStarted() {
        const QDateTime time = runStartOf("20261121-174052-080-pa5q");
        QVERIFY(time.isValid());
        QCOMPARE(time.date(), QDate(2026, 11, 21));
        QCOMPARE(time.time(), QTime(17, 40, 52, 80));
        QVERIFY(!runStartOf("nonsense").isValid());
    }

    void onlyTheResultsOfARunAreListedNotItsInputsOrSteps() {
        const QString save = makePng(dir.filePath("save.png"));
        const QString step = makePng(dir.filePath("step.png"));
        const QString input = makePng(dir.filePath("input.png"));
        makeRun("20261121-100000-000-aaaa", "/w/a.fugue",
                {{"save", {{"file", image(save)}}}, {"gen", {{"image", image(step)}}}, {"load", {{"image", image(input)}}}});
        const QList<MediaItem> items = mediaOfRun(root(), "20261121-100000-000-aaaa");
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].path, save);
        QCOMPARE(items[0].name, QString("save.png"));
        QCOMPARE(items[0].kind, QString("image"));
        QCOMPARE(items[0].node, QString("save"));
        QCOMPARE(items[0].workflow, QString("/w/a.fugue"));
    }

    void whatAPreviewShowsCountsAsAResult() {
        // A Preview node keeps what it shows under "shown", not "outputs".
        const QString shown = makePng(dir.filePath("shown.png"));
        const RunFolder folder = RunFolder::create(root(), "20261121-090000-000-pppp").value();
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", "/w/p.fugue"}}},
                                   {"nodes", QJsonObject{{"save", QJsonObject{{"state", "cached"}, {"shown", QJsonObject{{"value", image(shown)}}}}}}}}));
        QCOMPARE(mediaOfRun(root(), "20261121-090000-000-pppp").size(), 1);
        forget({"20261121-090000-000-pppp"});
    }

    void aBatchAndAllThreeKindsComeOut() {
        const QString a = makePng(dir.filePath("b1.png"));
        const QString b = makePng(dir.filePath("b2.png"));
        QFile clip(dir.filePath("clip.mp4"));
        QVERIFY(clip.open(QIODevice::WriteOnly));
        clip.write("x");
        clip.close();
        QFile song(dir.filePath("song.flac"));
        QVERIFY(song.open(QIODevice::WriteOnly));
        song.write("x");
        song.close();
        makeRun("20261121-110000-000-bbbb", "/w/b.fugue",
                {{"save", {{"images", QJsonArray{image(a), image(b)}}}},
                 {"gen", {}}}); // gen is not a sink here, but the workflow says save is
        QList<MediaItem> items = mediaOfRun(root(), "20261121-110000-000-bbbb");
        QCOMPARE(items.size(), 2);
        makeRun("20261121-110001-000-bbbb", "/w/b.fugue",
                {{"save", {{"v", image(dir.filePath("clip.mp4"), "video")}, {"s", image(dir.filePath("song.flac"), "audio")}}}});
        items = mediaOfRun(root(), "20261121-110001-000-bbbb");
        QStringList kinds;
        for (const MediaItem &item : items)
            kinds << item.kind;
        kinds.sort();
        QCOMPARE(kinds, QStringList({"audio", "video"}));
    }

    void aRunIsNamedByItsFirstImageAndItsPrompt() {
        const QString first = makePng(dir.filePath("id1.png"));
        const QString second = makePng(dir.filePath("id2.png"));
        QFile clip(dir.filePath("id.mp4"));
        QVERIFY(clip.open(QIODevice::WriteOnly));
        clip.write("x");
        clip.close();
        // The lead is the first image even when a video comes first; the label is the prompt.
        makeRun("20261121-140000-000-ffff", "/w/f.fugue",
                {{"save", {{"a", image(dir.filePath("id.mp4"), "video")}, {"b", image(first)}, {"c", image(second)}}}});
        const RunIdentity picture = runIdentityOf(root(), "20261121-140000-000-ffff");
        QCOMPARE(picture.leadPath, first);
        QCOMPARE(picture.leadKind, QString("image"));
        QCOMPARE(picture.label, QString("a red fox"));
        forget({"20261121-140000-000-ffff"});
    }

    void withNoPictureAndNoPromptTheLeadIsTheVideoAndTheLabelItsFileName() {
        // A workflow with no Text node: the label falls back to the first output's file name.
        const RunFolder folder = RunFolder::create(root(), "20261121-141000-000-gggg").value();
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", "/w/g.fugue"}}},
                                   {"nodes", QJsonObject{{"save", QJsonObject{{"state", "done"}, {"outputs", QJsonObject{{"v", image(dir.filePath("id.mp4"), "video")}}}}}}}}));
        const RunIdentity video = runIdentityOf(root(), "20261121-141000-000-gggg");
        QCOMPARE(video.leadKind, QString("video"));
        QCOMPARE(video.label, QString("id.mp4"));
        // A run that made nothing, or that is not there, has no identity: the caller keeps the name.
        const RunIdentity none = runIdentityOf(root(), "20261121-141001-000-gggg");
        QVERIFY(none.leadPath.isEmpty() && none.leadKind.isEmpty() && none.label.isEmpty());
        forget({"20261121-141000-000-gggg"});
    }

    void aLongPromptIsOneLine() {
        const RunFolder folder = RunFolder::create(root(), "20261121-142000-000-hhhh").value();
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", "/w/h.fugue"}}}, {"nodes", QJsonObject()}}));
        QJsonObject document = graph();
        QJsonObject nodes = document.value("graph").toObject().value("nodes").toObject();
        nodes.insert("prompt", QJsonObject{{"type", "fugue/text"}, {"inputs", QJsonObject{{"text", "a red\n\nfox   at dawn"}}}});
        document.insert("graph", QJsonObject{{"nodes", nodes}});
        QVERIFY(folder.writeWorkflow(document).isEmpty());
        QCOMPARE(runIdentityOf(root(), "20261121-142000-000-hhhh").label, QString("a red fox at dawn"));
        forget({"20261121-142000-000-hhhh"});
    }

    void theResultsOfOneRunAloneAreListedForTheBrowser() {
        const QString only = makePng(dir.filePath("alone.png"));
        makeRun("20261121-143000-000-iiii", "/w/i.fugue", {{"save", {{"a", image(only)}}}});
        makeRun("20261121-143001-000-iiii", "/w/i.fugue", {{"save", {{"a", image(only)}}}});
        MediaLibrary library(root(), dir.filePath("thumbs"));
        const QVariantList rows = library.resultsOfRun("20261121-143000-000-iiii");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value("run").toString(), QString("20261121-143000-000-iiii"));
        QVERIFY(rows[0].toMap().value("thumb").toString().startsWith("image://fuguemedia/"));
        QVERIFY(library.resultsOfRun("nonsense").isEmpty());
        forget({"20261121-143000-000-iiii", "20261121-143001-000-iiii"});
    }

    void withoutTheWorkflowFileEveryNodeCounts() {
        const QString only = makePng(dir.filePath("nowf.png"));
        makeRun("20261121-120000-000-cccc", "", {{"anything", {{"image", image(only)}}}}, "cached", false);
        QCOMPARE(mediaOfRun(root(), "20261121-120000-000-cccc").size(), 1);
    }

    void aFileThatIsGoneOrANodeThatFailedIsLeftOut() {
        const QString kept = makePng(dir.filePath("kept.png"));
        makeRun("20261121-130000-000-dddd", "/w/d.fugue", {{"save", {{"a", image(kept)}, {"b", image(dir.filePath("gone.png"))}}}});
        QCOMPARE(mediaOfRun(root(), "20261121-130000-000-dddd").size(), 1);
        makeRun("20261121-130001-000-dddd", "/w/d.fugue", {{"save", {{"a", image(kept)}}}}, "error");
        QCOMPARE(mediaOfRun(root(), "20261121-130001-000-dddd").size(), 0);
    }

    void aWorkflowsRunsComeNewestFirstAndAFileStaysWithTheNewest() {
        const QString one = makePng(dir.filePath("w1.png"));
        const QString two = makePng(dir.filePath("w2.png"));
        const QString three = makePng(dir.filePath("w3.png"));
        makeRun("20261122-100000-000-eeee", "/w/e.fugue", {{"save", {{"a", image(one)}}}});
        makeRun("20261122-110000-000-eeee", "/w/e.fugue", {{"save", {{"a", image(one)}, {"b", image(two)}}}}); // reran: `one` cached
        makeRun("20261122-120000-000-eeee", "/w/other.fugue", {{"save", {{"a", image(three)}}}});
        const QList<MediaItem> items = mediaOfWorkflow(root(), "/w/e.fugue");
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].run, QString("20261122-110000-000-eeee"));
        QCOMPARE(items[1].run, QString("20261122-110000-000-eeee"));
        QVERIFY(mediaOfWorkflow(root(), "").isEmpty());
        QVERIFY(mediaOfWorkflow(root(), "/w/none.fugue").isEmpty());
        QCOMPARE(mediaOfWorkflow(root(), "/w/e.fugue", 1).size(), 2);
    }

    void groupsKeepTheOrderAndSplitByRun() {
        const QString p = makePng(dir.filePath("g1.png"));
        const QString q = makePng(dir.filePath("g2.png"));
        const QString r = makePng(dir.filePath("g3.png"));
        makeRun("20261123-100000-000-ffff", "/w/f.fugue", {{"save", {{"a", image(p)}}}});
        makeRun("20261123-110000-000-ffff", "/w/f.fugue", {{"save", {{"a", image(q)}, {"b", image(r)}}}});
        const QList<MediaGroup> groups = groupByRun(mediaOfWorkflow(root(), "/w/f.fugue"));
        QCOMPARE(groups.size(), 2);
        QCOMPARE(groups[0].run, QString("20261123-110000-000-ffff"));
        QCOMPARE(groups[0].items.size(), 2);
        QCOMPARE(groups[1].items.size(), 1);
        QVERIFY(groupByRun({}).isEmpty());
    }

    void anEmbeddedWorkflowGivesThePromptTheSeedAndTheModel() {
        QJsonObject document = graph();
        document.insert("run", QJsonObject{{"nodes", QJsonObject{{"gen", QJsonObject{{"auto", QJsonObject{{"seed", 12345}}},
                                                                                       {"model", "flux-dev"}}}}}});
        const QString path = makePng(dir.filePath("emb.png"), 8, 8, QJsonDocument(document).toJson(QJsonDocument::Compact));
        const QJsonObject summary = embeddedSummary(path);
        QVERIFY(summary.value("embedded").toBool());
        QCOMPARE(summary.value("prompt").toString(), QString("a red fox"));
        QCOMPARE(summary.value("seed").toInt(), 12345);
        QCOMPARE(summary.value("model").toString(), QString("flux-dev"));
        QCOMPARE(summary.value("nodes").toInt(), 4);
        QVERIFY(!embeddedSummary(makePng(dir.filePath("plain.png"))).value("embedded").toBool());
        QVERIFY(!embeddedSummary(dir.filePath("clip.mp4")).value("embedded").toBool());
        QVERIFY(!embeddedSummary(dir.filePath("missing.png")).value("embedded").toBool());
    }

    void aThumbnailIsMadeOnceFitsItsBoxAndIsReadBack() {
        const QString big = makePng(dir.filePath("big.png"), 1000, 500);
        const QString cache = dir.filePath("thumbs");
        bool fromCache = true;
        const QImage first = thumbnail(cache, big, 128, &fromCache);
        QVERIFY(!fromCache);
        QCOMPARE(first.size(), QSize(128, 64));
        QCOMPARE(QDir(cache).entryList(QDir::Files).size(), 1);
        const QImage second = thumbnail(cache, big, 128, &fromCache);
        QVERIFY(fromCache);
        QCOMPARE(second.size(), first.size());
        // Another size is its own thumbnail; a file that is no image gives none.
        thumbnail(cache, big, 64);
        QCOMPARE(QDir(cache).entryList(QDir::Files).size(), 2);
        QVERIFY(thumbnail(cache, dir.filePath("clip.mp4"), 128).isNull());
        QVERIFY(thumbnail(cache, dir.filePath("missing.png"), 128).isNull());
        // A small image is not enlarged.
        QCOMPARE(thumbnail(cache, makePng(dir.filePath("small.png"), 40, 20), 128).size(), QSize(40, 20));
    }

    void aChangedFileGetsANewThumbnail() {
        const QString path = makePng(dir.filePath("change.png"), 300, 300);
        const QString before = thumbnailCachePath(dir.filePath("thumbs"), path, 128);
        QTest::qWait(20);
        makePng(path, 301, 300);
        QVERIFY(thumbnailCachePath(dir.filePath("thumbs"), path, 128) != before);
    }

    void theLibraryHandsTheBrowserRowsTokensAndDetails() {
        const QString shot = makePng(dir.filePath("lib.png"), 10, 10);
        makeRun("20261124-100000-000-gggg", "/w/g.fugue", {{"save", {{"a", image(shot)}}}});
        MediaLibrary library(root(), dir.filePath("thumbs"));
        const QVariantList rows = library.resultsOf("/w/g.fugue");
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows[0].toMap();
        QCOMPARE(row.value("title").toString(), QString("lib.png"));
        QCOMPARE(row.value("url").toUrl(), QUrl::fromLocalFile(shot));
        const QString thumb = row.value("thumb").toString();
        QVERIFY(thumb.startsWith("image://fuguemedia/256/"));
        QCOMPARE(library.pathOfToken(thumb.section('/', -1)), shot);
        QVERIFY(library.resultsOf("/w/none.fugue").isEmpty());
        const QVariantList details = library.detailsOf(shot, "20261124-100000-000-gggg", "save");
        QStringList labels;
        for (const QVariant &d : details)
            labels << d.toMap().value("label").toString();
        QVERIFY(labels.contains("File") && labels.contains("Size") && labels.contains("Run") && labels.contains("Node"));
        QUrl opened;
        library.setOpenUrl([&opened](const QUrl &url) { opened = url; });
        library.openFolder(shot);
        QCOMPARE(opened, QUrl::fromLocalFile(dir.path()));
        library.openFile(shot);
        QCOMPARE(opened, QUrl::fromLocalFile(shot));
        library.openFile(dir.filePath("missing.png"));
        QCOMPARE(opened, QUrl::fromLocalFile(shot)); // nothing opened for a file that is not there
    }
};

QTEST_MAIN(TestMediaItems)
#include "test_media_items.moc"
