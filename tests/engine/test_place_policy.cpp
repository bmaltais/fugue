// The Place policy (engine/place_policy.h): which paths a search or the first-run sheet may touch, at its own interface.
//
// Seam: PlacePolicy built from offered places and a FinderEnv (mounts and a probe that sees every path touched). Places are plain
// values here; only the link test makes real links in a scratch folder.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/place_policy.h"

using namespace fugue;

namespace {

FinderLocation place(const QString &id, const QString &path, const QString &scope = {}) {
    FinderLocation location;
    location.id = id;
    location.path = path;
    location.scope = scope.isEmpty() ? path : scope;
    return location;
}

} // namespace

class TestPlacePolicy : public QObject {
    Q_OBJECT

private slots:
    void theLongestPlaceDecides() {
        const FinderLocation home = place("home", "/h");
        const FinderLocation comfy = place("probe:/h/ComfyUI/models", "/h/ComfyUI/models", "/h/ComfyUI");
        // Ticked inside unticked: only the ticked one is open.
        const PlacePolicy a({comfy}, {home, comfy}, {});
        QVERIFY(a.inLeftOffPlace("/h/other/x"));
        QVERIFY(!a.mayTouch("/h/other"));
        QVERIFY(a.mayTouch("/h/ComfyUI/models/checkpoints"));
        QVERIFY(!a.inLeftOffPlace("/h/ComfyUI"));
        // Unticked inside ticked: the inner one is closed.
        const PlacePolicy b({home}, {home, comfy}, {});
        QVERIFY(b.mayTouch("/h/other/x"));
        QVERIFY(b.inLeftOffPlace("/h/ComfyUI/models/x"));
        QVERIFY(!b.mayBeDownloadChoice("/h/ComfyUI/models"));
        // A path in no offered place was never left off.
        QVERIFY(b.mayTouch("/elsewhere"));
        // Nothing offered: nothing left off.
        QVERIFY(PlacePolicy({home}, {}, {}).mayTouch("/h/ComfyUI"));
    }

    void ownRootIsAlwaysAllowed() {
        const FinderLocation home = place("home", "/h");
        const PlacePolicy policy({}, {home}, {}, "/h/.local/share/fugue/models");
        QVERIFY(!policy.mayTouch("/h/x"));
        QVERIFY(policy.mayTouch("/h/.local/share/fugue/models"));
        QVERIFY(policy.mayTouch("/h/.local/share/fugue/models/checkpoints"));
        QVERIFY(!policy.inLeftOffPlace("/h/.local/share/fugue/models"));
        // Until the places are confirmed, only it.
        FinderEnv env;
        const PlacePolicy waiting = PlacePolicy::unconfirmed(env, "/own/models");
        QVERIFY(waiting.mayTouch("/own/models"));
        QVERIFY(!waiting.mayTouch("/anything"));
        QVERIFY(waiting.inLeftOffPlace("/anything"));
        QCOMPARE(waiting.freeBytes("/anything", [](const QString &) { return qint64(1); }), qint64(-1));
    }

    void networkAndAutofsAreBlocked() {
        FinderEnv env;
        env.networkMounts = {"/net", "/auto"};
        env.autofsMounts = {"/auto"};
        const PlacePolicy policy(env);
        QVERIFY(!policy.mayTouch("/net/x"));
        QVERIFY(!policy.mayTouch("/net/x", true));
        QVERIFY(policy.isNetwork("/net"));
        QVERIFY(policy.isAutofs("/auto/y"));
        QVERIFY(!policy.isAutofs("/net/y"));
        QVERIFY(policy.mayTouch("/netx"));
        // A network folder is not a place left off: it stays a (listed, uncounted) download choice, and its free space is unknown.
        QVERIFY(!policy.inLeftOffPlace("/net/x"));
        QVERIFY(policy.mayBeDownloadChoice("/net/x"));
        QCOMPARE(policy.freeBytes("/net/x", [](const QString &) { return qint64(1); }), qint64(-1));
        QCOMPARE(policy.freeBytes("/plain", [](const QString &) { return qint64(7); }), qint64(7));
    }

    void traversalPassesAFolderThatLeadsToATickedPlace() {
        const FinderLocation home = place("home", "/h");
        const FinderLocation comfy = place("probe:/h/ComfyUI/models", "/h/ComfyUI/models", "/h/ComfyUI");
        const PlacePolicy policy({comfy}, {home, comfy}, {});
        QVERIFY(!policy.mayTouch("/h"));
        QVERIFY(policy.mayTouch("/h", true)); // by name, on the way
        QVERIFY(!policy.mayTouch("/h/other", true));
    }

    void aLinkIntoAPlaceLeftOffIsResolvedWithoutTouchingIt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = QDir(dir.path()).canonicalPath();
        QVERIFY(QDir().mkpath(root + "/home"));
        QVERIFY(QDir().mkpath(root + "/mnt/data/models"));
        QVERIFY(QFile::link(root + "/mnt/data/models", root + "/home/models"));
        QStringList touched;
        FinderEnv env;
        env.touched = [&touched](const QString &path) { touched << path; };
        const FinderLocation home = place("home", root + "/home");
        const FinderLocation mnt = place("drive:" + root + "/mnt", root + "/mnt");
        const PlacePolicy policy({home}, {home, mnt}, env);
        QVERIFY(policy.resolve(root + "/home/models").isEmpty());
        QVERIFY(!policy.mayBeDownloadChoice(root + "/home/models"));
        QCOMPARE(policy.freeBytes(root + "/home/models", [](const QString &) { return qint64(5); }), qint64(-1));
        for (const QString &path : std::as_const(touched))
            QVERIFY2(!path.startsWith(root + "/mnt/"), qPrintable("touched " + path));
        QVERIFY(!touched.isEmpty()); // the link itself was read
        // With the place ticked the same link resolves, and the probe sees the steps.
        const PlacePolicy open({home, mnt}, {home, mnt}, env);
        QCOMPARE(open.resolve(root + "/home/models"), root + "/mnt/data/models");
        QVERIFY(open.mayBeDownloadChoice(root + "/home/models"));
        QCOMPARE(open.freeBytes(root + "/home/models", [](const QString &) { return qint64(5); }), qint64(5));
    }
};

QTEST_GUILESS_MAIN(TestPlacePolicy)
#include "test_place_policy.moc"
