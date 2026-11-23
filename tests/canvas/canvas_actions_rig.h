#pragma once

// The window's run objects for the canvas actions' tests (test_canvas_actions, test_canvas_actions_window): RunService
// with a FakeProvider on the run thread (image.generate), CanvasRuns and CanvasActions, every folder in the test's own,
// and two actions of the test's own beside the bundled ones: Test fill (Canvas Region, Generate Image, Canvas Write: a
// fill the fake serves) and Broken (its block names a node the graph lacks).

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QtTest>

#include <memory>

#include "canvas_app.h"
#include "canvas/canvas_actions.h"
#include "engine/fake_provider.h"
#include "run/canvas_runs.h"
#include "run/run_service.h"

namespace actions_test {

using namespace canvas_test;
using namespace fugue;
using namespace fugue::mask;


// A fill the fake provider can serve: Canvas Region, Generate Image from the region (image.generate), Canvas Write.
inline const QByteArray kTestFill = R"({
  "fugue": 1,
  "action": {"format": 1, "name": "Test fill", "glyph": "sparkles", "selection": "required", "region": "region",
             "inputs": [{"role": "prompt", "targets": ["gen.prompt"]}, {"role": "seed", "targets": ["gen.seed"]}]},
  "graph": {"nodes": {
    "region": {"type": "fugue/canvas-region", "version": "1.2.0",
               "inputs": {"canvas": "", "revision": "head", "x": 0, "y": 0, "width": 16, "height": 16, "padding": 8, "scale": 1,
                          "max_megapixels": 1, "feather": 0, "multiple": 16}},
    "gen": {"type": "fugue/generate-image", "version": "1.1.0",
            "inputs": {"prompt": "old", "image": {"from": "region.image"}, "strength": 1, "seed": 0}},
    "write": {"type": "fugue/canvas-write", "version": "1.3.0",
              "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "gen.image"}, "stitch": {"from": "region.stitch"},
                         "blend": 0, "name": "Fill"}}
  }}
})";

// Test fill with Advanced inputs (slice 4): a Strength slider (default 0.9), a Style choice that stands for a Guidance
// number (Light 2, Default 5, Restyle 9), and a plain Steps input.
inline const QByteArray kTestAdvanced = QByteArray(kTestFill)
    .replace("Test fill", "Test advanced")
    .replace(R"({"role": "seed", "targets": ["gen.seed"]}])",
             R"({"role": "seed", "targets": ["gen.seed"]},
                {"role": "strength", "targets": ["gen.strength"], "default": 0.9, "min": 0.1, "max": 1, "section": "Advanced"},
                {"name": "style", "label": "Style", "targets": ["gen.guidance"], "choices": ["Light", "Default", "Restyle"],
                 "choice_values": [2, 5, 9], "default": "Default", "section": "Advanced"},
                {"name": "steps", "label": "Steps", "targets": ["gen.steps"], "section": "Advanced"}])");

// Its block names a node the graph lacks.
inline const QByteArray kBroken = R"({
  "fugue": 1,
  "action": {"name": "Broken", "inputs": [{"role": "prompt", "targets": ["nowhere.prompt"]}]},
  "graph": {"nodes": {
    "region": {"type": "fugue/canvas-region", "version": "1.2.0", "inputs": {"canvas": ""}},
    "write": {"type": "fugue/canvas-write", "version": "1.3.0", "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "region.image"}}}
  }}
})";

inline QByteArray png(int width, int height, QColor color = Qt::darkCyan) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

inline Description imageDescription() {
    OfferedModel model;
    model.handle = QJsonObject{{"resource", "fugue/image-model"}, {"name", "flux"}};
    model.status = ModelStatus::Ready;
    CapabilityOffer offer;
    offer.capability = QStringLiteral("image.generate");
    offer.models.append(model);
    Description description;
    description.engine = QStringLiteral("fake");
    description.capabilities.append(offer);
    return description;
}

inline MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

inline void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

// The window's run objects without a window: RunService with the fake, CanvasRuns, CanvasActions.
struct Rig {
    App app;
    std::shared_ptr<FakeProvider> fake;
    std::unique_ptr<RunService> service;
    std::unique_ptr<CanvasRuns> runs;
    std::unique_ptr<CanvasActions> actions;
    QString actionsDir, templatesDir;

    explicit Rig(const QByteArray &document = R"({"fugue": 1, "graph": {"nodes": {}}})", const Look &look = {}) : app(load(document), {}, look) {
        RunSetup setup;
        setup.addNodeTypes = [](Registry &registry) { nodes::addBuiltinNodeTypes(registry); };
        setup.addKernels = nodes::addBuiltinKernels;
        auto *self = this;
        setup.registerProviders = [self](Registry &registry) {
            if (!self->fake)
                self->fake = std::make_shared<FakeProvider>(imageDescription());
            ProviderInstance instance;
            instance.name = QStringLiteral("This machine");
            instance.type = QStringLiteral("fake");
            return registry.setProviders({ProviderEntry{instance, self->fake}});
        };
        const QString root = app.dir.path();
        setup.runsRoot = root + "/runs";
        setup.queueDir = root + "/queue";
        setup.lockPath = root + "/engine.lock";
        setup.cacheRoot = root + "/results";
        setup.hashesPath = root + "/hashes.json";
        setup.thumbnailDir = root + "/thumbnails";
        setup.collectGarbage = false;
        service = std::make_unique<RunService>(setup);
        runs = std::make_unique<CanvasRuns>(&app.model, &app.workflows, service.get(), &app.registry);
        actionsDir = root + "/actions";
        QDir().mkpath(actionsDir);
        writeFile(actionsDir + "/test-fill.fugue", kTestFill);
        writeFile(actionsDir + "/test-advanced.fugue", kTestAdvanced);
        writeFile(actionsDir + "/broken.fugue", kBroken);
        // The template library is the test's own (empty), never the repository's or the owner's.
        templatesDir = root + "/templates";
        QDir().mkpath(templatesDir);
        app.workflows.setTemplateRoots({{templatesDir, true}});
        actions = std::make_unique<CanvasActions>(service.get(), &app.registry, &app.workflows);
        actions->setRoots({CanvasActions::bundledRoot(), actionsDir});
        actions->setHistoryPath(root + "/state/canvas-prompts.json");
        actions->setSettingsPath(root + "/state/canvas-actions.json");
        app.runs = runs.get();
        app.canvasActions = actions.get();
    }
    ~Rig() {
        qDeleteAll(app.engine.rootObjects()); // the window goes before the runs it shows, as in main()
        actions.reset();
        runs.reset();
        service.reset(); // stops the run thread before the fake goes
    }
    QStringList jobs() {
        QStringList list;
        service->onRunThread([&] { list = fake ? fake->jobs() : QStringList(); });
        return list;
    }
    void onFake(const std::function<void(FakeProvider &)> &call) { service->onRunThread([&] { call(*fake); }); }
    // A template of the test's library: <id>/template.json and <id>/workflow.fugue; then the actions read again.
    void addTemplate(const QString &id, const QByteArray &workflow, const QString &title = QStringLiteral("A template")) {
        QDir().mkpath(templatesDir + "/" + id);
        writeFile(templatesDir + "/" + id + "/template.json", QByteArray(R"({"title": ")") + title.toUtf8() + R"(", "description": "d", "category": "x"})");
        writeFile(templatesDir + "/" + id + "/workflow.fugue", workflow);
        actions->refresh();
    }
    QVariantMap row(const QString &id) const {
        for (const QVariant &v : actions->actions())
            if (v.toMap().value("id") == id)
                return v.toMap();
        return {};
    }
};

inline QVariantMap rect(int x, int y, int w, int h) { return {{"x", x}, {"y", y}, {"width", w}, {"height", h}}; }

} // namespace actions_test
