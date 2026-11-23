#include "canvas/canvas_choices.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <cstring>

#include "engine/new_canvas.h"
#include "engine/paint_file.h"

namespace fugue {

using canvas::CanvasStore;

namespace {

// The first error as one sentence with its hint.
QString sentence(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint;
    return QObject::tr("The canvas could not be made.");
}

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}

} // namespace

QVariantList canvasRows(const CanvasStore &store) {
    QVariantList rows;
    for (const canvas::CanvasInfo &info : store.list()) {
        int width = 0, height = 0;
        QString changed = info.created;
        if (!info.head.isEmpty()) {
            if (const Result<canvas::Revision> head = store.revision(info.id, info.head)) {
                changed = head->time;
                if (const Result<canvas::Tree> tree = store.tree(head->tree)) {
                    width = tree->width;
                    height = tree->height;
                }
            }
        }
        const QDateTime time = QDateTime::fromString(changed, Qt::ISODateWithMs).toLocalTime();
        rows.append(QVariantMap{{"id", info.id},
                                {"name", info.name},
                                {"head", info.head},
                                {"created", info.created},
                                {"width", width},
                                {"height", height},
                                {"size", width > 0 ? QStringLiteral("%1 × %2").arg(width).arg(height) : QString()},
                                {"changed", changed},
                                {"changedText", time.isValid() ? QLocale().toString(time, QLocale::ShortFormat) : QString()}});
    }
    return rows;
}

CanvasChoices::CanvasChoices(QObject *parent) : QObject(parent) {
    // A burst of file events (a revision is several files) is one change.
    m_settle.setSingleShot(true);
    m_settle.setInterval(120);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        watch();
        ++m_revision;
        emit changed();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { touched(); });
    watch();
}

CanvasStore CanvasChoices::store() const { return m_root.isEmpty() ? CanvasStore::forSession() : CanvasStore(m_root); }

void CanvasChoices::setStoreRoot(const QString &root) {
    if (root == m_root)
        return;
    m_root = root;
    watch();
    ++m_revision;
    emit changed();
}

void CanvasChoices::touched() { m_settle.start(); }

void CanvasChoices::watch() {
    // The folder that holds the canvases and each canvas's folder (where a head is renamed into
    // place); until there is one, the nearest folder that exists above it, which sees it made
    // (nothing is made on disk to be watched).
    const QString root = store().root();
    QStringList want;
    const QDir canvases(root + QStringLiteral("/canvases"));
    if (canvases.exists()) {
        want << canvases.path();
        for (const QString &id : canvases.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            want << canvases.filePath(id);
    } else {
        QDir above(root);
        while (!above.exists() && above.cdUp()) {
        }
        if (above.exists())
            want << above.path();
        if (QDir(root).exists())
            want = QStringList{root};
    }
    const QStringList have = m_watcher.directories();
    for (const QString &path : have)
        if (!want.contains(path))
            m_watcher.removePath(path);
    for (const QString &path : want)
        if (!have.contains(path))
            m_watcher.addPath(path);
}

QVariantList CanvasChoices::rows() {
    // Re-armed here too: a store that did not exist yet (no canvas, no folder) gets watched once asked.
    watch();
    return canvasRows(store());
}

QVariantMap CanvasChoices::row(const QString &id) {
    for (const QVariant &row : rows())
        if (row.toMap().value("id").toString() == id)
            return row.toMap();
    return {};
}

QString CanvasChoices::make(const QString &name, const Result<mask::PaintDocument> &document, const QString &producer, QString *id) {
    if (!document)
        return sentence(document.errors());
    CanvasStore s = store();
    const Result<canvas::CanvasInfo> made = s.create(name.trimmed().isEmpty() ? QStringLiteral("Untitled") : name.trimmed().left(120), document.value(), producer);
    if (!made)
        return sentence(made.errors());
    *id = made->id;
    touched();
    return {};
}

QString CanvasChoices::makeBlank(const QString &name, int width, int height, QString *id) {
    return make(name, mask::blankCanvas(width, height, {}), QStringLiteral("new"), id);
}

QString CanvasChoices::makeBlank(const QString &name, int width, int height, const QString &background, QString *id) {
    const Result<mask::CanvasBackground> fill = mask::parseBackground(background);
    if (!fill)
        return sentence(fill.errors());
    return make(name, mask::blankCanvas(width, height, fill.value()), QStringLiteral("new"), id);
}

QString CanvasChoices::makeFromImage(const QString &name, const QImage &image, QString *id) {
    if (image.isNull())
        return tr("There is no picture to make a canvas from.");
    if (const QString why = mask::canvasSizeProblem(image.width(), image.height()); !why.isEmpty())
        return tr("The picture is %1 × %2. %3").arg(image.width()).arg(image.height()).arg(why);
    const QImage straight = image.convertedTo(QImage::Format_RGBA8888); // straight (not premultiplied) bytes, as a layer holds them
    std::vector<uint8_t> pixels(size_t(straight.width()) * size_t(straight.height()) * 4);
    for (int y = 0; y < straight.height(); ++y)
        std::memcpy(pixels.data() + size_t(y) * size_t(straight.width()) * 4, straight.constScanLine(y), size_t(straight.width()) * 4);
    return make(name, mask::canvasFromPixels(straight.width(), straight.height(), pixels, QStringLiteral("Layer 1")), QStringLiteral("import"), id);
}

QString CanvasChoices::freeName(const QString &base) {
    QSet<QString> taken;
    for (const QVariant &row : rows())
        taken.insert(row.toMap().value("name").toString());
    for (int n = 1;; ++n)
        if (const QString name = QStringLiteral("%1 %2").arg(base).arg(n); !taken.contains(name))
            return name;
}

QString CanvasChoices::makeFromFile(const QString &path, const QString &name, QString *id, QStringList *dropped) {
    const QString file = localPath(path);
    const Result<mask::PaintDocument> document = mask::readCanvasFile(file);
    if (!document)
        return sentence(document.errors());
    CanvasStore s = store();
    const QString title = name.trimmed().isEmpty() ? QFileInfo(file).completeBaseName() : name.trimmed().left(120);
    const Result<canvas::CanvasInfo> made = s.create(title.isEmpty() ? QStringLiteral("Untitled") : title, document.value(), QStringLiteral("import"));
    if (!made)
        return sentence(made.errors());
    *id = made->id;
    if (dropped)
        for (const Error &warning : document.warnings())
            dropped->append(warning.message);
    touched();
    return {};
}

} // namespace fugue
