// Not checks: the README's pictures (#94, docs/recording-screenshots.md). Each test skips unless
// FUGUE_SHOT_DIR names a folder, then saves the real window (Main.qml, offscreen) there as a PNG.
// Nothing here uses the owner's files: the window, its settings, templates folder and results are
// the test's own, the templates are the bundled ones. FUGUE_SHOT_TEMPLATE names another template
// for hero.png.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQmlPropertyMap>
#include <QQuickStyle>
#include <QtTest>

#include "canvas_app.h"
#include "../readme_shot.h"

using namespace canvas_test;

namespace {

QString shotDir() { return qEnvironmentVariable("FUGUE_SHOT_DIR"); }

QByteArray template_(const QString &name) {
    QFile file(QFINDTESTDATA("../../templates/" + name + "/workflow.fugue"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The window as the README shows it: a wide one, no trust or notice banner, the graph fitted.
void prepare(App &app, const QSize &size = {1600, 1000}) {
    app.window->resize(size);
    app.workflows.dismissPendingTrust();
    app.workflows.dismissNotice();
    QTest::qWait(300); // the canvas takes its new size before it fits
    app.model.select({});
    app.model.requestFit();
    QTest::qWait(800);
}

// A template's workflow with its sample files as absolute paths, so the window shows the pictures (the
// test's document has no folder of its own to resolve "inputs/..." against).
QByteArray templateWithPictures(const QString &name) {
    QByteArray json = template_(name);
    const QString inputs = QDir(QFINDTESTDATA("../../templates/" + name)).absoluteFilePath("inputs/");
    json.replace("\"inputs/", ("\"" + inputs).toUtf8());
    return json;
}

bool saveGraph(App &app, const QString &name, int skipTop = 0) {
    return readme_shot::graphOnly(app.window, app.canvas, 28, skipTop).save(shotDir() + "/" + name);
}

} // namespace

class TestReadmeShots : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); } // as main() does

    void init() {
        if (shotDir().isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the README screenshots");
        qputenv("FUGUE_TEMPLATES", QFINDTESTDATA("../../templates").toUtf8());
    }

    // The bundled text-to-image template on the canvas, in the dark look and in a light one.
    void hero_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("file");
        QTest::newRow("dark") << QString() << "hero.png";
        QTest::newRow("light") << "catppuccin-latte" << "hero-light.png";
    }
    void hero() {
        QFETCH(QString, theme);
        QFETCH(QString, file);
        const QString name = qEnvironmentVariable("FUGUE_SHOT_TEMPLATE", "text-to-image");
        App app(load(template_(name)), {}, Look{theme, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(saveGraph(app, file));
    }

    // File > Browse templates: the dialog over the canvas.
    void templatesDialog() {
        App app(load(template_("text-to-image")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(app.commands.run("file.templates"));
        QTest::qWait(1500); // the cards' previews load
        // The dialog and a margin of the dimmed canvas. Its footnote names this test's own
        // temporary folder, so that line is painted over with the sidebar's color.
        QImage shot = test_support::grab(app.window).convertToFormat(QImage::Format_ARGB32);
        const QRect dialog(200, 100, 1200, 800); // centered, as Popup sizes it in a 1600 x 1000 window
        QPainter(&shot).fillRect(QRect(dialog.left() + 1, dialog.bottom() - 60, 207, 58), shot.pixelColor(dialog.left() + 60, dialog.bottom() - 100));
        QVERIFY(shot.copy(dialog.adjusted(-40, -40, 40, 40)).save(shotDir() + "/templates.png"));
    }

    // The same dialog with a Source and a Model family chosen, in the light theme (a look check, not a README picture).
    void templatesFiltered() {
        App app(load(template_("text-to-image")), {}, Look{"catppuccin-latte", 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(app.commands.run("file.templates"));
        QObject *dialog = app.window->findChild<QObject *>("templatesDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(dialog->property("sourceOptions").toList().size() > 1);
        const auto pick = [&](const char *options, const QString &word) {
            for (const QVariant &option : dialog->property(options).toList())
                if (option.toMap().value("value").toString().contains(word))
                    return option.toMap().value("value");
            return QVariant();
        };
        QVERIFY(dialog->setProperty("source", pick("sourceOptions", "DA flux2")));
        QVERIFY(dialog->setProperty("family", pick("familyOptions", "9B")));
        QTest::qWait(1500); // the cards' previews load
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/templates-filtered.png"));
        // The family list open with a word typed.
        QObject *family = dialog->findChild<QObject *>("templatesFamily");
        QVERIFY(family && QMetaObject::invokeMethod(family, "open"));
        for (const char c : QByteArray("klei"))
            QTest::keyClick(app.window, c);
        QTest::qWait(300);
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/templates-family-search.png"));
    }

    // A template's details over the dialog's grid (the DA restore one: a long description, models missing), dark and light.
    void templatesDetails_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("file");
        QTest::newRow("dark") << QString() << "templates-details.png";
        QTest::newRow("light") << "catppuccin-latte" << "templates-details-light.png";
    }
    void templatesDetails() {
        QFETCH(QString, theme);
        QFETCH(QString, file);
        App app(load(template_("text-to-image")), {}, Look{theme, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(app.commands.run("file.templates"));
        QObject *dialog = app.window->findChild<QObject *>("templatesDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "showDetails", Q_ARG(QVariant, "flux2-klein-9b-da-restore"), Q_ARG(QVariant, QVariant())));
        QTest::qWait(1000); // the banner and the sample inputs load
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/" + file));
    }

    // The Get ready sheet (#379) with a pack, its environment and four models, in review, open on one row's details, and
    // part-way through the run, dark and light. The sheet shows a made-up plan (a property map in place of the C++ model).
    void getReady_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("suffix");
        QTest::newRow("dark") << QString() << "";
        QTest::newRow("light") << "catppuccin-latte" << "-light";
    }
    void getReady() {
        QFETCH(QString, theme);
        QFETCH(QString, suffix);
        App app(load(template_("text-to-image")), {}, Look{theme, 8});
        QVERIFY(app.open());
        prepare(app, {1200, 900});
        app.workflows.readiness()->show(); // makes the sheet (the window builds it the first time it opens)
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = app.window->findChild<QObject *>("getReadySheet")));
        const auto model = [](const QString &name, const QString &size, const QString &license, const QString &typeFrom) {
            return QVariantMap{{"name", name}, {"size", size}, {"host", "huggingface.co"}, {"license", license}, {"licenseUrl", ""},
                               {"folder", typeFrom.isEmpty() ? "~/.local/share/fugue/models/diffusion_models" : "its folder, once the pack is installed"},
                               {"typeFrom", typeFrom}, {"state", ""}, {"detail", ""}, {"fraction", 0.0}};
        };
        QVariantMap view{
            {"headline", "Fugue will install and trust 1 pack, set up 1 environment and download 4 models, 19.7 GB."},
            {"packs", QVariantList{QVariantMap{{"id", "segment"}, {"name", "Segment Nodes"}, {"version", "1.2.0"}, {"install", true}, {"pending", false},
                                               {"summary", "from github.com/example/fugue-segment at v1.2.0 · declares network, files · runs uv sync"},
                                               {"source", "github.com/example/fugue-segment at v1.2.0"}, {"runtime", "python"},
                                               {"permissions", "network, files"}, {"setup", "uv sync"}, {"setupNote", "Installs the segmentation libraries."},
                                               {"setupSize", "1.3 GB"}, {"state", ""}, {"detail", ""}}}},
            {"models", QVariantList{model("Qwen3-8B-Q8_0.gguf", "8.7 GB", "Apache-2.0", ""),
                                    model("flux-2-klein-9b-Q4_K_M.gguf", "5.9 GB", "FLUX Non-Commercial", ""),
                                    model("ae.safetensors", "335 MB", "", "type from Segment Nodes: Segment by Points (needs a mask)"),
                                    model("sam2_hiera_large.pt", "898 MB", "Apache-2.0", "type from Segment Nodes")}},
            {"destination", "/home/me/.local/share/fugue/models"}, {"destinationTitle", "Fugue's models folder"},
            {"destinations", QVariantList{}}, {"hasModels", true}, {"freeText", "343.2 GB free"}, {"sizeText", "19.7 GB"},
            {"lowSpace", false}, {"problems", QStringList{}}, {"question", QVariantMap{}}, {"message", ""}, {"ok", false},
            {"retry", QStringList{}}, {"empty", false}};
        auto *map = QQmlPropertyMap::create(sheet);
        map->insert("open", true);
        map->insert("phase", "review");
        map->insert("view", view);
        QVERIFY(sheet->setProperty("sheetModel", QVariant::fromValue(static_cast<QObject *>(map))));
        QTRY_VERIFY(sheet->property("visible").toBool());
        QTest::qWait(500);
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/get-ready" + suffix + ".png"));
        // Open on a model's details.
        QVERIFY(sheet->setProperty("expanded", QVariantMap{{"model:Qwen3-8B-Q8_0.gguf", true}, {"pack:segment", true}}));
        QTest::qWait(300);
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/get-ready-details" + suffix + ".png"));
        // Part-way through the run: the pack done, one download going, one failed to start, two waiting.
        QVariantMap running = view;
        QVariantList packs = running.value("packs").toList();
        QVariantMap pack = packs.at(0).toMap();
        pack["state"] = "done";
        packs[0] = pack;
        QVariantList models = running.value("models").toList();
        QVariantMap first = models.at(0).toMap();
        first["state"] = "done";
        QVariantMap second = models.at(1).toMap();
        second["state"] = "running";
        second["fraction"] = 0.42;
        models[0] = first;
        models[1] = second;
        running["packs"] = packs;
        running["models"] = models;
        map->insert("phase", "running");
        map->insert("view", running);
        QVERIFY(sheet->setProperty("expanded", QVariantMap{}));
        QTest::qWait(300);
        QVERIFY(test_support::grab(app.window).save(shotDir() + "/get-ready-running" + suffix + ".png"));
    }

    // The node library panel (N) beside a workflow.
    void nodeLibrary() {
        App app(load(template_("z-image-turbo")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        app.key(Qt::Key_N);
        QTest::qWait(300);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, QPoint(84, 202)); // Image
        QTest::mouseClick(app.window, Qt::LeftButton, {}, QPoint(84, 174)); // Generate
        QTest::qWait(300);
        QTest::mouseMove(app.window, QPoint(900, 900)); // off the list
        app.model.requestFit(); // the canvas beside the panel
        QTest::qWait(800);
        QVERIFY(test_support::grab(app.window).copy(0, 0, 1600, 760).save(shotDir() + "/node-library.png"));
    }

    // The prompt encoder, sampler and decoder made into one group node.
    void group() {
        App app(load(template_("z-image-turbo")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        app.model.select({"encode", "sample", "decode"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        app.key(Qt::Key_Escape); // ends the name edit the new group starts
        app.model.select({});
        app.model.requestFit();
        QTest::qWait(800);
        QVERIFY(saveGraph(app, "group.png"));
    }

    // The two masking templates (#177): each one's graph, with the sample picture and its painted
    // mask on the Load Image node.
    void maskTemplates_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("file");
        QTest::newRow("inpaint") << "inpaint-region" << "inpaint-graph.png";
        QTest::newRow("outpaint") << "outpaint" << "outpaint-graph.png";
    }
    void maskTemplates() {
        QFETCH(QString, name);
        QFETCH(QString, file);
        App app(load(templateWithPictures(name)), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app, {2400, 900}); // wide enough that the nodes keep their text at the fitted zoom
        QVERIFY(saveGraph(app, file));
    }

    // Paint Mask wired after Load Image: the mask ports are hollow squares, the picture ports filled.
    void paintMaskNode() {
        const QString inputs = QDir(QFINDTESTDATA("../../templates/inpaint-region")).absoluteFilePath("inputs/");
        const QString mask = QDir(inputs).entryList({"reading-nook-mask-*.png"}).value(0);
        const QByteArray json = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "%1reading-nook.jpg"}},
            "paint": {"type": "fugue/paint-mask", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "mask": "%1%2"}},
            "feather": {"type": "fugue/feather-mask", "version": "1.0.0", "inputs": {"mask": {"from": "paint.mask"}}}}},
            "layout": {"nodes": {"load": {"pos": [0, 0]}, "paint": {"pos": [340, 0]}, "feather": {"pos": [680, 0]}}}})")
                                    .arg(inputs, mask).toUtf8();
        App app(load(json), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(saveGraph(app, "mask-paint-node.png"));
    }

    // Inside the group Inpaint (crop and stitch): its six nodes.
    void maskGroup() {
        App app(load(templateWithPictures("inpaint-region")), {}, Look{{}, 8});
        QVERIFY(app.open());
        prepare(app);
        QVERIFY(app.model.groups()->enterGroup("inpaint"));
        app.model.select({});
        app.model.requestFit();
        QTest::qWait(1000);
        QVERIFY(saveGraph(app, "mask-group.png", 70));
    }
};

QTEST_MAIN(TestReadmeShots)
#include "test_readme_shots.moc"
