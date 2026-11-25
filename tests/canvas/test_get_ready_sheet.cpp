// The Get ready sheet by keys alone (#464, the owner's keyboard rule): it opens with the focus on Install, Tab and Shift+Tab move
// between the buttons, Enter is the yes, Escape cancels, and the focus goes back to what had it. The sheet shows a fake model (what
// the real one does is test_readiness and test_templates; nothing is installed or downloaded here).

#include <QQmlProperty>
#include <QQuickItem>
#include <QtTest>

#include "canvas_app.h"
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

    Q_INVOKABLE void start() { ++started; }
    Q_INVOKABLE void cancel() {
        ++cancelled;
        open = false;
        emit changed();
    }
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
                           {"typeFrom", typeFrom}, {"state", ""}, {"detail", ""}, {"fraction", 0.0}};
    };
    return {{"headline", "Fugue will install and trust 1 pack, set up 1 environment and download 4 models, 19.7 GB."},
            {"packs", QVariantList{QVariantMap{{"id", "segment"}, {"name", "Segment Nodes"}, {"version", "1.2.0"}, {"install", true}, {"pending", false},
                                               {"summary", "from github.com/example/fugue-segment at v1.2.0"},
                                               {"source", "github.com/example/fugue-segment at v1.2.0"}, {"runtime", "python"},
                                               {"permissions", "network, files"}, {"setup", "uv sync"}, {"setupNote", "Installs the segmentation libraries."},
                                               {"setupSize", "1.3 GB"}, {"state", ""}, {"detail", ""}}}},
            {"models", QVariantList{model("Qwen3-8B-Q8_0.gguf", "8.7 GB", "Apache-2.0", ""),
                                    model("flux-2-klein-9b-Q4_K_M.gguf", "5.9 GB", "FLUX Non-Commercial", ""),
                                    model("ae.safetensors", "335 MB", "", "type from Segment Nodes"),
                                    model("sam2_hiera_large.pt", "898 MB", "Apache-2.0", "type from Segment Nodes")}},
            {"destination", "/home/me/.local/share/fugue/models"}, {"destinationTitle", "Fugue's models folder"}, {"destinations", QVariantList{}},
            {"hasModels", true}, {"freeText", "343.2 GB free"}, {"sizeText", "19.7 GB"}, {"lowSpace", false}, {"problems", QStringList{}},
            {"question", QVariantMap{}}, {"message", ""}, {"ok", false}, {"retry", QStringList{}}, {"empty", false}};
}

QVariantMap fakeView() {
    const QVariantMap model{{"name", "flux-2-klein-9b-Q4_K_M.gguf"}, {"size", "5.9 GB"}, {"host", "huggingface.co"}, {"license", "FLUX Non-Commercial"},
                            {"licenseUrl", ""}, {"folder", "~/.local/share/fugue/models/diffusion_models"}, {"typeFrom", ""},
                            {"state", ""}, {"detail", ""}, {"fraction", 0.0}};
    return {{"headline", "Fugue will download 1 model, 5.9 GB."}, {"packs", QVariantList{}}, {"models", QVariantList{model}},
            {"destination", "/models"}, {"destinationTitle", "Fugue's models folder"}, {"destinations", QVariantList{}}, {"hasModels", true},
            {"freeText", "300 GB free"}, {"sizeText", "5.9 GB"}, {"lowSpace", false}, {"problems", QStringList{}}, {"question", QVariantMap{}},
            {"message", ""}, {"ok", false}, {"retry", QStringList{}}, {"empty", false}};
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
        // Open on a model's details, then part-way through the run.
        sheet->setProperty("expanded", QVariantMap{{"model:Qwen3-8B-Q8_0.gguf", true}, {"pack:segment", true}});
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
