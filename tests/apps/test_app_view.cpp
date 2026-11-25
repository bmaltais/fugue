// The App view's store (#385): a workflow with an `app` block as a form, one Run that binds the form into a copy, the run's
// events as progress, results and errors, per tab, and the switch to the graph and back. A fake run (the events a RunService
// would send, fed in by hand); no window. Seam: apps/app_view.h.

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include "apps/app_view.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

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
        QCOMPARE(field("seed").value("kind"), QStringLiteral("number"));
        QVERIFY(!field("seed").value("value").isValid());
        QCOMPARE(field("steps").value("value").toInt(), 4);
        QCOMPARE(field("steps").value("max").toInt(), 8);
        QVERIFY(field("steps").value("advanced").toBool());
        QCOMPARE(field("quality").value("value"), QStringLiteral("Light"));
        QCOMPARE(field("quality").value("choices").toList().size(), 2);
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
};

QTEST_GUILESS_MAIN(TestAppView)
#include "test_app_view.moc"
