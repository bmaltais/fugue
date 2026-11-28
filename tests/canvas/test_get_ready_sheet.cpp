// The Get ready sheet by keys alone (#464, the owner's keyboard rule): it opens with the focus on Install, Tab and Shift+Tab move
// between the buttons, Enter is the yes, Escape cancels, and the focus goes back to what had it. The sheet shows a fake model (what
// the real one does is test_readiness and test_templates; nothing is installed or downloaded here).

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlProperty>
#include <QQuickItem>
#include <QtTest>

#include "canvas_app.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "../test_shard.h"
#include "../window_grab.h"

using namespace canvas_test;

namespace {

const QByteArray emptyDoc = R"({"fugue": 1, "graph": {"nodes": {}}})";

// What ReadinessSheet shows QML: the open flag, the phase, the view, and the calls the sheet makes.
class FakeSheet : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open MEMBER open NOTIFY changed)
    Q_PROPERTY(QString phase MEMBER phase NOTIFY changed)
    Q_PROPERTY(QVariantMap view MEMBER view NOTIFY changed)
public:
    bool open = true;
    QString phase = QStringLiteral("review");
    QVariantMap view;
    int started = 0;
    int cancelled = 0;
    QList<QPair<QString, bool>> choices; // setChoice(hash, useDisk), as the sheet asked

    Q_INVOKABLE void start() { ++started; }
    Q_INVOKABLE void cancel() {
        ++cancelled;
        open = false;
        emit changed();
    }
    Q_INVOKABLE void setChoice(const QString &hash, bool useDisk) { choices.append({hash, useDisk}); }
    Q_INVOKABLE void close() {}
    Q_INVOKABLE void show() {}
    Q_INVOKABLE void changeDestination(const QString &) {}
    Q_INVOKABLE void pickDestination(const QString &) {}
    Q_INVOKABLE void answer(bool) {}
signals:
    void changed();
};

// A pack, its environment and four models, as the documentation's picture has them.
QVariantMap bigView() {
    const auto model = [](const QString &name, const QString &size, const QString &license, const QString &typeFrom) {
        return QVariantMap{{"name", name}, {"size", size}, {"host", "huggingface.co"}, {"license", license}, {"licenseUrl", ""},
                           {"folder", typeFrom.isEmpty() ? "~/.local/share/fugue/models/diffusion_models" : "its folder, once the pack is installed"},
                           {"typeFrom", typeFrom}, {"state", ""}, {"detail", ""}, {"fraction", 0.0}, {"hash", name}, {"use", QVariantMap()},
                           {"choice", ""}, {"sizeKnown", true}, {"error", ""}};
    };
    return {{"headline", "Fugue will install and trust 1 pack, set up 1 environment and download 4 models, 19.7 GB."},
            {"packs", QVariantList{QVariantMap{{"id", "segment"}, {"name", "Segment Nodes"}, {"version", "1.2.0"}, {"install", true}, {"pending", false},
                                               {"summary", "from github.com/example/fugue-segment at v1.2.0"},
                                               {"source", "github.com/example/fugue-segment at v1.2.0"}, {"runtime", "python"},
                                               {"permissions", "Can use the network, Can reach services on this computer and its X11/Xwayland display, Can read ~/Documents/Scans"}, {"setup", "uv sync"}, {"setupNote", "Installs the segmentation libraries."}, {"secrets", ""},
                                               {"setupLines", QStringList{"Setup uses the network", "Setup can reach services on this computer and its X11/Xwayland display"}},
                                               {"setupSize", "1.3 GB"}, {"state", ""}, {"detail", ""}}}},
            {"models", QVariantList{model("Qwen3-8B-Q8_0.gguf", "8.7 GB", "Apache-2.0", ""),
                                    model("flux-2-klein-9b-Q4_K_M.gguf", "5.9 GB", "FLUX Non-Commercial", ""),
                                    model("ae.safetensors", "335 MB", "", "type from Segment Nodes"),
                                    model("sam2_hiera_large.pt", "898 MB", "Apache-2.0", "type from Segment Nodes")}},
            {"destination", "/home/me/.local/share/fugue/models"}, {"destinationTitle", "Fugue's models folder"}, {"destinations", QVariantList{}},
            {"hasModels", true}, {"optionalModels", QVariantList{}}, {"hasDownloads", true}, {"busy", false}, {"primary", "Install"}, {"usesCount", 0}, {"freeText", "343.2 GB free"},
            {"sizeText", "19.7 GB"}, {"lowSpace", false}, {"problems", QStringList{}},
            {"question", QVariantMap{}}, {"message", ""}, {"ok", false}, {"retry", QStringList{}}, {"empty", false}};
}

QVariantMap fakeView() {
    const QVariantMap model{{"name", "flux-2-klein-9b-Q4_K_M.gguf"}, {"size", "5.9 GB"}, {"host", "huggingface.co"}, {"license", "FLUX Non-Commercial"},
                            {"licenseUrl", ""}, {"folder", "~/.local/share/fugue/models/diffusion_models"}, {"typeFrom", ""},
                            {"state", ""}, {"detail", ""}, {"fraction", 0.0}, {"hash", "h1"}, {"use", QVariantMap()}, {"choice", ""},
                            {"sizeKnown", true}, {"error", ""}};
    return {{"headline", "Fugue will download 1 model, 5.9 GB."}, {"packs", QVariantList{}}, {"models", QVariantList{model}},
            {"destination", "/models"}, {"destinationTitle", "Fugue's models folder"}, {"destinations", QVariantList{}}, {"hasModels", true}, {"optionalModels", QVariantList{}},
            {"hasDownloads", true}, {"busy", false}, {"primary", "Install"}, {"usesCount", 0},
            {"freeText", "300 GB free"}, {"sizeText", "5.9 GB"}, {"lowSpace", false}, {"problems", QStringList{}}, {"question", QVariantMap{}},
            {"message", ""}, {"ok", false}, {"retry", QStringList{}}, {"empty", false}};
}

// The Qwen3-8B case: the model is on disk in another file, and the sheet starts on "Use my file" (the owner's #616). `mixed` adds a model
// that has no file of its own.
QVariantMap coveredView(bool mixed = false, const QString &choice = "disk") {
    const auto row = [](const QString &name, const QString &size, const QString &hash, bool covered, const QString &choice) {
        return QVariantMap{{"name", name}, {"size", size}, {"host", "huggingface.co"}, {"license", "Apache-2.0"}, {"licenseUrl", ""},
                           {"folder", "~/.local/share/fugue/models/diffusion_models"}, {"typeFrom", ""}, {"state", ""}, {"detail", ""},
                           {"fraction", 0.0}, {"hash", hash}, {"sizeKnown", true}, {"error", ""},
                           {"use", covered ? QVariantMap{{"file", "qwen_3_8b_int8_convrot.safetensors"}, {"same", "same model, INT8"}} : QVariantMap()},
                           {"choice", covered ? choice : QString()}};
    };
    QVariantList models{row("Qwen3-8B-Q8_0.gguf", "8.7 GB", "qwen", true, choice)};
    if (mixed)
        models << row("flux-2-klein-9b-Q4_K_M.gguf", "5.9 GB", "flux", false, "");
    const bool downloads = mixed || choice == "download";
    QString headline = mixed ? "Fugue will download 1 model, 5.9 GB and use 1 file you already have."
                       : choice == "download" ? "Fugue will download 1 model, 8.7 GB."
                                              : "Nothing to download: Fugue will use the file you already have.";
    return {{"headline", headline}, {"packs", QVariantList{}}, {"models", models},
            {"destination", "/models"}, {"destinationTitle", "Fugue's models folder"}, {"destinations", QVariantList{}}, {"hasModels", true}, {"optionalModels", QVariantList{}},
            {"hasDownloads", downloads}, {"busy", false}, {"primary", downloads ? "Install" : "Use my file"}, {"usesCount", mixed ? 1 : choice == "disk" ? 1 : 0},
            {"freeText", "1979 GB free"}, {"sizeText", mixed ? "5.9 GB" : choice == "download" ? "8.7 GB" : ""}, {"lowSpace", false},
            {"problems", QStringList{}}, {"question", QVariantMap{}}, {"message", ""}, {"ok", false}, {"retry", QStringList{}}, {"empty", false}};
}

QQuickItem *visibleItem(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visibleItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *find(QQuickWindow *window, const QString &name) {
    QQuickItem *root = window->contentItem();
    while (root->parentItem())
        root = root->parentItem();
    return visibleItem(root, name);
}

} // namespace

class TestGetReadySheet : public QObject {
    Q_OBJECT

private slots:
    void enterInstallsEscapeCancelsAndTheFocusComesBack() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.canvas->forceActiveFocus();
        QTRY_VERIFY(app.canvas->hasActiveFocus());
        QQuickItem *before = app.window->activeFocusItem();
        app.workflows.readiness()->show(); // makes the sheet (the window builds it the first time it opens)
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = app.window->findChild<QObject *>("getReadySheet")));
        FakeSheet fake;
        fake.view = fakeView();
        QVERIFY(sheet->setProperty("sheetModel", QVariant::fromValue(static_cast<QObject *>(&fake))));
        QTRY_VERIFY(sheet->property("visible").toBool());
        QTRY_VERIFY(find(app.window, "getReadyAll")); // the fake plan is shown; the sheet had opened on the real, empty one
        // no focusDefault call: the sheet focuses its default control itself, whenever the plan arrives
        // Opening: the focus is on Install, the one primary.
        QQuickItem *install = nullptr;
        QTRY_VERIFY((install = find(app.window, "getReadyAll")) && install->hasActiveFocus());
        // The first key does its work with no Tab before it (the plan came after the sheet opened: #394 triage).
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(fake.started, 1);
        // Shift+Tab goes back to Cancel (the buttons are in reading order, the yes last), Tab to Install again.
        QTest::keyClick(app.window, Qt::Key_Backtab, Qt::ShiftModifier);
        QQuickItem *cancel = find(app.window, "getReadyCancel");
        QVERIFY(cancel);
        QTRY_VERIFY(cancel->hasActiveFocus() || !install->hasActiveFocus());
        QTest::keyClick(app.window, Qt::Key_Tab);
        QTRY_VERIFY(install->hasActiveFocus() || !cancel->hasActiveFocus());
        install->forceActiveFocus();
        // Enter is the yes.
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(fake.started, 2);
        // Escape cancels: the sheet goes and the focus is back on what had it.
        QTest::keyClick(app.window, Qt::Key_Escape);
        QCOMPARE(fake.cancelled, 1);
        QTRY_VERIFY(!sheet->property("visible").toBool());
        QTRY_COMPARE(app.window->activeFocusItem(), before);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #616 (UI design 2026-11-26): a model with a file of its own has a segmented choice on "Use my file"; the primary reads "Use my file"
    // and has the focus, Enter runs it, Tab reaches the choice, Left and Right choose, Enter on the choice runs the primary too.
    void theChoiceIsOneTabStopAndEnterGoesToThePrimary() {
        App app(load(emptyDoc));
        QVERIFY(app.open());
        app.workflows.readiness()->show();
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = app.window->findChild<QObject *>("getReadySheet")));
        FakeSheet fake;
        fake.view = coveredView();
        QVERIFY(sheet->setProperty("sheetModel", QVariant::fromValue(static_cast<QObject *>(&fake))));
        QQuickItem *primary = nullptr;
        QTRY_VERIFY((primary = find(app.window, "getReadyAll")) && primary->hasActiveFocus());
        QCOMPARE(primary->property("text").toString(), QString("Use my file"));
        QVERIFY(!find(app.window, "getReadyDestination")); // nothing is downloaded: no destination
        QTest::keyClick(app.window, Qt::Key_Return);
        QCOMPARE(fake.started, 1);
        QQuickItem *choice = find(app.window, "getReadyChoice-Qwen3-8B-Q8_0.gguf");
        QVERIFY(choice);
        for (int i = 0; i < 8 && !choice->hasActiveFocus(); ++i)
            QTest::keyClick(app.window, Qt::Key_Tab);
        QVERIFY(choice->hasActiveFocus()); // reached by Tab
        QTest::keyClick(app.window, Qt::Key_Right);
        QCOMPARE(fake.choices.size(), 1);
        QCOMPARE(fake.choices.first(), (QPair<QString, bool>{"qwen", false}));
        QTest::keyClick(app.window, Qt::Key_Return); // Enter on the choice is the yes, not a toggle of the choice
        QCOMPARE(fake.started, 2);
        QTest::keyClick(app.window, Qt::Key_Escape);
        QCOMPARE(fake.cancelled, 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join("\n")));
    }

    // #544: a workflow with a MOSS Image model whose Qwen3.5 tokenizer is not on disk opens with the real plan offering the tokenizer
    // (not a made-up view): the banner's Get ready sheet lists qwen3.5_tokenizer.json with its size and license, like a model file.
    void aMossWorkflowWithoutTheTokenizerOffersItsDownload() {
        QFile::remove(fugue::dataDir() + "/models/tokenizers/qwen3.5_tokenizer.json");
        QFile::remove(fugue::dataDir() + "/catalog/test.json");
        const QString file = fugue::dataDir() + "/models/diffusion_models/moss-test.safetensors";
        QJsonObject header;
        const auto tensor = [&](const QString &name, const QJsonArray &shape) {
            header.insert(name, QJsonObject{{"dtype", "F16"}, {"shape", shape}, {"data_offsets", QJsonArray{0, 0}}});
        };
        tensor("video_embedder.weight", QJsonArray{3072, 128});
        tensor("shared_block_modulation.weight", QJsonArray{18432, 3072});
        tensor("text_embedder.proj.weight", QJsonArray{3072, 4096});
        const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
        QByteArray bytes;
        for (int i = 0; i < 8; ++i)
            bytes.append(char((quint64(json.size()) >> (8 * i)) & 0xff));
        bytes += json;
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(bytes);
        out.close();
        const QJsonObject handle{{"resource", "fugue/diffusion-model"}, {"name", "moss-test.safetensors"}, {"hash", fugue::sha256(bytes)}};
        const QJsonObject loader{{"type", "fugue/load-models"}, {"version", "1.2.0"}, {"inputs", QJsonObject{{"diffusion_model", handle}}}};
        const QByteArray text = QJsonDocument(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", QJsonObject{{"l", loader}}}}}}).toJson();
        App app(load(text));
        QVERIFY(app.open());
        app.workflows.readiness()->show();
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = app.window->findChild<QObject *>("getReadySheet")));
        QTRY_VERIFY(find(app.window, "getReadyModel-qwen3.5_tokenizer.json"));
        const QVariantMap view = app.workflows.readiness()->view();
        const QVariantList models = view.value("models").toList();
        QCOMPARE(models.size(), 1);
        QCOMPARE(models.first().toMap().value("name").toString(), QStringLiteral("qwen3.5_tokenizer.json"));
        QCOMPARE(models.first().toMap().value("license").toString(), QStringLiteral("Apache-2.0"));
        QVERIFY2(models.first().toMap().value("size").toString().contains("13 MB"), qPrintable(models.first().toMap().value("size").toString()));
        const QString out2 = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (!out2.isEmpty()) {
            QDir().mkpath(out2);
            app.window->resize(1100, 700);
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(out2 + "/get-ready-moss-tokenizer.png"));
        }
        QFile::remove(file);
    }

    // FUGUE_SHOT_DIR=<dir> [FUGUE_SHOT_THEME=..] [FUGUE_SHOT_LOOK="preset=flat"]: the sheet in review and part-way through, at the two sizes.
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (out.isEmpty())
            QSKIP("Set FUGUE_SHOT_DIR to a folder to write the pictures there.");
        QDir().mkpath(out);
        App app(load(emptyDoc), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        app.workflows.readiness()->show();
        QObject *sheet = nullptr;
        QTRY_VERIFY((sheet = app.window->findChild<QObject *>("getReadySheet")));
        FakeSheet fake;
        fake.view = bigView();
        QVERIFY(sheet->setProperty("sheetModel", QVariant::fromValue(static_cast<QObject *>(&fake))));
        QTRY_VERIFY(find(app.window, "getReadyAll"));
        // no focusDefault call: the sheet focuses its default control itself, whenever the plan arrives
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/get-ready-%2x%3.png").arg(out).arg(size.width()).arg(size.height())));
        }
        // The Use what you have states (UI design 2026-11-26): everything covered, Download chosen, and a mixed plan.
        for (const auto &state : {QPair<QString, QVariantMap>{"covered", coveredView()}, {"download", coveredView(false, "download")},
                                  {"mixed", coveredView(true)}}) {
            fake.view = state.second;
            emit fake.changed();
            app.window->resize(900, 700);
            QTest::qWait(400);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/get-ready-%2.png").arg(out, state.first)));
        }
        fake.view = bigView();
        emit fake.changed();
        // Open on a model's details, then part-way through the run.
        sheet->setProperty("expanded", QVariantMap{{"model:Qwen3-8B-Q8_0.gguf", true}, {"pack:segment", true}, {"env:segment", true}});
        QVariantMap running = bigView();
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
        fake.phase = "running";
        fake.view = running;
        emit fake.changed();
        app.window->resize(1888, 971);
        QTest::qWait(400);
        QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/get-ready-running-1888x971.png").arg(out)));
    }
};

FUGUE_TEST_MAIN(TestGetReadySheet, QGuiApplication)
#include "test_get_ready_sheet.moc"
