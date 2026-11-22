// Where a model is available from (#164): this machine, a paired host, both, or nowhere.
//
// Seam: placesOf, summarizePlaces and placeGlyphs over the models the providers listed.

#include <QJsonObject>
#include <QtTest>

#include "engine/model_places.h"

using namespace fugue;

namespace {

QJsonObject handle(const QString &name, const QString &resource = "fugue/diffusion-model", const QString &hash = {}) {
    QJsonObject h{{"resource", resource}, {"name", name}};
    if (!hash.isEmpty())
        h.insert("hash", hash);
    return h;
}

ListedModel served(const QJsonObject &model, const QString &status, const QString &host = {}) {
    ListedModel listed;
    listed.name = model.value("name").toString();
    listed.handle = model;
    listed.status = status;
    listed.provider = host.isEmpty() ? QStringLiteral("This machine") : host;
    listed.host = host;
    return listed;
}

} // namespace

class ModelPlacesTest : public QObject {
    Q_OBJECT
private slots:
    void nowhereIsMissing() {
        const ModelPlaces places = placesOf(handle("a.gguf"), false, {});
        QVERIFY(!places.local);
        QVERIFY(places.hosts.isEmpty());
        QVERIFY(!places.available());
    }
    void aFileOnDiskIsThisMachine() {
        const ModelPlaces places = placesOf(handle("a.gguf"), true, {});
        QVERIFY(places.local);
        QVERIFY(places.available());
    }
    void aHostThatServesItIsAPlace() {
        const QList<ListedModel> offered{served(handle("a.gguf"), "ready", "gx10")};
        const ModelPlaces places = placesOf(handle("a.gguf"), false, offered);
        QVERIFY(!places.local);
        QCOMPARE(places.hosts, QStringList{"gx10"});
        QVERIFY(places.available());
    }
    void aLocalServerIsThisMachine() {
        const QList<ListedModel> offered{served(handle("a.gguf"), "ready")};
        QVERIFY(placesOf(handle("a.gguf"), false, offered).local);
    }
    void aStoppedLocalServerIsNot() {
        const QList<ListedModel> offered{served(handle("a.gguf"), "unavailable")};
        QVERIFY(!placesOf(handle("a.gguf"), false, offered).available());
    }
    void anOfflineHostIsListedButNotAvailable() {
        const QList<ListedModel> offered{served(handle("a.gguf"), "unavailable", "gx10")};
        const ModelPlaces places = placesOf(handle("a.gguf"), false, offered);
        QVERIFY(places.hosts.isEmpty());
        QCOMPARE(places.offlineHosts, QStringList{"gx10"});
        QVERIFY(!places.available());
    }
    void bothPlacesAreKept() {
        const QList<ListedModel> offered{served(handle("a.gguf"), "ready", "gx10"), served(handle("a.gguf"), "ready", "gx10")};
        const ModelPlaces places = placesOf(handle("a.gguf"), true, offered);
        QVERIFY(places.local);
        QCOMPARE(places.hosts, QStringList{"gx10"}); // once
    }
    void aHashNamesTheModelNotTheName() {
        const QList<ListedModel> offered{served(handle("renamed.gguf", "fugue/diffusion-model", "sha256:bb"), "ready", "gx10")};
        QVERIFY(!placesOf(handle("a.gguf", "fugue/diffusion-model", "sha256:aa"), false, offered).available());
        QVERIFY(placesOf(handle("a.gguf", "fugue/diffusion-model", "sha256:bb"), false, offered).available());
    }
    void anotherResourceTypeIsNotTheModel() {
        const QList<ListedModel> offered{served(handle("a.gguf", "fugue/vae"), "ready", "gx10")};
        QVERIFY(!placesOf(handle("a.gguf"), false, offered).available());
    }

    void noModelsIsReadyWithNoIcons() {
        const TemplatePlaces t = summarizePlaces({});
        QVERIFY(t.ready);
        QVERIFY(!t.local);
        QVERIFY(t.hosts.isEmpty());
        QVERIFY(placeGlyphs(t.local, !t.hosts.isEmpty()).isEmpty());
    }
    void aMixShowsBothPlaces() {
        ModelPlaces disk;
        disk.local = true;
        ModelPlaces remote;
        remote.hosts = {"gx10"};
        const TemplatePlaces t = summarizePlaces({disk, remote});
        QVERIFY(t.ready);
        QVERIFY(t.local);
        QCOMPARE(t.hosts, QStringList{"gx10"});
        QCOMPARE(placeGlyphs(t.local, !t.hosts.isEmpty()), (QStringList{"monitor", "server"}));
    }
    void oneMissingModelIsNotReady() {
        ModelPlaces disk;
        disk.local = true;
        ModelPlaces offline;
        offline.offlineHosts = {"gx10"};
        const TemplatePlaces t = summarizePlaces({disk, offline, ModelPlaces()});
        QVERIFY(!t.ready);
        QCOMPARE(t.missing, 2);
        QVERIFY(t.local);
        QCOMPARE(t.offlineHosts, QStringList{"gx10"});
    }
    void anOfflineHostDoesNotCountWhereAnotherPlaceHasTheModel() {
        ModelPlaces both;
        both.local = true;
        both.offlineHosts = {"gx10"};
        const TemplatePlaces t = summarizePlaces({both});
        QVERIFY(t.ready);
        QVERIFY(t.offlineHosts.isEmpty());
    }
    void glyphsAreOnePerPlace() {
        QCOMPARE(placeGlyphs(true, false), QStringList{"monitor"});
        QCOMPARE(placeGlyphs(false, true), QStringList{"server"});
    }
};

QTEST_GUILESS_MAIN(ModelPlacesTest)
#include "test_model_places.moc"
