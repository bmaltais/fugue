// The Get ready sheet's choice for a model whose same weights are on disk (UI design 2026-11-26, #616): "Use my file" (the default) or
// "Download", kept in the sheet and acting only at the yes. The sheet is ReadinessSheet over a made-up request (two models that are not on
// disk, a use source that says one or both have a file of their own); the window's part (reading the file, setting it in the document) is
// played by the test, so nothing is read, installed or downloaded here. The Workflows side is test_templates.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "../engine/fake_http.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "translator.h"
#include "workflow/readiness_sheet.h"

using namespace fugue;

namespace {

const QString hashA = QStringLiteral("sha256:") + QString(64, u'a');
const QString hashB = QStringLiteral("sha256:") + QString(64, u'b');

QJsonObject model(const QString &name, const QString &hash, double size) {
    return {{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", name}, {"source", "http://127.0.0.1:9/" + name}, {"size", size}};
}

struct Rig {
    QTemporaryDir dir;
    Registry registry;
    Translator translator{dir.filePath("language")}; // English plural forms come from the catalog, as in the app
    QJsonArray models; // what the "workflow" still lacks: the test plays the window and takes a model out when its file is set
    QStringList covered; // the hashes that have a file of the same weights
    QJsonArray optional; // #594: models only another Switch branch needs
    ReadinessSheet sheet;

    explicit Rig(const QStringList &haveFiles, const QJsonArray &wanted)
        : models(wanted), covered(haveFiles),
          sheet((nodes::addBuiltinNodeTypes(registry), &registry), dir.filePath("trust.json"), [this] {
              ReadinessRequest request;
              request.models = models;
              request.optionalModels = optional;
              return request;
          }) {
        QCoreApplication::installTranslator(&translator);
        sheet.setUseSource([this](const QString &hash) {
            return covered.contains(hash) ? QVariantMap{{"file", "qwen_3_8b_int8_convrot.safetensors"}, {"same", "same model, INT8"}} : QVariantMap();
        });
    }
    ~Rig() { QCoreApplication::removeTranslator(&translator); }
    // The window's part: the files of `hashes` were read and set, so the workflow no longer lacks those models.
    void windowSets(const QStringList &hashes, int token) {
        QJsonArray left;
        for (const QJsonValue &m : std::as_const(models))
            if (!hashes.contains(m.toObject().value("hash").toString()))
                left.append(m);
        models = left;
        sheet.refresh();
        sheet.usesFinished(hashes, {}, token);
    }
    QVariantMap view() const { return sheet.view(); }
    QVariantMap row(int i) const { return sheet.view().value("models").toList().at(i).toMap(); }
};

} // namespace

class TestGetReadyChoice : public QObject {
    Q_OBJECT

private slots:
    // Check 1: one covered model starts on its file, says there is nothing to download, and the yes asks the window for the file and
    // starts no download; once it is set the sheet has nothing left and closes itself.
    void oneCoveredModelStartsOnItsFileAndTheYesDownloadsNothing() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9)});
        rig.sheet.show();
        QCOMPARE(rig.row(0).value("choice").toString(), QString("disk"));
        QCOMPARE(rig.view().value("headline").toString(), QString("Nothing to download: Fugue will use the file you already have."));
        QVERIFY(!rig.view().value("hasDownloads").toBool());
        QCOMPARE(rig.view().value("sizeText").toString(), QString());
        QCOMPARE(rig.view().value("primary").toString(), QString("Use my file"));
        QSignalSpy asked(&rig.sheet, &ReadinessSheet::useRequested);
        QSignalSpy succeeded(&rig.sheet, &ReadinessSheet::succeeded);
        rig.sheet.start();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().first().toStringList(), QStringList{hashA});
        QVERIFY(rig.view().value("busy").toBool()); // the rows wait for the file to be read
        QCOMPARE(rig.sheet.phase(), QString("review")); // no run: nothing is downloaded
        rig.windowSets({hashA}, asked.first().at(1).toInt());
        QCOMPARE(succeeded.size(), 1);
        QVERIFY(!rig.sheet.isOpen());
        QCOMPARE(rig.sheet.phase(), QString("review"));
        QVERIFY(rig.sheet.waitIdle(10)); // no thread was ever made
    }

    // #418: a download that stopped keeps its `.part`, and the row says it will resume.
    void aKeptPartIsShownAsPartial() {
        Rig rig({}, {model("part-model.safetensors", hashA, 2.0e9)});
        rig.sheet.show();
        const QString folder = rig.row(0).value("folder").toString();
        QVERIFY(QDir::isAbsolutePath(folder));
        QFile part(folder + "/part-model.safetensors.part");
        part.remove(); // the test home outlives a run
        rig.sheet.refresh();
        QVERIFY(!rig.row(0).value("partial").toBool());
        QDir().mkpath(folder);
        QVERIFY(part.open(QIODevice::WriteOnly));
        part.write("half");
        part.close();
        rig.sheet.refresh();
        QVERIFY(rig.row(0).value("partial").toBool());
        part.remove();
    }

    // Owner, 2026-11-27: Stop keeps what a download had. The row says Paused with its size, the retry says Resume and the headline counts what is left, the message is a plain note
    // (not an error), and Delete partial download removes the part (never through a link) and the row stops saying so.
    void aStopKeepsThePartAndItCanBeDeleted() {
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/stop-model.safetensors", [](const fugue::test::HttpRequest &) {
            fugue::test::HttpResponse response{200, QByteArray(300000, 'x'), "application/octet-stream"};
            response.cutAfter = 70000; // a break; the retry waits 2 s, and the person stops
            return response;
        });
        QJsonObject wanted = model("stop-model.safetensors", hashA, 300000);
        wanted["source"] = QString::fromLatin1(server.origin() + "/stop-model.safetensors");
        Rig rig({}, {wanted});
        rig.sheet.show();
        const QString part = rig.row(0).value("folder").toString() + "/stop-model.safetensors.part";
        QFile::remove(part);
        rig.sheet.refresh();
        rig.sheet.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(part).size() >= 70000, 10000);
        QVERIFY(!rig.row(0).value("partial").toBool()); // it runs: no flag, and no delete
        const QString before = rig.view().value("headline").toString();
        QVERIFY(!rig.view().value("resumes").toBool());
        rig.sheet.deletePartial(hashA);
        QVERIFY(QFileInfo::exists(part));
        rig.sheet.cancel();
        QVERIFY(rig.sheet.waitIdle(30000));
        QTRY_COMPARE_WITH_TIMEOUT(rig.sheet.phase(), QString("finished"), 10000);
        QVERIFY(QFileInfo(part).size() >= 70000);
        QVERIFY(rig.row(0).value("partial").toBool());
        QVERIFY(rig.row(0).value("partialText").toString().contains(" of ")); // "70 KB of 300 KB"
        QVERIFY(rig.row(0).value("paused").toBool()); // the row says Paused, not "Not done"
        QVERIFY(!rig.row(0).value("partialSize").toString().isEmpty()); // the trash button's tooltip
        QVERIFY(rig.view().value("resumes").toBool()); // the retry button says Resume
        QVERIFY(rig.view().value("headline").toString().contains(" left.")); // what is left, not the whole size
        QVERIFY(rig.view().value("headline").toString() != before);
        QVERIFY(rig.view().value("stopped").toBool()); // a note, not an error
        QVERIFY(!rig.view().value("ok").toBool());
        QVERIFY(rig.view().value("message").toString().startsWith("Stopped. Downloads keep what they had."));
        QVERIFY(!rig.view().value("message").toString().contains("install"));
        QVERIFY(!rig.view().value("message").toString().contains("setup"));
        rig.sheet.deletePartial(hashA);
        QVERIFY(!QFileInfo::exists(part));
        QVERIFY(!rig.row(0).value("partial").toBool());
        QVERIFY(!rig.row(0).value("paused").toBool()); // back to "Not done"
        QVERIFY(!rig.view().value("resumes").toBool()); // and the retry says Try again
        QCOMPARE(rig.view().value("headline").toString(), before);
    }

    // #594: a model only another branch needs is listed apart, "Not now" by default (the yes downloads nothing of it, and with nothing
    // else there is nothing to run); chosen, it is part of the download, and turned off again it goes.
    void anOptionalModelWaitsForItsOwnDownload() {
        Rig rig({}, {});
        rig.optional = QJsonArray{model("other-branch.gguf", hashA, 2.0e9)};
        rig.sheet.show();
        QVERIFY(rig.view().value("models").toList().isEmpty());
        QVariantList optional = rig.view().value("optionalModels").toList();
        QCOMPARE(optional.size(), 1);
        QVERIFY(!optional.at(0).toMap().value("on").toBool());
        QVERIFY(rig.view().value("empty").toBool()); // nothing for the yes to do
        QVERIFY(!rig.view().value("hasDownloads").toBool());
        QCOMPARE(rig.view().value("headline").toString(), QString("Nothing is needed to run. Models only another branch uses can be downloaded below."));
        rig.sheet.setOptional(hashA, true);
        optional = rig.view().value("optionalModels").toList();
        QCOMPARE(optional.size(), 1); // the same row, now on
        QVERIFY(optional.at(0).toMap().value("on").toBool());
        QVERIFY(rig.view().value("models").toList().isEmpty()); // not repeated in Models
        QVERIFY(!rig.view().value("empty").toBool());
        QVERIFY(rig.view().value("hasDownloads").toBool());
        QCOMPARE(rig.view().value("headline").toString(), QString("Fugue will download 1 model, 2.0 GB."));
        QVERIFY(rig.sheet.currentPlan().models.isEmpty()); // the Needs panel's plan: a sheet's choice never makes it a need
        rig.sheet.setOptional(hashA, false);
        QVERIFY(rig.view().value("empty").toBool());
        rig.sheet.show(); // a new look starts from Not now again
        QVERIFY(!rig.view().value("optionalModels").toList().at(0).toMap().value("on").toBool());
    }

    // A required model that cannot be had, beside an optional one: the headline says so, never "Nothing is needed to run".
    void anOptionalModelBesideAnUnobtainableOneSaysTheProblem() {
        QJsonObject nowhere = model("gone.gguf", hashB, 1.0e9);
        nowhere.remove("source");
        Rig rig({}, {nowhere});
        rig.optional = QJsonArray{model("other-branch.gguf", hashA, 2.0e9)};
        rig.sheet.show();
        QVERIFY(rig.view().value("headline").toString() != QString("Nothing is needed to run. Models only another branch uses can be downloaded below."));
    }

    // Check 2: Download chosen, the headline and the totals are the download's, the primary is Install and the destination is shown.
    void switchedToDownloadTheTotalsFollow() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9)});
        rig.sheet.show();
        rig.sheet.setChoice(hashA, false);
        QCOMPARE(rig.row(0).value("choice").toString(), QString("download"));
        QCOMPARE(rig.view().value("headline").toString(), QString("Fugue will download 1 model, 8.7 GB."));
        QCOMPARE(rig.view().value("sizeText").toString(), QString("8.7 GB"));
        QVERIFY(rig.view().value("hasDownloads").toBool());
        QCOMPARE(rig.view().value("primary").toString(), QString("Install"));
        rig.sheet.setChoice(hashA, true); // and back: the choice is only kept, nothing was done
        QCOMPARE(rig.view().value("primary").toString(), QString("Use my file"));
    }

    // Check 3: one covered, one not: the headline says both, the size is the download's alone, and the yes sends only the covered one to
    // the window; once it is set, the rest of the plan runs (the download, which fails to connect here and ends the run).
    void aMixedPlanDownloadsOnlyWhatIsNotCovered() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9), model("flux-2-klein-9b-Q4_K_M.gguf", hashB, 5.9e9)});
        rig.sheet.show();
        QCOMPARE(rig.view().value("headline").toString(), QString("Fugue will download 1 model, 5.9 GB and use 1 file you already have."));
        QCOMPARE(rig.view().value("sizeText").toString(), QString("5.9 GB"));
        QCOMPARE(rig.view().value("primary").toString(), QString("Install"));
        QCOMPARE(rig.row(1).value("choice").toString(), QString()); // no file of its own: no choice
        QSignalSpy asked(&rig.sheet, &ReadinessSheet::useRequested);
        rig.sheet.start();
        QCOMPARE(asked.first().first().toStringList(), QStringList{hashA});
        rig.windowSets({hashA}, asked.first().at(1).toInt());
        QCOMPARE(rig.sheet.phase(), QString("running")); // the rest of the plan goes on
        QCOMPARE(rig.view().value("models").toList().size(), 1);
        rig.sheet.cancel();
        QVERIFY(rig.sheet.waitIdle(30000));
    }

    // Another surface's read of a file (the Needs panel's) that ends while this sheet waits for its own is not the sheet's answer: the sheet
    // goes on only when its own request (its token) is answered, so a model set to Use my file is not downloaded early.
    void onlyTheAnswerToItsOwnRequestContinuesTheSheet() {
        Rig rig({hashA, hashB}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9), model("flux-2-klein-9b-Q4_K_M.gguf", hashB, 5.9e9)});
        rig.sheet.show();
        QSignalSpy asked(&rig.sheet, &ReadinessSheet::useRequested);
        rig.sheet.start();
        QCOMPARE(asked.size(), 1);
        const int mine = asked.first().at(1).toInt();
        QVERIFY(mine != 0);
        // The Needs panel's earlier read of A alone ends first (token 0, or any other): the plan is not run, B is not downloaded.
        rig.models = QJsonArray{model("flux-2-klein-9b-Q4_K_M.gguf", hashB, 5.9e9)};
        rig.sheet.refresh();
        rig.sheet.usesFinished({hashA}, {}, 0);
        rig.sheet.usesFinished({hashA}, {}, mine + 1);
        QCOMPARE(rig.sheet.phase(), QString("review"));
        QVERIFY(rig.view().value("busy").toBool()); // still waiting for its own
        rig.models = QJsonArray{};
        rig.sheet.refresh();
        rig.sheet.usesFinished({hashA, hashB}, {}, mine);
        QVERIFY(!rig.sheet.isOpen()); // its own answer: done
    }

    // A model still set to Use my file when the answer comes (it was not set: the tab changed) is never downloaded: it leaves what is run.
    void aModelStillOnItsFileIsNeverRun() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9), model("flux-2-klein-9b-Q4_K_M.gguf", hashB, 5.9e9)});
        rig.sheet.show();
        QSignalSpy asked(&rig.sheet, &ReadinessSheet::useRequested);
        rig.sheet.start();
        rig.sheet.usesFinished({}, {}, asked.first().at(1).toInt()); // nothing applied, no failure: A is still in the plan
        QCOMPARE(rig.view().value("models").toList().size(), 1);
        rig.sheet.cancel();
        QVERIFY(rig.sheet.waitIdle(30000));
    }

    // Check 4 (the sheet's part): Cancel discards the choices; the next opening starts on the files again.
    void cancelDiscardsTheChoices() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9)});
        rig.sheet.show();
        rig.sheet.setChoice(hashA, false);
        QSignalSpy asked(&rig.sheet, &ReadinessSheet::useRequested);
        rig.sheet.cancel();
        QVERIFY(!rig.sheet.isOpen());
        QCOMPARE(asked.size(), 0); // nothing was asked of the window
        rig.sheet.show();
        QCOMPARE(rig.row(0).value("choice").toString(), QString("disk"));
    }

    // Cancel while the file is read tells the window to drop the result.
    void cancelWhileTheFileIsReadStopsItsResult() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9)});
        rig.sheet.show();
        QSignalSpy cancelled(&rig.sheet, &ReadinessSheet::useCancelled);
        rig.sheet.start();
        rig.sheet.cancel();
        QCOMPARE(cancelled.size(), 1);
        QVERIFY(!rig.sheet.isOpen());
        rig.sheet.usesFinished({hashA}, {}, 1); // a late answer: nothing happens
        QVERIFY(!rig.sheet.isOpen());
    }

    // A failure said for the file goes away when the choice changes (it would be stale), and a file that could not be read is
    // offered again with Download chosen.
    void aFailureIsStaleOnceTheChoiceChanges() {
        Rig rig({hashA}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9)});
        rig.sheet.show();
        rig.sheet.start();
        rig.sheet.say("Could not read qwen_3_8b_int8_convrot.safetensors, so it is not used.");
        rig.sheet.usesFinished({}, {hashA}, 1);
        QVERIFY(rig.sheet.isOpen()); // the person decides what next
        QCOMPARE(rig.row(0).value("choice").toString(), QString("download"));
        QVERIFY(rig.row(0).value("error").toString().contains("is not used"));
        QVERIFY(rig.view().value("message").toString().contains("Could not read"));
        QVERIFY(!rig.view().value("busy").toBool());
        rig.sheet.setChoice(hashA, true);
        QCOMPARE(rig.view().value("message").toString(), QString());
        QCOMPARE(rig.row(0).value("error").toString(), QString());
        QCOMPARE(rig.row(0).value("choice").toString(), QString("disk"));
    }

    // The reading of each file is shown on its own row, not on the others.
    void eachRowShowsItsOwnReading() {
        Rig rig({hashA, hashB}, {model("Qwen3-8B-Q8_0.gguf", hashA, 8.7e9), model("flux-2-klein-9b-Q4_K_M.gguf", hashB, 5.9e9)});
        rig.sheet.show();
        rig.sheet.setReading({{hashA, 42}, {hashB, -1}});
        QCOMPARE(rig.row(0).value("state").toString(), QString("reading"));
        QCOMPARE(rig.row(0).value("fraction").toDouble(), 0.42);
        QCOMPARE(rig.row(1).value("state").toString(), QString("reading"));
        QCOMPARE(rig.row(1).value("fraction").toDouble(), -1.0);
        rig.sheet.setReading({{hashB, 10}});
        QCOMPARE(rig.row(0).value("state").toString(), QString("waiting"));
        QTRY_COMPARE(rig.row(1).value("fraction").toDouble(), 0.10); // a percent alone is published soon, not at once
    }
};

QTEST_MAIN(TestGetReadyChoice)
#include "test_get_ready_choice.moc"
