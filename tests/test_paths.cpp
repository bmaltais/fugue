#include "test_paths.h"

// The only translation unit that sees the worktree's absolute paths (CMakeLists.txt, fugue_test_support).
#ifndef FUGUE_TEST_SOURCE_DIR
#error "test_paths.cpp needs the FUGUE_TEST_* definitions from CMakeLists.txt"
#endif

namespace fugue_test {

QString sourceDir() { return QStringLiteral(FUGUE_TEST_SOURCE_DIR); }
QString buildDir() { return QStringLiteral(FUGUE_TEST_BUILD_DIR); }
QString fugueBinary() { return QStringLiteral(FUGUE_TEST_BINARY); }
QString fakeBinary() { return QStringLiteral(FUGUE_TEST_FAKE_BINARY); }
QString fakeHelper() { return QStringLiteral(FUGUE_TEST_FAKE_HELPER); }
QString fakeModuleProvider() { return QStringLiteral(FUGUE_TEST_FAKE_MODULE_PROVIDER); }

} // namespace fugue_test
