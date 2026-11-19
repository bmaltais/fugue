#include "workflow/image_inputs.h"

#include "canvas/canvas_model.h"
#include "engine/atomic_file.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/vips.h"

#include <QBuffer>
#include <cerrno>
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

// Whether `path` is a regular file (not a link) whose content is exactly `bytes`.
bool holdsBytes(const QString &path, const QByteArray &bytes) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() != bytes.size())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    constexpr qint64 chunk = 1 << 20;
    for (qint64 at = 0; at < bytes.size(); at += chunk)
        if (file.read(chunk) != QByteArrayView(bytes).sliced(at, std::min(chunk, bytes.size() - at)))
            return false;
    return file.atEnd();
}

// Writes `bytes` as <folder>/<name>, or finds them there already. A file there with other
// bytes (or a link) is never replaced: the name takes `-2`, `-3`, ... before the extension.
Errors writeWhole(const QByteArray &bytes, const QString &folder, const QString &name, const FileRoots &roots,
                  QString *path) {
    const QFileInfo named(name);
    for (int n = 1;; ++n) {
        const QString candidate =
            n == 1 ? name : QStringLiteral("%1-%2.%3").arg(named.completeBaseName()).arg(n).arg(named.suffix());
        const QString there = QDir(folder).filePath(candidate);
        if (QFileInfo(there).isSymLink())
            continue;
        const Result<QString> allowed = roots.checkWrite(there, QStringLiteral("file"));
        if (!allowed)
            return allowed.errors();
        *path = allowed.value();
        if (QFileInfo(*path).absolutePath() != canonicalPath(folder))
            return {makeError(QStringLiteral("permission_denied"),
                              QStringLiteral("%1 is not a name Fugue can store.").arg(candidate))};
        if (const Errors made = makeFolder(folder); hasErrors(made))
            return made;
        // The same bytes again are the same file: nothing to write.
        if (holdsBytes(*path, bytes))
            return {};
        if (QFileInfo::exists(*path))
            continue;
        PartialFile partial(folder, candidate);
        if (const Errors written = partial.write(bytes, QStringLiteral("the image")); hasErrors(written))
            return written;
        const Result<bool> committed = partial.commit(*path, Overwrite::Never);
        if (!committed)
            return committed.errors();
        if (*committed || holdsBytes(*path, bytes))
            return {};
    }
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
    // The same bytes are the file already there, whatever it was called: only a regular file
    // whose whole content is these bytes, so a planted name of the right size is not reused.
    const QString tail = QStringLiteral("-%1.%2").arg(hash, format->extension);
    const QStringList patterns{QStringLiteral("*") + tail, QStringLiteral("*-%1-*.%2").arg(hash, format->extension)};
    for (const QFileInfo &same : QDir(folder).entryInfoList(patterns, QDir::Files | QDir::NoSymLinks, QDir::Name))
        if (holdsBytes(same.absoluteFilePath(), bytes)) {
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

Result<QByteArray> clipboardImageBytes(const QMimeData &data, bool pixels) {
    // Fugue's own Copy Image names the type of the original among the ones it offers.
    QStringList types = imageTypes;
    if (const QString original = QString::fromLatin1(data.data(clipboardOriginalType)); types.contains(original))
        types.prepend(original);
    for (const QString &type : std::as_const(types)) {
        if (!data.hasFormat(type))
            continue;
        const QByteArray bytes = data.data(type);
        // The bytes decide what it is: a source may label a JPEG as a PNG.
        if (sniffImage(bytes))
            return bytes;
    }
    if (pixels && data.hasImage()) {
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

namespace {

bool sameBytes(const QString &a, const QString &b) {
    if (QFileInfo(a).isSymLink())
        return false;
    QFile one(a);
    QFile two(b);
    if (one.size() != two.size() || !one.open(QIODevice::ReadOnly) || !two.open(QIODevice::ReadOnly))
        return false;
    while (!one.atEnd())
        if (one.read(1 << 16) != two.read(1 << 16))
            return false;
    return true;
}

// Copies the rest of `from` into a new file at `to`, a megabyte at a time, so a big image is
// never held whole in memory.
Errors copyInto(QFile &from, const QString &to) {
    QFile out(to);
    errno = 0;
    if (!out.open(QIODevice::WriteOnly))
        return {makeError(writeFailureCode(errno != 0 ? errno : EIO),
                          QStringLiteral("Cannot write the image: %1.").arg(out.errorString()))};
    while (!from.atEnd()) {
        const QByteArray chunk = from.read(1 << 20);
        if (chunk.isEmpty())
            return {makeError(QStringLiteral("file_not_found"),
                              QStringLiteral("Cannot read %1: %2.").arg(QFileInfo(from.fileName()).fileName(), from.errorString()))};
        errno = 0;
        if (out.write(chunk) != chunk.size())
            return {makeError(writeFailureCode(errno != 0 ? errno : EIO),
                              QStringLiteral("Cannot write the image: %1.").arg(out.errorString()))};
    }
    if (!out.flush())
        return {makeError(writeFailureCode(errno != 0 ? errno : EIO),
                          QStringLiteral("Cannot write the image: %1.").arg(out.errorString()))};
    return {};
}

// The name `source` takes in `folder`: its own, else `<stem>-2.<ext>`, `-3`, ...; a file there
// with the same bytes is reused. `claimed` holds the names other sources took this time.
QString nameIn(const QString &folder, const QString &source, QHash<QString, QString> &claimed) {
    const QFileInfo info(source);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? info.fileName() : QStringLiteral("%1-%2%3").arg(info.completeBaseName()).arg(n).arg(suffix);
        const auto taken = claimed.constFind(name);
        if (taken != claimed.constEnd()) {
            if (*taken == source)
                return name;
            continue;
        }
        const QString there = QDir(folder).filePath(name);
        const QFileInfo found(there);
        // A link (even one that leads nowhere) holds its name: it is never reused or replaced.
        if (!found.isSymLink() && (!found.exists() || sameBytes(there, source))) {
            claimed.insert(name, source);
            return name;
        }
    }
}

} // namespace

Errors relocateInputs(Document &document, const Registry &registry, const QString &from, const QString &to) {
    const QString global = canonicalPath(inputsDir());
    const QString target = inputsDirBeside(to);
    const QString targetCanonical = canonicalPath(target);
    const QString besideFrom = from.isEmpty() ? QString() : canonicalPath(inputsDirBeside(from));
    const bool newFolder = from.isEmpty()
                           || canonicalPath(QFileInfo(from).absolutePath()) != canonicalPath(QFileInfo(to).absolutePath());
    const auto under = [](const QString &path, const QString &folder) {
        return !folder.isEmpty() && path != folder && isInside(path, folder);
    };

    // Find what to change before touching anything.
    struct Change {
        Nodes *nodes;
        QString node, input;
        QString copy;    // the file to copy into `target`, or empty
        QString literal; // what the input holds afterwards
    };
    QList<Change> changes;
    QHash<QString, QString> claimed; // name in `target` -> the file copied there
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
                const QString text = in->literal().toString();
                const Result<QString> located = inputFilePath(text, from, in.key());
                if (!located || located->isEmpty())
                    continue; // relative in an unsaved workflow: it names nothing yet
                const QString path = canonicalPath(located.value());
                if (path.isEmpty())
                    continue;
                Change change{&nodes, it.key(), in.key(), {}, {}};
                if (under(path, targetCanonical)) {
                    change.literal = QStringLiteral("inputs/") + path.mid(targetCanonical.size() + 1);
                } else if (under(path, global) || under(path, besideFrom)) {
                    change.copy = path;
                    change.literal = QStringLiteral("inputs/") + nameIn(target, path, claimed);
                } else if (newFolder && !QDir::isAbsolutePath(expandHome(text.trimmed()))) {
                    change.literal = path;
                } else {
                    continue;
                }
                if (change.literal != text || !change.copy.isEmpty())
                    changes.append(change);
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
    if (changes.isEmpty())
        return {};

    for (const Change &change : std::as_const(changes)) {
        if (change.copy.isEmpty())
            continue;
        if (const Errors made = makeFolder(target); hasErrors(made))
            return made;
        const QString name = change.literal.mid(7);
        const QString destination = QDir(target).filePath(name);
        if (QFileInfo(destination).isFile() && !QFileInfo(destination).isSymLink())
            continue; // the same bytes (nameIn)
        QFile file(change.copy);
        if (!file.open(QIODevice::ReadOnly))
            return {makeError(QStringLiteral("file_not_found"),
                              QStringLiteral("The image %1 is gone, so the workflow's inputs cannot be moved: %2.")
                                  .arg(QFileInfo(change.copy).fileName(), file.errorString()))};
        PartialFile partial(target, name);
        if (const Errors written = copyInto(file, partial.path()); hasErrors(written))
            return written;
        const Result<bool> committed = partial.commit(destination, Overwrite::Never);
        if (!committed)
            return committed.errors();
        // Something took the name since it was chosen: the node must not point at it.
        if (!*committed && !sameBytes(destination, change.copy))
            return {makeError(QStringLiteral("node_failed"),
                              QStringLiteral("Another file appeared at %1 while the workflow's inputs were copied; "
                                             "save again.").arg(destination))};
    }
    for (const Change &change : std::as_const(changes))
        (*change.nodes)[change.node].inputs.insert(change.input, InputValue::literal(change.literal));
    return {};
}

void makeInputsAbsolute(Document &document, const Registry &registry, const QString &from) {
    if (from.isEmpty())
        return;
    const auto scan = [&](Nodes &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const NodeType *type = registry.nodeType(it->type);
            if (!type)
                continue;
            for (auto in = it->inputs.begin(); in != it->inputs.end(); ++in) {
                const InputSpec *spec = type->input(in.key());
                if (!spec || spec->widget() != QLatin1String("file") || in->kind() != InputValue::Kind::Literal
                    || !in->literal().isString())
                    continue;
                const QString text = in->literal().toString();
                if (text.trimmed().isEmpty() || QDir::isAbsolutePath(expandHome(text.trimmed())))
                    continue;
                const Result<QString> located = inputFilePath(text, from, in.key());
                if (!located || located->isEmpty())
                    continue;
                const QString canonical = canonicalPath(located.value());
                *in = InputValue::literal(canonical.isEmpty() ? located.value() : canonical);
            }
        }
    };
    scan(document.graph.nodes);
    for (auto group = document.groups.begin(); group != document.groups.end(); ++group)
        scan(group->nodes);
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
        list.append(QVariantMap{{"path", input.path},
                                {"url", QUrl::fromLocalFile(input.path)},
                                {"name", input.name},
                                {"modified", input.modified}});
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
    // Encoded bytes first, then a file manager's copy (the files themselves, which its decoded
    // pixels would lose), and only then the pixels (Appendix A).
    Result<QByteArray> bytes = clipboardImageBytes(data, false);
    if (!bytes) {
        if (const QStringList files = clipboardFiles(data); !files.isEmpty())
            return storeFiles(files);
        bytes = clipboardImageBytes(data);
    }
    if (!bytes) {
        emit message(tr("Could not paste the image: %1").arg(bytes.error().message));
        return {};
    }
    const Result<QString> stored = storeImage(bytes.value(), inputsDir(), FileRoots::forInputs());
    if (stored)
        return {stored.value()};
    emit message(tr("Could not paste the image: %1").arg(stored.error().message));
    return {};
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
