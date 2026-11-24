// No test reads or writes the owner's own files (CMakeLists.txt, tests/test_environment.cpp):
// every test runs with HOME and the XDG folders on folders of its own, so Omarchy's state
// (~/.local/state/omarchy, OmarchyPaths::standard()), the run queue, the caches and the
// settings are the test's, and a result never depends on the machine it runs on.
//
// Seam: the environment this test is started with, which ctest gives every test.

#include <QDir>
#include <QtTest>

#include <pwd.h>
#include <unistd.h>

#include "engine/omarchy_provider.h"
#include "engine/paths.h"

using namespace fugue;

namespace {

QString ownersHome() {
    const passwd *entry = ::getpwuid(::getuid());
    return entry && entry->pw_dir ? QDir::cleanPath(QString::fromLocal8Bit(entry->pw_dir)) : QString();
}

} // namespace

class TestIsolation : public QObject {
    Q_OBJECT

private slots:
    void homeIsNotTheOwnersHome() {
        const QString owner = ownersHome();
        QVERIFY(!owner.isEmpty());
        QVERIFY2(QDir::cleanPath(QDir::homePath()) != owner, qPrintable(QDir::homePath()));
    }

    void omarchysStateIsNotTheOwners() {
        const QString owner = ownersHome();
        const OmarchyPaths paths = OmarchyPaths::standard();
        QVERIFY2(!paths.stateDir.startsWith(owner + "/.local/"), qPrintable(paths.stateDir));
        QVERIFY2(!paths.configDir.startsWith(owner + "/.config/"), qPrintable(paths.configDir));
    }

    void theXdgFoldersAreNotTheOwners() {
        const QString owner = ownersHome();
        const QStringList defaults{owner + "/.config", owner + "/.local/state", owner + "/.local/share", owner + "/.cache"};
        for (const char *name : {"XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
            const QString value = QDir::cleanPath(qEnvironmentVariable(name));
            QVERIFY2(!value.isEmpty() && value != QLatin1String("."), name);
            for (const QString &mine : defaults)
                QVERIFY2(value != mine && !value.startsWith(mine + '/'), qPrintable(QStringLiteral("%1=%2").arg(name, value)));
        }
    }

    // The GPU lock lives in the runtime folder and is shared across roots by design (spec 1.4.2), so
    // with the owner's /run/user/<uid> a test's CPU-only `fugue run` waited behind a real GPU run on
    // the machine and timed out (test_basic_cli under load, 2026-11-23).
    void theGpuLockIsNotTheOwners() {
        const QString owner = QStringLiteral("/run/user/%1").arg(::getuid());
        QVERIFY2(!gpuLockPath().startsWith(owner + '/'), qPrintable(gpuLockPath()));
    }
};

QTEST_GUILESS_MAIN(TestIsolation)
#include "test_isolation.moc"
