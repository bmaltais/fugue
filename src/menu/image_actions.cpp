#include "menu/image_actions.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QPointer>
#include <QStandardPaths>
#include <QThreadPool>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/atomic_file.h"
#include "engine/file_roots.h"
#include "engine/paths.h"
#include "run/canvas_runs.h"
#include "workflow/image_inputs.h"

namespace fugue {

namespace {

QString say(const char *text) { return QCoreApplication::translate("ImageActions", text); }

// The whole file, refused when it is not an image Fugue knows or is absurdly big.
Result<QByteArray> readImage(const QString &path, ImageFormat &format) {
    QFile file(path);
    if (QFileInfo(path).size() > maxImageBytes)
        return makeError("invalid_value", say("The image is too big to use."));
    if (!file.open(QIODevice::ReadOnly))
        return makeError("file_not_found", say("The image file could not be read: %1").arg(file.errorString()));
    const QByteArray bytes = file.readAll();
    const auto sniffed = sniffImage(bytes);
    if (!sniffed)
        return makeError("invalid_value", say("The file is not an image Fugue can use."));
    format = *sniffed;
    return bytes;
}

// The format named by the first bytes of the file.
std::optional<ImageFormat> formatOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return sniffImage(file.read(64));
}

bool hasExtension(const QString &path, const QString &extension) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == extension || (extension == QLatin1String("jpg") && suffix == QLatin1String("jpeg"))
           || (extension == QLatin1String("tif") && suffix == QLatin1String("tiff"));
}

QString suggestedFolder() {
    for (const QString &dir : {outputDir(), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)})
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            return dir;
    return QDir::homePath();
}

} // namespace

ImageActions::ImageActions(CommandRegistry &commands, CanvasModel &canvas, CanvasRuns &runs,
                           std::function<bool(const QUrl &)> opener, QObject *parent)
    : QObject(parent), m_commands(commands), m_canvas(canvas), m_runs(runs), m_opener(std::move(opener)) {
    if (!m_opener)
        m_opener = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    // The commands are registered with the menus' (menu_commands.cpp), not available until here.
    for (const char *id : {"node.open-image", "node.copy-image", "node.save-image"})
        commands.setEnabledWhen(QString::fromLatin1(id), [this] { return !m_runs.imageFile(node()).isEmpty(); });
    commands.setHandler("node.open-image", [this] { open(); });
    commands.setHandler("node.copy-image", [this] { copy(); });
    commands.setHandler("node.save-image", [this] { save(); });
}

QString ImageActions::node() const {
    const QString target = m_commands.target().value("node").toString();
    if (!target.isEmpty())
        return target;
    const QStringList selected = m_canvas.selectedIds();
    return selected.isEmpty() ? QString() : selected.first();
}

void ImageActions::open() {
    const QString path = m_runs.imageFile(node());
    if (path.isEmpty())
        return;
    QString shown = path;
    const auto format = formatOf(path);
    // A viewer picks the program by the extension: a cache file has none, or a wrong one.
    if (format && !hasExtension(path, format->extension)) {
        const QString key = QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        const QString dir = QDir::tempPath() + QStringLiteral("/fugue-view");
        shown = dir + '/' + key + '.' + format->extension;
        if (!QFileInfo::exists(shown)) {
            QDir().mkpath(dir);
            const QString part = shown + QStringLiteral(".part");
            QFile::remove(part);
            if (!QFile::copy(path, part) || !QFile::rename(part, shown)) {
                QFile::remove(part);
                emit message(say("Could not prepare the image to open it."));
                return;
            }
        }
    }
    if (!m_opener(QUrl::fromLocalFile(shown)))
        emit message(say("Could not open the image: no program is set to show it."));
}

void ImageActions::copy() {
    const QString path = m_runs.imageFile(node());
    if (path.isEmpty())
        return;
    QPointer<ImageActions> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        ImageFormat format;
        const Result<QByteArray> bytes = readImage(path, format);
        QImage image;
        if (bytes) {
            QByteArray content = *bytes;
            QBuffer source(&content);
            source.open(QIODevice::ReadOnly);
            QImageReader reader(&source);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.isValid() && qint64(size.width()) * size.height() <= maxImagePixels)
                image = reader.read();
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, bytes, format, image] {
            if (!self)
                return;
            if (!bytes) {
                emit self->message(bytes.error().message);
                return;
            }
            auto *data = new QMimeData;
            if (!image.isNull())
                data->setImageData(image);
            data->setData(format.mime, *bytes);
            QGuiApplication::clipboard()->setMimeData(data);
            emit self->copied();
        }, Qt::QueuedConnection);
    });
}

void ImageActions::save() {
    const QString path = m_runs.imageFile(node());
    if (path.isEmpty())
        return;
    m_pendingSource = path;
    QString name = QFileInfo(path).fileName();
    // A result file in the cache is named by its hash: name the file after the node instead.
    if (isInside(canonicalPath(path), canonicalPath(cacheDir()))) {
        const auto format = formatOf(path);
        name = safeStem(m_canvas.nodeTitle(node())) + '.' + (format ? format->extension : QStringLiteral("png"));
    }
    emit saveRequested(QUrl::fromLocalFile(suggestedFolder()), name);
}

void ImageActions::saveTo(const QString &target) {
    const QString source = std::exchange(m_pendingSource, QString());
    const QString destination = target.startsWith(QLatin1String("file:")) ? QUrl(target).toLocalFile() : target;
    if (source.isEmpty() || destination.isEmpty())
        return;
    QPointer<ImageActions> self(this);
    QThreadPool::globalInstance()->start([self, source, destination] {
        ImageFormat format;
        const Result<QByteArray> bytes = readImage(source, format);
        QString failure;
        QString path = destination;
        if (!bytes) {
            failure = bytes.error().message;
        } else {
            // A name typed without an extension gets the real one, so the file opens anywhere.
            if (QFileInfo(path).suffix().isEmpty())
                path += '.' + format.extension;
            // The dialog has asked about an existing file by now.
            const Errors errors = replaceFile(path, *bytes, QFileInfo(path).fileName());
            if (!errors.isEmpty())
                failure = say("The image was not saved: %1").arg(errors.first().message);
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, failure, path] {
            if (!self)
                return;
            if (failure.isEmpty())
                emit self->saved(path);
            else
                emit self->message(failure);
        }, Qt::QueuedConnection);
    });
}

} // namespace fugue
