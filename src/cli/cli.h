#pragma once

// The command line (spec 13). It is the same binary as the window app:
// main() asks wantsCli() (args.h) before it creates a QGuiApplication, and
// calls run() when the answer is yes. Nothing here may touch Gui, Quick, QML or
// open a dialog.

#include <QStringList>

namespace fugue::cli {

// Runs one command to the end and returns the process exit code (0 or 1).
// Creates its own QCoreApplication, so call it from main() only.
int run(int argc, char **argv);

} // namespace fugue::cli
