#pragma once

// A test that deletes library or user files (the group library, the packs folder) first proves the
// folder is inside the test's own home: HOME is not the owner's real home (the one the passwd file
// gives) and the folder lies under HOME. tests/test_environment.cpp and ctest both arrange that;
// this is the last check before a removeRecursively, so a missed arrangement fails the test
// instead of deleting the owner's ~/.local/share/fugue.
//
//   FUGUE_REQUIRE_TEST_HOME(fugue::libraryDir());   // in a test function or init(): QFAILs and returns

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <pwd.h>
#include <unistd.h>

namespace test_home {

inline bool isUnderTestHome(const QString &path) {
    const passwd *owner = ::getpwuid(::getuid());
    const QString real = owner && owner->pw_dir ? QDir::cleanPath(QString::fromLocal8Bit(owner->pw_dir)) : QString();
    const QString home = QDir::cleanPath(QDir::homePath());
    if (path.isEmpty() || home.isEmpty() || home == QLatin1String("/") || home == real)
        return false;
    // The test's home may itself sit below the owner's (build/test-home/...), so no prefix test
    // against the owner's home: only that it is not that home, and the folder is below it.
    const QString where = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    return where.startsWith(home + QLatin1Char('/'));
}

} // namespace test_home

#define FUGUE_REQUIRE_TEST_HOME(path)                                                                                   \
    do {                                                                                                                \
        if (!test_home::isUnderTestHome(path))                                                                          \
            QFAIL(qPrintable(QStringLiteral("refusing to delete %1: it is not under the test's own home (%2)")          \
                                 .arg(path, QDir::homePath())));                                                        \
    } while (false)
