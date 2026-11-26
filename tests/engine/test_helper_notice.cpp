// The notice that the faster CUDA helper is not installed (#97, spec 8.10), with the folders and
// the NVIDIA flag faked.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/helper_provider.h"

using namespace fugue;

class HelperNoticeTest : public QObject {
    Q_OBJECT

    static void make(const QTemporaryDir &dir, const QString &name) {
        QFile file(dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n");
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    QByteArray m_helper, m_mark;
    bool m_helperSet = false, m_markSet = false;

private slots:
    void init() { // the tests below change the helper variables: put them back after each
        m_helperSet = qEnvironmentVariableIsSet("FUGUE_HELPER");
        m_helper = qgetenv("FUGUE_HELPER");
        m_markSet = qEnvironmentVariableIsSet("FUGUE_HELPER_FROM_APPRUN");
        m_mark = qgetenv("FUGUE_HELPER_FROM_APPRUN");
    }
    void cleanup() {
        if (m_helperSet) qputenv("FUGUE_HELPER", m_helper); else qunsetenv("FUGUE_HELPER");
        if (m_markSet) qputenv("FUGUE_HELPER_FROM_APPRUN", m_mark); else qunsetenv("FUGUE_HELPER_FROM_APPRUN");
    }

    void showsOnlyForNvidiaWithVulkanHelper() {
        QTemporaryDir lib, state;
        make(lib, "fugue-helper-vulkan");
        QVERIFY(CudaHelperNotice::due({lib.path()}, true, state.path()));
        QVERIFY(!CudaHelperNotice::due({lib.path()}, false, state.path())); // no NVIDIA card

        make(lib, "fugue-helper-cuda"); // installed
        QVERIFY(!CudaHelperNotice::due({lib.path()}, true, state.path()));

        QTemporaryDir empty; // no helper at all: nothing to speed up
        QVERIFY(!CudaHelperNotice::due({empty.path()}, true, state.path()));
    }

    void showsUntilDismissedThenStaysDismissed() {
        QTemporaryDir lib, state;
        make(lib, "fugue-helper-vulkan");
        QVERIFY(CudaHelperNotice::due({lib.path()}, true, state.path()));
        QVERIFY(CudaHelperNotice::due({lib.path()}, true, state.path())); // not used up by looking
        CudaHelperNotice::dismiss(state.path() + "/fugue");               // makes the folder
        QVERIFY(!CudaHelperNotice::due({lib.path()}, true, state.path() + "/fugue"));
        QVERIFY(CudaHelperNotice::due({lib.path()}, true, state.path())); // another state folder
    }

    void anAppRunChoiceDoesNotHideTheOffer() {
        // AppRun exports FUGUE_HELPER for the helper it found; only a helper the person named hides the notice.
        qunsetenv("FUGUE_HELPER");
        qunsetenv("FUGUE_HELPER_FROM_APPRUN");
        QVERIFY(!CudaHelperNotice::helperChosenByUser());

        qputenv("FUGUE_HELPER", "/x/fugue-helper-vulkan");
        QVERIFY(CudaHelperNotice::helperChosenByUser()); // the person's own override
        qputenv("FUGUE_HELPER_FROM_APPRUN", "1");
        QVERIFY(!CudaHelperNotice::helperChosenByUser()); // AppRun's pick
        qputenv("FUGUE_HELPER", "");
        QVERIFY(!CudaHelperNotice::helperChosenByUser()); // still AppRun's mark

        qunsetenv("FUGUE_HELPER_FROM_APPRUN");
        QVERIFY(CudaHelperNotice::helperChosenByUser()); // empty, no mark: the person chose no helper
    }

    void dueFollowsTheOverrideOnThisMachine() {
        // The no-argument due() reads this machine's folders and GPU, so only the override's effect is checked: a
        // person's FUGUE_HELPER makes it false, whatever the machine has.
        qputenv("FUGUE_HELPER", "/x/fugue-helper-vulkan");
        qunsetenv("FUGUE_HELPER_FROM_APPRUN");
        QVERIFY(!CudaHelperNotice::due());
    }

    void commandIsThePacmanInstallOnArch() {
        QCOMPARE(CudaHelperNotice::command(true), QStringLiteral("sudo pacman -S fugue-helper-cuda"));
    }

    void commandIsTheTarballDownloadElsewhere() {
        const QString command = CudaHelperNotice::command(false);
        QVERIFY(command.contains(QStringLiteral("install.sh")));
        QVERIFY(command.endsWith(QStringLiteral("--helper-only")));
        QVERIFY(!command.contains(QStringLiteral("pacman")));
    }

    void anAppImageIsNotAPacmanHost() {
        qputenv("APPIMAGE", "/tmp/Fugue.AppImage");
        QVERIFY(!CudaHelperNotice::usesPacman());
        QCOMPARE(CudaHelperNotice::command(), CudaHelperNotice::command(false));
        qunsetenv("APPIMAGE");
    }
};

QTEST_GUILESS_MAIN(HelperNoticeTest)
#include "test_helper_notice.moc"
