#include "canvas/canvas_export_model.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>

#include "engine/atomic_file.h"
#include "engine/new_canvas.h"

namespace fugue {

namespace {

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}

QString messageOf(const Errors &errors) {
    const Error &first = errors.front();
    return first.hint.isEmpty() ? first.message : first.message + QLatin1Char(' ') + first.hint;
}

int wholeNumber(const QString &text) {
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    return ok && value > 0 ? value : 0;
}

constexpr int previewSide = 640;

} // namespace

// What one background run made: the picture or the layer files, and what to show of them.
struct CanvasExportModel::Job {
    canvas::ExportedPicture picture;
    canvas::LayerFiles layers;
    QString problem;
    QString estimate;
    QByteArray previewPng;
    QVariantList files;
    QString skipped;
};

CanvasExportModel::CanvasExportModel(QObject *parent) : QObject(parent) {}
CanvasExportModel::~CanvasExportModel() { ++m_generation; } // a job still running finds nobody to tell

void CanvasExportModel::setStoreRoot(const QString &root) {
    if (root == m_root)
        return;
    m_root = root;
    emit storeChanged();
}

QString CanvasExportModel::open(const QString &canvasId, const QString &revision, const QString &mode) {
    ++m_generation;
    m_document.reset();
    m_last.reset();
    m_ready = false;
    m_busy = false;
    m_error.clear();
    m_notice.clear();
    m_problem.clear();
    m_estimate.clear();
    m_preview = QUrl();
    m_files.clear();
    m_skipped.clear();
    const canvas::CanvasStore store = m_root.isEmpty() ? canvas::CanvasStore::forSession() : canvas::CanvasStore(m_root);
    const Result<canvas::CanvasInfo> info = store.info(canvasId);
    if (!info) {
        emit changed();
        emit resultChanged();
        return messageOf(info.errors());
    }
    const Result<QString> resolved = store.resolve(canvasId, revision.isEmpty() ? QStringLiteral("head") : revision);
    if (!resolved) {
        emit changed();
        emit resultChanged();
        return messageOf(resolved.errors());
    }
    Result<mask::PaintDocument> loaded = store.load(canvasId, resolved.value());
    if (!loaded) {
        emit changed();
        emit resultChanged();
        return messageOf(loaded.errors());
    }
    m_canvasId = canvasId;
    m_revision = resolved.value();
    m_name = info->name.trimmed().isEmpty() ? canvasId : info->name.trimmed();
    m_layers = mode == QLatin1String("layers");
    m_document = std::make_shared<const mask::PaintDocument>(std::move(loaded.value()));
    m_source = QSize(m_document->width(), m_document->height());
    m_width = m_source.width();
    m_height = m_source.height();
    m_format = QStringLiteral("png");
    m_quality = 92;
    m_lossless = true;
    m_background = QStringLiteral("auto");
    m_visibleOnly = false;
    m_applyMasks = true;
    m_prefix = canvas::safeFileName(m_name);
    emit changed();
    touch();
    return {};
}

bool CanvasExportModel::hasQuality() const { return m_format == QLatin1String("jpg") || (m_format == QLatin1String("webp") && !m_lossless); }

void CanvasExportModel::setFormat(const QString &format) {
    const QString normal = canvas::normalFormat(format, !m_layers);
    if (normal.isEmpty() || (m_layers && normal == QLatin1String("jpg")) || normal == m_format)
        return;
    m_format = normal;
    // A jpg is never transparent: leave "transparent" for the format's own.
    if (m_format == QLatin1String("jpg") && m_background == QLatin1String("transparent"))
        m_background = QStringLiteral("auto");
    emit changed();
    touch();
}

void CanvasExportModel::setQuality(int quality) {
    quality = std::clamp(quality, 1, 100);
    if (quality == m_quality)
        return;
    m_quality = quality;
    emit changed();
    touch();
}

void CanvasExportModel::setLossless(bool lossless) {
    if (lossless == m_lossless)
        return;
    m_lossless = lossless;
    emit changed();
    touch();
}

void CanvasExportModel::setWidthText(const QString &text) {
    const int width = wholeNumber(text);
    if (width == 0 || width == m_width || m_source.isEmpty())
        return;
    m_width = width;
    m_height = std::max(1, int(std::lround(double(m_source.height()) * width / m_source.width())));
    emit changed();
    touch();
}

void CanvasExportModel::setHeightText(const QString &text) {
    const int height = wholeNumber(text);
    if (height == 0 || height == m_height || m_source.isEmpty())
        return;
    m_height = height;
    m_width = std::max(1, int(std::lround(double(m_source.width()) * height / m_source.height())));
    emit changed();
    touch();
}

int CanvasExportModel::percent() const { return m_source.width() > 0 ? int(std::lround(100.0 * m_width / m_source.width())) : 100; }

void CanvasExportModel::setPercent(int percent) {
    if (percent < 1 || m_source.isEmpty() || percent == this->percent())
        return;
    m_width = std::max(1, int(std::lround(m_source.width() * percent / 100.0)));
    m_height = std::max(1, int(std::lround(m_source.height() * percent / 100.0)));
    emit changed();
    touch();
}

void CanvasExportModel::setBackground(const QString &background) {
    if (background == m_background || (background == QLatin1String("transparent") && m_format == QLatin1String("jpg")))
        return;
    if (background != QLatin1String("auto") && background != QLatin1String("transparent") && background != QLatin1String("white") &&
        background != QLatin1String("black") && background != QLatin1String("colour"))
        return;
    m_background = background;
    emit changed();
    touch();
}

void CanvasExportModel::setColour(const QString &colour) {
    if (colour == m_colour)
        return;
    m_colour = colour;
    emit changed();
    if (m_background == QLatin1String("colour"))
        touch();
}

void CanvasExportModel::setVisibleOnly(bool on) {
    if (on == m_visibleOnly)
        return;
    m_visibleOnly = on;
    emit changed();
    touch();
}

void CanvasExportModel::setApplyMasks(bool on) {
    if (on == m_applyMasks)
        return;
    m_applyMasks = on;
    emit changed();
    touch();
}

void CanvasExportModel::setPrefix(const QString &prefix) {
    if (prefix == m_prefix)
        return;
    m_prefix = prefix;
    emit changed();
    touch();
}

QString CanvasExportModel::fileName() const {
    if (m_layers || m_canvasId.isEmpty())
        return {};
    return canvas::safeFileName(m_name) + QLatin1Char('.') + m_format;
}

QString CanvasExportModel::backgroundText() const {
    return m_background == QLatin1String("auto") ? QString() : m_background == QLatin1String("colour") ? m_colour : m_background;
}

canvas::PictureExport CanvasExportModel::pictureOptions() const {
    canvas::PictureExport options;
    options.format = m_format;
    options.quality = m_quality;
    options.lossless = m_lossless;
    options.width = m_width;
    options.height = m_height;
    options.background = backgroundText();
    return options;
}

canvas::LayersExport CanvasExportModel::layerOptions() const {
    canvas::LayersExport options;
    options.format = m_format;
    options.quality = m_quality;
    options.lossless = m_lossless;
    options.visibleOnly = m_visibleOnly;
    options.applyMasks = m_applyMasks;
    options.prefix = m_prefix;
    return options;
}

void CanvasExportModel::touch() {
    ++m_generation;
    m_ready = false;
    m_last.reset();
    m_error.clear();
    m_notice.clear();
    if (!m_document) {
        emit resultChanged();
        return;
    }
    m_busy = true;
    emit resultChanged();
    start();
}

void CanvasExportModel::start() {
    const quint64 generation = m_generation;
    const std::shared_ptr<const mask::PaintDocument> document = m_document;
    const bool layers = m_layers;
    const canvas::PictureExport picture = pictureOptions();
    const canvas::LayersExport layerOptions = this->layerOptions();
    QPointer<CanvasExportModel> self(this);
    QThreadPool::globalInstance()->start([=]() {
        auto job = std::make_shared<Job>();
        const QLocale locale;
        if (layers) {
            const Result<canvas::LayerFiles> made = canvas::exportLayers(*document, layerOptions);
            if (!made) {
                job->problem = messageOf(made.errors());
            } else {
                job->layers = made.value();
                qint64 total = 0;
                for (const canvas::LayerFile &file : job->layers.files) {
                    total += file.bytes.size();
                    job->files.append(QVariantMap{{"name", file.name}, {"fileName", file.fileName}, {"size", locale.formattedDataSize(file.bytes.size())}});
                }
                job->skipped = job->layers.skipped.join(QStringLiteral(", "));
                job->estimate = QStringLiteral("%1 · %2").arg(job->layers.files.size() == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(job->layers.files.size()),
                                                              locale.formattedDataSize(total));
            }
        } else {
            const Result<canvas::ExportedPicture> made = canvas::exportPicture(*document, picture);
            if (!made) {
                job->problem = messageOf(made.errors());
            } else {
                job->picture = made.value();
                job->skipped = job->picture.notes.join(QLatin1Char(' ')); // a layered file's own sentences: what it could not keep
                job->estimate = QStringLiteral("%1 · %2 × %3 px").arg(locale.formattedDataSize(job->picture.bytes.size())).arg(job->picture.width).arg(job->picture.height);
                // The preview is the encoded picture decoded again (so a JPEG's artifacts show), fitted to a small box.
                Result<mask::MaskPaint> shown = job->picture.format == QLatin1String("ora") || job->picture.format == QLatin1String("psd") ? Result<mask::MaskPaint>(document->flatten())
                                                                                              : mask::MaskPaint::fromPicture(job->picture.bytes);
                if (shown) {
                    mask::MaskPaint small = shown.value();
                    const double fit = std::min(1.0, double(previewSide) / std::max(small.width(), small.height()));
                    if (fit < 1) {
                        const Result<mask::MaskPaint> smaller = small.resampled(std::max(1, int(small.width() * fit)), std::max(1, int(small.height() * fit)));
                        if (smaller)
                            small = smaller.value();
                    }
                    const Result<QByteArray> png = small.toPng(1);
                    if (png)
                        job->previewPng = png.value();
                }
            }
        }
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, generation, job]() { if (self) self->finish(generation, job); }, Qt::QueuedConnection);
    });
}

void CanvasExportModel::finish(quint64 generation, std::shared_ptr<Job> job) {
    if (generation != m_generation)
        return; // an option changed meanwhile: a newer job is on its way
    m_busy = false;
    m_problem = job->problem;
    m_estimate = job->estimate;
    m_files = job->files;
    m_skipped = job->skipped;
    m_preview = QUrl();
    if (!job->previewPng.isEmpty() && m_folder.isValid()) {
        const QString file = m_folder.filePath(QStringLiteral("preview-%1.png").arg(generation));
        QFile out(file);
        if (out.open(QIODevice::WriteOnly) && out.write(job->previewPng) == job->previewPng.size()) {
            out.close();
            m_preview = QUrl::fromLocalFile(file);
        }
    }
    m_ready = job->problem.isEmpty();
    m_last = job;
    emit resultChanged();
}

void CanvasExportModel::setError(const QString &text) {
    m_error = text;
    m_notice.clear();
    emit resultChanged();
}

bool CanvasExportModel::exportTo(const QString &target) {
    if (!m_document)
        return false;
    const QString path = localPath(target);
    if (path.isEmpty())
        return false;
    m_error.clear();
    m_notice.clear();
    // What is shown, as made; when a change is still being made, make it here.
    std::shared_ptr<Job> job = m_ready ? m_last : nullptr;
    if (!job) {
        job = std::make_shared<Job>();
        if (m_layers) {
            const Result<canvas::LayerFiles> made = canvas::exportLayers(*m_document, layerOptions());
            if (!made) {
                setError(messageOf(made.errors()));
                return false;
            }
            job->layers = made.value();
        } else {
            const Result<canvas::ExportedPicture> made = canvas::exportPicture(*m_document, pictureOptions());
            if (!made) {
                setError(messageOf(made.errors()));
                return false;
            }
            job->picture = made.value();
        }
    }
    if (m_layers) {
        const Errors written = canvas::writeLayerFiles(path, job->layers);
        if (hasErrors(written)) {
            setError(messageOf(written));
            return false;
        }
        const int count = int(job->layers.files.size());
        m_notice = (count == 1 ? tr("Saved 1 file in %1") : tr("Saved %1 files in %2").arg(count)).arg(QFileInfo(path).fileName());
    } else {
        QString file = path;
        // A name typed without the format's extension gets it, as Save Image does.
        if (QFileInfo(file).suffix().toLower() != m_format && !(m_format == QLatin1String("jpg") && QFileInfo(file).suffix().toLower() == QLatin1String("jpeg")))
            file += QLatin1Char('.') + m_format;
        const Errors written = createFile(file, job->picture.bytes, tr("the exported canvas"));
        if (hasErrors(written)) {
            setError(messageOf(written));
            return false;
        }
        m_notice = tr("Saved %1").arg(QFileInfo(file).fileName());
    }
    emit resultChanged();
    return true;
}

} // namespace fugue
