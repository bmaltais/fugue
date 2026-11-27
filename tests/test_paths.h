// Where the source tree, the build folder and the test helper programs are, for a test that reads a
// file of the repo or starts a built program. The paths are compiled into one object (tests/test_paths.cpp,
// the fugue_test_support library) instead of into every test's compile line, so a test's objects are
// the same in every git worktree and ccache can reuse them across worktrees.
#pragma once

#include <QString>

namespace fugue_test {

QString sourceDir();           // the repo root
QString buildDir();            // the build folder
QString fugueBinary();         // the built `fugue`
QString fakeBinary();          // fugue_with_fakes: `fugue` with fake node kernels
QString fakeHelper();          // fake_fugue_helper
QString fakeModuleProvider();  // fake_module_provider

} // namespace fugue_test

// The names the tests write (they are functions, not compile-line defines).
inline QString fugueSourceDir() { return fugue_test::sourceDir(); }
inline QString fugueBuildDir() { return fugue_test::buildDir(); }
inline QString fugueBinary() { return fugue_test::fugueBinary(); }
inline QString fugueFakeBinary() { return fugue_test::fakeBinary(); }
inline QString fugueFakeHelper() { return fugue_test::fakeHelper(); }
inline QString fugueFakeModuleProvider() { return fugue_test::fakeModuleProvider(); }
