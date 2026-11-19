#pragma once

// Open Image, Copy Image and Save Image on a node's context menu (#57, spec 12.3).
//
// They act on the node's picture slot (CanvasRuns::imageFile): its last result, else the file
// a Load Image names, and always the FULL file, never the 512 px thumbnail. The node is the one
// the menu was opened on (CommandRegistry::target "node"), else the first selected one; the
// commands are enabled only when that node has an image file.
//
//   node.open-image  the system's default viewer, through `opener` (QDesktopServices). A file
//                    whose name does not carry its real format's extension (a cache file) is
//                    copied to a temporary name that does, so the viewer recognizes it.
//   node.copy-image  the clipboard gets the decoded QImage and the file's own bytes under their
//                    real MIME type (png, jpeg, webp...), so another app takes the original and
//                    a Fugue paste (image_inputs.h) stores it as it is. Reading and decoding
//                    happen on a worker thread: a big image never holds the window.
//   node.save-image  asks the window for a save dialog (`saveRequested`); the answer comes to
//                    saveTo, which copies the original bytes (the embedded workflow stays)
//                    atomically. The dialog has already asked about an existing file; a name
//                    typed without an extension gets the real one and never replaces a file
//                    already at that name (a notice instead), since the dialog did not ask.
//
// A failure is a `message` for the window banner (Workflows notice).

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

class CanvasModel;

namespace fugue {

class CanvasRuns;
class CommandRegistry;

class ImageActions : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

public:
    ImageActions(CommandRegistry &commands, CanvasModel &canvas, CanvasRuns &runs,
                 std::function<bool(const QUrl &)> opener = {}, QObject *parent = nullptr);

    // The node the commands act on now.
    QString node() const;

    // The file dialog's answer (a file URL or a path). Writes in the background; `saved` or
    // `message` follows.
    Q_INVOKABLE void saveTo(const QString &target);
    // Nothing was chosen: forgets the image the dialog was for.
    Q_INVOKABLE void cancelSave() { m_pendingSource.clear(); }

signals:
    // The window should ask where to save: the folder to start in, the suggested file in it
    // (file URLs made from paths, so any name survives) and the extension of the image's real
    // format (the dialog's default suffix and filter).
    void saveRequested(const QUrl &folder, const QUrl &file, const QString &suffix);
    void saved(const QString &path);
    // The image reached the clipboard.
    void copied();
    void message(const QString &text);

private:
    void open();
    void copy();
    void save();

    CommandRegistry &m_commands;
    CanvasModel &m_canvas;
    CanvasRuns &m_runs;
    std::function<bool(const QUrl &)> m_opener;
    QString m_pendingSource;
};

} // namespace fugue
