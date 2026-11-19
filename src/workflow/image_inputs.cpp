#include "workflow/image_inputs.h"

#include "canvas/canvas_model.h"
#include "engine/atomic_file.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/vips.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QUrl>

namespace fugue {

namespace {

// The MIME types of the clipboard that carry an encoded image, best first: lossless formats
// ahead of lossy ones, a format that holds more than a plain bitmap ahead of one that holds
// less.
const QStringList imageTypes = {
    QStringLiteral("image/png"),  QStringLiteral("image/webp"), QStringLiteral("image/avif"),
    QStringLiteral("image/tiff"), QStringLiteral("image/jpeg"), QStringLiteral("image/gif"),
    QStringLiteral("image/bmp"),
};

// Whether a file name's extension is one this module writes.
bool sniffExtension(const QString &suffix) {
    static const QStringList known = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("webp"),
                                      QStringLiteral("gif"), QStringLiteral("tif"), QStringLiteral("bmp"),
                                      QStringLiteral("avif")};
    return known.contains(suffix);
}

QString megabytes(qint64 bytes) { return QString::number(bytes / (1024 * 1024)); }

// Whether libvips can read the header of the bytes, and how big the image is.
Result<QSize> readableSize(const QByteArray &bytes, const ImageFormat &format) {
    if (!initVips())
        return vipsUnavailable();
    try {
        const vips::VImage image = vips::VImage::new_from_buffer(bytes.constData(), size_t(bytes.size()), "");
        return QSize(image.width(), image.height());
    } catch (const vips::VError &) {
        return makeError(QStringLiteral("media_unreadable"),
                         QStringLiteral("This is not an image Fugue can read (it looks like %1, but it is damaged or "
                                        "incomplete).").arg(format.extension));
    }
}

Errors writeWhole(const QByteArray &bytes, const QString &folder, const QString &name, const FileRoots &roots,
                  QString *path) {
    const Result<QString> allowed = roots.checkWrite(QDir(folder).filePath(name), QStringLiteral("file"));
    if (!allowed)
        return allowed.errors();
    *path = allowed.value();
    if (QFileInfo(*path).absolutePath() != canonicalPath(folder))
        return {makeError(QStringLiteral("permission_denied"), QStringLiteral("%1 is not a name Fugue can store.").arg(name))};
    if (const Errors made = makeFolder(folder); hasErrors(made))
        return made;
    // The same bytes again are the same file: nothing to write.
    if (QFileInfo info(*path); info.isFile() && info.size() == bytes.size())
        return {};
    PartialFile partial(folder, name);
    if (const Errors written = partial.write(bytes, QStringLiteral("the image")); hasErrors(written))
        return written;
    const Result<bool> committed = partial.commit(*path, Overwrite::Replace);
    return committed ? Errors() : committed.errors();
}

} // namespace

std::optional<ImageFormat> sniffImage(QByteArrayView b) {
    if (b.startsWith(QByteArrayView("\x89PNG\r\n\x1a\n", 8)))
        return ImageFormat{QStringLiteral("png"), QStringLiteral("image/png")};
    if (b.startsWith(QByteArrayView("\xff\xd8\xff", 3)))
        return ImageFormat{QStringLiteral("jpg"), QStringLiteral("image/jpeg")};
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP")
        return ImageFormat{QStringLiteral("webp"), QStringLiteral("image/webp")};
    if (b.startsWith("GIF87a") || b.startsWith("GIF89a"))
        return ImageFormat{QStringLiteral("gif"), QStringLiteral("image/gif")};
    if (b.startsWith(QByteArrayView("II*\0", 4)) || b.startsWith(QByteArrayView("MM\0*", 4)))
        return ImageFormat{QStringLiteral("tif"), QStringLiteral("image/tiff")};
    if (b.size() >= 14 && b.startsWith("BM") && b.mid(6, 4) == QByteArrayView("\0\0\0\0", 4))
        return ImageFormat{QStringLiteral("bmp"), QStringLiteral("image/bmp")};
    if (b.size() >= 12 && b.mid(4, 4) == "ftyp" && (b.mid(8, 4) == "avif" || b.mid(8, 4) == "avis"))
        return ImageFormat{QStringLiteral("avif"), QStringLiteral("image/avif")};
    return std::nullopt;
}

QString safeStem(const QString &name) {
    QString text = name.mid(std::max(name.lastIndexOf(QLatin1Char('/')), name.lastIndexOf(QLatin1Char('\\'))) + 1);
    if (const int dot = text.lastIndexOf(QLatin1Char('.')); dot > 0)
        text.truncate(dot);
    QString stem;
    for (const QChar c : text)
        stem += (c.isLetterOrNumber() && c.unicode() < 0x250) || c == QLatin1Char('-') || c == QLatin1Char('_') ||
                        c == QLatin1Char('.')
                    ? c
                    : QLatin1Char('_');
    while (stem.startsWith(QLatin1Char('.')))
        stem.remove(0, 1);
    stem.truncate(48);
    return stem.isEmpty() || stem.count(QLatin1Char('_')) == stem.size() ? QStringLiteral("image") : stem;
}

Result<QString> storeImage(const QByteArray &bytes, const QString &folder, const FileRoots &roots, const QString &stem) {
    if (bytes.isEmpty())
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("There is no image data."));
    if (bytes.size() > maxImageBytes)
        return makeError(QStringLiteral("value_out_of_range"),
                         QStringLiteral("The image is %1 MB; Fugue takes at most %2 MB.")
                             .arg(megabytes(bytes.size()), megabytes(maxImageBytes)));
    const std::optional<ImageFormat> format = sniffImage(bytes);
    if (!format)
        return makeError(QStringLiteral("media_unreadable"),
                         QStringLiteral("This is not an image Fugue knows (PNG, JPEG, WebP, GIF, TIFF, BMP or AVIF)."));
    const Result<QSize> size = readableSize(bytes, *format);
    if (!size)
        return size.errors();
    if (qint64(size->width()) * size->height() > maxImagePixels)
        return makeError(QStringLiteral("value_out_of_range"),
                         QStringLiteral("The image is %1 x %2 pixels; Fugue takes at most %3 million.")
                             .arg(size->width()).arg(size->height()).arg(maxImagePixels / 1000000));
    const QString hash = sha256(bytes).mid(QStringLiteral("sha256:").size()).left(10);
    // The same bytes are the file already there, whatever it was called.
    const QString tail = QStringLiteral("-%1.%2").arg(hash, format->extension);
    for (const QFileInfo &same : QDir(folder).entryInfoList({QStringLiteral("*") + tail}, QDir::Files))
        if (same.size() == bytes.size()) {
            const Result<QString> allowed = roots.checkRead(same.absoluteFilePath(), QStringLiteral("file"));
            return allowed ? Result<QString>(allowed.value()) : Result<QString>(allowed.errors());
        }
    const QString name = (stem.isEmpty() ? QDateTime::currentDateTime().toString(QStringLiteral("'pasted-'yyyyMMdd-HHmmss"))
                                         : safeStem(stem)) + tail;
    QString path;
    if (const Errors failed = writeWhole(bytes, folder, name, roots, &path);
        hasErrors(failed))
        return failed;
    return path;
}

Result<QString> storeImageFile(const QString &path, const QString &folder, const FileRoots &roots) {
    const QFileInfo info(path);
    if (!info.isFile()) // a folder, a device or a pipe is never read
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("%1 is not a file.").arg(path));
    if (info.size() > maxImageBytes)
        return makeError(QStringLiteral("value_out_of_range"),
                         QStringLiteral("%1 is %2 MB; Fugue takes at most %3 MB.")
                             .arg(info.fileName(), megabytes(info.size()), megabytes(maxImageBytes)));
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"),
                         QStringLiteral("%1 cannot be read: %2.").arg(info.fileName(), file.errorString()));
    return storeImage(file.read(maxImageBytes + 1), folder, roots, info.fileName());
}

QStringList clipboardImageTypes() { return imageTypes; }

QStringList clipboardFiles(const QMimeData &data) {
    QStringList files;
    for (const QUrl &url : data.urls())
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    return files;
}

Result<QByteArray> clipboardImageBytes(const QMimeData &data) {
    for (const QString &type : imageTypes) {
        if (!data.hasFormat(type))
            continue;
        const QByteArray bytes = data.data(type);
        // The bytes decide what it is: a source may label a JPEG as a PNG.
        if (sniffImage(bytes))
            return bytes;
    }
    if (data.hasImage()) {
        const QImage image = qvariant_cast<QImage>(data.imageData());
        if (!image.isNull()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (image.save(&buffer, "PNG"))
                return png;
        }
    }
    return makeError(QStringLiteral("media_unreadable"), QStringLiteral("The clipboard holds no image Fugue can read."));
}

bool clipboardHasImage(const QMimeData &data) {
    if (data.hasImage())
        return true;
    for (const QString &type : imageTypes)
        if (data.hasFormat(type))
            return true;
    return !clipboardFiles(data).isEmpty();
}

Errors relocateInputs(Document &document, const Registry &registry, const QString &to) {
    const QString source = canonicalPath(inputsDir());
    if (source.isEmpty() || source == canonicalPath(to))
        return {};

    // Find what to move before touching anything.
    struct Move {
        Nodes *nodes;
        QString node, input, from, target;
    };
    QList<Move> moves;
    auto scan = [&](Nodes &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const NodeType *type = registry.nodeType(it->type);
            if (!type)
                continue;
            for (auto in = it->inputs.begin(); in != it->inputs.end(); ++in) {
                const InputSpec *spec = type->input(in.key());
                if (!spec || spec->widget() != QLatin1String("file") || in->kind() != InputValue::Kind::Literal
                    || !in->literal().isString())
                    continue;
                const QString path = canonicalPath(expandHome(in->literal().toString()));
                if (!path.isEmpty() && path != source && isInside(path, source))
                    moves.append({&nodes, it.key(), in.key(), path, QDir(to).filePath(QFileInfo(path).fileName())});
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
    if (moves.isEmpty())
        return {};

    if (const Errors made = makeFolder(to); hasErrors(made))
        return made;
    for (const Move &move : moves) {
        const QFileInfo target(move.target);
        if (target.isFile() && target.size() == QFileInfo(move.from).size())
            continue;
        QFile file(move.from);
        if (!file.open(QIODevice::ReadOnly))
            return {makeError(QStringLiteral("file_not_found"),
                              QStringLiteral("The image %1 is gone, so the workflow's inputs cannot be moved: %2.")
                                  .arg(QFileInfo(move.from).fileName(), file.errorString()))};
        PartialFile partial(to, QFileInfo(move.from).fileName());
        if (const Errors written = partial.write(file.readAll(), QStringLiteral("the image")); hasErrors(written))
            return written;
        if (const Result<bool> committed = partial.commit(move.target, Overwrite::Replace); !committed)
            return committed.errors();
    }
    for (const Move &move : moves)
        (*move.nodes)[move.node].inputs.insert(move.input, InputValue::literal(move.target));
    return {};
}

QList<RecentInput> recentInputs(int limit) {
    QList<RecentInput> list;
    const QFileInfoList files = QDir(inputsDir()).entryInfoList(QDir::Files, QDir::Time);
    for (const QFileInfo &file : files) {
        if (list.size() >= limit)
            break;
        if (sniffExtension(file.suffix()))
            list.append({file.absoluteFilePath(), file.fileName(), file.lastModified().toMSecsSinceEpoch()});
    }
    return list;
}

// --- on the canvas ---

ImageInputs::ImageInputs(CanvasModel *canvas, const Registry *registry, QObject *parent)
    : QObject(parent), m_canvas(canvas), m_registry(registry) {
    m_canvas->setImages(this);
}

QVariantList ImageInputs::recent() const {
    QVariantList list;
    for (const RecentInput &input : recentInputs(recentLimit))
        list.append(QVariantMap{{"path", input.path}, {"name", input.name}, {"modified", input.modified}});
    return list;
}

bool ImageInputs::use(const QString &node, const QString &path) {
    // Only what is in the inputs folder, by its canonical path; anything else is the file browser's.
    const QString canonical = canonicalPath(path);
    if (!accepts(node, QStringLiteral("file")) || !isInside(canonical, canonicalPath(inputsDir()))
        || !QFileInfo(canonical).isFile())
        return false;
    return take({canonical}, node, QPointF());
}

bool ImageInputs::accepts(const QString &node, const QString &input) const {
    const Document document = m_canvas->document();
    return input == QLatin1String("file") && document.graph.nodes.value(node).type == QLatin1String(loadImageType);
}

// The one selected Load Image, when the selection is exactly that.
bool ImageInputs::replacing(QString &node) const {
    const QStringList selected = m_canvas->selectedIds();
    if (selected.size() != 1 || !accepts(selected.first(), QStringLiteral("file")))
        return false;
    node = selected.first();
    return true;
}

QPointF ImageInputs::pastePoint() const {
    const QVariant pointer = m_canvas->pointer();
    if (pointer.isValid())
        return pointer.toPointF();
    const QJsonArray center = m_canvas->document().layout.value("view").toObject().value("center").toArray();
    return center.size() == 2 ? QPointF(center.at(0).toDouble(), center.at(1).toDouble()) : QPointF(80, 80);
}

QStringList ImageInputs::storeClipboard(const QMimeData &data) {
    const FileRoots roots = FileRoots::forInputs();
    const Result<QByteArray> bytes = clipboardImageBytes(data);
    if (bytes) {
        const Result<QString> stored = storeImage(bytes.value(), inputsDir(), roots);
        if (stored)
            return {stored.value()};
        emit message(tr("Could not paste the image: %1").arg(stored.error().message));
        return {};
    }
    // No image data: a file manager's copy offers the files themselves.
    const QStringList files = clipboardFiles(data);
    if (files.isEmpty())
        emit message(tr("Could not paste the image: %1").arg(bytes.error().message));
    return storeFiles(files);
}

QStringList ImageInputs::storeFiles(const QStringList &paths) {
    const FileRoots roots = FileRoots::forInputs();
    QStringList stored, failures;
    for (const QString &path : paths) {
        const Result<QString> result = storeImageFile(path, inputsDir(), roots);
        if (result)
            stored.append(result.value());
        else
            failures.append(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), result.error().message));
    }
    if (!failures.isEmpty())
        emit message(tr("Could not take the image: %1").arg(failures.join(QLatin1Char(' '))));
    return stored;
}

bool ImageInputs::take(const QStringList &files, const QString &replace, QPointF at) {
    if (files.isEmpty())
        return false;
    if (!replace.isEmpty()
        && m_canvas->document().graph.nodes.value(replace).inputs.value(QStringLiteral("file")).kind()
               == InputValue::Kind::Link) {
        emit message(tr("The file of this node comes from a link; take the link out to paste an image."));
        return false;
    }
    // Several images are several nodes, one step in all.
    m_canvas->beginEdit();
    QStringList nodes;
    int made = 0;
    for (int i = 0; i < files.size(); ++i) {
        QString node = i == 0 ? replace : QString();
        if (node.isEmpty())
            node = m_canvas->addNode(QLatin1String(loadImageType), at.x(), at.y() + made++ * newNodeStep);
        if (node.isEmpty())
            continue;
        Document document = m_canvas->document();
        document.graph.nodes[node].inputs.insert(QStringLiteral("file"), InputValue::literal(files.at(i)));
        m_canvas->commit(document, {});
        nodes.append(node);
    }
    m_canvas->select(nodes, QStringLiteral("replace"));
    m_canvas->endEdit(replace.isEmpty() ? tr("Paste image") : tr("Replace image"));
    return !nodes.isEmpty();
}

bool ImageInputs::paste(const QMimeData &data) {
    if (!clipboardHasImage(data))
        return false;
    QString node;
    replacing(node);
    take(storeClipboard(data), node, pastePoint());
    return true;
}

bool ImageInputs::pasteInto(const QString &node) {
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!accepts(node, QStringLiteral("file")) || !data || !clipboardHasImage(*data)) {
        emit message(tr("The clipboard holds no image."));
        return false;
    }
    return take(storeClipboard(*data), node, pastePoint());
}

bool ImageInputs::dropFiles(const QStringList &paths, QPointF at, const QString &onNode) {
    return take(storeFiles(paths), accepts(onNode, QStringLiteral("file")) ? onNode : QString(), at);
}

} // namespace fugue
