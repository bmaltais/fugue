// Linked into every test that makes a QGuiApplication, so it holds when a test binary is run
// by hand too, not only under ctest (CMakeLists.txt sets the same for ctest). It runs before
// main(), so before Qt reads the environment.
//
// The owner's desktop sets QT_QPA_PLATFORMTHEME (qtengine on Omarchy), whose file and colour
// pickers are the xdg-desktop-portal's: a test opening a FileDialog put a real picker on the
// owner's screen, offscreen platform or not. The generic theme has no native dialogs, so
// Qt Quick Dialogs uses its own, inside the test's window. No portal, and no session bus to
// reach one through.

#include <QByteArray>
#include <QtGlobal>

namespace {

void isolateFromTheDesktop() {
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/fugue-tests-have-no-session-bus");
}

} // namespace

Q_CONSTRUCTOR_FUNCTION(isolateFromTheDesktop)
