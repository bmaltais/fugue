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
// test's. Run with a HOME of the caller's, an XDG folder still naming the owner's own goes
// under that HOME. Under ctest HOME and the XDG folders are the test's already and nothing
// changes here.
//
// A test that is killed never removes its home, so each home names its owner (.owner: pid and start time) and the next
// test to start removes the homes of dead owners (test_home_sweep.h).

#include "test_home_sweep.h"

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

// The XDG folders a test reads and writes, with where the owner's own are when the variable is unset.
struct XdgFolder {
    const char *variable;
    const char *underHome;
};
constexpr XdgFolder kXdgFolders[] = {{"XDG_CONFIG_HOME", "/.config"},
                                     {"XDG_STATE_HOME", "/.local/state"},
                                     {"XDG_DATA_HOME", "/.local/share"},
                                     {"XDG_CACHE_HOME", "/.cache"}};

// Lexically, a trailing slash ignored: `~/.config/` is the owner's `~/.config`.
std::filesystem::path normal(const std::string &path) {
    std::filesystem::path p = std::filesystem::path(path).lexically_normal();
    return p.has_filename() || p == p.root_path() ? p : p.parent_path();
}

bool samePath(const std::string &a, const std::string &b) { return normal(a) == normal(b); }

void isolateFromTheOwnersFiles() {
    const passwd *owner = ::getpwuid(::getuid());
    if (!owner || !owner->pw_dir)
        return;
    const std::string ownerHome = owner->pw_dir;
    const char *home = std::getenv("HOME");
    if (!home || !*home || samePath(home, ownerHome)) {
        // First the homes of runs that died (killed by a timeout or a crash, so they never removed theirs).
        test_home_sweep::sweep(std::filesystem::temp_directory_path().string());
        std::string made = (std::filesystem::temp_directory_path() / "fugue-test-home-XXXXXX").string();
        if (!::mkdtemp(made.data()))
            qFatal("cannot make a home for the test under %s", made.c_str());
        g_home = made;
        test_home_sweep::claim(g_home);
        std::atexit(removeHome);
        qputenv("HOME", QByteArray::fromStdString(g_home));
        for (const XdgFolder &folder : kXdgFolders)
            qputenv(folder.variable, QByteArray::fromStdString(g_home + folder.underHome));
        return;
    }
    // A home of the caller's (a scratch HOME, ctest's): its XDG folders are kept, except one that is still the owner's own.
    // A desktop session exports XDG_CONFIG_HOME=~/.config and the rest, so `HOME=/tmp/x build/test_x` from such a shell wrote
    // the owner's settings.json (2026-11-26: test_templates left model_paths on a deleted temporary folder).
    for (const XdgFolder &folder : kXdgFolders) {
        const char *value = std::getenv(folder.variable);
        if (value && samePath(value, ownerHome + folder.underHome))
            qputenv(folder.variable, QByteArray::fromStdString(normal(home).string() + folder.underHome));
    }
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
    // Qt Multimedia's FFmpeg backend probes the machine's GPU decoders and encoders (VAAPI: opens the render node and
    // creates a device per type) the first time a window loads a video or a sound, 1 to 3 s of the first window of
    // every test and more when 12 tests do it at once (2026-11-25). Decoding by software is all a test needs.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", "");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/fugue-tests-have-no-session-bus");
#ifdef FUGUE_TEST_BIN
    // Nor the real omarchy-local-ai (it surveys the tailnet and rewrites state; a machine that has it listed its models in a picker
    // test that expects none): ctest puts the stand-in in build/test-bin first on PATH, and so does a run by hand.
    {
        const QByteArray bin = FUGUE_TEST_BIN;
        const QByteArray path = qgetenv("PATH");
        if (!path.startsWith(bin + ':'))
            qputenv("PATH", path.isEmpty() ? bin : bin + ':' + path);
    }
#endif
    // Nor the machine's GPU helper (a source checkout's build-helper-*/ sits beside build/): none,
    // unless a test names one (empty FUGUE_HELPER: no helper, engine/helper_provider.h).
    if (!qEnvironmentVariableIsSet("FUGUE_HELPER"))
        qputenv("FUGUE_HELPER", "");
    isolateFromTheOwnersFiles();
}

} // namespace

Q_CONSTRUCTOR_FUNCTION(isolateFromTheDesktop)
