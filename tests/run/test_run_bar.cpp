// What the run toolbar remembers (#60, #120): its place as fractions of the room it has, kept in a
// small file; an older file with a dock edge is read as the place it meant.
//
// Seam: run/run_bar.h.

#include <QtTest>

#include "run/run_bar.h"

class TestRunBar : public QObject {
    Q_OBJECT

    static void write(const QString &path, const QByteArray &json) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(json);
    }
    static QJsonObject read(const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }

private slots:
    void noFileIsCenteredAtTheTop() {
        QTemporaryDir dir;
        fugue::RunBar bar(nullptr, dir.path() + "/run-bar.json");
        QCOMPARE(bar.place(), QPointF(0.5, 0));
    }

    void aPlaceIsKeptAndHeldToTheRoom() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/run-bar.json";
        {
            fugue::RunBar bar(nullptr, path);
            bar.setPlace({0.25, 0.75});
            QCOMPARE(bar.place(), QPointF(0.25, 0.75));
            bar.setPlace({-1, 9});
            QCOMPARE(bar.place(), QPointF(0, 1));
            bar.setPlace({0.25, 0.75});
        }
        fugue::RunBar again(nullptr, path);
        QCOMPARE(again.place(), QPointF(0.25, 0.75));
        QVERIFY(!read(path).contains("dock"));
    }

    void anOlderDockEdgeIsReadAsAPlace() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/run-bar.json";
        write(path, R"({"visible": true, "dock": "bottom", "batch": 3})");
        fugue::RunBar bottom(nullptr, path);
        QCOMPARE(bottom.place(), QPointF(0.5, 1));
        write(path, R"({"visible": true, "dock": "top", "batch": 3})");
        fugue::RunBar top(nullptr, path);
        QCOMPARE(top.place(), QPointF(0.5, 0));
        // The next save writes the place and drops the dock.
        top.setPlace({0.1, 0.2});
        QVERIFY(!read(path).contains("dock"));
        QCOMPARE(read(path).value("place").toObject().value("x").toDouble(), 0.1);
    }
};

QTEST_GUILESS_MAIN(TestRunBar)
#include "test_run_bar.moc"
