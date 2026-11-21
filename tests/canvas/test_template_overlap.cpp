// A finished run must never make a shipped template's nodes overlap (#163). The templates are the
// bundled ones; each node that can show a picture gets the largest result slot a run can give it
// (the tallest picture), and no node rectangle may touch another. Also: a result slot is bounded
// (docs/canvas-look.md 4.6), whatever the picture's aspect.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

QVariantMap node(CanvasModel &canvas, const QString &id) {
    const QHash<int, QByteArray> roles = canvas.nodes()->roleNames();
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i, 0);
        if (canvas.nodes()->data(index, roles.key("nodeId")).toString() != id)
            continue;
        QVariantMap values;
        for (auto it = roles.begin(); it != roles.end(); ++it)
            values.insert(QString::fromUtf8(it.value()), canvas.nodes()->data(index, it.key()));
        return values;
    }
    return {};
}

QRectF rectOf(const QVariantMap &n) {
    return QRectF(n.value("posX").toDouble(), n.value("posY").toDouble(), n.value("nodeWidth").toDouble(),
                  n.value("nodeHeight").toDouble());
}

// A node shows a picture or a clip when it makes one, or is a place to look at one (a sink fed by one).
bool makesPicture(const QVariantMap &n) {
    for (const QVariant &p : n.value("outputs").toList()) {
        const QString type = p.toMap().value("type").toString();
        if (type == "image" || type == "video")
            return true;
    }
    return false;
}

} // namespace

class TestTemplateOverlap : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void aResultSlotIsBoundedWhateverThePicturesAspect() {
        const QByteArray json = R"({"fugue": 1, "graph": {"nodes": {
            "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {}}}},
            "layout": {"nodes": {"show": {"pos": [0, 0]}}}})";
        CanvasModel canvas(&m_registry);
        canvas.setDocument(Document::load(json).value());
        for (const qreal aspect : {0.1, 0.5, 1.0, 1.5, 4.0, 20.0}) {
            canvas.setRunState("show", {{"state", "cached"}, {"thumbnail", "file:///x.png"}, {"thumbnailAspect", aspect}});
            const QRectF slot = node(canvas, "show").value("thumbnailRect").toRectF();
            QVERIFY2(slot.height() <= slot.width() + 0.5, qPrintable(QString::number(aspect)));
            QVERIFY2(slot.height() >= slot.width() * 0.5 - 0.5, qPrintable(QString::number(aspect)));
        }
    }

    void shippedTemplatesDoNotOverlapAfterARun_data() {
        QTest::addColumn<QString>("file");
        const QDir root(QFINDTESTDATA("../../templates"));
        const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &name : names)
            if (QFile::exists(root.filePath(name + "/workflow.fugue")))
                QTest::newRow(qPrintable(name)) << root.filePath(name + "/workflow.fugue");
    }
    void shippedTemplatesDoNotOverlapAfterARun() {
        QFETCH(QString, file);
        QFile source(file);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray bytes = source.readAll();
        const Result<Document> loaded = Document::load(bytes);
        QVERIFY2(loaded.ok(), qPrintable(loaded.error().message));
        CanvasModel canvas(&m_registry);
        canvas.setDocument(loaded.value());
        const QJsonObject graph = QJsonDocument::fromJson(bytes).object().value("graph").toObject().value("nodes").toObject();
        QStringList ids;
        for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
            ids << canvas.nodes()->data(canvas.nodes()->index(i, 0), canvas.nodes()->roleNames().key("nodeId")).toString();

        // Preview, Save and Compare show a picture when what feeds them makes one.
        static const QStringList sinks{"fugue/preview", "fugue/save-image", "fugue/save-video", "fugue/compare-images"};
        const auto shows = [&](const QString &id) {
            const QVariantMap n = node(canvas, id);
            if (makesPicture(n))
                return true;
            if (!sinks.contains(n.value("typeId").toString()))
                return false;
            const QJsonObject inputs = graph.value(id).toObject().value("inputs").toObject();
            for (const QJsonValue &value : inputs) {
                const QString from = value.toObject().value("from").toString().section('.', 0, 0);
                if (!from.isEmpty() && makesPicture(node(canvas, from)))
                    return true;
            }
            return false;
        };
        const auto check = [&](const QString &when) {
            QList<QPair<QString, QRectF>> rects;
            for (const QString &id : ids)
                rects << qMakePair(id, rectOf(node(canvas, id)));
            for (int a = 0; a < rects.size(); ++a)
                for (int b = a + 1; b < rects.size(); ++b)
                    QVERIFY2(!rects[a].second.adjusted(-8, -8, 8, 8).intersects(rects[b].second),
                             qPrintable(QStringLiteral("%1: %2 %3 overlaps %4 %5")
                                            .arg(when, rects[a].first, QString::number(rects[a].second.height()), rects[b].first,
                                                 QString::number(rects[b].second.height()))));
        };
        check("before the run");
        // The worst result a run can give: every node that shows a picture, at the tallest the slot goes.
        for (const QString &id : ids)
            if (shows(id))
                canvas.setRunState(id, {{"state", "cached"}, {"thumbnail", "file:///x.png"}, {"thumbnailAspect", 3.0}});
        check("after the run");
    }
};

QTEST_MAIN(TestTemplateOverlap)
#include "test_template_overlap.moc"
