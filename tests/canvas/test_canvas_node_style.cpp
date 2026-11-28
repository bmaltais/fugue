// The node card's two faces (docs/canvas-look.md 20.14, #466): Compact (the Drift card) and Flat (today's). What the style changes is
// the face's metrics, so a port's row and a wire's end follow it (tested at the model, without a window); what it must not change is
// the document, the selection and the wires, and the face must draw every widget in both (tested in the real window, offscreen).
//
// Opt-in pictures of a small graph in both styles, near and far, on two themes:
//
//   FUGUE_NODE_SHOTS=/tmp/shots build/test_canvas_node_style screenshots

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>

#include "canvas_app.h"
#include "widget_fixture.h"
#include "../test_shard.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::item;
using canvas_test::load;
using canvas_test::Look;

namespace {

// The all-widgets node linked to a Text Concat, as the widget tests have them; zoom 1, the world's origin at the window's top left.
const char *fixture = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "all": {"type": "io.example.widgets/all", "version": "1.0.0", "inputs": {"count": 7, "image": {"from": "src.image"}}},
    "src": {"type": "io.example.widgets/all", "version": "1.0.0", "inputs": {}},
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "join": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": {"src": {"pos": [20, 20]}, "all": {"pos": [300, 20]}, "prompt": {"pos": [620, 20], "title": "Prompt"},
                       "join": {"pos": [620, 200]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

QVariantMap row(QAbstractItemModel *model, int index) {
    QVariantMap values;
    const QHash<int, QByteArray> roles = model->roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        values.insert(QString::fromUtf8(it.value()), model->data(model->index(index, 0), it.key()));
    return values;
}

QVariantMap node(CanvasModel &canvas, const QString &id) {
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
        if (row(canvas.nodes(), i).value("nodeId") == id)
            return row(canvas.nodes(), i);
    return {};
}

QVariantMap port(const QVariantMap &node, const QString &side, const QString &name) {
    for (const QVariant &p : node.value(side).toList())
        if (p.toMap().value("name") == name)
            return p.toMap();
    return {};
}

struct StyleApp : App {
    explicit StyleApp(const QString &style, const char *graph = fixture, const QString &theme = {})
        : App(load(graph), widget_test::addWidgetType, Look{theme, -1, style}) {
        workflows.allowPendingTrust(); // the fixture's node type is a command node: allow its pack, as the trust question would
    }

    // The node's item in the canvas.
    QQuickItem *nodeItem(const QString &id) const {
        QObject *nodes = item(window->contentItem(), "nodes");
        for (int i = 0; nodes && i < nodes->property("count").toInt(); ++i) {
            QQuickItem *found = nullptr;
            QMetaObject::invokeMethod(nodes, "itemAt", Q_RETURN_ARG(QQuickItem *, found), Q_ARG(int, i));
            if (found && found->property("nodeId").toString() == id)
                return found;
        }
        return nullptr;
    }
    // The centre of a port's mark, in world units.
    QPointF dot(const QString &id, const QString &name, bool output) const {
        QQuickItem *node = nodeItem(id);
        QQuickItem *mark = node ? item(node, (output ? "outputDot-" : "inputDot-") + name) : nullptr;
        if (!mark)
            return {-1e9, -1e9};
        return world->mapFromScene(mark->mapToScene(QPointF(mark->width() / 2, mark->height() / 2)));
    }
    QQuickItem *widget(const QString &id, const QString &input) const {
        QList<QQuickItem *> todo{window->contentItem()};
        while (!todo.isEmpty()) {
            QQuickItem *at = todo.takeFirst();
            if (at->objectName() == "inputWidget" && at->property("nodeId").toString() == id && at->property("name").toString() == input)
                return at;
            todo << at->childItems();
        }
        return nullptr;
    }
    QVariantMap link(int index) { return row(model.links(), index); }
    // A wire's two ends are on the marks of the ports it joins.
    void verifyWiresMeetTheirPorts() {
        for (int i = 0; i < model.links()->rowCount(); ++i) {
            const QVariantMap l = link(i);
            const QPointF from = dot(l.value("fromNode").toString(), l.value("fromPort").toString(), true);
            const QPointF to = dot(l.value("toNode").toString(), l.value("toPort").toString(), false);
            QVERIFY2(qAbs(from.x() - l.value("x1").toDouble()) < 1.5 && qAbs(from.y() - l.value("y1").toDouble()) < 1.5,
                     qPrintable(QStringLiteral("output of link %1: mark at %2,%3, wire at %4,%5").arg(i).arg(from.x()).arg(from.y())
                                    .arg(l.value("x1").toDouble()).arg(l.value("y1").toDouble())));
            QVERIFY2(qAbs(to.x() - l.value("x2").toDouble()) < 1.5 && qAbs(to.y() - l.value("y2").toDouble()) < 1.5,
                     qPrintable(QStringLiteral("input of link %1: mark at %2,%3, wire at %4,%5").arg(i).arg(to.x()).arg(to.y())
                                    .arg(l.value("x2").toDouble()).arg(l.value("y2").toDouble())));
        }
    }
};

} // namespace

class TestCanvasNodeStyle : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The model seam: the style is the face's metrics, and a port's row, its y and the node's height follow them.
    void portAnchorsFollowTheStyle() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        const Result<Document> loaded = Document::load(R"({"fugue": 1, "graph": {"nodes": {
            "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}}}},
          "layout": {"nodes": {"prompt": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}}}})");
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());

        // Flat: 7u header, 6u rows, 60u wide, 1u above the first row (the numbers of canvas-look.md 4).
        QCOMPARE(canvas.nodeStyle(), QString("flat"));
        QCOMPARE(canvas.rowHeight(), 24.0);
        QCOMPARE(port(node(canvas, "gen"), "outputs", "image").value("y").toDouble(), 28.0 + 4 + 12);
        QCOMPARE(node(canvas, "gen").value("nodeWidth").toDouble(), 240.0);
        const double flatHeight = node(canvas, "gen").value("nodeHeight").toDouble();

        canvas.setNodeStyle("compact");
        // Compact: the same header, 6.5u rows (26 px), 56u wide (224 px), half a unit above the first row.
        QCOMPARE(canvas.rowHeight(), 26.0);
        QCOMPARE(canvas.headerHeight(), 28.0);
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(gen.value("nodeWidth").toDouble(), 224.0);
        QCOMPARE(port(gen, "outputs", "image").value("rowTop").toDouble(), 28.0 + 2);
        QCOMPARE(port(gen, "outputs", "image").value("y").toDouble(), 28.0 + 2 + 13);
        // Every port sits at the middle of a normal row, whatever the row's own height (a multiline's port is level with the first
        // line's row), and the node ends under its last row.
        double bottom = 0;
        for (const QVariant &p : gen.value("inputs").toList()) {
            const QVariantMap input = p.toMap();
            if (input.value("hidden").toBool())
                continue;
            QCOMPARE(input.value("y").toDouble(), input.value("rowTop").toDouble() + 13.0);
            bottom = std::max(bottom, input.value("rowTop").toDouble() + input.value("rowHeight").toDouble());
        }
        QVERIFY(gen.value("nodeHeight").toDouble() >= bottom + 6.0); // a section header takes a row of its own
        // A wire's end is the port's y in the node's own place: the link ends moved with the rows.
        const QVariantMap link = row(canvas.links(), 0);
        QCOMPARE(link.value("y1").toDouble(), node(canvas, "prompt").value("posY").toDouble() + 28 + 2 + 13);
        QCOMPARE(link.value("y2").toDouble(), gen.value("posY").toDouble() + port(gen, "inputs", "prompt").value("y").toDouble());
        QVERIFY(gen.value("nodeHeight").toDouble() != flatHeight);

        // Back to Flat: today's numbers exactly.
        canvas.setNodeStyle("flat");
        QCOMPARE(canvas.rowHeight(), 24.0);
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), flatHeight);
        QCOMPARE(node(canvas, "gen").value("nodeWidth").toDouble(), 240.0);
    }

    // #476: in Compact an output shares its row with a socket-only input (a picture in, a picture out), so the card is a row shorter;
    // an input with a widget keeps its own row, and Flat keeps one row per port.
    void anOutputSharesItsRowWithASocketOnlyInputInCompact() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        const Result<Document> loaded = Document::load(R"({"fugue": 1, "graph": {"nodes": {
            "blur": {"type": "fugue/blur", "version": "1.0.0", "inputs": {}}}},
          "layout": {"nodes": {"blur": {"pos": [0, 0]}}}})");
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());
        const QVariantMap flat = node(canvas, "blur");
        QVERIFY(port(flat, "inputs", "image").value("rowTop").toDouble() > port(flat, "outputs", "image").value("rowTop").toDouble());
        QVERIFY(!port(flat, "inputs", "image").value("sharesRow").toBool());

        canvas.setNodeStyle("compact");
        const QVariantMap compact = node(canvas, "blur");
        const QVariantMap in = port(compact, "inputs", "image"), out = port(compact, "outputs", "image");
        QVERIFY(in.value("sharesRow").toBool() && out.value("sharesRow").toBool());
        QCOMPARE(in.value("rowTop").toDouble(), out.value("rowTop").toDouble());
        QCOMPARE(in.value("y").toDouble(), out.value("y").toDouble());
        // The first value input comes right under the shared row, and the node is a row shorter than one row per port would be.
        for (const QVariant &p : compact.value("inputs").toList()) {
            const QVariantMap input = p.toMap();
            if (input.value("widget").toBool() && !input.value("hidden").toBool()) {
                QCOMPARE(input.value("rowTop").toDouble(), out.value("rowTop").toDouble() + 26.0);
                QVERIFY(!input.value("sharesRow").toBool());
                break;
            }
        }
        canvas.setNodeStyle("flat");
        QCOMPARE(node(canvas, "blur").value("nodeHeight").toDouble(), flat.value("nodeHeight").toDouble());
        QVERIFY(!port(node(canvas, "blur"), "inputs", "image").value("sharesRow").toBool());
    }

    // A socket-only input with a `shown_when` never shares an output's row, shown or not: when its condition flipped, the pairs and
    // every row under them would move.
    void anInputWithAConditionNeverSharesARow() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const Result<NodeType> type = NodeType::fromJson(QJsonDocument::fromJson(R"({"manifest": 1, "id": "io.example.cond/mix",
            "version": "1.0.0", "name": "Mix", "runtime": {"kind": "builtin"},
            "inputs": [{"name": "mode", "type": "text", "choices": ["over", "under"], "default": "over"},
                       {"name": "under", "type": "image", "optional": true, "shown_when": {"mode": ["under"]}},
                       {"name": "image", "type": "image"}],
            "outputs": [{"name": "image", "type": "image"}]})").object());
        QVERIFY(type.ok());
        QVERIFY(registry.add(type.value()).isEmpty());
        CanvasModel canvas(&registry);
        const Result<Document> loaded = Document::load(R"({"fugue": 1, "graph": {"nodes": {
            "mix": {"type": "io.example.cond/mix", "version": "1.0.0", "inputs": {"mode": "under"}}}},
          "layout": {"nodes": {"mix": {"pos": [0, 0]}}}})");
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());
        canvas.setNodeStyle("compact");
        QVERIFY(!port(node(canvas, "mix"), "inputs", "under").value("sharesRow").toBool()); // shown now, and still not paired
        QVERIFY(port(node(canvas, "mix"), "inputs", "image").value("sharesRow").toBool());
        QCOMPARE(canvas.widgets()->setValue("mix", "mode", "over"), QString());
        QVERIFY(port(node(canvas, "mix"), "inputs", "under").value("notUsed").toBool());
        QVERIFY(port(node(canvas, "mix"), "inputs", "image").value("sharesRow").toBool()); // the same pair as before
    }

    // #476: the model says each node's top-level category, which colours the Compact header's dot.
    void aNodeSaysItsTopLevelCategory() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        const Result<Document> loaded = Document::load(R"({"fugue": 1, "graph": {"nodes": {
            "blur": {"type": "fugue/blur", "version": "1.0.0", "inputs": {}},
            "gone": {"type": "io.example.nothing/here", "version": "1.0.0", "inputs": {}}}},
          "layout": {"nodes": {"blur": {"pos": [0, 0]}, "gone": {"pos": [300, 0]}}}})");
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());
        QCOMPARE(node(canvas, "blur").value("category").toString(), QString("Image"));
        QCOMPARE(node(canvas, "gone").value("category").toString(), QString());
    }

    // A node of the file's own size keeps it in both styles; the style writes nothing into the document.
    void theStyleChangesNoDocument() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        const Result<Document> loaded = Document::load(R"({"fugue": 1, "graph": {"nodes": {
            "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}}}},
          "layout": {"nodes": {"prompt": {"pos": [10, 20], "size": [300, 200]}}}})");
        QVERIFY(loaded.ok());
        canvas.setDocument(loaded.value());
        const QByteArray before = canvas.document().save();
        canvas.setNodeStyle("compact");
        QCOMPARE(node(canvas, "prompt").value("nodeWidth").toDouble(), 300.0);
        QCOMPARE(node(canvas, "prompt").value("nodeHeight").toDouble(), 200.0);
        QCOMPARE(canvas.document().save(), before);
        QVERIFY(!canvas.undoStack()->canUndo());
        canvas.setNodeStyle("flat");
        QCOMPARE(canvas.document().save(), before);
    }

    // One window per style: the widget set draws, every widget has its item, and each wire meets its ports' marks.
    void everyWidgetDrawsAndWiresMeetTheirPortsInBothStyles_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<double>("rowHeight");
        QTest::newRow("compact") << "compact" << 26.0;
        QTest::newRow("flat") << "flat" << 24.0;
    }
    void everyWidgetDrawsAndWiresMeetTheirPortsInBothStyles() {
        QFETCH(QString, style);
        QFETCH(double, rowHeight);
        StyleApp app(style);
        QVERIFY(app.open());
        QCOMPARE(app.model.nodeStyle(), style);
        QCOMPARE(app.model.rowHeight(), rowHeight);
        QCOMPARE(app.theme.look()->property("nodeStyle").toString(), style);
        QQuickItem *all = app.nodeItem("all");
        QVERIFY(all);
        QTRY_VERIFY(all->property("rowsMade").toBool());
        QCOMPARE(all->height(), node(app.model, "all").value("nodeHeight").toDouble());
        // Every unfolded input with a widget has its item, and each widget has the size and place of its row.
        int widgets = 0;
        for (const QVariant &p : node(app.model, "all").value("inputs").toList()) {
            const QVariantMap input = p.toMap();
            if (input.value("hidden").toBool() || !input.value("widget").toBool())
                continue;
            QQuickItem *w = app.widget("all", input.value("name").toString());
            if (input.value("wired").toBool()) {
                QVERIFY2(!w || !w->property("editable").toBool(), qPrintable(input.value("name").toString()));
                continue;
            }
            QVERIFY2(w, qPrintable(input.value("name").toString()));
            QVERIFY2(w->height() > 0 && w->width() > 0, qPrintable(input.value("name").toString()));
            ++widgets;
        }
        QVERIFY2(widgets >= 9, qPrintable(QString::number(widgets)));
        app.verifyWiresMeetTheirPorts();
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The setting is live: nothing is reloaded, the document is not touched, the selection and the wires stay, and the wires follow.
    void theSettingSwitchesLiveAndKeepsTheSelectionAndTheWires() {
        StyleApp app("compact");
        QVERIFY(app.open());
        QVERIFY(app.nodeItem("all"));
        app.model.select({"all", "join"});
        const QByteArray before = app.model.document().save();
        const int steps = app.model.undoStack()->count();
        const double compactHeight = app.nodeItem("join")->height();
        QCOMPARE(app.model.links()->rowCount(), 2);
        app.verifyWiresMeetTheirPorts();

        QCOMPARE(app.settings.set("node_style", "flat"), QString());
        QTRY_COMPARE(app.model.nodeStyle(), QString("flat"));
        QCOMPARE(app.theme.look()->property("nodeStyle").toString(), QString("flat"));
        QCOMPARE(app.model.rowHeight(), 24.0);
        QVERIFY(app.nodeItem("join")->height() != compactHeight);
        QStringList selected = app.model.selectedIds();
        selected.sort();
        QCOMPARE(selected, QStringList({"all", "join"}));
        QCOMPARE(app.model.links()->rowCount(), 2);
        QCOMPARE(app.model.document().save(), before);
        QCOMPARE(app.model.undoStack()->count(), steps);
        QTest::qWait(50);
        app.verifyWiresMeetTheirPorts();

        QCOMPARE(app.settings.set("node_style", "compact"), QString());
        QTRY_COMPARE(app.model.nodeStyle(), QString("compact"));
        QCOMPARE(app.nodeItem("join")->height(), compactHeight);
        QTest::qWait(50);
        app.verifyWiresMeetTheirPorts();
        QVERIFY(!app.settings.set("node_style", "round").isEmpty()); // only the two names
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The presets carry the style as data: Flat sets it to flat, Calm to compact; no code looks at a preset's name.
    void thePresetsSetTheNodeStyle() {
        StyleApp app("compact");
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset("flat"), QString());
        QCOMPARE(app.settings.nodeStyle(), QString("flat"));
        QCOMPARE(app.settings.lookPreset(), QString("flat")); // the style does not make it Custom
        QTRY_COMPARE(app.model.nodeStyle(), QString("flat"));
        // The two can be mixed: Compact under the Flat preset is still the Flat preset.
        QCOMPARE(app.settings.set("node_style", "compact"), QString());
        QCOMPARE(app.settings.lookPreset(), QString("flat"));
        QCOMPARE(app.settings.applyLookPreset("calm"), QString());
        QCOMPARE(app.settings.nodeStyle(), QString("compact"));
        QTRY_COMPARE(app.model.nodeStyle(), QString("compact"));
    }

    // The keys of a Compact widget are the Flat widget's: a click opens the field, Tab goes to the next typed field, Escape leaves.
    void tabAndEnterEditACompactWidget() {
        StyleApp app("compact");
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(360, 30)); // select the node
        const auto center = [&](const QString &input) {
            QQuickItem *w = app.widget("all", input);
            return w->mapToScene(QPointF(w->width() / 2, w->height() / 2)).toPoint();
        };
        QTest::mouseClick(app.window, Qt::LeftButton, {}, center("name"));
        QTRY_VERIFY(app.commands.textFieldFocus());
        QCOMPARE(app.window->activeFocusItem()->objectName(), QString("widgetInput"));
        for (const char c : {'a', 'b'})
            QTest::keyClick(app.window, c);
        QTest::keyClick(app.window, Qt::Key_Tab); // the next typed field (notes), not node search
        QTRY_COMPARE(app.window->activeFocusItem()->objectName(), QString("widgetArea"));
        QCOMPARE(app.model.document().graph.nodes.value("all").inputs.value("name").toJson(), QJsonValue("ab")); // typed over the selected value
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_VERIFY(!app.commands.textFieldFocus());
    }

    // Opt-in pictures: a small graph in both styles, near and far, on two themes.
    void screenshots_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<QString>("theme");
        QTest::addColumn<double>("zoom");
        for (const char *theme : {"tokyo-night", "catppuccin-latte"})
            for (const char *style : {"compact", "flat"})
                for (const double zoom : {1.0, 0.3})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-z%3").arg(style, theme).arg(zoom)))
                        << QString(style) << QString(theme) << zoom;
    }
    void screenshots() {
        QFETCH(QString, style);
        QFETCH(QString, theme);
        QFETCH(double, zoom);
        const QString out = qEnvironmentVariable("FUGUE_NODE_SHOTS");
        if (out.isEmpty())
            QSKIP("set FUGUE_NODE_SHOTS to a folder to take the pictures");
        if (!QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        // FUGUE_NODE_GRAPH: a .fugue file to draw instead of the fixture (its own view, no run states).
        const QString file = qEnvironmentVariable("FUGUE_NODE_GRAPH");
        QByteArray graph = fixture;
        if (!file.isEmpty()) {
            QFile in(file);
            QVERIFY(in.open(QIODevice::ReadOnly));
            graph = in.readAll();
        }
        StyleApp app(style, graph.constData(), theme);
        QVERIFY(app.open());
        if (file.isEmpty()) {
            app.model.moveView(480, 300, zoom);
            app.model.setRunState("all", {{"state", "running"}, {"progress", 0.62}});
            app.model.select({"prompt"});
        } else {
            app.model.moveView(480, 300, zoom);
        }
        QTest::qWait(500);
        const QImage shot = test_support::grab(app.window);
        QVERIFY(shot.save(QStringLiteral("%1/nodes-%2-%3-z%4.png").arg(out, style, theme).arg(int(zoom * 100))));
    }
};

FUGUE_TEST_MAIN(TestCanvasNodeStyle, QGuiApplication)
#include "test_canvas_node_style.moc"
