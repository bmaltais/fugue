// What the media browser keeps and does to a file (#133): favorites, the remembered mode and view,
// the favorites-only filter, the trash and what a file can be used for. Seam: media/media_state.h and
// media/media_library.h over a temporary folder; the trash is a folder of the test's own, never the
// real one.

#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/embed.h"
#include "engine/run_folder.h"
#include "media/media_library.h"
#include "media/media_state.h"

using namespace fugue;

namespace {

QString makePng(const QString &path, const QByteArray &workflow = {}) {
    QImage image(40, 30, QImage::Format_RGB32);
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

} // namespace

class TestMediaState : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString root() const { return dir.path() + "/runs"; }
    // Every test has its own file, so what one keeps does not reach the next.
    QString state() const { return dir.filePath(QString(QTest::currentTestFunction()) + ".json"); }

    void makeRun(const QString &id, const QStringList &files) {
        const RunFolder folder = RunFolder::create(root(), id).value();
        QJsonObject outputs;
        for (int i = 0; i < files.size(); ++i)
            outputs.insert("f" + QString::number(i), QJsonObject{{"type", "image"}, {"path", files[i]}});
        QVERIFY(folder.writeState({{"workflow", QJsonObject{{"path", "/w/a.fugue"}}},
                                   {"nodes", QJsonObject{{"save", QJsonObject{{"state", "cached"}, {"outputs", outputs}}}}}}));
    }

private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    void whatIsKeptComesBackAndNonsenseIsIgnored() {
        const QString file = makePng(dir.filePath("fav.png"));
        {
            MediaState first(state());
            QVERIFY(first.setFavorite(file, true));
            QVERIFY(!first.setFavorite(file, true));
            QVERIFY(first.setMode("filmstrip"));
            QVERIFY(!first.setMode("sideways"));
            QVERIFY(first.setView("results"));
            QVERIFY(first.setOnlyFavorites(true));
        }
        MediaState second(state());
        QVERIFY(second.isFavorite(file));
        QCOMPARE(second.mode(), QString("filmstrip"));
        QCOMPARE(second.view(), QString("results"));
        QVERIFY(second.onlyFavorites());
        // A favorite whose file is gone is dropped on request, not by itself.
        QVERIFY(second.setFavorite(dir.filePath("gone.png"), true));
        QCOMPARE(second.prune(), 1);
        QVERIFY(second.isFavorite(file));
    }

    void aDamagedFileGivesTheDefaults() {
        QFile bad(dir.filePath("bad.json"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{ not json");
        bad.close();
        MediaState broken(dir.filePath("bad.json"));
        QCOMPARE(broken.mode(), QString("grid"));
        QCOMPARE(broken.view(), QString("files"));
        QVERIFY(!broken.onlyFavorites());
    }

    void favoritesMarkTheRowsAndNarrowTheList() {
        const QString one = makePng(dir.filePath("r1.png"));
        const QString two = makePng(dir.filePath("r2.png"));
        makeRun("20261125-100000-000-aaaa", {one, two});
        MediaLibrary library(root(), dir.filePath("thumbs"), state());
        QSignalSpy changed(&library, &MediaLibrary::resultsChanged);
        QVERIFY(library.toggleFavorite(two));
        QCOMPARE(changed.count(), 1);
        QVariantList rows = library.resultsOf("/w/a.fugue");
        QCOMPARE(rows.size(), 2);
        int marked = 0;
        for (const QVariant &row : rows)
            marked += row.toMap().value("favorite").toBool();
        QCOMPARE(marked, 1);
        QCOMPARE(rows[0].toMap().value("width").toInt(), 40);
        QCOMPARE(rows[0].toMap().value("height").toInt(), 30);
        library.setOnlyFavorites(true);
        rows = library.resultsOf("/w/a.fugue");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap().value("path").toString(), two);
        QVERIFY(!library.toggleFavorite(two));
        QVERIFY(library.resultsOf("/w/a.fugue").isEmpty());
    }

    void theTrashTakesAFileAndTheFavoriteGoes() {
        const QString file = makePng(dir.filePath("doomed.png"));
        MediaLibrary library(root(), dir.filePath("thumbs"), state());
        QDir().mkpath(dir.filePath("trash"));
        library.setTrash([this](const QString &path) { return QFile::rename(path, dir.filePath("trash/") + QFileInfo(path).fileName()); });
        library.toggleFavorite(file);
        QVERIFY(library.trash(file));
        QVERIFY(!QFileInfo::exists(file));
        QVERIFY(QFileInfo::exists(dir.filePath("trash/doomed.png")));
        QVERIFY(!library.isFavorite(file));
        // Not a file, or a trash that fails: nothing is claimed.
        QVERIFY(!library.trash(file));
        QVERIFY(!library.trash(dir.path()));
        const QString stays = makePng(dir.filePath("stays.png"));
        library.setTrash([](const QString &) { return false; });
        QVERIFY(!library.trash(stays));
        QVERIFY(QFileInfo::exists(stays));
    }

    void aFileSaysWhatItCanBeUsedFor() {
        MediaLibrary library(root(), dir.filePath("thumbs"), state());
        const QString plain = makePng(dir.filePath("plain.png"));
        const QString withWorkflow = makePng(dir.filePath("wf.png"),
                                             R"({"fugue": 1, "graph": {"nodes": {"p": {"type": "fugue/text", "inputs": {"text": "x"}}}}})");
        QVERIFY(library.capabilitiesOf(plain).value("loadImage").toBool());
        QVERIFY(!library.capabilitiesOf(plain).value("workflow").toBool());
        QVERIFY(library.capabilitiesOf(withWorkflow).value("workflow").toBool());
        QFile text(dir.filePath("note.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("hello");
        text.close();
        QVERIFY(!library.capabilitiesOf(text.fileName()).value("loadImage").toBool());
    }

    void modeAndViewAreTheLibrarys() {
        MediaLibrary library(root(), dir.filePath("thumbs"), dir.filePath("other.json"));
        QCOMPARE(library.resultsMode(), QString("grid"));
        library.setResultsMode("single");
        library.setResultsView("results");
        MediaLibrary again(root(), dir.filePath("thumbs"), dir.filePath("other.json"));
        QCOMPARE(again.resultsMode(), QString("single"));
        QCOMPARE(again.resultsView(), QString("results"));
    }
};

QTEST_MAIN(TestMediaState)
#include "test_media_state.moc"
