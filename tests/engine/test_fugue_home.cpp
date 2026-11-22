// FUGUE_HOME / --home (spec 1.4.1, epic #204): the one root everything Fugue writes lies under.
// Seam: engine/fugue_home.h and the path functions in engine/paths.h. The environment is the
// process's, so each test saves and restores what it sets. The root's end-to-end guarantee (nothing
// outside it, an empty HOME stays empty) is tests/cli/test_cli_home_guard.cpp.

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <vector>

#include "engine/file_roots.h"
#include "engine/fugue_home.h"
#include "engine/paths.h"

using namespace fugue;

namespace {

const char *const kVariables[] = {"FUGUE_HOME",   "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                  "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
                                  "TMPDIR",       "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                                  "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH", "PIPEWIRE_RUNTIME_DIR",
                                  "PULSE_RUNTIME_PATH"};

// argv as applyFugueHome takes it: writable strings.
struct Args {
    std::vector<QByteArray> words;
    std::vector<char *> pointers;
    int argc = 0;
    explicit Args(const QList<QByteArray> &list) : words(list.begin(), list.end()) {
        for (QByteArray &word : words)
            pointers.push_back(word.data());
        pointers.push_back(nullptr);
        argc = int(words.size());
    }
    QStringList left() const {
        QStringList out;
        for (int i = 0; i < argc; ++i)
            out << QString::fromLocal8Bit(pointers[size_t(i)]);
        return out;
    }
};

} // namespace

class TestFugueHome : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;

private slots:
    void init() {
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
        }
        qunsetenv("FUGUE_HOME");
    }
    void cleanup() {
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it)
            qputenv(it.key().constData(), it.value());
        for (const QByteArray &variable : std::as_const(m_unset))
            qunsetenv(variable.constData());
        m_saved.clear();
        m_unset.clear();
    }

    void withoutARootNothingChanges() {
        const QString config = configDir();
        const QString output = defaultOutputDir();
        Args args({"fugue", "run", "a.fugue"});
        QString error;
        QVERIFY(applyFugueHome(args.argc, args.pointers.data(), &error));
        QCOMPARE(args.left(), (QStringList{"fugue", "run", "a.fugue"}));
        QVERIFY(fugueHome().isEmpty());
        QVERIFY(fugueHomeDir("outputs").isEmpty());
        QCOMPARE(configDir(), config);
        QCOMPARE(defaultOutputDir(), output);
        QCOMPARE(documentsDir(), QDir::homePath() + "/Documents/Fugue");
        QCOMPARE(providersFilePath(), QString("~/.config/fugue/providers.json"));
        QCOMPARE(mediaThumbnailsDir(), cacheDir() + "/thumbs");
    }

    void aRootRedirectsTheXdgFoldersAndTheTempFolder() {
        QTemporaryDir dir;
        const QString root = dir.filePath("root");
        qputenv("HOME", qgetenv("HOME")); // HOME stays as it is
        const QByteArray home = qgetenv("HOME");
        QString error;
        QVERIFY2(setFugueHome(root, &error), qPrintable(error));
        QCOMPARE(fugueHome(), root);
        QCOMPARE(configDir(), root + "/config/fugue");
        QCOMPARE(dataDir(), root + "/data/fugue");
        QCOMPARE(stateDir(), root + "/state/fugue");
        QCOMPARE(cacheDir(), root + "/cache/fugue");
        QCOMPARE(qgetenv("TMPDIR"), root.toLocal8Bit() + "/tmp");
        QCOMPARE(qgetenv("XDG_RUNTIME_DIR"), root.toLocal8Bit() + "/run");
        QCOMPARE(qgetenv("HOME"), home);
        // The runtime folder is private, as XDG_RUNTIME_DIR must be.
        QCOMPARE(QFileInfo(root + "/run").permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
    }

    void theFoldersTheEnvironmentCannotMoveFollowTheRoot() {
        QTemporaryDir dir;
        const QString root = dir.filePath("root");
        QVERIFY(setFugueHome(root, nullptr));
        QCOMPARE(defaultOutputDir(), root + "/outputs");
        QCOMPARE(outputDir(), root + "/outputs");
        QCOMPARE(inputsDir(), root + "/outputs/inputs");
        QCOMPARE(documentsDir(), root + "/documents");
        QCOMPARE(runtimeDir(), root + "/run");
        QCOMPARE(mediaThumbnailsDir(), root + "/cache/fugue/thumbs");
        QCOMPARE(providersFilePath(), root + "/config/fugue/providers.json");
        const FileRoots roots = FileRoots::defaults(root + "/work");
        QVERIFY(roots.write.contains(root + "/documents"));
        QVERIFY(roots.write.contains(root + "/outputs"));
        QVERIFY(!roots.write.contains(QDir::homePath() + "/Documents/Fugue"));
    }

    void aRootMakesDocumentsAndOutputsAndHealsAnOlderOne() {
        QTemporaryDir dir;
        const QString root = dir.filePath("root");
        QVERIFY(setFugueHome(root, nullptr));
        QVERIFY(QFileInfo(root + "/documents").isDir());
        QVERIFY(QFileInfo(root + "/outputs").isDir());
        QVERIFY(QDir(root + "/documents").removeRecursively());
        QVERIFY(setFugueHome(root, nullptr)); // the next session heals it
        QVERIFY(QFileInfo(root + "/documents").isDir());
    }

    void nearestExistingFolderWalksUp() {
        QTemporaryDir dir;
        QCOMPARE(nearestExistingFolder(dir.filePath("a/b/c")), dir.path());
        QCOMPARE(nearestExistingFolder(dir.path()), dir.path());
    }

    void theDesktopSocketsStayReachable() {
        QTemporaryDir dir;
        qputenv("XDG_RUNTIME_DIR", dir.path().toLocal8Bit());
        qputenv("WAYLAND_DISPLAY", "wayland-1");
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        QVERIFY(QFile(dir.filePath("bus")).open(QIODevice::WriteOnly));
        qunsetenv("PIPEWIRE_RUNTIME_DIR");
        QVERIFY(setFugueHome(dir.filePath("root"), nullptr));
        QCOMPARE(qgetenv("WAYLAND_DISPLAY"), dir.path().toLocal8Bit() + "/wayland-1");
        QCOMPARE(qgetenv("DBUS_SESSION_BUS_ADDRESS"), "unix:path=" + dir.path().toLocal8Bit() + "/bus");
        QCOMPARE(qgetenv("PIPEWIRE_RUNTIME_DIR"), dir.path().toLocal8Bit());
    }

    void theOptionIsTakenOutOfArgvWhereverItIs() {
        QTemporaryDir dir;
        const QByteArray root = dir.filePath("a").toLocal8Bit();
        {
            Args args({"fugue", "--home", root, "run", "x.fugue"});
            QVERIFY(applyFugueHome(args.argc, args.pointers.data(), nullptr));
            QCOMPARE(args.left(), (QStringList{"fugue", "run", "x.fugue"}));
            QVERIFY(args.pointers[size_t(args.argc)] == nullptr);
            QCOMPARE(fugueHome(), QString(root));
        }
        const QByteArray other = dir.filePath("b").toLocal8Bit();
        {
            Args args({"fugue", "run", "--home=" + other, "x.fugue"});
            QVERIFY(applyFugueHome(args.argc, args.pointers.data(), nullptr));
            QCOMPARE(args.left(), (QStringList{"fugue", "run", "x.fugue"}));
            QCOMPARE(fugueHome(), QString(other)); // the option beats FUGUE_HOME
        }
    }

    void theEnvironmentVariableAloneWorks() {
        QTemporaryDir dir;
        qputenv("FUGUE_HOME", dir.filePath("env").toLocal8Bit());
        Args args({"fugue", "node", "list"});
        QVERIFY(applyFugueHome(args.argc, args.pointers.data(), nullptr));
        QCOMPARE(args.argc, 3);
        QCOMPARE(configDir(), dir.filePath("env") + "/config/fugue");
    }

    void aBadRootIsRefusedAndChangesNothing() {
        QString error;
        QVERIFY(!setFugueHome("relative/folder", &error));
        QVERIFY(error.contains("absolute"));
        QTemporaryDir dir;
        QFile file(dir.filePath("file"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(!setFugueHome(dir.filePath("file"), &error));
        Args missing({"fugue", "--home"});
        QVERIFY(!applyFugueHome(missing.argc, missing.pointers.data(), &error));
        QVERIFY(error.contains("--home"));
        QVERIFY(fugueHome().isEmpty());
        QVERIFY(!qEnvironmentVariableIsSet("TMPDIR") || !qgetenv("TMPDIR").contains("file"));
    }

    void applyingTwiceIsTheSame() { // a child process (the run worker) applies the inherited root again
        QTemporaryDir dir;
        QVERIFY(setFugueHome(dir.filePath("r"), nullptr));
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
        Args args({"fugue"});
        QVERIFY(applyFugueHome(args.argc, args.pointers.data(), nullptr));
        QCOMPARE(qgetenv("XDG_RUNTIME_DIR"), runtime);
        QCOMPARE(configDir(), dir.filePath("r") + "/config/fugue");
    }
};

QTEST_GUILESS_MAIN(TestFugueHome)
#include "test_fugue_home.moc"
