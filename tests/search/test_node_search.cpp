// Node search without a window (spec 12.8): ranking, filtering by a dragged link's port,
// and what accepting a result does to the canvas.

#include <QtTest>

#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "search/node_search.h"

using namespace fugue;

namespace {

Document load(const QByteArray &json) {
    const Result<Document> loaded = Document::load(json);
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}, "save": {"pos": [660, 300]}}}
})";

QStringList names(SearchResults *results) {
    QStringList out;
    for (int i = 0; i < results->rowCount(); ++i)
        out << results->data(results->index(i), SearchResults::Name).toString();
    return out;
}

} // namespace

class TestNodeSearch : public QObject {
    Q_OBJECT

    Registry registry;
    std::unique_ptr<CanvasModel> canvas;
    NodeSearch *search = nullptr;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(registry); }
    void init() {
        canvas = std::make_unique<CanvasModel>(&registry);
        canvas->setDocument(load(small));
        search = canvas->search();
    }

    void emptyQueryListsEveryNodeTypeByCategory() {
        search->openAt(10, 20);
        QVERIFY(search->active());
        QCOMPARE(search->count(), int(registry.nodeTypes().size()));
        QCOMPARE(names(search->results()).first(), QString("Load Image")); // Basic, then by name
    }

    void ranksNameBeforeKeywordBeforeDescription() {
        search->openAt(0, 0);
        search->setQuery("blend");
        QCOMPARE(names(search->results()).first(), QString("Blend"));
        search->setQuery("upscale"); // a keyword of Resize
        QCOMPARE(names(search->results()).first(), QString("Resize"));
        search->setQuery("bright");
        QCOMPARE(names(search->results()).first(), QString("Brightness and Contrast"));
    }

    void matchesWordsInAnyOrderCaseInsensitively() {
        search->openAt(0, 0);
        search->setQuery("CROP image");
        QCOMPARE(names(search->results()).first(), QString("Crop"));
        search->setQuery("contrast bright");
        QCOMPARE(names(search->results()).first(), QString("Brightness and Contrast"));
    }

    void matchesLettersInOrderWhenNothingBetterDoes() {
        search->openAt(0, 0);
        search->setQuery("brct");
        QCOMPARE(names(search->results()), QStringList{"Brightness and Contrast"});
    }

    void matchesTheCategory() {
        search->openAt(0, 0);
        search->setQuery("transform");
        const QStringList found = names(search->results());
        QCOMPARE(QSet<QString>(found.begin(), found.end()), (QSet<QString>{"Crop", "Resize"}));
    }

    void noMatchGivesNoResults() {
        search->openAt(0, 0);
        search->setQuery("zzzzqq");
        QCOMPARE(search->count(), 0);
    }

    void resultsCarryCategoryPortsAndVersion() {
        search->openAt(0, 0);
        search->setQuery("crop");
        auto *r = search->results();
        const QModelIndex i = r->index(0);
        QCOMPARE(r->data(i, SearchResults::Category).toString(), QString("Image/Transform"));
        QCOMPARE(r->data(i, SearchResults::Version).toString(), QString("1.0.0"));
        const QVariantList inputs = r->data(i, SearchResults::Inputs).toList();
        QCOMPARE(inputs.first().toMap().value("type").toString(), QString("image"));
        const QVariantList outputs = r->data(i, SearchResults::Outputs).toList();
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(outputs.first().toMap().value("type").toString(), QString("image"));
    }

    void aLinkFromAnOutputKeepsNodesWithACompatibleInput() {
        search->openFromLink({{"node", "prompt"}, {"port", "text"}, {"output", true}}, 300, 300);
        const QStringList found = names(search->results());
        QVERIFY(found.contains("Generate Image")); // a text prompt input
        QVERIFY(found.contains("Text Concat"));
        QVERIFY(found.contains("Preview")); // takes any
        QVERIFY(!found.contains("Brightness and Contrast")); // image and numbers only
        QVERIFY(search->filterText().contains("text"));
        // Typing narrows within the filter.
        search->setQuery("preview");
        QCOMPARE(names(search->results()), QStringList{"Preview"});
        search->setQuery("brightness");
        QCOMPARE(search->count(), 0);
    }

    void aLinkFromAnInputKeepsNodesWithACompatibleOutput() {
        search->openFromLink({{"node", "save"}, {"port", "image"}, {"output", false}}, 300, 300);
        const QStringList found = names(search->results());
        QVERIFY(found.contains("Load Image"));
        QVERIFY(found.contains("Blend"));
        QVERIFY(!found.contains("Number"));
        QVERIFY(!found.contains("Text"));
        QVERIFY(!found.contains("Preview")); // no outputs
    }

    void aPlaceholdersPortFindsNothing() {
        search->openFromLink({{"node", "nobody"}, {"port", "x"}, {"output", true}}, 0, 0);
        QCOMPARE(search->count(), 0);
        QVERIFY(!search->filterText().isEmpty());
    }

    void acceptingAddsTheNodeAtThePointAndSelectsIt() {
        search->openAt(120, 340);
        search->setQuery("number");
        const int before = canvas->nodes()->rowCount();
        const QString id = search->accept(0);
        QVERIFY(!id.isEmpty());
        QVERIFY(!search->active());
        QCOMPARE(canvas->nodes()->rowCount(), before + 1);
        QCOMPARE(canvas->position(id), QPointF(120, 340));
        QCOMPARE(canvas->selectedIds(), QStringList{id});
        QCOMPARE(canvas->links()->rowCount(), 0);
    }

    void acceptingFromAnOutputLinksItToTheFirstCompatibleInput() {
        search->openFromLink({{"node", "prompt"}, {"port", "text"}, {"output", true}}, 300, 300);
        search->setQuery("text concat");
        const QString id = search->accept(0);
        QVERIFY(!id.isEmpty());
        QCOMPARE(canvas->links()->rowCount(), 1);
        const QModelIndex link = canvas->links()->index(0);
        QCOMPARE(canvas->links()->data(link, LinkList::FromNode).toString(), QString("prompt"));
        QCOMPARE(canvas->links()->data(link, LinkList::ToNode).toString(), id);
        QCOMPARE(canvas->links()->data(link, LinkList::ToPort).toString(), QString("a"));
        QCOMPARE(canvas->position(id), QPointF(300, 300));
    }

    void acceptingFromAnInputLinksTheNewNodesOutputAndPlacesItToTheLeft() {
        search->openFromLink({{"node", "save"}, {"port", "image"}, {"output", false}}, 500, 300);
        search->setQuery("load image");
        const QString id = search->accept(0);
        QVERIFY(!id.isEmpty());
        QCOMPARE(canvas->links()->rowCount(), 1);
        const QModelIndex link = canvas->links()->index(0);
        QCOMPARE(canvas->links()->data(link, LinkList::FromNode).toString(), id);
        QCOMPARE(canvas->links()->data(link, LinkList::ToNode).toString(), QString("save"));
        QCOMPARE(canvas->position(id).x(), 500 - CanvasModel::defaultWidth);
    }

    void aRowThatIsNotThereAddsNothing() {
        search->openAt(0, 0);
        const int before = canvas->nodes()->rowCount();
        QVERIFY(search->accept(9999).isEmpty());
        QCOMPARE(canvas->nodes()->rowCount(), before);
        QVERIFY(search->active());
    }

    void closingLeavesTheGraphAlone() {
        search->openAt(0, 0);
        const int before = canvas->nodes()->rowCount();
        search->close();
        QVERIFY(!search->active());
        QCOMPARE(canvas->nodes()->rowCount(), before);
    }
};

QTEST_MAIN(TestNodeSearch)
#include "test_node_search.moc"
