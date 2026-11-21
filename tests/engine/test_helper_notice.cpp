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

private slots:
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

    void commandIsThePacmanInstall() {
        QCOMPARE(CudaHelperNotice::command(), QStringLiteral("sudo pacman -S fugue-helper-cuda"));
    }
};

QTEST_GUILESS_MAIN(HelperNoticeTest)
#include "test_helper_notice.moc"
