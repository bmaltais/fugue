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
#include "apps/app_takes.h"
#include "engine/port_type.h"
#include "engine/run_folder.h"
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

// An Expand-like app: a picture and the room added on each side, with `compare` naming them (the right side is left out of the block).
QJsonObject expandWorkflow(bool withCompare = true) {
    QJsonObject json = QJsonDocument::fromJson(R"({
      "fugue": 1,
      "app": {"format": 1, "title": "Expand",
              "inputs": [{"role": "image", "targets": ["load.file"]},
                         {"name": "left", "targets": ["pad.left"], "min": 0, "max": 1024},
                         {"name": "top", "targets": ["pad.top"], "min": 0, "max": 1024},
                         {"name": "bottom", "targets": ["pad.bottom"], "min": 0, "max": 1024}],
              "outputs": [{"node": "save"}],
              "compare": {"input": "image", "left": "left", "top": "top", "bottom": "bottom"}},
      "graph": {"nodes": {
        "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "inputs/sample.png"}},
        "pad": {"type": "fugue/pad", "version": "1.0.1", "inputs": {"left": 0, "top": 0, "right": 0, "bottom": 16}},
        "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
      }}
    })").object();
    if (!withCompare) {
        QJsonObject app = json.value("app").toObject();
        app.remove("compare");
        json.insert("app", app);
    }
    return json;
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

    // #395, #581: where the app runs is the window's paired verdict, given as hosts or as a silent host; a tab shown again clears it, and a
    // service off this machine (#404) takes the place of both.
    void theFormSaysWhereItRunsOrThatTheHostIsNotAnswering() {
        QSignalSpy changed(m_view.get(), &AppView::runsOnChanged);
        m_view->show("a", load(workflow()), {});
        QVERIFY(m_view->runsOn().isEmpty() && m_view->silentHost().isEmpty());

        m_view->setPairedHosts("gx10, gx11", {});
        QCOMPARE(m_view->runsOn(), QString("gx10, gx11"));
        QCOMPARE(changed.size(), 1);
        m_view->setPairedHosts("gx10, gx11", {}); // nothing changed
        QCOMPARE(changed.size(), 1);
        m_view->setPairedHosts({}, "gx10"); // the host went quiet: no host is named
        QVERIFY(m_view->runsOn().isEmpty());
        QCOMPARE(m_view->silentHost(), QString("gx10"));
        m_view->setOffMachine("Cloud (api.example.com)"); // a service off this machine is the one line
        QCOMPARE(m_view->runsOn(), QString("Cloud (api.example.com)"));
        QVERIFY(m_view->silentHost().isEmpty());
        QCOMPARE(changed.size(), 3);
        m_view->setOffMachine("Cloud (api.example.com)"); // nothing changed
        QCOMPARE(changed.size(), 3);
        m_view->setOffMachine({});
        QCOMPARE(m_view->silentHost(), QString("gx10"));

        m_view->show("a", load(workflow()), {}); // the same tab again keeps it
        QCOMPARE(m_view->silentHost(), QString("gx10"));
        m_view->show("b", load(workflow()), {}); // another tab: not its host
        QVERIFY(m_view->runsOn().isEmpty() && m_view->silentHost().isEmpty());
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

    // Every free text is a wrapping box (#567): a prompt starts three lines tall; another text one line tall; newlines only
    // where the target's widget is multiline (a prompt role, a Text Template slot), never in a line widget.
    void aTextFieldWrapsAndTakesNewlinesOnlyWhereItsTargetDoes() {
        const QJsonObject json = QJsonDocument::fromJson(R"({
          "fugue": 1,
          "app": {"format": 1, "title": "Text",
                  "inputs": [{"role": "prompt", "targets": ["encode.prompt"]},
                             {"name": "hero", "targets": ["tpl.a"]},
                             {"name": "title", "targets": ["save.name"]}],
                  "outputs": [{"node": "save"}]},
          "graph": {"nodes": {
            "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "p"}},
            "tpl": {"type": "fugue/text-template", "version": "1.0.0", "inputs": {"a": "a long hero"}},
            "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"name": "out"}}
          }}
        })").object();
        m_view->show("a", load(json), {});
        QVERIFY(m_view->shown());
        QCOMPARE(field("prompt").value("minLines").toInt(), 3);
        QVERIFY(field("prompt").value("multiline").toBool());
        QCOMPARE(field("hero").value("minLines").toInt(), 1);
        QVERIFY(field("hero").value("multiline").toBool());
        QCOMPARE(field("title").value("minLines").toInt(), 1);
        QVERIFY(!field("title").value("multiline").toBool());
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
        // The form draws the number (in the range) and binds it, so the take knows it; the mode stays Random for the next run.
        const QJsonValue drawn = inputOf(m_started.last().document, "sample", "seed");
        QVERIFY(drawn.isDouble() && drawn.toInt() >= 0 && drawn.toInt() <= 10);
        QCOMPARE(m_view->seedMode("seed"), QStringLiteral("random"));
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

    void clearingEmptiesTheScreenAndTheStripOnlyWhenNoRunGoes() {
        m_view->show("a", load(workflow()), {});
        QVERIFY(!m_view->canClear());
        m_view->run();
        m_view->clearResults(); // a run goes
        QCOMPARE(m_view->status(), QStringLiteral("waiting"));
        m_view->onEvent("run1", evt("run.started"));
        m_view->onEvent("run1", saved("save", "a.png"));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_view->results().size(), 1);
        QCOMPARE(m_view->takes().size(), 1);
        QVERIFY(m_view->canClear());
        QSignalSpy changed(m_view.get(), &AppView::runChanged);
        m_view->clearResults();
        QVERIFY(m_view->takes().isEmpty()); // the strip too (owner, 2026-11-25)
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

    // An app opens ready: the picture the workflow holds is the form's first value and the first sample, and Use sample brings it
    // back after another was chosen.
    void anAppOpensOnItsSampleAndUseSampleBringsItBack() {
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkpath("inputs"));
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(folder.filePath("inputs/sample.png")));
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(folder.filePath("inputs/other.png")));
        m_view->show("a", load(mediaWorkflow()), folder.filePath("app.fugue"));
        const QString sample = QFileInfo(folder.filePath("inputs/sample.png")).canonicalFilePath();
        QCOMPARE(field("image").value("sample").toString(), sample);
        QCOMPARE(m_view->fileUrl(m_view->valueOf("image").toString()), QUrl::fromLocalFile(sample)); // the well shows it
        QCOMPARE(field("image").value("samples").toList().first().toMap().value("path").toString(), sample); // listed first
        m_view->useImage("image", folder.filePath("inputs/other.png"));
        QVERIFY(m_view->valueOf("image").toString() != sample);
        m_view->useSample("image");
        QCOMPARE(m_view->valueOf("image").toString(), sample);
        m_view->run();
        QCOMPARE(inputOf(m_started.first().document, "load", "file"), QJsonValue(sample));
    }

    // #484: an optional picture starts empty and says so; left empty its branch is out of the run, filled it is in.
    void anOptionalPictureStartsEmptyIsHintedAndSwitchesItsBranch() {
        QJsonObject json = mediaWorkflow();
        QJsonObject block = json.value("app").toObject();
        QJsonArray inputs = block.value("inputs").toArray();
        inputs.append(QJsonObject{{"role", "image"}, {"name", "extra"}, {"targets", QJsonArray{"load2.file"}}, {"optional", true}});
        block.insert("inputs", inputs);
        json.insert("app", block);
        QJsonObject nodes = json.value("graph").toObject().value("nodes").toObject();
        nodes.insert("load2", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.1.0"}, {"mode", "bypass"}, {"inputs", QJsonObject{{"file", "inputs/sample.png"}}}});
        json.insert("graph", QJsonObject{{"nodes", nodes}});
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkpath("inputs"));
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(folder.filePath("inputs/sample.png")));
        m_view->show("a", load(json), folder.filePath("app.fugue"));
        QCOMPARE(field("extra").value("hint").toString(), QStringLiteral("Optional"));
        QVERIFY(field("extra").value("optional").toBool());
        QVERIFY(field("extra").value("sample").toString().isEmpty()); // no sample needed, and the workflow's file is not one
        QVERIFY(!m_view->valueOf("extra").isValid() || m_view->valueOf("extra").toString().isEmpty());
        QVERIFY(field("image").value("hint").toString().isEmpty()); // a required picture carries no hint
        QVERIFY(m_view->canRun());
        const QString first = m_view->run();
        QCOMPARE(m_started.last().document.graph.nodes.value("load2").mode, NodeMode::Bypass);
        m_view->onEvent(first, evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_started.last().document.graph.nodes.value("load").mode, NodeMode::Normal);
        m_view->setValue("extra", folder.filePath("inputs/sample.png"));
        m_view->run();
        QCOMPARE(m_started.last().document.graph.nodes.value("load2").mode, NodeMode::Normal);
        QVERIFY(!inputOf(m_started.last().document, "load2", "file").toString().isEmpty());
    }

    // Takes (#389): each run is one, newest first, with the inputs it had; the stage shows the newest that made something or the one
    // picked; Reuse inputs puts a take's form back; a run cancelled with nothing made leaves none.
    void everyRunIsATakeWithItsInputsAndCanBeShownAndReused() {
        m_view->show("a", load(workflow()), {});
        QCOMPARE(m_view->takeIndex(), -1);
        QVERIFY(m_view->takes().isEmpty());
        m_view->setValue("prompt", "a red fox");
        m_view->setValue("steps", 3);
        m_view->run();
        QCOMPARE(m_view->takes().size(), 1);
        QVERIFY(m_view->takes().first().toMap().value("pending").toBool());
        m_view->onEvent("run1", evt("run.started"));
        m_view->onEvent("run1", saved("save", "one.png"));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QVERIFY(!m_view->takes().first().toMap().value("pending").toBool());
        QCOMPARE(m_view->takes().first().toMap().value("url").toString(), QStringLiteral("file:///t/one.png"));
        QVERIFY(m_view->shownInputs().contains("a red fox"));
        QVERIFY(m_view->shownInputs().contains("Steps 3") || m_view->shownInputs().contains("steps 3", Qt::CaseInsensitive));

        m_view->setValue("prompt", "a blue fox");
        m_view->run();
        // The new run has made nothing: the stage keeps the earlier take, the strip has the pending one first.
        QCOMPARE(m_view->takes().size(), 2);
        QVERIFY(m_view->takes().first().toMap().value("pending").toBool());
        QCOMPARE(m_view->takeIndex(), 1);
        QCOMPARE(m_view->shownResults().size(), 1);
        m_view->onEvent("run2", saved("save", "two.png"));
        m_view->onEvent("run2", evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_view->takeIndex(), 0);
        m_view->onEvent("run2", evt("run.done", {{"status", "ok"}}));
        QCOMPARE(m_view->shownResults().first().toMap().value("full").toString(), QStringLiteral("file:///t/two.png"));

        // Pick the older one: its picture and its words; the newest picked follows new runs again.
        m_view->selectTake(1);
        QCOMPARE(m_view->takeIndex(), 1);
        QCOMPARE(m_view->shownResults().first().toMap().value("full").toString(), QStringLiteral("file:///t/one.png"));
        QVERIFY(m_view->shownInputs().contains("red fox"));
        m_view->reuseInputs(1);
        QCOMPARE(m_view->valueOf("prompt").toString(), QStringLiteral("a red fox"));
        const QString again = m_view->runAgain(1);
        QVERIFY(!again.isEmpty());
        QCOMPARE(inputOf(m_started.last().document, "encode", "prompt"), QJsonValue("a red fox"));
        QCOMPARE(m_view->takes().size(), 3);
        QCOMPARE(m_view->takeIndex(), 1); // a new run is the one to look at, once it makes something; until then the last made
        m_view->cancel();
        m_view->onEvent(again, evt("run.done", {{"status", "cancelled"}}));
        QCOMPARE(m_view->takes().size(), 2); // nothing made, no take
    }

    // Compare in the App view (the block's `compare`): where the input picture sits in the result, from the take's own values.
    void theInputSitsInTheResultWhereTheTakesPaddingPutsIt() {
        QTemporaryDir folder;
        const QString picture = folder.filePath("square.png");
        QVERIFY(QImage(100, 100, QImage::Format_RGB32).save(picture));
        m_view->show("a", load(expandWorkflow()), {});
        QCOMPARE(m_view->inputPlacement(100, 300), QRectF()); // no take yet
        m_view->setValue("image", picture);
        m_view->setValue("top", 100);
        m_view->setValue("bottom", 100);
        m_view->run();
        m_view->onEvent("run1", saved("save", "one.png"));
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        // The square whole, the new rows above and below it; a side left out of the block is none, one left alone is the workflow's.
        QCOMPARE(m_view->inputPlacement(100, 300), QRectF(0, 100, 100, 100));
        // A result at another scale: the same place in its pixels.
        QCOMPARE(m_view->inputPlacement(50, 150), QRectF(0, 50, 50, 50));
        // The width unknown: the height says the scale.
        QCOMPARE(m_view->inputPlacement(0, 150), QRectF(0, 50, 50, 50));
        QCOMPARE(m_view->inputPlacement(0, 0), QRectF());
        // The take is what was run: the form changing after it moves nothing, B included (the picture it ran with).
        QCOMPARE(m_view->takeInputPicture().value("width").toInt(), 100);
        QVERIFY(QImage(40, 30, QImage::Format_RGB32).save(folder.filePath("other.png")));
        m_view->setValue("image", folder.filePath("other.png"));
        QCOMPARE(m_view->takeInputPicture().value("url").toUrl(), QUrl::fromLocalFile(picture));
        QCOMPARE(m_view->takeInputPicture().value("height").toInt(), 100);
        m_view->setValue("image", picture);
        m_view->setValue("top", 0);
        m_view->setValue("left", 100);
        QCOMPARE(m_view->inputPlacement(100, 300), QRectF(0, 100, 100, 100));

        // A take that left a side alone has the workflow's value for it (a bottom of 16 here).
        m_view->show("b", load(expandWorkflow()), {});
        m_view->setValue("image", picture);
        m_view->run();
        m_view->onEvent("run2", saved("save", "two.png"));
        QCOMPARE(m_view->inputPlacement(100, 116), QRectF(0, 0, 100, 100));
    }

    void anAppWithNoCompareBlockPlacesNothing() {
        QTemporaryDir folder;
        const QString picture = folder.filePath("square.png");
        QVERIFY(QImage(100, 100, QImage::Format_RGB32).save(picture));
        m_view->show("a", load(expandWorkflow(false)), {});
        m_view->setValue("image", picture);
        m_view->setValue("top", 100);
        m_view->run();
        m_view->onEvent("run1", saved("save", "one.png"));
        QCOMPARE(m_view->inputPlacement(100, 300), QRectF());
    }

    // --- takes that outlive the window (#389) ---

    // What RunService does for a run, on disk: its folder with the bound copy and its end.
    static void recordRun(const QString &root, const QString &id, const RunRequest &request, RunStatus status = RunStatus::Ok) {
        const Result<RunFolder> folder = RunFolder::create(root, id);
        QVERIFY(folder.ok());
        QVERIFY(folder->writeWorkflow(request.document.toJson()).isEmpty());
        QVERIFY(folder->queued(request.workflow));
        folder->finish(status, {}, QDateTime::currentDateTimeUtc());
    }

    void takesComeBackAfterARestartAndRunAgainMakesTheSamePicture() {
        QTemporaryDir runs;
        const QString file = runs.filePath("wf.fugue");
        const QString first = "20261125-000001-000-aaaa", second = "20261125-000002-000-bbbb";
        // Two runs, the seed left to Random.
        m_view->show("a", load(workflow()), file);
        m_view->setValue("prompt", "a red fox");
        m_view->run();
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->setValue("prompt", "a blue fox");
        m_view->run();
        QCOMPARE(m_started.size(), 2);
        QVERIFY(inputOf(m_started.at(0).document, "sample", "seed").isDouble());
        recordRun(runs.path(), first, m_started.at(0));
        recordRun(runs.path(), second, m_started.at(1));
        // A run of the graph (no form record) and one of another file are not takes of this app.
        RunRequest graphRun = m_started.at(1);
        graphRun.document.extra.remove("app_take");
        recordRun(runs.path(), "20261125-000003-000-cccc", graphRun);
        RunRequest other = m_started.at(1);
        other.document.extra.insert("app_take", app::takeRecord(runs.filePath("other.fugue"), {}, {}));
        recordRun(runs.path(), "20261125-000004-000-dddd", other);

        // A restart: a new view over the same history.
        QStringList replayed;
        const QJsonValue firstSeed = inputOf(m_started.at(0).document, "sample", "seed");
        m_started.clear();
        AppView again([this](const RunRequest &request) { m_started << request; return QStringLiteral("again%1").arg(m_started.size()); },
                      [](const QString &) {}, &m_registry);
        again.setHistory(runs.path(), [&replayed](const QString &run) { replayed << run; });
        again.show("a", load(workflow()), file);
        QCOMPARE(again.takes().size(), 2);
        QCOMPARE(again.takes().at(0).toMap().value("run").toString(), second); // newest first
        QVERIFY(again.takes().at(0).toMap().value("persisted").toBool());
        QVERIFY(again.takes().at(0).toMap().value("line").toString().contains("a blue fox"));
        QVERIFY(again.takes().at(1).toMap().value("line").toString().contains("a red fox"));
        // Only the newest take's results are asked for at once; the older one when it is drawn.
        QCOMPARE(replayed, QStringList{second});
        again.ensureTake(1);
        again.ensureTake(1);
        QCOMPARE(replayed, (QStringList{second, first})); // once each
        again.onEvent(second, saved("save", "two.png"));
        again.onEvent(second, evt("run.done", {{"status", "ok"}}));
        again.onEvent(first, saved("save", "one.png"));
        again.onEvent(first, evt("run.done", {{"status", "ok"}}));
        QCOMPARE(again.takes().at(0).toMap().value("url").toString(), QStringLiteral("file:///t/two.png"));
        QCOMPARE(again.takes().at(1).toMap().value("url").toString(), QStringLiteral("file:///t/one.png"));
        QCOMPARE(again.shownResults().first().toMap().value("full").toString(), QStringLiteral("file:///t/two.png"));
        // Run again with the older take: its words and the seed its run used, so the same inputs make the same picture.
        const QString run = again.runAgain(1);
        QVERIFY(!run.isEmpty());
        QCOMPARE(inputOf(m_started.last().document, "encode", "prompt"), QJsonValue("a red fox"));
        QCOMPARE(inputOf(m_started.last().document, "sample", "seed"), firstSeed);
    }

    // Clear takes the strip's takes away for good: a restart does not read them back, and a run after the clear is a take again.
    void clearedTakesStayAwayAfterARestart() {
        QTemporaryDir runs;
        const QString file = runs.filePath("wf.fugue");
        m_view->show("a", load(workflow()), file);
        m_view->run();
        recordRun(runs.path(), "20261125-000001-000-aaaa", m_started.at(0));
        const auto restart = [&] {
            auto view = std::make_unique<AppView>([this](const RunRequest &request) { m_started << request; return QStringLiteral("r%1").arg(m_started.size()); },
                                                  [](const QString &) {}, &m_registry);
            view->setHistory(runs.path(), [](const QString &) {});
            view->show("a", load(workflow()), file);
            return view;
        };
        auto first = restart();
        QCOMPARE(first->takes().size(), 1);
        QVERIFY(first->canClear()); // takes on the strip, nothing on the stage yet
        first->clearResults();
        QVERIFY(first->takes().isEmpty());
        QVERIFY(QFileInfo::exists(runs.filePath("20261125-000001-000-aaaa"))); // the run folder stays (History, Results)
        QVERIFY(restart()->takes().isEmpty());
        recordRun(runs.path(), "20261125-000002-000-bbbb", m_started.at(0));
        QCOMPARE(restart()->takes().size(), 1);
    }

    void anAppWithNoFileKeepsTheSessionsTakesOnly() {
        QTemporaryDir runs;
        m_view->setHistory(runs.path(), [](const QString &) {});
        m_view->show("a", load(workflow()), {});
        m_view->run();
        QCOMPARE(m_view->takes().size(), 1);
        QVERIFY(!m_started.last().document.extra.contains("app_take")); // no file, no key: nothing to find it by
        m_view->show("b", load(workflow()), {});
        QVERIFY(m_view->takes().isEmpty());
        // A template's tab has no file of its own: the template's workflow is the app's key.
        m_view->show("c", load(workflow()), {}, "/t/lib/restore");
        m_view->run();
        QCOMPARE(m_started.last().document.extra.value("app_take").toObject().value("app").toString(), QStringLiteral("/t/lib/restore/workflow.fugue"));
    }

    // --- Live (#471) ---

    void liveIsOffAtFirstAndArmsNothing() {
        m_view->setLiveDelay(10);
        m_view->show("a", load(workflow()), {});
        QCOMPARE(m_view->liveMode(), QStringLiteral("off"));
        m_view->setValue("prompt", "x");
        QVERIFY(!m_view->liveArmed());
        QTest::qWait(40);
        QVERIFY(m_started.isEmpty());
    }

    void anInputChangeQueuesExactlyOneRunAfterTheDebounce() {
        m_view->setLiveDelay(30);
        m_view->show("a", load(workflow()), {});
        m_view->setLiveMode("live");
        QVERIFY(m_started.isEmpty()); // switching it on runs nothing
        m_view->setValue("prompt", "a");
        m_view->setValue("prompt", "ab");
        m_view->setValue("prompt", "abc");
        QVERIFY(m_view->liveArmed());
        QVERIFY(m_started.isEmpty()); // still waiting for the form to be still
        QTRY_COMPARE(m_started.size(), 1);
        QTest::qWait(80);
        QCOMPARE(m_started.size(), 1);
        QCOMPARE(inputOf(m_started.last().document, "encode", "prompt"), QJsonValue("abc"));
        // The store's own changes (a seed advanced after the run) are not edits.
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QTest::qWait(80);
        QCOMPARE(m_started.size(), 1);
    }

    // The file chosen for a missing model (or put back) is an input of the run: Live runs again, as for a field edit.
    void choosingOrUndoingAFileInPlaceOfAMissingModelRunsLiveAgain() {
        const QString type = QStringLiteral("fugue/upscale-model");
        const QString missing = QStringLiteral("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
        const QJsonObject wanted{{"resource", type}, {"hash", missing}, {"name", "x4plus.pth"}};
        const QJsonObject replacement{{"resource", type}, {"hash", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {"name", "x4.pth"}};
        OnDiskUse use{wanted, {}};
        use.match.name = QStringLiteral("x4.pth");
        m_view->setLiveDelay(10);
        m_view->show("a", load(workflow()), {});
        m_view->setLiveMode("live");
        m_view->takeUses("a", {use}, {{missing, replacement}});
        QVERIFY(m_view->liveArmed());
        QTRY_COMPARE(m_started.size(), 1);
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->undoUse(missing);
        QVERIFY(m_view->liveArmed());
        QTRY_COMPARE(m_started.size(), 2);
    }

    void aSupersededWaitingRunIsCancelledAndARunningOneIsNot() {
        m_view->setLiveDelay(10);
        m_view->show("a", load(workflow()), {});
        m_view->setLiveMode("live");
        m_view->setValue("prompt", "one");
        QTRY_COMPARE(m_started.size(), 1); // run1 waits in the queue
        m_view->setValue("prompt", "two");
        QTRY_COMPARE(m_started.size(), 2);
        QCOMPARE(m_cancelled, QStringList{"run1"});
        QCOMPARE(m_view->takes().size(), 1); // the superseded run left no take
        // A run that works is never cut: the owed run starts when it ends.
        m_view->onEvent("run2", evt("run.started"));
        m_view->setValue("prompt", "three");
        QTest::qWait(60);
        QCOMPARE(m_started.size(), 2);
        QCOMPARE(m_cancelled, QStringList{"run1"});
        m_view->onEvent("run2", evt("run.done", {{"status", "ok"}}));
        QTRY_COMPARE(m_started.size(), 3);
        QCOMPARE(inputOf(m_started.last().document, "encode", "prompt"), QJsonValue("three"));
    }

    void repeatRunsAgainWhenOneEndsAndCancelStopsIt() {
        m_view->show("a", load(workflow()), {});
        m_view->setLiveMode("repeat");
        QCOMPARE(m_started.size(), 1); // starts at once
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QTRY_COMPARE(m_started.size(), 2);
        // A new seed each time (the form draws it): Random keeps being Random.
        QCOMPARE(m_view->seedMode("seed"), QStringLiteral("random"));
        m_view->onEvent("run2", evt("run.done", {{"status", "failed"}})); // a failure ends the chain, the mode stays
        QTest::qWait(40);
        QCOMPARE(m_started.size(), 2);
        QCOMPARE(m_view->liveMode(), QStringLiteral("repeat"));
        m_view->run();
        m_view->cancel(); // the person said stop
        QCOMPARE(m_view->liveMode(), QStringLiteral("off"));
        m_view->onEvent("run3", evt("run.done", {{"status", "cancelled"}}));
        QTest::qWait(40);
        QCOMPARE(m_started.size(), 3);
    }

    // Repeat makes something new each run only through a seed (owner, 2026-11-25: Repeat on Upscale, which has none, filled the strip
    // with identical takes in a second, each from the cache). No seed: no Repeat. A Fixed seed: one run, then it waits until a seed varies.
    void repeatNeedsASeedThatVaries() {
        QJsonObject json = workflow();
        QJsonObject app = json.value("app").toObject();
        QJsonArray inputs = app.value("inputs").toArray();
        inputs.removeAt(1); // the seed
        app.insert("inputs", inputs);
        json.insert("app", app);
        m_view->show("a", load(json), {});
        QVERIFY(!m_view->canRepeat());
        m_view->setLiveMode("repeat");
        QCOMPARE(m_view->liveMode(), QStringLiteral("off"));
        QVERIFY(m_started.isEmpty());

        m_view->show("b", load(workflow()), {});
        QVERIFY(m_view->canRepeat());
        m_view->setSeedMode("seed", "fixed");
        m_view->setLiveMode("repeat");
        QCOMPARE(m_started.size(), 1); // the first run still goes
        QVERIFY(!m_view->repeatWaits());
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QTest::qWait(40);
        QCOMPARE(m_started.size(), 1); // the next would be the same picture
        QVERIFY(m_view->repeatWaits());
        m_view->setSeedMode("seed", "random");
        QCOMPARE(m_started.size(), 2); // a seed varies: Repeat goes on
        QVERIFY(!m_view->repeatWaits());
    }

    void aStepPictureShowsWhileTheRunGoesAndAResultCanBeSavedAndUsedAsAnInput() {
        QTemporaryDir folder;
        QVERIFY(QDir(folder.path()).mkpath("inputs"));
        const QString file = folder.filePath("made.png");
        QVERIFY(QImage(8, 6, QImage::Format_RGB32).save(file));
        m_view->show("a", load(mediaWorkflow()), folder.filePath("app.fugue"));
        m_view->run();
        m_view->onEvent("run1", evt("node.preview", {{"thumbnails", QJsonObject{{"preview", QJsonObject{{"url", "file:///t/step.png"}}}}}}));
        QCOMPARE(m_view->liveFrame(), QStringLiteral("file:///t/step.png"));
        QJsonObject done = saved("save", "made.png");
        QJsonObject image = done.value("thumbnails").toObject().value("image").toObject();
        image.insert("source", file);
        done.insert("thumbnails", QJsonObject{{"image", image}});
        m_view->onEvent("run1", done);
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        QVERIFY(m_view->liveFrame().isEmpty());

        const QString full = m_view->shownResults().first().toMap().value("full").toString();
        QVERIFY(m_view->saveResult(full, QUrl::fromLocalFile(folder.filePath("copy.png"))).isEmpty());
        QVERIFY(QFileInfo(folder.filePath("copy.png")).isFile());
        QVERIFY(!m_view->saveResult("file:///nowhere.png", QUrl::fromLocalFile(folder.filePath("x.png"))).isEmpty());
        QCOMPARE(m_view->resultPictures().size(), 1);
        QCOMPARE(m_view->imageKey(), QStringLiteral("image"));
        QVERIFY(m_view->useResultAsInput("image", full).isEmpty());
        QVERIFY(m_view->valueOf("image").toString().startsWith(inputsDir() + "/"));
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

    // #605: the same model is on disk in another file and the person chose it. A model field that holds the missing model takes the file
    // (so Reuse inputs and the takes know it); a model the form does not show is the tab's own override; Run binds both into the copy it
    // runs, the shown document is never changed, and Undo puts the template's file back.
    void aFileChosenInPlaceOfAMissingModelIsTheAppsOwnAndCanBeUndone() {
        const QString type = QStringLiteral("fugue/upscale-model");
        const QString missing = QStringLiteral("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
        const QString chosen = QStringLiteral("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        const QJsonObject wanted{{"resource", type}, {"hash", missing}, {"name", "x4plus.pth"}};
        const QJsonObject replacement{{"resource", type}, {"hash", chosen}, {"name", "x4.pth"}};
        const auto withMissing = [&](bool asInput) {
            QJsonObject json = mediaWorkflow();
            QJsonObject graph = json.value("graph").toObject(), nodes = graph.value("nodes").toObject(), up = nodes.value("up").toObject();
            up.insert("inputs", QJsonObject{{"model", wanted}});
            nodes.insert("up", up);
            graph.insert("nodes", nodes);
            json.insert("graph", graph);
            if (!asInput) {
                QJsonObject app = json.value("app").toObject();
                QJsonArray inputs;
                for (const QJsonValue input : app.value("inputs").toArray())
                    if (input.toObject().value("role").toString() != QLatin1String("model"))
                        inputs.append(input);
                app.insert("inputs", inputs);
                json.insert("app", app);
            }
            return json;
        };
        OnDiskUse use{wanted, {}};
        use.match.name = QStringLiteral("x4.pth");
        use.match.tier = QStringLiteral("bf16");
        const QList<OnDiskUse> chosenUses{use};
        const Uses resolved{{missing, replacement}};

        m_view->show("a", load(withMissing(true)), {});
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), missing);
        QSignalSpy changed(m_view.get(), &AppView::usesChanged);
        m_view->takeUses("a", chosenUses, resolved);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), chosen); // the field holds the file
        QCOMPARE(m_view->usesOf("a").value(missing), replacement);
        QCOMPARE(m_view->usesList().size(), 1);
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "up", "model").toObject().value("hash").toString(), chosen);
        m_view->onEvent("run1", evt("run.done", {{"status", "ok"}}));
        m_view->undoUse(missing);
        QVERIFY(m_view->usesOf("a").isEmpty());
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), missing); // back to the template's file

        // Not an input of the form: the override alone, bound into the run's copy.
        m_view->show("b", load(withMissing(false)), {});
        m_view->takeUses("b", chosenUses, resolved);
        QCOMPARE(m_view->usesOf("b").value(missing), replacement);
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "up", "model").toObject().value("hash").toString(), chosen);
        m_view->undoUse(missing);
        m_view->onEvent("run2", evt("run.done", {{"status", "ok"}}));
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "up", "model").toObject().value("hash").toString(), missing);
    }

    // The choice is the App view's: the graph is judged by the workflow's own models, so what the graph's readiness reads is empty while
    // the tab shows its graph. The lines follow the shown tab, and a forgotten one.
    void theChoiceIsTheAppViewsAndTheLinesFollowTheShownTab() {
        const QString type = QStringLiteral("fugue/upscale-model");
        const QString missing = QStringLiteral("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
        const QJsonObject wanted{{"resource", type}, {"hash", missing}, {"name", "x4plus.pth"}};
        const QJsonObject replacement{{"resource", type}, {"hash", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {"name", "x4.pth"}};
        OnDiskUse use{wanted, {}};
        use.match.name = QStringLiteral("x4.pth");
        m_view->show("a", load(mediaWorkflow()), {});
        m_view->takeUses("a", {use}, {{missing, replacement}});
        QCOMPARE(m_view->usesInForce("a").value(missing), replacement);
        QCOMPARE(m_view->usesList().size(), 1);
        m_view->openInGraph();
        QVERIFY(m_view->usesInForce("a").isEmpty());
        QVERIFY(!m_view->usesOf("a").isEmpty()); // kept for when the App view comes back
        m_view->showApp();
        QCOMPARE(m_view->usesInForce("a").value(missing), replacement);

        QSignalSpy changed(m_view.get(), &AppView::usesChanged);
        m_view->show("b", load(mediaWorkflow()), {});
        QVERIFY(changed.count() >= 1);
        QVERIFY(m_view->usesList().isEmpty()); // not a's line
        changed.clear();
        m_view->forget("b");
        QVERIFY(changed.count() >= 1);
    }

    // The file is chosen in tab A, and tab B is shown by the time its hash is read: A's model field still takes the file, so A shows it
    // when it comes back and its run (and the take it leaves) has it.
    void aFileChosenForATabThatIsNoLongerShownIsInItsFormWhenItComesBack() {
        const QString type = QStringLiteral("fugue/upscale-model");
        const QString missing = QStringLiteral("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
        const QString chosen = QStringLiteral("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        const QJsonObject wanted{{"resource", type}, {"hash", missing}, {"name", "x4plus.pth"}};
        const QJsonObject replacement{{"resource", type}, {"hash", chosen}, {"name", "x4.pth"}};
        QJsonObject json = mediaWorkflow();
        QJsonObject graph = json.value("graph").toObject(), nodes = graph.value("nodes").toObject(), up = nodes.value("up").toObject();
        up.insert("inputs", QJsonObject{{"model", wanted}});
        nodes.insert("up", up);
        graph.insert("nodes", nodes);
        json.insert("graph", graph);
        OnDiskUse use{wanted, {}};
        use.match.name = QStringLiteral("x4.pth");
        m_view->show("a", load(json), {});
        m_view->show("b", load(mediaWorkflow()), {});
        m_view->takeUses("a", {use}, {{missing, replacement}}); // A's file was read while B is shown
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), QString()); // B's form is not touched
        m_view->show("a", load(json), {});
        QCOMPARE(m_view->valueOf("model").toMap().value("hash").toString(), chosen);
        m_view->run();
        QCOMPARE(inputOf(m_started.last().document, "up", "model").toObject().value("hash").toString(), chosen);
        QCOMPARE(m_view->takes().size(), 1);
        QCOMPARE(m_view->values().value("model").toObject().value("hash").toString(), chosen); // what the take records and Reuse restores
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
