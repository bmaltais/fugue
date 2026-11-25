// The App view's store (#385): a workflow with an `app` block as a form, one Run that binds the form into a copy, the run's
// events as progress, results and errors, per tab, and the switch to the graph and back. A fake run (the events a RunService
// would send, fed in by hand); no window. Seam: apps/app_view.h.

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>

#include "apps/app_models.h"
#include "apps/app_view.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/port_type.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "run/run_service.h"

using namespace fugue;

namespace {

QJsonObject workflow(const QString &view = QStringLiteral("app")) {
    return QJsonDocument::fromJson(QString(R"({
      "fugue": 1,
      "app": {"format": 1, "title": "Restyle", "description": "Paints a picture in a style.", "defaultView": "%1",
              "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"},
                         {"role": "seed", "targets": ["sample.seed"]},
                         {"name": "steps", "targets": ["sample.steps"], "min": 1, "max": 8, "section": "Advanced"},
                         {"name": "quality", "targets": ["sample.denoise"], "choices": ["Light", "Strong"],
                          "choice_values": [0.4, 0.9], "default": "Light"}],
              "outputs": [{"node": "save", "label": "Picture"}, {"node": "save2"}]},
      "graph": {"nodes": {
        "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
        "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"conditioning": {"from": "encode.conditioning"}, "seed": {"auto": "random"}, "steps": 4}},
        "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}},
        "save2": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out2"}}
      }}
    })").arg(view).toUtf8()).object();
}

Document load(const QJsonObject &json) {
    const Result<Document> loaded = Document::fromJson(json);
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// One input of every kind the form has besides the plain ones: a picture and its mask (Load Image), a toggle (Grow Mask),
// a model (Load Upscale Model) and a seed that is a number the workflow holds.
QJsonObject mediaWorkflow() {
    return QJsonDocument::fromJson(R"({
      "fugue": 1,
      "app": {"format": 1, "title": "Media",
              "inputs": [{"role": "image", "targets": ["load.file"], "label": "Picture"},
                         {"role": "mask", "targets": ["load.mask"]},
                         {"name": "tapered", "targets": ["grow.tapered_corners"]},
                         {"role": "model", "targets": ["up.model"]},
                         {"role": "seed", "targets": ["sample.seed"], "min": 0, "max": 10}],
              "outputs": [{"node": "save"}]},
      "graph": {"nodes": {
        "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/sample.png"}},
        "grow": {"type": "fugue/grow-mask", "version": "1.0.1", "inputs": {"tapered_corners": true}},
        "up": {"type": "fugue/load-upscale-model", "version": "1.0.0", "inputs": {}},
        "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 3, "steps": 4}},
        "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
      }}
    })").object();
}

QJsonValue inputOf(const Document &document, const char *node, const char *input) {
    return document.graph.nodes.value(QLatin1String(node)).inputs.value(QLatin1String(input)).toJson();
}

QJsonObject saved(const QString &node, const QString &file) {
    return {{"event", "node.done"}, {"node", node},
            {"thumbnails", QJsonObject{{"image", QJsonObject{{"url", "file:///t/" + file}, {"source", "/t/" + file},
                                                             {"width", 64}, {"height", 48}}}}}};
}

QJsonObject evt(const char *kind, QJsonObject more = {}) {
    more.insert("event", QLatin1String(kind));
    return more;
}

} // namespace

class TestAppView : public QObject {
    Q_OBJECT
    Registry m_registry;
    QList<RunRequest> m_started;
    QStringList m_cancelled;
    std::unique_ptr<AppView> m_view;

    QVariantMap field(const QString &key) const {
        for (const QVariant &f : m_view->fields())
            if (f.toMap().value("key") == key)
                return f.toMap();
        return {};
    }

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void init() {
        m_started.clear();
        m_cancelled.clear();
        m_view = std::make_unique<AppView>(
            [this](const RunRequest &request) {
                m_started << request;
                return QStringLiteral("run%1").arg(m_started.size());
            },
            [this](const QString &run) { m_cancelled << run; }, &m_registry);
    }

    void aWorkflowWithoutABlockIsNoApp() {
        QJsonObject plain = workflow();
        plain.remove("app");
        m_view->show("a", load(plain), {});
        QVERIFY(!m_view->isApp());
        QVERIFY(!m_view->shown());
        QVERIFY(m_view->fields().isEmpty());
        QVERIFY(!m_view->canRun());
        QVERIFY(m_view->run().isEmpty());
        QVERIFY(m_started.isEmpty());
    }

    void theBlockIsTheFormInItsOrderWithWhatTheWorkflowHas() {
        m_view->show("a", load(workflow()), {});
        QVERIFY(m_view->shown());
        QCOMPARE(m_view->title(), QStringLiteral("Restyle"));
        QCOMPARE(m_view->fields().size(), 4);
        QCOMPARE(field("prompt").value("kind"), QStringLiteral("text"));
        QCOMPARE(field("prompt").value("value"), QStringLiteral("old words"));
        QCOMPARE(field("prompt").value("label"), QStringLiteral("Prompt"));
        // An input with no label is its role in words; a seed left random has no number.
        QCOMPARE(field("seed").value("label"), QStringLiteral("Seed"));
        QCOMPARE(field("seed").value("kind"), QStringLiteral("seed"));
        QCOMPARE(field("seed").value("mode"), QStringLiteral("random"));
        QVERIFY(!field("seed").value("value").isValid());
        QVERIFY(field("prompt").value("multiline").toBool()); // a prompt role is several lines
        QCOMPARE(field("steps").value("value").toInt(), 4);
        QCOMPARE(field("steps").value("max").toInt(), 8);
        QVERIFY(field("steps").value("advanced").toBool());
        QCOMPARE(field("quality").value("value"), QStringLiteral("Light"));
        QCOMPARE(field("quality").value("choices").toList().size(), 2);
    }

    // An input bound to a group instance's exposed input is shaped by the inner input (spec 5.4): a number with its range, a choice.
    void aGroupInstanceInputHasTheInnerInputsKind() {
        const QJsonObject json = QJsonDocument::fromJson(R"({
          "fugue": 1,
          "app": {"format": 1, "title": "Shade",
                  "inputs": [{"name": "opacity", "targets": ["shade.opacity"]},
                             {"name": "mode", "targets": ["shade.mode"]}],
                  "outputs": [{"node": "save"}]},
          "graph": {"nodes": {
            "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "a.png"}},
            "shade": {"type": "local/shade", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "shade.image"}, "name": "out"}}}},
          "groups": {"local/shade": {"version": "1.0.0", "name": "Shade",
            "inputs": [{"name": "image", "targets": ["mix.base", "mix.top"]},
                       {"name": "opacity", "targets": ["mix.opacity"]},
                       {"name": "mode", "targets": ["mix.mode"]}],
            "outputs": [{"name": "image", "from": "mix.image"}],
            "nodes": {"mix": {"type": "fugue/blend", "version": "1.0.0", "inputs": {}}}}}
        })").object();
        m_view->show("a", load(json), {});
        QCOMPARE(field("opacity").value("kind"), QStringLiteral("number"));
        QCOMPARE(field("opacity").value("max").toDouble(), 1.0);
        QCOMPARE(field("opacity").value("value").toDouble(), 0.5); // the inner input's default
        QCOMPARE(field("mode").value("kind"), QStringLiteral("choice"));
        QVERIFY(field("mode").value("choices").toList().size() > 2);
        QCOMPARE(field("mode").value("value"), QStringLiteral("normal"));
    }

    void aChoiceWithNoDefaultHasTheFirstInForceAndItIsWhatARunBinds() {
        QJsonObject json = workflow();
        QJsonObject block = json.value("app").toObject();
        QJsonArray inputs = block.value("inputs").toArray();
        inputs.append(QJsonObject{{"name", "format"}, {"targets", QJsonArray{"save.format"}}, {"choices", QJsonArray{"png"}}});
        block.insert("inputs", inputs);
        json.insert("app", block);
        m_view->show("a", load(json), {});
        QVERIFY(m_view->problem().isEmpty());
        QCOMPARE(field("format").value("value"), QStringLiteral("png"));
        QCOMPARE(m_view->values().value("format"), QJsonValue("png"));
    }

    void aTabOpensInTheViewTheBlockNamesAndTheChoiceIsKeptPerTab() {
        m_view->show("a", load(workflow("graph")), {});
        QVERIFY(m_view->isApp());
        QVERIFY(!m_view->shown());
        m_view->show("b", load(workflow()), {});
        QVERIFY(m_view->shown());
        m_view->openInGraph();
        QVERIFY(!m_view->shown());
        m_view->show("a", load(workflow("graph")), {});
        QVERIFY(!m_view->shown());
        m_view->showApp();
        QVERIFY(m_view->shown());
        m_view->show("b", load(workflow()), {}); // opened in the graph by the person: stays so
        QVERIFY(!m_view->shown());
    }

    void runBindsTheFormIntoACopyAndNeverChangesTheDocument() {
        const Document original = load(workflow());
        m_view->show("a", original, "/w/restyle.fugue");
        m_view->setValue("prompt", "a red fox");
        m_view->setValue("seed", 7);
        m_view->setValue("quality", "Strong");
        QCOMPARE(m_view->run(), QStringLiteral("run1"));
        QCOMPARE(m_started.size(), 1);
        const Document &bound = m_started.first().document;
        QCOMPARE(m_started.first().workflow, QStringLiteral("/w/restyle.fugue"));
        QCOMPARE(inputOf(bound, "encode", "prompt"), QJsonValue("a red fox"));
        QCOMPARE(inputOf(bound, "sample", "seed"), QJsonValue(7));
        QCOMPARE(inputOf(bound, "sample", "denoise"), QJsonValue(0.9));
        QCOMPARE(inputOf(bound, "sample", "steps"), QJsonValue(4)); // never touched: the workflow's own
        QCOMPARE(original.toJson(), load(workflow()).toJson());
        QVERIFY(m_view->busy());
        QVERIFY(!m_view->canRun()); // one run of a tab at a time
    }

    void anEmptyNumberTakesTheValueAwayAndTheWorkflowsOwnStays() {
        m_view->show("a", load(workflow()), {});
        m_view->setValue("seed", 7);
        m_view->setValue("seed", "");
        QVERIFY(!m_view->values().contains("seed"));
        m_view->run();
        QCOMPARE(inputOf(m_started.first().document, "sample", "seed"), QJsonValue(QJsonObject{{"auto", "random"}}));
    }

    void theRunsEventsAreProgressAndTheOutputsOfTheAppAreTheResults() {
        m_view->show("a", load(workflow()), {});
        const QString run = m_view->run();
        QCOMPARE(m_view->status(), QStringLiteral("waiting"));
        m_view->onEvent(run, evt("run.started"));
        QCOMPARE(m_view->status(), QStringLiteral("running"));
        QSignalSpy progress(m_view.get(), &AppView::progressChanged);
        m_view->onEvent(run, evt("node.progress", {{"node", "sample"}, {"value", 0.5}}));
        QCOMPARE(m_view->progress(), 0.5);
        QCOMPARE(progress.count(), 1);
        // A node that is not an output of the app shows nothing; the second output arrives first and sits in the block's order.
        m_view->onEvent(run, saved("encode", "x.png"));
        m_view->onEvent(run, saved("save2", "b.png"));
        m_view->onEvent(run, saved("save", "a.png"));
        const QVariantList results = m_view->results();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).toMap().value("label"), QStringLiteral("Picture"));
        QCOMPARE(results.at(0).toMap().value("full"), QUrl::fromLocalFile("/t/a.png").toString());
        QCOMPARE(results.at(1).toMap().value("label"), QStringLiteral("Result 2"));
        m_view->onEvent(run, evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_view->status(), QStringLiteral("done"));
        QVERIFY(!m_view->busy());
        QVERIFY(m_view->error().isEmpty());
        QCOMPARE(m_view->results().size(), 2); // kept until the next run
        m_view->run();
        QVERIFY(m_view->results().isEmpty());
    }

    void aFailedRunSaysOneLineAndKeepsTheRestInDetails() {
        m_view->show("a", load(workflow()), {});
        const QString run = m_view->run();
        const QJsonObject first{{"code", "model_missing"}, {"message", "No model can run this."}, {"hint", "Download one."}};
        const QJsonObject second{{"code", "oom"}, {"message", "Not enough GPU memory."}};
        m_view->onEvent(run, evt("node.error", {{"node", "sample"}, {"error", first}}));
        m_view->onEvent(run, evt("run.done", {{"status", "failed"}, {"errors", QJsonArray{first, second}}}));
        QCOMPARE(m_view->status(), QStringLiteral("failed"));
        QCOMPARE(m_view->error(), QStringLiteral("No model can run this."));
        QCOMPARE(m_view->details().size(), 2); // the node's error is not listed twice
        QCOMPARE(m_view->details().at(0).toMap().value("hint"), QStringLiteral("Download one."));
        QVERIFY(m_view->canRun()); // run again
    }

    void cancelEndsTheRunWithoutAnError() {
        m_view->show("a", load(workflow()), {});
        const QString run = m_view->run();
        m_view->cancel();
        QCOMPARE(m_cancelled, QStringList{run});
        m_view->onEvent(run, evt("run.done", {{"status", "cancelled"}}));
        QCOMPARE(m_view->status(), QStringLiteral("cancelled"));
        QVERIFY(m_view->error().isEmpty());
        QVERIFY(m_view->details().isEmpty());
    }

    void aTabKeepsItsRunWhileAnotherIsShown() {
        m_view->show("a", load(workflow()), {});
        const QString run = m_view->run();
        m_view->show("b", load(workflow()), {});
        QVERIFY(!m_view->busy());
        QVERIFY(m_view->canRun());
        m_view->onEvent(run, saved("save", "a.png")); // for the tab not shown
        m_view->onEvent(run, evt("run.done", {{"status", "ok"}}));
        QVERIFY(m_view->results().isEmpty());
        m_view->show("a", load(workflow()), {});
        QCOMPARE(m_view->status(), QStringLiteral("done"));
        QCOMPARE(m_view->results().size(), 1);
        m_view->forget("a");
        QVERIFY(!m_view->isApp());
    }

    void anInputWhoseNodeWasDeletedIsLeftOutAndNothingIsMadeUp() {
        QJsonObject json = workflow();
        QJsonObject graph = json.value("graph").toObject();
        QJsonObject nodes = graph.value("nodes").toObject();
        nodes.remove("encode");
        nodes.insert("sample", QJsonObject{{"type", "fugue/sampler"}, {"version", "1.2.0"}, {"inputs", QJsonObject{{"steps", 4}}}});
        graph.insert("nodes", nodes);
        json.insert("graph", graph);
        m_view->show("a", load(json), {});
        QCOMPARE(m_view->fields().size(), 3); // the prompt is gone
        m_view->run();
        QVERIFY(!m_started.first().document.graph.nodes.contains("encode"));
    }

    // --- one test per input type (#386): edit the value at the store, the run's bound copy uses it ---

    void aToggleIsASwitchAndItsValueIsWhatTheRunBinds() {
        m_view->show("a", load(mediaWorkflow()), {});
        QVERIFY2(m_view->problem().isEmpty(), qPrintable(m_view->problem()));
        QCOMPARE(field("tapered").value("kind"), QStringLiteral("toggle"));
        QCOMPARE(field("tapered").value("value"), QVariant(true));
        m_view->setValue("tapered", false);
        m_view->run();
        QCOMPARE(inputOf(m_started.first().document, "grow", "tapered_corners"), QJsonValue(false));
    }

    void aSeedKeepsItsModeAndEachModeBindsAsANodeDoes() {
        m_view->show("a", load(mediaWorkflow()), {});
        QCOMPARE(field("seed").value("mode"), QStringLiteral("fixed")); // the workflow holds a number
        QCOMPARE(m_view->valueOf("seed").toInt(), 3);
        // Random: the engine's own, a new number each run; the one shown stays for a return to Fixed.
        m_view->setSeedMode("seed", "random");
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "sample", "seed"), QJsonValue(QJsonObject{{"auto", "random"}}));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        // Increment: this run uses the number shown, the next one the next number, told to the control.
        m_view->setSeedMode("seed", "increment");
        QSignalSpy told(m_view.get(), &AppView::valueChanged);
        m_view->setValue("seed", 9);
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "sample", "seed"), QJsonValue(9));
        QCOMPARE(m_view->valueOf("seed").toInt(), 10);
        QCOMPARE(told.count(), 1);
        m_view->onEvent("run2", evt("run.done", {{"status", "ok"}}));
        m_view->run(); // 10 is the range's end: the next one wraps to its start
        QCOMPARE(inputOf(m_started.last().document, "sample", "seed"), QJsonValue(10));
        QCOMPARE(m_view->valueOf("seed").toInt(), 0);
        m_view->onEvent("run3", evt("run.done", {{"status", "ok"}}));
        // A number typed is a fixed seed; the dice picks one in the range.
        m_view->setSeedMode("seed", "random");
        m_view->setValue("seed", 5);
        QCOMPARE(m_view->seedMode("seed"), QStringLiteral("fixed"));
        m_view->rollSeed("seed");
        QVERIFY(m_view->valueOf("seed").toInt() >= 0 && m_view->valueOf("seed").toInt() <= 10);
    }

    void fixedKeepsAConcreteNumberAndEveryRunUsesIt() {
        // A workflow whose seed is left to chance has no number: Fixed picks one, shows it and keeps it for every run.
        m_view->show("a", load(workflow()), {});
        QCOMPARE(m_view->seedMode("seed"), QStringLiteral("random"));
        QVERIFY(!m_view->valueOf("seed").isValid());
        m_view->setSeedMode("seed", "fixed");
        QCOMPARE(m_view->seedMode("seed"), QStringLiteral("fixed"));
        const QVariant shown = m_view->valueOf("seed");
        QVERIFY(shown.isValid() && shown.typeId() != QMetaType::QVariantMap);
        m_view->setSeedMode("seed", "fixed"); // pressing it again keeps the number
        QCOMPARE(m_view->valueOf("seed"), shown);
        m_view->run();
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->run();
        QCOMPARE(inputOf(m_started.at(0).document, "sample", "seed"), QJsonValue::fromVariant(shown));
        QCOMPARE(inputOf(m_started.at(1).document, "sample", "seed"), QJsonValue::fromVariant(shown));
        // Back to Random and again to Fixed: the number is still there, not the engine's {"auto": "random"}.
        m_view->setSeedMode("seed", "random");
        m_view->setSeedMode("seed", "fixed");
        QCOMPARE(m_view->valueOf("seed"), shown);
    }

    void aFixedSeedTheWorkflowHoldsNoNumberForGetsOneAtOnce() {
        // The block's default may be the engine's own: Fixed must still show a number, never an object.
        QJsonObject json = workflow();
        QJsonObject app = json.value("app").toObject();
        QJsonArray inputs = app.value("inputs").toArray();
        QJsonObject seed = inputs.at(1).toObject();
        seed.insert("default", QJsonObject{{"auto", "random"}});
        inputs.replace(1, seed);
        app.insert("inputs", inputs);
        json.insert("app", app);
        m_view->show("a", load(json), {});
        m_view->setSeedMode("seed", "fixed");
        QVERIFY(m_view->valueOf("seed").isValid());
        QVERIFY(m_view->valueOf("seed").canConvert<double>() && m_view->valueOf("seed").typeId() != QMetaType::QVariantMap);
    }

    void clearingTheResultEmptiesTheScreenOnlyWhenNoRunGoes() {
        m_view->show("a", load(workflow()), {});
        QVERIFY(!m_view->canClear());
        m_view->run();
        m_view->clearResults(); // a run goes
        QCOMPARE(m_view->status(), QStringLiteral("waiting"));
        m_view->onEvent("run1", evt("run.started"));
        m_view->onEvent("run1", saved("save", "a.png"));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_view->results().size(), 1);
        QVERIFY(m_view->canClear());
        QSignalSpy changed(m_view.get(), &AppView::runChanged);
        m_view->clearResults();
        QCOMPARE(changed.count(), 1);
        QVERIFY(m_view->results().isEmpty());
        QVERIFY(m_view->details().isEmpty());
        QVERIFY(m_view->error().isEmpty());
        QCOMPARE(m_view->status(), QStringLiteral("idle"));
        QVERIFY(!m_view->canClear());
        QVERIFY(m_view->canRun()); // the form is as it was
    }

    void aPictureTakenIntoTheInputsFolderIsWhatTheRunBindsAndSamplesAreOffered() {
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkpath("inputs"));
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(folder.filePath("inputs/sample.png")));
        const QString file = folder.filePath("app.fugue");
        m_view->show("a", load(mediaWorkflow()), file);
        QCOMPARE(field("image").value("kind"), QStringLiteral("image"));
        QCOMPARE(field("image").value("samples").toList().size(), 1); // the template's own
        QCOMPARE(m_view->fileUrl("inputs/sample.png"), QUrl::fromLocalFile(folder.filePath("inputs/sample.png")));

        // A picture from elsewhere (a drop, the file picker) is copied in, the same bytes once.
        QTemporaryDir other;
        const QString dropped = other.filePath("dropped.png");
        QVERIFY(QImage(16, 12, QImage::Format_RGB32).save(dropped));
        QSignalSpy told(m_view.get(), &AppView::valueChanged);
        QVERIFY(m_view->setImageFiles("image", {QUrl::fromLocalFile(dropped)}).isEmpty());
        const QString stored = m_view->valueOf("image").toString();
        QVERIFY(stored.startsWith(inputsDir() + "/"));
        QVERIFY(QFileInfo(stored).isFile());
        QCOMPARE(told.count(), 1);
        QVERIFY(m_view->setImageFiles("image", {QUrl::fromLocalFile(dropped)}).isEmpty());
        QCOMPARE(m_view->valueOf("image").toString(), stored);
        QVERIFY(!m_view->recentImages().isEmpty());
        QVERIFY(!m_view->setImageFiles("image", {QUrl("https://example.com/a.png")}).isEmpty()); // a refusal says why

        // A paste of pixels is stored as a PNG.
        QMimeData mime;
        mime.setImageData(QImage(4, 4, QImage::Format_RGB32));
        QVERIFY(m_view->setImageFromMime("image", mime).isEmpty());
        QVERIFY(m_view->valueOf("image").toString() != stored);

        // The strip's pictures: the samples and the inputs folder's, never an arbitrary file.
        m_view->useImage("image", dropped);
        QVERIFY(m_view->valueOf("image").toString() != dropped);
        m_view->useImage("image", folder.filePath("inputs/sample.png"));
        QCOMPARE(m_view->valueOf("image").toString(), QFileInfo(folder.filePath("inputs/sample.png")).canonicalFilePath());
        m_view->run();
        QCOMPARE(inputOf(m_started.first().document, "load", "file"), QJsonValue(m_view->valueOf("image").toString()));
    }

    void aMaskIsPaintedOverTheChosenPictureAndItsFileIsWhatTheRunBinds() {
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkpath("inputs"));
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(folder.filePath("inputs/sample.png")));
        m_view->show("a", load(mediaWorkflow()), folder.filePath("app.fugue"));
        const QVariantMap picture = m_view->maskPicture(); // the workflow's own picture, relative to its file
        QCOMPARE(picture.value("width").toInt(), 8);
        QCOMPARE(picture.value("height").toInt(), 6);
        m_view->setValue("mask", "/in/mask.png");
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "load", "mask"), QJsonValue("/in/mask.png"));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->setValue("mask", ""); // Clear: no mask, not the workflow's
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "load", "mask"), QJsonValue(""));
        m_view->show("b", load(mediaWorkflow()), {}); // a picture with no file to resolve it against has none to paint on
        QVERIFY(m_view->maskPicture().isEmpty());
    }

    void aModelIsPickedFromTheFilesOnDiskWithItsHashAndBoundAsAHandle() {
        QTemporaryDir models;
        QTemporaryDir state;
        const QString type = QStringLiteral("fugue/upscale-model");
        ModelLibrary library({models.path()}, &m_registry);
        const QString folder = library.folders(*PortType::parse(type)).value(0);
        QVERIFY(QDir().mkpath(folder));
        const QString file = folder + "/x4.pth";
        QFile weights(file);
        QVERIFY(weights.open(QIODevice::WriteOnly));
        weights.write("weights");
        weights.close();
        auto hashes = std::make_shared<FileHashes>(state.filePath("hashes.json"));
        hashes->remember(file, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        AppModels *store = m_view->modelStore();
        store->setLibrary(library);
        store->setHashes(hashes);
        store->setCatalog(ModelCatalog::parse(R"({"models": []})", "test"));
        store->setSource([](const QString &, const QStringList &, std::function<void(QList<ListedModel>)> done) { done({}); });

        m_view->show("a", load(mediaWorkflow()), {});
        QCOMPARE(field("model").value("kind"), QStringLiteral("model"));
        QCOMPARE(field("model").value("resource"), type);
        m_view->requestModels("model");
        const QVariantList listed = m_view->models("model");
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().toMap().value("path").toString(), file);
        m_view->pickModelFile("model", file); // its hash is remembered: no read
        QVERIFY(m_view->waitingModels().isEmpty());
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), QStringLiteral("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        m_view->run();
        const QJsonObject handle = inputOf(m_started.first().document, "up", "model").toObject();
        QCOMPARE(handle.value("resource").toString(), type);
        QCOMPARE(handle.value("hash").toString(), QStringLiteral("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        // A handle a provider lists is picked as it is.
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->pickModel("model", {{"resource", type}, {"name", "served"}});
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "up", "model").toObject().value("name").toString(), QStringLiteral("served"));
    }

    // A hash read on the pool answers the tab and the field that asked, even when another tab is shown by then.
    void aModelReadAfterATabSwitchGoesToTheTabThatAskedForIt() {
        QTemporaryDir models;
        QTemporaryDir state;
        const QString type = QStringLiteral("fugue/upscale-model");
        ModelLibrary library({models.path()}, &m_registry);
        const QString folder = library.folders(*PortType::parse(type)).value(0);
        QVERIFY(QDir().mkpath(folder));
        const QString file = folder + "/x4.pth";
        QFile weights(file);
        QVERIFY(weights.open(QIODevice::WriteOnly));
        weights.write("weights");
        weights.close();
        AppModels *store = m_view->modelStore();
        store->setLibrary(library);
        store->setHashes(std::make_shared<FileHashes>(state.filePath("hashes.json"))); // nothing remembered: a read
        store->setCatalog(ModelCatalog::parse(R"({"models": []})", "test"));

        QSignalSpy answered(store, &AppModels::hashed);
        m_view->show("a", load(mediaWorkflow()), {});
        m_view->pickModelFile("model", file);
        QCOMPARE(m_view->waitingModels(), QStringList{"model"});
        m_view->show("b", load(mediaWorkflow()), {});
        QVERIFY(m_view->waitingModels().isEmpty()); // b asked for nothing
        QVERIFY(answered.count() || answered.wait());
        QVERIFY(m_view->valueOf("model").toMap().value("hash").toString().isEmpty()); // not b's
        m_view->show("a", load(mediaWorkflow()), {});
        QVERIFY(m_view->waitingModels().isEmpty());
        QVERIFY(m_view->valueOf("model").toMap().value("hash").toString().startsWith("sha256:"));
    }
};

QTEST_GUILESS_MAIN(TestAppView)
#include "test_app_view.moc"
