// The Needs panel's store (#469): a fake readiness plan becomes the rows of what an app still needs, with size, licence and the node
// that holds each; Download for one, Download all and Show in graph only ask the window (signals); the panel clears when a refresh
// finds the app ready. No window, no disk, no network. Seam: apps/needs_model.h.

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include "apps/needs_model.h"
#include "engine/document.h"

using namespace fugue;

namespace {

const QString hashA = "sha256:" + QString(64, QLatin1Char('a'));
const QString hashB = "sha256:" + QString(64, QLatin1Char('b'));

ReadinessModel model(const QString &name, const QString &hash, qint64 size, const QString &license, bool deferred = false) {
    ReadinessModel m;
    m.name = name;
    m.resource = QStringLiteral("fugue/diffusion-model");
    m.license = license;
    m.host = QStringLiteral("huggingface.co");
    m.size = size;
    m.entry = QJsonObject{{"resource", m.resource}, {"hash", hash}, {"name", name}};
    m.deferred = deferred;
    return m;
}

ReadinessPlan modelsPlan() {
    ReadinessPlan plan;
    plan.models << model(QStringLiteral("klein.gguf"), hashA, 5'800'000'000, QStringLiteral("Apache-2.0"))
                << model(QStringLiteral("vae.safetensors"), hashB, 335'000'000, QString());
    plan.downloadBytes = 5'800'000'000 + 335'000'000;
    return plan;
}

ReadinessPlan withPack(ReadinessPlan plan) {
    ReadinessPack pack;
    pack.pack.id = QStringLiteral("extras");
    pack.pack.name = QStringLiteral("Extras");
    pack.pack.version = QStringLiteral("1.0.0");
    pack.pack.setupCommand = {QStringLiteral("uv"), QStringLiteral("sync")};
    pack.pack.setupSize = 120'000'000;
    plan.packs << pack;
    return plan;
}

Document load(const QByteArray &json) {
    const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

} // namespace

class TestNeedsModel : public QObject {
    Q_OBJECT

private slots:

    // #605: a model whose same weights are on disk carries the offer; choosing it only asks the window, and the model is not offered
    // as a download once the plan no longer lists it.
    void aModelWithAFileOfTheSameWeightsOffersToUseIt() {
        NeedsModel needs;
        ReadinessPlan plan = modelsPlan();
        needs.setUseSource([](const QString &hash) {
            return hash == hashA ? QVariantMap{{"file", "klein-q8.gguf"}, {"same", "same model, Q8_0"}} : QVariantMap();
        });
        needs.setPlan(plan);
        QVERIFY(needs.items().at(0).toMap().value("use").toMap().contains("file"));
        QVERIFY(needs.items().at(1).toMap().value("use").toMap().isEmpty());
        QSignalSpy asked(&needs, &NeedsModel::useRequested);
        needs.useOnDisk(1); // no offer on this row: nothing asked
        QCOMPARE(asked.size(), 0);
        needs.useOnDisk(0);
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().first().toStringList(), QStringList{hashA});
        plan.models.removeFirst(); // the window applied it: the plan has one model less
        needs.setPlan(plan);
        QCOMPARE(needs.items().size(), 1);
    }

    // Check 7 (UI design 2026-11-26): the header chip. Every model covered and no pack: it uses the files at once (useRequested with every
    // hash, no sheet); some covered: Get ready… with the covered ones counted apart, never a download-all that would fetch them.
    void theHeaderChipUsesTheFilesWhenEveryModelIsCovered() {
        NeedsModel needs;
        needs.setUseSource([](const QString &) { return QVariantMap{{"file", "have.safetensors"}, {"same", "same model, INT8"}}; });
        needs.setPlan(modelsPlan());
        QCOMPARE(needs.headerAction(), QString("use"));
        QCOMPARE(needs.coveredCount(), 2);
        QVERIFY2(needs.headline().contains("you have them on disk"), qPrintable(needs.headline()));
        QSignalSpy used(&needs, &NeedsModel::useRequested);
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        needs.downloadAll();
        QCOMPARE(used.size(), 1);
        QCOMPARE(used.first().first().toStringList(), (QStringList{hashA, hashB}));
        QCOMPARE(sheet.size(), 0);
    }
    void theHeaderChipAsksGetReadyInTheMixedCaseAndCountsOnlyDownloads() {
        NeedsModel needs;
        needs.setUseSource([](const QString &hash) { return hash == hashA ? QVariantMap{{"file", "have.safetensors"}, {"same", "same model, INT8"}} : QVariantMap(); });
        needs.setPlan(modelsPlan());
        QCOMPARE(needs.headerAction(), QString("getReady"));
        QVERIFY(!needs.startsAtOnce()); // Download all would fetch the covered model
        QVERIFY2(needs.headline().contains("needs 1 model") && needs.headline().contains("You have 1 on disk."), qPrintable(needs.headline()));
        QSignalSpy used(&needs, &NeedsModel::useRequested);
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        needs.downloadAll();
        QCOMPARE(used.size(), 0);
        QCOMPARE(sheet.size(), 1);
        QCOMPARE(sheet.first().first().toBool(), false);
    }
    void aRowReadsItsOwnFileOnly() {
        NeedsModel needs;
        needs.setPlan(modelsPlan());
        QSignalSpy moved(&needs, &NeedsModel::readingChanged);
        needs.setReading({{hashA, 42}});
        QCOMPARE(moved.size(), 1);
        QVERIFY(needs.reading().contains(hashA));
        QVERIFY(!needs.reading().contains(hashB));
        needs.setReading({{hashA, 42}}); // the same again: no redraw
        QCOMPARE(moved.size(), 1);
    }
    // #626: a file read for another tab's model must not hold this list's header chip.
    void theHeaderWaitsOnlyForReadsOfItsOwnModels() {
        NeedsModel needs;
        needs.setPlan(modelsPlan());
        QSignalSpy moved(&needs, &NeedsModel::readingOwnChanged);
        QVERIFY(!needs.readingOwn());
        needs.setReading({{"another-tab-hash", 10}});
        QVERIFY(!needs.readingOwn());
        QCOMPARE(moved.size(), 0);
        needs.setReading({{"another-tab-hash", 10}, {hashB, -1}});
        QVERIFY(needs.readingOwn());
        QCOMPARE(moved.size(), 1);
        needs.setReading({});
        QVERIFY(!needs.readingOwn());
        QCOMPARE(moved.size(), 2);
    }
    void aPlanBecomesRowsWithSizeAndLicence() {
        NeedsModel needs;
        QVERIFY(needs.ready());
        QSignalSpy changed(&needs, &NeedsModel::changed);
        needs.setPlan(modelsPlan());
        QCOMPARE(changed.size(), 1);
        QVERIFY(!needs.ready());
        const QVariantList items = needs.items();
        QCOMPARE(items.size(), 2);
        const QVariantMap first = items.at(0).toMap();
        QCOMPARE(first.value("kind").toString(), QStringLiteral("model"));
        QCOMPARE(first.value("name").toString(), QStringLiteral("klein.gguf"));
        QCOMPARE(first.value("licence").toString(), QStringLiteral("Apache-2.0"));
        QVERIFY(first.value("size").toString().contains("GB"));
        QVERIFY(first.value("canDownload").toBool());
        QVERIFY(needs.headline().contains("2 models"));
        // The same plan again changes nothing: no redraw of an idle view.
        needs.setPlan(modelsPlan());
        QCOMPARE(changed.size(), 1);
        // Ready: the panel clears.
        needs.setPlan(ReadinessPlan());
        QVERIFY(needs.ready());
        QVERIFY(needs.headline().isEmpty());
        QCOMPARE(changed.size(), 2);
    }

    // #493: the paired host cannot be asked whether it has a model, so a row stays, and says whose copy the run would use.
    void aModelThatMayBeOnThePairedHostStaysAndSaysSo() {
        NeedsModel needs;
        ReadinessPlan plan = modelsPlan();
        plan.models[0].onHost = QStringLiteral("gx10");
        needs.setPlan(plan);
        QCOMPARE(needs.items().size(), 2);
        QVERIFY(needs.items().at(0).toMap().value("detail").toString().contains("on gx10 if it has it"));
        QVERIFY(!needs.items().at(1).toMap().value("detail").toString().contains("gx10"));
        QVERIFY(needs.items().at(0).toMap().value("canDownload").toBool()); // still downloadable here
    }

    void aPackRowComesFirstAndDownloadAllOpensTheSheetForTheYes() {
        NeedsModel needs;
        needs.setPlan(withPack(modelsPlan()));
        QCOMPARE(needs.items().size(), 3);
        QCOMPARE(needs.items().at(0).toMap().value("kind").toString(), QStringLiteral("pack"));
        QVERIFY(!needs.items().at(0).toMap().value("canDownload").toBool());
        QVERIFY(!needs.startsAtOnce()); // a pack needs the person's yes in the sheet
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        QSignalSpy single(&needs, &NeedsModel::downloadRequested);
        needs.downloadAll();
        QCOMPARE(sheet.size(), 1);
        QCOMPARE(sheet.first().first().toBool(), false);
        needs.download(0); // the pack: the sheet again, never a download of its own
        QCOMPARE(sheet.size(), 2);
        QCOMPARE(single.size(), 0);
    }

    void onlyModelsStartAtOnceAndOneModelDownloadsAlone() {
        NeedsModel needs;
        needs.setPlan(modelsPlan());
        QVERIFY(needs.startsAtOnce());
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        QSignalSpy single(&needs, &NeedsModel::downloadRequested);
        needs.downloadAll();
        QCOMPARE(sheet.size(), 1);
        QCOMPARE(sheet.first().first().toBool(), true);
        needs.download(1);
        QCOMPARE(single.size(), 1);
        QCOMPARE(single.first().first().toJsonObject().value("hash").toString(), hashB);
        needs.download(9); // no such row: nothing
        QCOMPARE(single.size(), 1);
    }

    // #594: a model only another Switch branch needs is a row with its download, listed after the blocking ones; it does not make the
    // panel show by itself and it is not in the headline.
    void anOptionalModelIsARowThatBlocksNothing() {
        NeedsModel needs;
        ReadinessPlan plan;
        plan.optionalModels << model(QStringLiteral("other-branch.gguf"), hashA, 1000, QString());
        needs.setPlan(plan);
        QVERIFY(needs.ready());
        QCOMPARE(needs.items().size(), 1);
        QCOMPARE(needs.items().at(0).toMap().value("kind").toString(), QString("optional"));
        QVERIFY(needs.items().at(0).toMap().value("canDownload").toBool());
        QVERIFY(needs.headline().isEmpty());
        QSignalSpy single(&needs, &NeedsModel::downloadRequested);
        needs.download(0);
        QCOMPARE(single.size(), 1);
        needs.setPlan(modelsPlan()); // a blocking model: the panel shows, the headline counts only it
        QVERIFY(!needs.ready());
        plan = modelsPlan();
        plan.optionalModels << model(QStringLiteral("other-branch.gguf"), hashA, 1000, QString());
        needs.setPlan(plan);
        QVERIFY(!needs.ready());
        QCOMPARE(needs.items().size(), 3);
        QVERIFY(needs.headline().contains("2 models"));
    }

    // #735: a model a paired host has is a quiet row ("on gx10"): the panel hides when every row is hosted or optional, and a blocking
    // model beside it still shows it.
    void aHostedModelIsARowThatBlocksNothing() {
        NeedsModel needs;
        ReadinessPlan plan;
        plan.hosted << HostedModel{QStringLiteral("z.gguf"), QStringLiteral("gx10"), QJsonObject{{"hash", hashA}}};
        needs.setPlan(plan);
        QVERIFY(needs.ready());
        QCOMPARE(needs.items().size(), 1);
        QCOMPARE(needs.items().at(0).toMap().value("kind").toString(), QString("hosted"));
        QCOMPARE(needs.items().at(0).toMap().value("detail").toString(), QString("on gx10"));
        QVERIFY(!needs.items().at(0).toMap().value("canDownload").toBool());
        plan.optionalModels << model(QStringLiteral("other-branch.gguf"), hashA, 1000, QString());
        needs.setPlan(plan);
        QVERIFY(needs.ready()); // hosted and optional only
        needs.setPlan(modelsPlan());
        QVERIFY(!needs.ready());
        QVERIFY(needs.headline().contains("models"));
    }

    void aModelWaitingForAPackComesWithTheSheet() {
        NeedsModel needs;
        ReadinessPlan plan = withPack(ReadinessPlan());
        plan.models << model(QStringLiteral("typed-by-pack.bin"), hashA, 10, QString(), true);
        needs.setPlan(plan);
        QVERIFY(!needs.startsAtOnce());
        QVERIFY(!needs.items().at(1).toMap().value("canDownload").toBool());
        QSignalSpy single(&needs, &NeedsModel::downloadRequested);
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        needs.download(1);
        QCOMPARE(single.size(), 0);
        QCOMPARE(sheet.size(), 1);
    }

    void aProblemIsAQuietLineWithNoAction() {
        NeedsModel needs;
        ReadinessPlan plan;
        Error problem;
        problem.code = QStringLiteral("no_source");
        problem.message = QStringLiteral("Nothing says where klein.gguf can be had.");
        plan.problems << problem;
        needs.setPlan(plan);
        QCOMPARE(needs.items().size(), 1);
        QCOMPARE(needs.items().at(0).toMap().value("kind").toString(), QStringLiteral("problem"));
        QSignalSpy sheet(&needs, &NeedsModel::getReadyRequested);
        needs.download(0);
        QCOMPARE(sheet.size(), 0);
    }

    void refreshAsksTheSourceAndASourceThatSaysReadyClearsThePanel() {
        NeedsModel needs;
        bool missing = true;
        int asked = 0;
        needs.setPlanSource([&] { ++asked; return missing ? modelsPlan() : ReadinessPlan(); });
        needs.refresh();
        QCOMPARE(needs.items().size(), 2);
        missing = false; // what a download finishing does: the window asks again
        needs.refreshSoon();
        needs.refreshSoon(); // two changes at once, one look
        QTRY_VERIFY(needs.ready());
        QCOMPARE(asked, 2);
    }

    void showInGraphFindsTheNodeThatHoldsTheModelAndOnlyWhenTheGraphIsThere() {
        const Document document = load(R"({"fugue": 1, "graph": {"nodes": {
            "enc": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "x"}},
            "unet": {"type": "fugue/load-diffusion-model", "version": "1.0.0",
                     "inputs": {"model": {"resource": "fugue/diffusion-model", "hash": ")" + hashB.toUtf8() + R"(", "name": "vae.safetensors"}}}
        }}})");
        QCOMPARE(NeedsModel::nodeFor(document, {{"kind", "model"}, {"hash", hashB}}), QStringLiteral("unet"));
        QCOMPARE(NeedsModel::nodeFor(document, {{"kind", "model"}, {"hash", hashA}}), QString());
        QCOMPARE(NeedsModel::nodeFor(document, {{"kind", "pack"}, {"id", "fugue"}}), QStringLiteral("enc")); // the first node of that pack

        NeedsModel needs;
        needs.setLocator([&](const QVariantMap &item) { return NeedsModel::nodeFor(document, item); });
        needs.setPlan(modelsPlan());
        QVERIFY(needs.canShowInGraph());
        QCOMPARE(needs.items().at(0).toMap().value("node").toString(), QString()); // klein is in no node
        QCOMPARE(needs.items().at(1).toMap().value("node").toString(), QStringLiteral("unet"));
        QSignalSpy show(&needs, &NeedsModel::showNodeRequested);
        needs.showInGraph(0);
        QCOMPARE(show.size(), 0);
        needs.showInGraph(1);
        QCOMPARE(show.size(), 1);
        QCOMPARE(show.first().first().toString(), QStringLiteral("unet"));
        needs.setGraphAvailable(false); // the app's own window
        QVERIFY(!needs.canShowInGraph());
        needs.showInGraph(1);
        QCOMPARE(show.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestNeedsModel)
#include "test_needs_model.moc"
