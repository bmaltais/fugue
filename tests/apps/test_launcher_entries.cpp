// The launcher entries of apps (#398, apps/launcher_entries.h) and the words `--app` takes (apps/app_ref.h), against a scratch
// data folder: nothing here touches the real ~/.local/share.

#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "apps/app_ref.h"
#include "apps/launcher_entries.h"

using namespace fugue;

namespace {

QString read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString line(const QString &text, const QString &key) {
    for (const QString &each : text.split('\n'))
        if (each.startsWith(key + '='))
            return each.mid(key.size() + 1);
    return QStringLiteral("<no %1>").arg(key);
}

} // namespace

class TestLauncherEntries : public QObject {
    Q_OBJECT

private slots:
    void anEntryStartsTheAppInItsOwnWindowWithItsPicture() {
        QTemporaryDir data;
        const QString picture = data.filePath("preview.png");
        QVERIFY(QImage(16, 16, QImage::Format_RGB32).save(picture));
        const LauncherEntries entries(data.filePath("share"), "fugue");
        QVERIFY(!entries.has("remove-background"));
        QVERIFY(entries.add({"remove-background", "Remove Background", "Cut a subject out", picture}).isEmpty());
        QVERIFY(entries.has("remove-background"));

        const QString path = data.filePath("share/applications/fugue-app-remove-background.desktop");
        QCOMPARE(entries.entryPath("remove-background"), path);
        const QString text = read(path);
        QCOMPARE(line(text, "Type"), QStringLiteral("Application"));
        QCOMPARE(line(text, "Name"), QStringLiteral("Remove Background"));
        QCOMPARE(line(text, "Comment"), QStringLiteral("Cut a subject out"));
        QCOMPARE(line(text, "Exec"), QStringLiteral("fugue --app remove-background"));
        QCOMPARE(line(text, "StartupWMClass"), LauncherEntries::desktopName("remove-background")); // the window's own name
        const QString icon = line(text, "Icon");
        QCOMPARE(icon, data.filePath("share/fugue/app-icons/fugue-app-remove-background.png"));
        QVERIFY(QFileInfo(icon).isFile()); // a copy: the app's preview may move or be replaced
    }

    void removingTakesTheEntryAndItsPictureAndNothingElse() {
        QTemporaryDir data;
        const QString picture = data.filePath("preview.png");
        QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(picture));
        const LauncherEntries entries(data.path(), "fugue");
        QVERIFY(entries.add({"a", "A", {}, picture}).isEmpty());
        QVERIFY(entries.add({"b", "B", {}, picture}).isEmpty());
        QVERIFY(entries.remove("a").isEmpty());
        QVERIFY(!entries.has("a"));
        QVERIFY(!QFileInfo::exists(data.filePath("fugue/app-icons/fugue-app-a.png")));
        QVERIFY(entries.has("b")); // the other app's entry stays
        QVERIFY(QFileInfo::exists(data.filePath("fugue/app-icons/fugue-app-b.png")));
        QVERIFY(entries.remove("a").isEmpty()); // none is not a problem
        QVERIFY(QFileInfo::exists(picture)); // the app's own preview is never touched
    }

    void withoutAPictureTheEntryUsesFugueAndAddingAgainReplaces() {
        QTemporaryDir data;
        const LauncherEntries entries(data.path(), "fugue");
        QVERIFY(entries.add({"a", "First", {}, {}}).isEmpty());
        QCOMPARE(line(read(entries.entryPath("a")), "Icon"), QStringLiteral("fugue"));
        QVERIFY(line(read(entries.entryPath("a")), "Comment").startsWith("<no"));
        QVERIFY(entries.add({"a", "Second", "More", {}}).isEmpty());
        QCOMPARE(line(read(entries.entryPath("a")), "Name"), QStringLiteral("Second"));
        QCOMPARE(QDir(data.filePath("applications")).entryList(QDir::Files).size(), 1);
    }

    void aPathIsQuotedAndTwoAppsOfTheSameFileNameStayApart() {
        QTemporaryDir data;
        const LauncherEntries entries(data.path(), "/opt/Fugue App/fugue.AppImage");
        const QString one = "/home/me/My Apps/lake.fugue";
        const QString two = "/home/me/Other/lake.fugue";
        QVERIFY(entries.add({one, "Lake", {}, {}}).isEmpty());
        QVERIFY(entries.add({two, "Lake", {}, {}}).isEmpty());
        QVERIFY(entries.entryPath(one) != entries.entryPath(two));
        QVERIFY(entries.has(one) && entries.has(two));
        QCOMPARE(line(read(entries.entryPath(one)), "Exec"),
                 QStringLiteral("\"/opt/Fugue App/fugue.AppImage\" --app \"/home/me/My Apps/lake.fugue\""));
        QCOMPARE(LauncherEntries::execLine("fugue", "a\"b$c%d"), QStringLiteral("fugue --app \"a\\\"b\\$c%%d\""));
        QVERIFY(LauncherEntries::desktopName(one).startsWith("fugue-app-lake-"));
        QVERIFY(!LauncherEntries::desktopName("../etc").contains('/'));
    }

    void anEntryNamesAnAppThatItsWindowMustResolve() {
        // The same words as `fugue app info`: an id of the catalog, a folder, a file; a file that exists wins over an id.
        const QList<TemplateRoot> roots{{QStringLiteral(FUGUE_SOURCE_DIR "/tests/apps/templates"), false}};
        QString why;
        const auto byId = resolveApp("test-app", roots, nullptr, &why);
        QVERIFY2(byId, qPrintable(why));
        QCOMPARE(byId->kind, AppRef::Kind::Template);
        QCOMPARE(byId->ref, QStringLiteral("test-app"));
        QVERIFY(byId->templ.has_value());

        const auto byFolder = resolveApp(FUGUE_SOURCE_DIR "/tests/apps/templates/test-app", roots, nullptr, &why);
        QVERIFY2(byFolder, qPrintable(why));
        QCOMPARE(byFolder->ref, QStringLiteral("test-app")); // the catalog has that folder: its id names it
        QCOMPARE(byFolder->kind, AppRef::Kind::Template);

        const auto byFile = resolveApp(FUGUE_SOURCE_DIR "/tests/apps/templates/test-app/workflow.fugue", roots, nullptr, &why);
        QVERIFY2(byFile, qPrintable(why));
        QCOMPARE(byFile->kind, AppRef::Kind::Workflow);
        QVERIFY(byFile->ref.endsWith("/workflow.fugue"));

        QVERIFY(!resolveApp("no-such-app", roots, nullptr, &why));
        QVERIFY(why.contains("no-such-app"));
        QTemporaryDir empty;
        QVERIFY(!resolveApp(empty.path(), roots, nullptr, &why)); // a folder that is not a template
    }

    void theOptionIsReadWithOrWithoutAnEquals() {
        const auto read = [](std::initializer_list<const char *> words, const char *option = "--app") {
            std::vector<char *> argv;
            for (const char *word : words)
                argv.push_back(const_cast<char *>(word));
            return appRefFromArgs(int(argv.size()), argv.data(), option);
        };
        QCOMPARE(read({"fugue", "--app", "remove-background"}), std::optional<QString>("remove-background"));
        QCOMPARE(read({"fugue", "--app=a b"}), std::optional<QString>("a b"));
        QCOMPARE(read({"fugue", "--edit", "x"}, "--edit"), std::optional<QString>("x"));
        QVERIFY(!read({"fugue", "file.fugue"}).has_value());
        QCOMPARE(read({"fugue", "--app"}), std::optional<QString>(QString())); // named, nothing after it
    }
};

QTEST_GUILESS_MAIN(TestLauncherEntries)
#include "test_launcher_entries.moc"
