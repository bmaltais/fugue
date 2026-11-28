// The model picker of Load Models with a paired helper's models (#735, spec 8.7, docs/ui-ergonomics.md 2026-11-28), in the real
// window (Main.qml, offscreen): the "On gx10" group stands under the local files, with the family and size of each file.
//
// A picture test, skipped unless FUGUE_SHOT_DIR names a folder: it saves the open menu before (no paired helper) and after (a list
// kept for gx10 from a fake host), in the default look and a light one. The model logic is proved without a window by
// test_widget_edit and test_host_models.
//   FUGUE_SHOT_DIR=/tmp/shots QT_QUICK_BACKEND=rhi build/test_canvas_model_host_window

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "../engine/fake_http.h"
#include "../window_grab.h"
#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "engine/file_hashes.h"
#include "engine/host_models.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

const char *loadModels = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "load": {"type": "fugue/load-models", "version": "1.4.0", "inputs": {}}}},
  "layout": {"nodes": {"load": {"pos": [60, 40]}}, "view": {"center": [330, 300], "zoom": 1}}
})";

void touch(const QString &path, qint64 size) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.resize(size);
}

QQuickItem *modelWidget(const App &app, const QString &node, const QString &input) {
    QList<QQuickItem *> todo{app.window->contentItem()};
    while (!todo.isEmpty()) {
        QQuickItem *at = todo.takeFirst();
        if (at->objectName() == "inputWidget" && at->property("nodeId").toString() == node && at->property("name").toString() == input)
            return at;
        todo << at->childItems();
    }
    return nullptr;
}

} // namespace

class TestCanvasModelHostWindow : public QObject {
    Q_OBJECT

private slots:
    void theMenuWithAndWithoutAPairedHelpersModels_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("default") << QString();
        QTest::newRow("flexoki-light") << "flexoki-light";
    }

    void theMenuWithAndWithoutAPairedHelpersModels() {
        const QString out = qEnvironmentVariable("FUGUE_SHOT_DIR");
        if (out.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the model menu's pictures");
        QFETCH(QString, theme);
        if (!theme.isEmpty() && !QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        QDir().mkpath(out);

        // The local files, and a gx10 whose list a fake host answers once.
        QTemporaryDir scratch;
        const QString models = scratch.filePath("models");
        for (const auto &[name, mb] : {std::pair{"diffusion_models/flux-2-klein-9b-Q8_0.gguf", 9400}, std::pair{"diffusion_models/z-image-turbo-Q8_0.gguf", 7200},
                                       std::pair{"diffusion_models/flux1-dev-Q8_0.gguf", 12700}})
            touch(models + "/" + name, qint64(mb) * 1000 * 1000);
        // Klein is here and there with the same content (the local file's hash is remembered): the host's row is left out. Z-Image is
        // there unhashed with the local file's name and size: it stays, saying "Also here".
        {
            FileHashes known(FileHashes::defaultPath());
            known.remember(models + "/diffusion_models/flux-2-klein-9b-Q8_0.gguf", "sha256:" + QString(64, 'a'));
            known.save();
        }
        const auto entry = [](const QString &name, double size, const QString &family) {
            return QJsonObject{{"name", name}, {"size", size}, {"sha256", name.contains("z-image") ? QJsonValue() : QJsonValue(QString(64, name.contains("klein") ? 'a' : 'c'))},
                               {"folder", name.section('/', 0, 0)}, {"family", family}};
        };
        fugue::test::FakeHttpServer host;
        QVERIFY(host.listen());
        const QJsonObject reply{{"models", QJsonArray{entry("diffusion_models/flux-2-klein-9b-Q8_0.gguf", 9.4e9, "Flux.2 klein"),
                                                       entry("diffusion_models/qwen-image-edit-2511-Q4_K_M.gguf", 13.1e9, "Qwen Image"),
                                                       entry("diffusion_models/wan2.2_t2v_high_noise_14B_Q4_K_S.gguf", 8.2e9, ""),
                                                       entry("diffusion_models/z-image-turbo-Q8_0.gguf", 7.2e9, "Z-Image")}},
                                {"hashing", 0}};
        host.on("POST", "/fugue/v1/models", [&reply](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, QJsonDocument(reply).toJson(QJsonDocument::Compact)};
        });
        ProviderInstance gx10;
        gx10.name = QStringLiteral("gx10");
        gx10.type = QStringLiteral("fugue-helper");
        gx10.address = QStringLiteral("http://gx10.example:8787");
        HostModelCache cache(scratch.filePath("lists"));
        const HostModelCache::Make reach = [&host](const ProviderInstance &paired) {
            ProviderInstance atTheFake = paired;
            atTheFake.address = QString::fromLatin1(host.origin());
            return createProvider(atTheFake);
        };
        bool fetched = false;
        cache.refresh(gx10, false, {}, [&fetched](const HostModelList &) { fetched = true; }, reach);
        QTRY_VERIFY_WITH_TIMEOUT(fetched, 5000);

        const QString name = theme.isEmpty() ? QStringLiteral("dark") : QStringLiteral("light");
        for (const bool paired : {false, true}) {
            App app(load(loadModels), {}, canvas_test::Look{theme, -1});
            QVERIFY(app.open());
            app.window->resize(760, 640);
            QTRY_COMPARE(app.window->contentItem()->width(), 760.0);
            WidgetEdit *edits = app.model.widgets();
            edits->setModelRoots({models});
            edits->setModelSource([](const QString &, const QStringList &, std::function<void(QList<WidgetEdit::Model>)> done) { done({}); });
            edits->setHostModels([paired, gx10] { return paired ? QList<ProviderInstance>{gx10} : QList<ProviderInstance>(); }, &cache, reach);
            QQuickItem *widget = modelWidget(app, "load", "diffusion_model");
            QVERIFY(widget);
            QTest::mouseClick(app.window, Qt::LeftButton, {}, widget->mapToScene(QPointF(widget->width() / 2, widget->height() / 2)).toPoint());
            QObject *menu = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT((menu = [&] {
                auto *pickers = widget->property("pickers").value<QObject *>();
                QObject *found = pickers ? pickers->findChild<QObject *>("widgetMenu") : nullptr;
                return found && found->property("opened").toBool() ? found : nullptr;
            }()), 5000);
            QStringList headings;
            for (const QVariant &each : menu->property("entries").toList())
                if (each.toMap().value("heading").toBool())
                    headings << each.toMap().value("text").toString();
            if (paired) {
                QVERIFY2(headings.contains(QStringLiteral("On gx10")), qPrintable(headings.join('|')));
                headings.removeAll(QStringLiteral("Download")); // the catalog's downloads come last of all
                QCOMPARE(headings.last(), QStringLiteral("On gx10")); // below every local group
            } else {
                QVERIFY2(!headings.contains(QStringLiteral("On gx10")), qPrintable(headings.join('|')));
            }
            QTest::qWait(300);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/model-picker-%2-%3.png").arg(out, name, paired ? "after" : "before")));
        }
    }
};

QTEST_MAIN(TestCanvasModelHostWindow)
#include "test_canvas_model_host_window.moc"
