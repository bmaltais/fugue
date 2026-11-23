#pragma once

// Pasting and dropping images into Load Image (spec 14.1, Appendix A "images in").
//
// Load Image reads a file path, so an image that arrives from the clipboard or a file
// manager is first made a file: written whole into the one inputs folder (paths.h inputsDir,
// ~/Pictures/Fugue/inputs), named by the SHA-256 of its bytes with the extension of the format found in those bytes, and the
// node's `file` input points to it. The same bytes twice are one file.
//
// The format is never changed: a dropped file is copied byte for byte (EXIF orientation,
// color profile and an embedded workflow stay), and from the clipboard the encoded bytes of
// the best image type on offer are stored as they came. Only when the clipboard holds
// decoded pixels alone (a QImage) is a PNG written, which is lossless.
//
// The pure parts (sniffing, naming, storing, moving a workflow's inputs on save) are free
// functions; ImageInputs puts them on the canvas, as undo steps.

#include "engine/document.h"
#include "engine/file_roots.h"
#include "engine/registry.h"
#include "engine/result.h"

#include <QByteArray>
#include <QMimeData>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/image_format.h"

class CanvasModel;

namespace fugue {


// A safe file name stem from any text (usually the name a file had): the last path component
// without its extension, anything but letters, digits, `-`, `_` and `.` made `_`, no leading
// dot, at most 48 characters; "image" when nothing is left.
QString safeStem(const QString &name);

// Checks the bytes are an image Fugue can read and not an absurd one, then writes them as
// <folder>/<stem>-<first 10 hex of the SHA-256>.<extension of the real format> (atomic) and
// returns that path. The same bytes again, under whatever stem, are the file already there: a
// regular file (not a link) whose whole content is these bytes, never one judged by name and
// size. A different file under the name stays; the name takes `-2`, `-3` before the extension.
// `stem` is made safe by safeStem; empty names it "pasted-YYYYMMDD-HHMMSS". `roots` must allow
// writing the folder. The errors are worded for the person who pasted.
Result<QString> storeImage(const QByteArray &bytes, const QString &folder, const FileRoots &roots,
                           const QString &stem = {});
// The same for a file on disk, copied byte for byte. It must be a regular file; the size is
// checked before it is read.
Result<QString> storeImageFile(const QString &path, const QString &folder, const FileRoots &roots);

// A copy from the Canvas view with a selection (#327) puts, beside the picture (its alpha is the selection), the selection itself
// under this type: a gray PNG of the picture's size, white where it was selected. A paste into a Load Image keeps it as the
// node's painted `mask`, so the one paste gives the picture and its mask.
inline constexpr const char *clipboardMaskType = "application/x-fugue-mask";

// The MIME types taken from the clipboard, best first.
QStringList clipboardImageTypes();
// Holds the MIME type of the original image when Fugue's Copy Image offers it beside a PNG made
// for other apps: a paste takes that type first.
inline constexpr const char *clipboardOriginalType = "application/x-fugue-image-type";
// The encoded bytes of the first of those types whose bytes really are an image, else (with
// `pixels`) the decoded pixels as PNG. Not ok when the data holds no such image. A paste tries
// a `text/uri-list`'s files between the two.
Result<QByteArray> clipboardImageBytes(const QMimeData &data, bool pixels = true);
// True when the data offers an image to paste: encoded, decoded, or as local image files.
bool clipboardHasImage(const QMimeData &data);
// The local files of a `text/uri-list` (a file manager's copy).
QStringList clipboardFiles(const QMimeData &data);

// Makes a workflow saved from `from` (its file until now, empty when unsaved) to `to` portable
// (Appendix A): every input-file literal (a `file` input's picture, a `mask` input's painted mask: InputSpec::holdsInputFile) that names a file inside the inputs folder (paths.h
// inputsDir), or inside the `inputs` folder beside `from`, is copied into the `inputs` folder
// beside `to` under the same name and the literal becomes `inputs/<name>`, relative to the
// workflow's folder; a full path into that folder becomes relative too. Any other relative
// literal becomes the full path it named from `from`'s folder, as it would lead nowhere from a
// new one. Copies, never moves: other workflows may use the file. The document is changed only
// when every copy worked.
Errors relocateInputs(Document &document, const Registry &registry, const QString &from, const QString &to);

// Makes every relative input-file literal the full path it names from `from`'s folder (a
// workflow file), so the document names the same files from any folder. Nothing when `from`
// is empty: a relative path in an unsaved workflow names nothing.
void makeInputsAbsolute(Document &document, const Registry &registry, const QString &from);

// Takes into the inputs folder (storeImageFile) every image an input-file literal names, by
// its full path, inside `folder` (a template's), and points the literal at the stored copy, so
// the document no longer leans on that folder. The document is changed only when every image
// was stored.
Errors takeInputsFrom(Document &document, const Registry &registry, const QString &folder);

// One image in the inputs folder, for the "Recent inputs" list.
struct RecentInput {
    QString path;
    QString name;
    qint64 modified = 0; // ms since the epoch
};
// The newest `limit` images of the inputs folder, newest first. Only names and times are
// read; thumbnails are the list's own, made as each row is shown.
QList<RecentInput> recentInputs(int limit);

// Pastes and drops on a canvas. Created by Workflows; reached by the paste command, the
// Paste button on the node and the window's drop area.
class ImageInputs : public QObject {
    Q_OBJECT

public:
    ImageInputs(CanvasModel *canvas, const Registry *registry, QObject *parent = nullptr);

    // The node type pasted and dropped images go into.
    static constexpr const char *loadImageType = "fugue/load-image";

    // Paste from the clipboard (after workflow data has been tried). With exactly one Load
    // Image selected it takes the image, else a Load Image node is made at the pointer, one
    // undo step either way. True when the data held an image, even if it was refused (the
    // reason is a `message`); false when it held none, and nothing was done.
    bool paste(const QMimeData &data);
    // The Paste button of a Load Image node: its image from the clipboard.
    Q_INVOKABLE bool pasteInto(const QString &node);
    // The input an image can be pasted or dropped into on this node: a Load Image's `file`, or the exposed input of a
    // group instance that ends in a Load Image's; empty for any other node.
    Q_INVOKABLE QString imageInput(const QString &node) const;
    // Whether the node and input are the ones an image can be pasted into.
    Q_INVOKABLE bool accepts(const QString &node, const QString &input) const;

    // Dropped files: each image goes to a Load Image node, the first into `onNode` when that is
    // one, the rest to new nodes below `at`. Files that are not images are skipped with a
    // message. True when any image was placed.
    bool dropFiles(const QStringList &paths, QPointF at, const QString &onNode = {});

    // The "Recent inputs" of the file picker: {path, url (a file URL made from the path), name,
    // modified}, newest first, at most 48.
    Q_INVOKABLE QVariantList recent() const;
    // Points a Load Image's file at one of them (or any file in the inputs folder): one undo step.
    Q_INVOKABLE bool use(const QString &node, const QString &path);

signals:
    // Why an image was not taken, worded for the person.
    void message(const QString &text);

private:
    QStringList storeClipboard(const QMimeData &data);
    QStringList storeFiles(const QStringList &paths);
    // Puts stored files on the canvas as one undo step: the first into `replace` when it is a
    // node, the others (or all) in new Load Image nodes down from `at`.
    // `mask`, when not empty, is a file of the inputs folder the first Load Image takes as its painted mask.
    bool take(const QStringList &files, const QString &replace, QPointF at, const QString &mask = {});
    // The clipboard's selection mask (clipboardMaskType) stored in the inputs folder; empty when it has none or it is no image.
    QString storeClipboardMask(const QMimeData &data);
    bool replacing(QString &node) const;
    QPointF pastePoint() const;

    // How far apart the nodes of several images are put (world units).
    static constexpr qreal newNodeStep = 320;
    static constexpr int recentLimit = 48;

    CanvasModel *m_canvas;
    const Registry *m_registry;
};

} // namespace fugue
