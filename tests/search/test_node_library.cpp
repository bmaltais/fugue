// The node library's tree and filter without a window (#56): categories from the `/` parts of a
// type's category, which levels are open, and the filter that is node search's ranking.

#include <QJsonObject>
#include <QtTest>

#include "engine/registry.h"
#include "nodes/builtin.h"
#include "search/node_library.h"

using namespace fugue;

namespace {

NodeType type(const QString &id, const QString &name, const QString &category, const QString &description = {}) {
    NodeType t;
    t.id = id;
    t.name = name;
    t.category = category;
    t.description = description;
    return t;
}

// "-Image", "+Blur": a category closed or open, then a node.
QStringList shown(const std::vector<LibraryRow> &rows) {
    QStringList out;
    for (const LibraryRow &row : rows)
        out << QString(row.depth * 2, QLatin1Char(' ')) + (row.category ? (row.open ? "+" : "-") : "") + row.label;
    return out;
}

} // namespace

class TestNodeLibrary : public QObject {
    Q_OBJECT

    NodeType a = type("p/blur", "Blur", "Image/Filter");
    NodeType b = type("p/crop", "Crop", "Image");
    NodeType c = type("p/text", "Text", "Text");
    NodeType d = type("p/sharpen", "Sharpen", "Image/Filter");
    NodeType e = type("p/loose", "Loose", "");
    QList<const NodeType *> all() { return {&a, &b, &c, &d, &e}; }

private slots:
    void categoriesComeFirstAndLevelsAreClosedAtStart() {
        const auto rows = libraryRows(all(), {}, {});
        QCOMPARE(shown(rows), (QStringList{"-Image", "-Text", "Loose"}));
        QCOMPARE(rows[0].count, 3); // Crop, Blur and Sharpen
        QCOMPARE(rows[0].key, QString("Image"));
    }
    void anOpenCategoryShowsItsChildren() {
        const auto rows = libraryRows(all(), {}, {"Image", "Image/Filter"});
        QCOMPARE(shown(rows), (QStringList{"+Image", "  +Filter", "    Blur", "    Sharpen", "  Crop", "-Text", "Loose"}));
        QCOMPARE(rows[1].key, QString("Image/Filter"));
        QCOMPARE(rows[2].key, QString("p/blur"));
    }
    void aQueryKeepsOnlyMatchesWithEveryCategoryOpen() {
        const auto rows = libraryRows(all(), "sharp", {});
        QCOMPARE(shown(rows), (QStringList{"+Image", "  +Filter", "    Sharpen"}));
        QCOMPARE(rows[0].count, 1);
    }
    void aQueryThatMatchesNothingShowsNothing() { QVERIFY(libraryRows(all(), "zzzzqq", {}).empty()); }
    void aBlankQueryIsNoQuery() { QCOMPARE(libraryRows(all(), "   ", {}).size(), libraryRows(all(), {}, {}).size()); }
    void aNodeCarriesItsDescription() {
        NodeType t = type("p/x", "Xform", "Image", "Does a thing");
        const auto rows = libraryRows({&t}, {}, {"Image"});
        QCOMPARE(rows[1].description, QString("Does a thing"));
    }
    void numbersSortAsNumbers() {
        NodeType x2 = type("p/2", "Step 2", ""), x10 = type("p/10", "Step 10", "");
        QCOMPARE(shown(libraryRows({&x10, &x2}, {}, {})), (QStringList{"Step 2", "Step 10"}));
    }

    // #156: only a group of the user's library (`local/<name>`, kind group) can be deleted there.
    void onlyLibraryGroupsAreMarkedForDeletion() {
        NodeType mine = type("local/fox", "Fox", "Groups");
        mine.runtime = QJsonObject{{"kind", "group"}};
        NodeType packs = type("io.example.tools/fox", "Pack fox", "Groups");
        packs.runtime = QJsonObject{{"kind", "group"}};
        NodeType notGroup = type("local/odd", "Odd", "Groups"); // the local pack holds groups only; a command is not one
        notGroup.runtime = QJsonObject{{"kind", "command"}};
        const auto rows = libraryRows({&mine, &packs, &notGroup, &a}, {}, {"Groups", "Image", "Image/Filter"});
        QStringList marked;
        for (const LibraryRow &row : rows)
            if (!row.category && row.library)
                marked << row.key;
        QCOMPARE(marked, QStringList{"local/fox"});
    }
    void theModelFollowsTheRegistryAndTheOpenSet() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        NodeLibrary library(&registry);
        QSignalSpy spy(&library, &NodeLibrary::rowsChanged);
        const int closed = library.rows().size();
        QVERIFY(closed > 0);
        QVERIFY(library.rows().first().toMap().value("category").toBool());
        const QString key = library.rows().first().toMap().value("key").toString();
        library.toggle(key);
        QVERIFY(library.rows().size() > closed);
        QVERIFY(library.rows().first().toMap().value("open").toBool());
        library.toggle(key);
        QCOMPARE(library.rows().size(), closed);
        library.setQuery("save");
        QVERIFY(library.nodeCount() > 0);
        QVERIFY(library.nodeCount() < int(registry.nodeTypes().size()));
        QVERIFY(spy.count() >= 3);
        library.setQuery({});
        QCOMPARE(library.rows().size(), closed);
    }
};

QTEST_MAIN(TestNodeLibrary)
#include "test_node_library.moc"
