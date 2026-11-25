// Linked into every test that makes a QGuiApplication, every engine test and every CLI test (the
// `fugue` they start inherits it), so it holds when a test binary is run
// by hand too, not only under ctest (CMakeLists.txt sets the same for ctest). It runs before
// main(), so before Qt reads the environment.
//
// The owner's desktop sets QT_QPA_PLATFORMTHEME (qtengine on Omarchy), whose file and colour
// pickers are the xdg-desktop-portal's: a test opening a FileDialog put a real picker on the
// owner's screen, offscreen platform or not. The generic theme has no native dialogs, so
// Qt Quick Dialogs uses its own, inside the test's window. No portal, and no session bus to
// reach one through.
//
// Nor does a test read or write the owner's files: run by hand with the owner's own HOME, a
// test gets a home of its own (a temporary folder, removed at exit) and XDG folders in it,
// so Omarchy's state (~/.local/state/omarchy), the queue, caches and settings are the
// test's. Under ctest HOME is the test's already and nothing changes here.

#include <QByteArray>
#include <QtGlobal>

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace {

std::string g_home; // the temporary home made here, removed at exit

void removeHome() {
    std::error_code ignored;
    std::filesystem::remove_all(g_home, ignored);
}

void isolateFromTheOwnersFiles() {
    const passwd *owner = ::getpwuid(::getuid());
    const char *home = std::getenv("HOME");
    if (!owner || !owner->pw_dir
        || (home && std::filesystem::path(home).lexically_normal() != std::filesystem::path(owner->pw_dir).lexically_normal()))
        return;
    std::string made = (std::filesystem::temp_directory_path() / "fugue-test-home-XXXXXX").string();
    if (!::mkdtemp(made.data()))
        qFatal("cannot make a home for the test under %s", made.c_str());
    g_home = made;
    std::atexit(removeHome);
    qputenv("HOME", QByteArray::fromStdString(g_home));
    qputenv("XDG_CONFIG_HOME", QByteArray::fromStdString(g_home + "/.config"));
    qputenv("XDG_STATE_HOME", QByteArray::fromStdString(g_home + "/.local/state"));
    qputenv("XDG_DATA_HOME", QByteArray::fromStdString(g_home + "/.local/share"));
    qputenv("XDG_CACHE_HOME", QByteArray::fromStdString(g_home + "/.cache"));
}

void isolateFromTheDesktop() {
    // Never the owner's screen: a test binary run by hand (an agent taking grabs, someone trying one test) gets the
    // offscreen platform as ctest does, and no display to reach (2026-11-24: grab runs from an agent's shell, whose
    // QT_QPA_PLATFORM was the desktop's, put windows on the owner's screen). FUGUE_TEST_ON_SCREEN=1 asks for a real
    // window on purpose.
    if (qgetenv("FUGUE_TEST_ON_SCREEN") != "1") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("WAYLAND_DISPLAY");
        qunsetenv("DISPLAY");
    }
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    // The FFmpeg backend of Qt Multimedia dumps every file it opens to stderr (main.cpp turns it off in the app).
    if (!qEnvironmentVariableIsSet("QT_LOGGING_RULES"))
        qputenv("QT_LOGGING_RULES", "qt.multimedia.ffmpeg.mediadataholder.info=false");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/fugue-tests-have-no-session-bus");
    // Nor the machine's GPU helper (a source checkout's build-helper-*/ sits beside build/): none,
    // unless a test names one (empty FUGUE_HELPER: no helper, engine/helper_provider.h).
    if (!qEnvironmentVariableIsSet("FUGUE_HELPER"))
        qputenv("FUGUE_HELPER", "");
    isolateFromTheOwnersFiles();
}

} // namespace

Q_CONSTRUCTOR_FUNCTION(isolateFromTheDesktop)
