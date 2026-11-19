// The resident-model cache (spec 9.3): an in-memory, least-recently-used set
// of loaded models, separate from the result cache.
//
// Seam: ResidentModels' public interface.

#include <QtTest>

#include "engine/resident_models.h"

using namespace fugue;

class TestResidentModels : public QObject {
    Q_OBJECT

private slots:
    void usingAModelMakesItTheMostRecent() {
        ResidentModels models;
        models.put("a");
        models.put("b");
        models.put("c");
        QCOMPARE(models.keys(), (QStringList{"a", "b", "c"}));
        models.touch("a");
        QCOMPARE(models.keys(), (QStringList{"b", "c", "a"}));
        models.touch("missing");
        QVERIFY(!models.contains("missing"));
        QCOMPARE(models.keys().size(), 3);
    }

    void evictionTakesTheLeastRecentlyUsedOnly() {
        ResidentModels models;
        models.put("a");
        models.put("b");
        models.put("c");
        QCOMPARE(models.evictLeastRecentlyUsed(), QStringLiteral("a"));
        QCOMPARE(models.keys(), (QStringList{"b", "c"}));
        // A model the node about to run needs is not the one to drop.
        QCOMPARE(models.evictLeastRecentlyUsed({"b"}), QStringLiteral("c"));
        QCOMPARE(models.evictLeastRecentlyUsed({"b"}), QString());
        QVERIFY(models.contains("b"));
    }

    void evictingAModelReleasesWhatWasLoaded() {
        ResidentModels models;
        auto weights = std::make_shared<int>(7);
        std::weak_ptr<int> watch = weights;
        models.put("a", weights);
        weights.reset();
        QVERIFY(!watch.expired());
        QCOMPARE(*std::static_pointer_cast<int>(models.get("a")), 7);

        QCOMPARE(models.evictLeastRecentlyUsed(), QStringLiteral("a"));
        QVERIFY(watch.expired());
        QVERIFY(!models.get("a"));
    }
};

QTEST_GUILESS_MAIN(TestResidentModels)
#include "test_resident_models.moc"
