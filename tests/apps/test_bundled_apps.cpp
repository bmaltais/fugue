// Every bundled template that carries an app block reads against its graph (#396): the same check fugue-workflows runs in CI with
// bin/check-apps (the built binary, `fugue app info`), here at the engine seam so a change to the block's reader or the node types
// fails in Fugue's own suite and not only after a merge. A template whose pack is not installed is read with its nodes unknown, so a
// block that targets one is skipped, the way `app info` says "needs a pack" and not "invalid".

#include <QtTest>

#include "engine/app_binding.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

class TestBundledApps : public QObject {
    Q_OBJECT

private slots:
    void everyBundledAppBlockReadsAndChecks() {
        const QDir root(QStringLiteral(FUGUE_SOURCE_DIR "/templates"));
        if (!root.exists("categories.json"))
            QSKIP("the templates submodule is not checked out");
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        int apps = 0;
        for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            QFile file(root.filePath(name + "/workflow.fugue"));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const Result<Document> document = Document::load(file.readAll());
            QVERIFY2(document, qPrintable(name));
            const Result<std::optional<app::App>> block = app::appOf(*document);
            QVERIFY2(block, qPrintable(name + ": the app block does not parse"));
            if (!block->has_value())
                continue;
            ++apps;
            bool known = true; // every node type the template uses is a built-in
            for (const auto &node : document->graph.nodes)
                known = known && registry.nodeType(node.type) != nullptr;
            if (!known)
                continue;
            QVERIFY2(app::checkApp(**block, *document, registry).isEmpty(), qPrintable(name));
        }
        QVERIFY2(apps > 0, "no bundled template is an app");
    }
};

QTEST_MAIN(TestBundledApps)
#include "test_bundled_apps.moc"
