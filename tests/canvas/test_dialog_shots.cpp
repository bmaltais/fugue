// Pictures of every dialog whose actions are a KitDialogButtons row (docs/ui-ergonomics.md 2026-11-27), for a person to look at:
// the row at the bottom right in GNOME's order, the default button focused with the keyboard's ring. Nothing runs without a folder:
//
//   FUGUE_SHOT_DIR=<dir> FUGUE_SHOT_THEME=catppuccin FUGUE_SHOT_TAG=dark build/test_dialog_shots
//   FUGUE_SHOT_DIR=<dir> FUGUE_SHOT_THEME=catppuccin-latte FUGUE_SHOT_TAG=light build/test_dialog_shots
//   FUGUE_SHOT_DIR=<dir> FUGUE_SHOT_THEME=catppuccin FUGUE_SHOT_TAG=rtl FUGUE_SHOT_RTL=1 build/test_dialog_shots
//
// Each picture is <dir>/<dialog>-<tag>.png. The Projects dialog's views are test_projects_window's pictures.

#include <QDir>
#include <QImage>
#include <QQmlComponent>
#include <QQmlPropertyMap>
#include <QQuickStyle>
#include <QtTest>

#include "canvas_app.h"
#include "engine/canvas_store.h"
#include "settings/model_setup.h"
#include "../test_paths.h"
#include "../window_grab.h"

using namespace canvas_test;
using namespace fugue;

namespace {

QString png(const QString &path) {
    QImage image(320, 200, QImage::Format_RGB32);
    image.fill(QColor("#4a7a5a"));
    image.save(path, "PNG");
    return path;
}

// A picture to crop and a canvas region with no canvas: the Crop and Region windows open on them.
QByteArray graphOf(const QString &file) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "%1"}},
    "crop": {"type": "fugue/crop", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "x": 20, "y": 10, "width": 120, "height": 90}},
    "region": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": ""}}
  }},
  "layout": {"nodes": {"load": {"pos": [0, 0]}, "crop": {"pos": [300, 0]}, "region": {"pos": [600, 0]}}, "view": {"center": [400, 200], "zoom": 1}}
})").arg(file).toUtf8();
}

} // namespace

class TestDialogShots : public QObject {
    Q_OBJECT

    QTemporaryDir files;
    QString out, tag;

    QObject *named(App &app, const char *name) const { return app.window->findChild<QObject *>(name); }
    void save(App &app, const QString &what) {
        QTest::qWait(350);
        QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3.png").arg(out, what, tag)));
    }
    // Opens a popup by its name (with `props` set first), saves it, closes it.
    // `overlay`: the popup's host is hidden here (the task bar without its actions), so it opens over the window instead.
    void popup(App &app, const char *name, const QString &file, const QVariantMap &props = {}, bool overlay = false) {
        QObject *box = nullptr;
        QTRY_VERIFY2((box = named(app, name)), name);
        if (overlay)
            box->setProperty("parent", QVariant::fromValue(app.window->contentItem()));
        for (auto it = props.begin(); it != props.end(); ++it)
            box->setProperty(it.key().toUtf8(), it.value());
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY2(box->property("opened").toBool(), name);
        save(app, file);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("visible").toBool());
    }
    // A settings page's inline question, made visible over whatever the page asks.
    void inlineQuestion(App &app, const QString &page, const char *name, const QString &file) {
        QObject *panel = named(app, "settingsPanel");
        panel->setProperty("page", page);
        QQuickItem *ask = nullptr;
        QTRY_VERIFY((ask = app.window->findChild<QQuickItem *>(name)));
        ask->setVisible(true);
        save(app, file);
        ask->setVisible(false);
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("dark"));
        if (!out.isEmpty())
            QDir().mkpath(out);
        qputenv("FUGUE_TEMPLATES", (fugueSourceDir() + "/templates").toLocal8Bit());
        // The Canvas view's canvas, in a store of the test's own.
        for (const char *variable : {"FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR"})
            qunsetenv(variable);
        for (const char *variable : {"FUGUE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
            QDir().mkpath(files.filePath(variable));
            qputenv(variable, files.filePath(variable).toLocal8Bit());
        }
        mask::PaintDocument canvas(256, 256);
        canvas.addLayer(mask::LayerKind::Paint, "Base");
        QVERIFY(canvas::CanvasStore::forSession().create("Sketch", canvas, "new"));
    }

    void shots() {
        if (out.isEmpty())
            QSKIP("Set FUGUE_SHOT_DIR to a folder to write the pictures there.");
        App app(load(graphOf(png(files.filePath("a.png")))), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("catppuccin")), 8});
        QVERIFY(app.open());
        if (qEnvironmentVariable("FUGUE_SHOT_RTL") == "1") // after open(): the translator sets the language's direction
            QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        app.window->resize(1000, 720);
        QTest::mouseMove(app.window, QPoint(990, 700));
        QTest::qWait(300);

        // The main window's questions.
        popup(app, "quitAsk", "quit");
        popup(app, "closeAsk", "close-tab", {{"title", "fox-pass.fugue"}});
        popup(app, "closeAllAsk", "close-all", {{"unsavedTabs", 3}});
        popup(app, "trustAsk", "trust");
        popup(app, "recoverAsk", "recover");
        popup(app, "remoteConsentAsk", "remote-consent");
        {
            QVERIFY(app.commands.trigger("panel.nodes")); // the question lives in the Node library panel
            QObject *group = nullptr;
            QTRY_VERIFY((group = named(app, "deleteGroupQuestion")));
            QVERIFY(QMetaObject::invokeMethod(group, "ask", Q_ARG(QVariant, "local/fox"), Q_ARG(QVariant, "Fox pass")));
            QTRY_VERIFY(group->property("opened").toBool());
            save(app, "delete-group");
            QVERIFY(QMetaObject::invokeMethod(group, "close"));
            QTRY_VERIFY(!group->property("visible").toBool());
            QVERIFY(app.commands.trigger("panel.nodes"));
        }
        {
            QQmlComponent component(&app.engine, QUrl("qrc:/canvas/widgets/ModelDownloadAsk.qml"));
            const QVariantMap info{{"id", "fox"}, {"title", "Fox XL"}, {"description", "A picture model."}, {"licenseLine", "Apache-2.0"},
                                   {"licenseUrl", "https://example.org/license"}, {"commercial", true}, {"author", "Fox Labs"},
                                   {"sizeText", "6.9 GB"}, {"host", "huggingface.co"}, {"folder", "~/.local/share/fugue/models/checkpoints"}};
            QObject *ask = component.createWithInitialProperties({{"info", info}, {"parent", QVariant::fromValue(app.window->contentItem())}});
            QVERIFY2(ask, qPrintable(component.errorString()));
            QQmlEngine::setObjectOwnership(ask, QQmlEngine::JavaScriptOwnership); // it destroys itself when it closes
            QVERIFY(QMetaObject::invokeMethod(ask, "open"));
            QTRY_VERIFY(ask->property("opened").toBool());
            save(app, "model-download");
            QVERIFY(QMetaObject::invokeMethod(ask, "close")); // it destroys itself
        }

        // The Graph's group questions and editor.
        popup(app, "groupSinksAsk", "group-sinks", {{"sinks", QStringList{"Save image"}}});
        popup(app, "groupUpdateAsk", "group-update", {{"changes", QVariantMap{{"name", "Fox pass"}, {"from", "1.0.0"}, {"to", "1.1.0"}}}});
        popup(app, "libraryRenameAsk", "library-rename", {{"libraryId", "local/fox"}, {"name", "Fox pass 2"}});
        popup(app, "renameAsk", "rename-port", {{"prompt", "Rename the input"}});
        QVERIFY(app.commands.trigger("app.make"));
        QTRY_VERIFY(named(app, "appBuilderDialog")->property("opened").toBool());
        save(app, "app-builder");
        QVERIFY(QMetaObject::invokeMethod(named(app, "appBuilderDialog"), "close"));
        QTRY_VERIFY(!named(app, "appBuilderDialog")->property("visible").toBool());

        // Crop, Region and the mask editor.
        QVERIFY(QMetaObject::invokeMethod(app.canvas, "openCrop", Q_ARG(QVariant, QString("crop"))));
        QTRY_VERIFY(named(app, "cropDialog") && named(app, "cropDialog")->property("opened").toBool());
        save(app, "crop");
        QVERIFY(QMetaObject::invokeMethod(named(app, "cropDialog"), "close"));
        QTRY_VERIFY(!named(app, "cropDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(app.canvas, "openRegion", Q_ARG(QVariant, QString("region"))));
        QObject *region = nullptr;
        QTRY_VERIFY((region = app.canvas->property("regionDialog").value<QObject *>()) && region->property("opened").toBool());
        save(app, "region");
        QVERIFY(QMetaObject::invokeMethod(region, "close"));
        QTRY_VERIFY(!region->property("visible").toBool());
        {
            QQmlComponent component(&app.engine, QUrl("qrc:/canvas/MaskEditor.qml"));
            QObject *mask = component.createWithInitialProperties({{"look", QVariant::fromValue(app.theme.look())},
                                                                   {"imageSource", QUrl::fromLocalFile(files.filePath("a.png"))},
                                                                   {"imageSize", QSize(320, 200)},
                                                                   {"parent", QVariant::fromValue(app.window->contentItem())}});
            QVERIFY2(mask, qPrintable(component.errorString()));
            QVERIFY(QMetaObject::invokeMethod(mask, "open"));
            QTRY_VERIFY(mask->property("opened").toBool());
            save(app, "mask-editor");
            QVERIFY(QMetaObject::invokeMethod(mask, "close"));
            QTRY_VERIFY(!mask->property("visible").toBool());
            delete mask;
        }

        // The canvas dialogs.
        QVERIFY(app.commands.trigger("canvas.new"));
        QTRY_VERIFY(named(app, "newCanvasDialog")->property("opened").toBool());
        save(app, "new-canvas");
        QVERIFY(QMetaObject::invokeMethod(named(app, "newCanvasDialog"), "close"));
        QTRY_VERIFY(!named(app, "newCanvasDialog")->property("visible").toBool());
        QVERIFY(app.commands.trigger("file.canvas-view"));
        QTRY_VERIFY(named(app, "canvasViewDialog") && named(app, "canvasViewDialog")->property("opened").toBool());
        popup(app, "canvasPropertiesAsk", "canvas-layer-properties");
        popup(app, "canvasRenameAsk", "canvas-rename");
        popup(app, "canvasShareAsk", "canvas-share");
        popup(app, "canvasClearAsk", "canvas-clear");
        popup(app, "canvasDeleteAsk", "canvas-delete");
        popup(app, "canvasModifyAsk", "canvas-selection-modify");
        popup(app, "canvasTransformAsk", "canvas-selection-transform");
        popup(app, "canvasBatchDialog", "canvas-batch", {}, true);
        popup(app, "canvasTaskClearAsk", "canvas-task-clear-history", {}, true);
        QVERIFY(QMetaObject::invokeMethod(named(app, "canvasView"), "exportWith", Q_ARG(QVariant, "picture")));
        QTRY_VERIFY(named(app, "canvasExportDialog") && named(app, "canvasExportDialog")->property("opened").toBool());
        save(app, "canvas-export");
        QVERIFY(QMetaObject::invokeMethod(named(app, "canvasExportDialog"), "close"));
        QTRY_VERIFY(!named(app, "canvasExportDialog")->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(named(app, "canvasViewDialog"), "close"));
        QTRY_VERIFY(!named(app, "canvasViewDialog")->property("visible").toBool());

        // Template details over the Templates dialog.
        QVERIFY(app.commands.trigger("file.templates"));
        QObject *templates = named(app, "templatesDialog");
        QTRY_VERIFY(templates && templates->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(templates, "showDetails", Q_ARG(QVariant, "text-to-image"), Q_ARG(QVariant, QVariant())));
        QTest::qWait(600);
        save(app, "template-details");
        QVERIFY(QMetaObject::invokeMethod(templates, "close"));
        QTRY_VERIFY(!templates->property("visible").toBool());

        // Get ready, with a made-up plan.
        app.workflows.readiness()->show();
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = named(app, "getReadySheet")));
        const QVariantMap model{{"name", "flux-2-klein-9b-Q4_K_M.gguf"}, {"size", "5.9 GB"}, {"host", "huggingface.co"}, {"license", "Apache-2.0"},
                                {"licenseUrl", ""}, {"folder", "~/.local/share/fugue/models/diffusion_models"}, {"typeFrom", ""}, {"state", ""},
                                {"detail", ""}, {"fraction", 0.0}, {"hash", "a"}, {"use", QVariantMap()}, {"choice", ""}, {"sizeKnown", true},
                                {"error", ""}};
        const QVariantMap view{{"headline", "Fugue will download 1 model, 5.9 GB."}, {"packs", QVariantList{}}, {"models", QVariantList{model}}, {"optionalModels", QVariantList{}},
                               {"destination", "/home/me/.local/share/fugue/models"}, {"destinationTitle", "Fugue's models folder"},
                               {"destinations", QVariantList{}}, {"hasModels", true}, {"hasDownloads", true}, {"busy", false},
                               {"primary", "Install"}, {"usesCount", 0}, {"freeText", "343.2 GB free"}, {"sizeText", "5.9 GB"},
                               {"lowSpace", false}, {"problems", QStringList{}}, {"question", QVariantMap{}}, {"message", ""}, {"ok", false},
                               {"retry", QStringList{}}, {"empty", false}};
        auto *plan = QQmlPropertyMap::create(sheet);
        plan->insert("open", true);
        plan->insert("phase", "review");
        plan->insert("view", view);
        QVERIFY(sheet->setProperty("sheetModel", QVariant::fromValue(static_cast<QObject *>(plan))));
        QTRY_VERIFY(sheet->property("opened").toBool());
        save(app, "get-ready");
        plan->insert("open", false);
        emit plan->valueChanged("open", false);
        QTRY_VERIFY(!sheet->property("visible").toBool());

        // The first run's model folder setup.
        app.settings.modelSetup()->show(false);
        QObject *setup = nullptr;
        QTRY_VERIFY((setup = named(app, "modelSetupSheet")) && setup->property("opened").toBool());
        save(app, "model-setup");
        app.settings.modelSetup()->skip();
        QTRY_VERIFY(!setup->property("visible").toBool());

        // Settings: the providers' remove question, the packs' and the cache's inline questions.
        QVERIFY(app.commands.trigger("app.settings"));
        QTRY_VERIFY(named(app, "settingsPanel")->property("opened").toBool());
        named(app, "settingsPanel")->setProperty("page", "providers");
        popup(app, "providerRemoveAsk", "provider-remove");
        inlineQuestion(app, "packs", "packQuestion", "packs-question");
        inlineQuestion(app, "cache", "cacheQuestion", "cache-question");
        QVERIFY(QMetaObject::invokeMethod(named(app, "settingsPanel"), "close"));
        QTRY_VERIFY(!named(app, "settingsPanel")->property("visible").toBool());

        // Last, as it changes the graph: the group interface editor of a group made of Crop.
        app.model.select({"crop"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        QVERIFY(app.commands.trigger("group.interface"));
        QTRY_VERIFY(named(app, "groupInterfaceEditor")->property("opened").toBool());
        save(app, "group-interface-editor");
    }
};

QTEST_MAIN(TestDialogShots)
#include "test_dialog_shots.moc"
