// The canvas look (docs/canvas-look.md) in the real window: every node state draws from the
// tokens, with nothing warned about, and opt-in screenshots of the look on several Omarchy
// themes and corner radii to compare with the mockups in docs/canvas-look/shots/.
//
// The two boards here are also the way to see the states #19 will set from a run (running,
// cached, stale, frozen, error, ...) before there is one:
//
//   FUGUE_LOOK_SHOTS=/tmp/shots QT_QUICK_BACKEND=rhi build/test_canvas_look screenshots
//
// writes <board>-<theme>-r<radius>.png for each. QT_QUICK_BACKEND=rhi draws offscreen with the
// GPU, as the app does on a desktop; without it Qt draws in software, which has no shaders, so
// the dot grid and rounded thumbnails are missing from the pictures.

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickStyle>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>

#include "canvas_app.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;
using canvas_test::Look;

namespace {

// A stand-in for a generated image, as the mockups draw one: a fox in a misty birch wood.
QString writeScene(const QString &path, int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 0, height);
    sky.setColorAt(0, QColor("#f2c49e"));
    sky.setColorAt(0.48, QColor("#b9c3b4"));
    sky.setColorAt(1, QColor("#5e776a"));
    p.fillRect(image.rect(), sky);
    QRadialGradient sun(width * 0.7, height * 0.26, width * 0.42);
    sun.setColorAt(0, QColor(255, 236, 200, 255));
    sun.setColorAt(0.3, QColor(255, 206, 160, 150));
    sun.setColorAt(1, QColor(255, 206, 160, 0));
    p.fillRect(image.rect(), sun);
    QLinearGradient mist(0, height, 0, height * 0.62);
    mist.setColorAt(0, QColor(232, 226, 214, 216));
    mist.setColorAt(1, QColor(232, 226, 214, 0));
    p.fillRect(image.rect(), mist);
    for (const auto &[x, w, a] : {std::tuple{0.12, 0.014, 230}, {0.26, 0.01, 140}, {0.79, 0.02, 242}, {0.9, 0.008, 128}})
        p.fillRect(QRectF(width * x, 0, width * w, height), QColor(240, 236, 228, a));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#d9652b"));
    p.drawEllipse(QPointF(width * 0.57, height * 0.77), width * 0.09, height * 0.055);
    p.setBrush(QColor("#e2733a"));
    p.drawEllipse(QPointF(width * 0.66, height * 0.7), width * 0.03, height * 0.045);
    p.setBrush(QColor("#c95a24"));
    p.drawEllipse(QPointF(width * 0.46, height * 0.8), width * 0.055, height * 0.018);
    p.end();
    image.save(path);
    return QUrl::fromLocalFile(path).toString();
}

// A real workflow from built-in nodes, laid out like docs/canvas-look/canvas.html: a frame,
// a comment, a selection, a running node, a bypassed one, an error, a reroute point.
const char *workflow = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0",
               "inputs": {"text": "a red fox in a misty birch wood at dawn, volumetric light, 35 mm, soft grain"}},
    "start": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "~/Pictures/fox-sketch.png"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "prompt.text"}, "negative_prompt": {"from": "prompt.text"},
                       "image": {"from": "start.image"}, "seed": 4818231, "steps": 8, "guidance": 3.5}},
    "up": {"type": "fugue/resize", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "width": 2048}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "up.image"}, "name": "fox"}},
    "sharp": {"type": "fugue/brightness-contrast", "version": "1.0.0", "mode": "bypass",
              "inputs": {"image": {"from": "gen.image"}, "contrast": 1.2}},
    "prev": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "sharp.image"}}},
    "edit": {"type": "fugue/edit-image", "version": "1.0.0",
             "inputs": {"images": [{"from": "gen.image"}], "instruction": "make it snow"}}
  }},
  "layout": {
    "nodes": {"prompt": {"pos": [60, 150], "size": [280, 0]}, "start": {"pos": [60, 410], "size": [280, 0]},
              "gen": {"pos": [420, 70], "size": [260, 0]}, "up": {"pos": [770, 120], "size": [250, 0]},
              "save": {"pos": [1110, 120], "size": [250, 0], "pinned": true},
              "sharp": {"pos": [770, 330], "size": [230, 0]}, "prev": {"pos": [1110, 400], "size": [250, 0]},
              "edit": {"pos": [770, 520], "size": [250, 0]}},
    "frames": [{"id": "f1", "title": "Inputs", "rect": [40, 96, 320, 470], "color": "muted", "nodes": ["prompt", "start"]}],
    "links": {"edit.images#0": {"points": [[720, 600]]}},
    "comments": [{"id": "c1", "text": "Try 8 steps for drafts, 20 for finals. The seed stays fixed so a change shows on its own.",
                  "pos": [48, 590], "size": [232, 60]}],
    "view": {"center": [800, 500], "zoom": 1}
  }
})";

// Every node state side by side (docs/canvas-look/states.html), each with its name under it.
struct State {
    const char *id, *caption;
    QVariantMap run;
};

QList<State> states(const QString &thumbnail) {
    return {{"cached", "Default (cached)", {{"state", "cached"}, {"seconds", 0.8}}},
            {"hover", "Hover", {{"state", "cached"}, {"seconds", 0.8}}},
            {"selected", "Selected", {{"state", "cached"}, {"seconds", 0.8}}},
            {"running", "Running", {{"state", "running"}, {"progress", 0.42}}},
            {"stale", "Stale (would run)", {{"state", "stale"}}},
            {"frozen", "Frozen", {{"state", "frozen"}}},
            {"pinned", "Pinned", {{"state", "cached"}, {"seconds", 0.8}}},
            {"error", "Error", {{"state", "error"}, {"error", "Model file is missing: 4x-UltraSharp.pth"}}},
            {"bypassed", "Bypassed", {}},
            {"muted", "Muted", {}},
            {"skipped", "Skipped", {{"state", "skipped"}}},
            {"missing", "Missing type", {}},
            {"collapsed", "Collapsed", {{"state", "cached"}}},
            {"color", "Node color: blue", {{"state", "cached"}}},
            {"warned", "Warning in title", {{"state", "cached"}, {"warnings", QStringList{"slow"}}}},
            {"thumb", "With its result", {{"state", "cached"}, {"seconds", 2.4}, {"thumbnail", thumbnail},
                                         {"thumbnailAspect", 0.5625}, {"thumbnailInfo", "1024 × 576"},
                                         {"values", QVariantMap{{"image", ""}}}}}};
}

Document statesBoard() {
    QJsonObject nodes, layout, sources;
    QJsonArray comments;
    const QList<State> all = states({});
    for (int i = 0; i < all.size(); ++i) {
        const QString id = all[i].id;
        const int column = i % 6, row = i / 6;
        const QJsonArray pos{56 + column * 252, 60 + row * 300};
        // Each takes its image from its left neighbour, so its input shows wired.
        QJsonObject inputs{{"contrast", 1.2}};
        if (column > 0)
            inputs.insert("image", QJsonObject{{"from", QString(all[i - 1].id) + ".image"}});
        QJsonObject node{{"type", id == "missing" ? "acme.tools/denoise" : "fugue/brightness-contrast"},
                         {"version", "1.0.0"},
                         {"inputs", inputs}};
        if (id == "bypassed")
            node.insert("mode", "bypass");
        if (id == "muted")
            node.insert("mode", "mute");
        nodes.insert(id, node);
        QJsonObject entry{{"pos", pos}, {"size", QJsonArray{200, 0}}, {"title", id == "missing" ? "" : "Sharpen"}};
        if (id == "missing")
            entry.remove("title");
        if (id == "pinned")
            entry.insert("pinned", true);
        if (id == "collapsed")
            entry.insert("collapsed", true);
        if (id == "color")
            entry.insert("color", "blue");
        layout.insert(id, entry);
        comments.append(QJsonObject{{"id", "c" + id}, {"text", all[i].caption},
                                    {"pos", QJsonArray{56 + column * 252, 60 + row * 300 - 26}}, {"size", QJsonArray{200, 20}}});
    }
    const QJsonObject json{{"fugue", 1},
                           {"graph", QJsonObject{{"nodes", nodes}}},
                           {"layout", QJsonObject{{"nodes", layout}, {"comments", comments},
                                                  {"view", QJsonObject{{"center", QJsonArray{800, 500}}, {"zoom", 1}}}}}};
    const Result<Document> made = Document::fromJson(json);
    if (!made.ok())
        qFatal("states board does not load: %s", qPrintable(made.error().message));
    return made.value();
}

// Opens the window at the mockups' size with the view at the layout's.
bool openAt(App &app, int width = 1600, int height = 1000) {
    if (!app.open())
        return false;
    app.window->resize(width, height);
    QTest::qWait(50);
    QMetaObject::invokeMethod(app.canvas, "applyView");
    return true;
}

void setStates(App &app, const QString &thumbnail) {
    // The placeholder's pack is not allowed, and that is not what this board shows.
    app.workflows.dismissPendingTrust();
    app.workflows.dismissNotice();
    for (const State &state : states(thumbnail))
        app.model.setRunState(state.id, state.run);
    app.model.select({"selected"});
    app.canvas->setProperty("hoveredNode", "hover");
}

} // namespace

class TestCanvasLook : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Every state draws with the tokens, and nothing in the QML complains.
    void everyStateDrawsFromTheTokens() {
        App app(statesBoard());
        QVERIFY(openAt(app));
        setStates(app, writeScene(app.dir.filePath("scene.png"), 512, 288));
        QTest::qWait(200);
        const QImage shot = app.window->grabWindow();
        const CanvasLook *look = app.theme.look();
        auto at = [&](qreal x, qreal y) { return QColor(shot.pixel(app.screen(x, y))).name(); };
        // The canvas between nodes, a node's body, its header, an error's header (cached sits at
        // column 0, row 0: x 56 to 256, y 60 on; error at column 1, row 1).
        QCOMPARE(at(40, 400), look->tokens().canvas.name());
        QCOMPARE(at(56 + 100, 60 + 28 + 4 + 24 + 12 + 2), look->tokens().body.name()); // between rows, left of the label
        QCOMPARE(at(56 + 120, 60 + 6), look->tokens().header.name());
        QCOMPARE(at(56 + 252 + 120, 360 + 6), look->tokens().errorHeader.name());
        // The error's message sits in a chip under it.
        QVERIFY(!app.model.nodes()->data(app.model.nodes()->index(0, 0), NodeList::Error).isNull());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Bypassed reads at a glance, as in ComfyUI: the whole node tinted magenta; muted is faded
    // toward the canvas and not tinted. On a dark and a light theme, square and round.
    void bypassedAndMutedAreDistinct_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("radius");
        QTest::addRow("dark, round") << "ethereal" << 8;
        QTest::addRow("light, square") << "flexoki-light" << 0;
        QTest::addRow("fallback palette") << "" << -1;
    }

    void bypassedAndMutedAreDistinct() {
        QFETCH(QString, theme);
        QFETCH(int, radius);
        if (!theme.isEmpty() && !QFileInfo::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        App app(statesBoard(), {}, Look{theme, radius});
        QVERIFY(openAt(app));
        setStates(app, writeScene(app.dir.filePath("scene.png"), 512, 288));
        QTest::qWait(200);
        const QImage shot = app.window->grabWindow();
        const fugue::look::Tokens &t = app.theme.look()->tokens();
        // The board's nodes: cached at (56, 60), bypassed at (560, 360), muted at (812, 360).
        auto header = [&](qreal x, qreal y) { return shot.pixelColor(app.screen(x + 120, y + 6)); };
        auto body = [&](qreal x, qreal y) { return shot.pixelColor(app.screen(x + 100, y + 28 + 4 + 24 + 12 + 2)); };
        auto near = [](const QColor &a, const QColor &b) {
            return qAbs(a.red() - b.red()) <= 2 && qAbs(a.green() - b.green()) <= 2 && qAbs(a.blue() - b.blue()) <= 2;
        };
        QVERIFY2(near(header(560, 360), t.bypassHeader), qPrintable(header(560, 360).name() + " vs " + t.bypassHeader.name()));
        QVERIFY2(near(body(560, 360), t.bypassBody), qPrintable(body(560, 360).name() + " vs " + t.bypassBody.name()));
        // The tint is a visible step from the plain node, not a hair.
        QVERIFY(fugue::look::deltaE(body(560, 360), body(56, 60)) > 0.02);
        QVERIFY(fugue::look::deltaE(header(560, 360), header(56, 60)) > 0.03);
        // Muted: the plain colors faded toward the canvas (its shadow darkens that a little), so
        // neither tinted nor plain: nearer the canvas than the body.
        const QColor faded = body(812, 360);
        QVERIFY2(fugue::look::deltaE(faded, t.canvas) < fugue::look::deltaE(faded, t.body), qPrintable(faded.name()));
        QVERIFY(fugue::look::deltaE(faded, t.canvas) > 0.005);
        QVERIFY(fugue::look::deltaE(body(812, 360), body(560, 360)) > 0.02);
        if (const QString out = qEnvironmentVariable("FUGUE_LOOK_SHOTS"); !out.isEmpty())
            QVERIFY(shot.save(QStringLiteral("%1/bypass-%2-r%3.png").arg(out, theme.isEmpty() ? "default" : theme).arg(radius)));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void screenshots_data() {
        QTest::addColumn<QString>("board");
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("radius");
        for (const char *board : {"canvas", "states", "search", "overlay"})
            for (const auto &[theme, radius] : {std::pair{"ethereal", 0}, {"ethereal", 8}, {"flexoki-light", 0},
                                                {"flexoki-light", 8}, {"rose-pine", 8}, {"tokyo-night", 8}, {"white", 0}})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-r%3").arg(board, theme).arg(radius)))
                    << QString(board) << QString(theme) << radius;
    }

    void screenshots() {
        QFETCH(QString, board);
        QFETCH(QString, theme);
        QFETCH(int, radius);
        const QString out = qEnvironmentVariable("FUGUE_LOOK_SHOTS");
        if (out.isEmpty())
            QSKIP("set FUGUE_LOOK_SHOTS to a folder to take the screenshots");
        if (!QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        App app(board == "states" ? statesBoard() : load(workflow), {}, Look{theme, radius});
        QTRY_COMPARE(app.theme.radius(), radius);
        QVERIFY(openAt(app));
        if (board != "states") {
            const QString scene = writeScene(app.dir.filePath("scene.png"), 512, 512);
            app.model.setRunState("prompt", {{"state", "cached"}});
            app.model.setRunState("start", {{"state", "cached"}, {"seconds", 0.1}});
            app.model.setRunState("gen", {{"state", "cached"}, {"seconds", 6.2}, {"thumbnail", scene},
                                          {"thumbnailInfo", "1024 × 1024"}, {"values", QVariantMap{{"seed", "4818231"}}}});
            app.model.setRunState("up", {{"state", "running"}, {"progress", 0.42}});
            app.model.setRunState("save", {{"state", "stale"}});
            app.model.setRunState("prev", {{"state", "cached"}, {"seconds", 0.1}, {"thumbnail", scene},
                                           {"thumbnailInfo", "1024 × 1024 · PNG"}});
            app.model.setRunState("edit", {{"state", "error"},
                                           {"error", "Out of GPU memory: needs 9.1 GB, 6.2 GB free. Lower the steps or free a model."}});
            app.model.select({"gen"});
        } else {
            setStates(app, writeScene(app.dir.filePath("scene.png"), 512, 288));
        }
        if (board == "search") {
            // Search from Tab at a point on the canvas, as docs/canvas-look/search.html shows it.
            app.model.search()->openAt(250, 150);
            QTRY_VERIFY(app.engine.rootObjects().first()->findChild<QObject *>("nodeSearch")->property("opened").toBool());
            for (const char c : {'i', 'm', 'a'})
                QTest::keyClick(app.window, c);
        } else if (board == "overlay") {
            QTest::keyClick(app.window, '?');
            QTRY_VERIFY(app.engine.rootObjects().first()->findChild<QObject *>("help")->property("opened").toBool());
        }
        QTest::qWait(400);
        if (board == "states")
            app.canvas->setProperty("hoveredNode", "hover");
        QTest::qWait(50);
        const QImage shot = app.window->grabWindow();
        QVERIFY(shot.save(QStringLiteral("%1/%2-%3-r%4.png").arg(out, board, theme).arg(radius)));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The wire dragged from any port has the color it will have once attached: a text wire is
    // the text port's color, from an output, an unconnected input, or an input whose link
    // was picked up, also right after a link was deleted.
    void aDraggedWireIsInThePortColor_data() {
        QTest::addColumn<bool>("deleteFirst");
        QTest::addColumn<QPointF>("from");
        QTest::addColumn<bool>("hover");
        QTest::addRow("output after delete") << true << QPointF(260, 144) << false;
        QTest::addRow("unconnected input after delete") << true << QPointF(340, 224) << false;
        QTest::addRow("picked-up input") << false << QPointF(340, 224) << false;
        QTest::addRow("output, link kept") << false << QPointF(260, 144) << false;
        QTest::addRow("output onto input") << true << QPointF(260, 144) << true;
        QTest::addRow("input onto output") << true << QPointF(340, 224) << true;
    }

    void aDraggedWireIsInThePortColor() {
        QFETCH(bool, deleteFirst);
        QFETCH(bool, hover);
        QFETCH(QPointF, from);
        App app(load(R"({"fugue": 1, "graph": {"nodes": {
            "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}}}},
          "layout": {"nodes": {"prompt": {"pos": [20, 100]}, "gen": {"pos": [340, 60]}},
                     "view": {"center": [480, 340], "zoom": 1}}})"));
        QVERIFY(openAt(app, 900, 700));
        QCOMPARE(app.model.links()->rowCount(), 1);
        if (deleteFirst) {
            // Delete the link with its cut handle, then press on a port.
            QTest::mouseMove(app.window, app.screen(300, 184));
            auto *cut = canvas_test::item(app.window->contentItem(), "cutHandle");
            QTRY_VERIFY(cut->isVisible());
            QTest::mouseClick(app.window, Qt::LeftButton, {}, cut->mapToScene(QPointF(cut->width() / 2, cut->height() / 2)).toPoint());
            QCOMPARE(app.model.links()->rowCount(), 0);
        }
        const QPointF to = hover ? (from.x() > 300 ? QPointF(260, 144) : QPointF(340, 224)) : QPointF(150, 450);
        const QPoint start = app.screen(from.x(), from.y()), end = app.screen(to.x(), to.y());
        QTest::mousePress(app.window, Qt::LeftButton, {}, start);
        // The pointer leaves from over its own port first, which is no target (an invalid
        // wire), and then goes on.
        QTest::mouseMove(app.window, start + QPoint(3, 3));
        QTest::qWait(100);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, start + (end - start) * i / 8);
        QTRY_VERIFY(app.canvas->property("dragging").toBool());
        QTest::qWait(100);
        const QImage shot = app.window->grabWindow();
        const QColor want = app.theme.look()->property("ports").toMap().value("text").value<QColor>();
        int right = 0;
        const QRect box = hover ? QRect(app.screen(262, 130), app.screen(338, 230)) : QRect(app.screen(100, 260), app.screen(335, 470));
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                if (qAbs(c.red() - want.red()) + qAbs(c.green() - want.green()) + qAbs(c.blue() - want.blue()) < 30)
                    ++right;
            }
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, end);
        QVERIFY2(right > 40, qPrintable(QStringLiteral("want %1: %2 pixels right").arg(want.name()).arg(right)));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasLook)
#include "test_canvas_look.moc"
