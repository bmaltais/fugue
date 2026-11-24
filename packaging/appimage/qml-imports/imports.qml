// The Qt QML modules the app imports, for linuxdeploy-plugin-qt. Fugue's own QML is compiled into the binary and its
// module (`import Fugue`) has no folder the plugin could scan, so the plugin scans this file instead. check-qml-imports.sh
// fails when src/ imports a Qt module that is not listed here (a missing one would be missing from the AppImage).
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Shapes
import QtCore
import QtMultimedia

Item {}
