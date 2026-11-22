#include "canvas/new_canvas_model.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMimeData>
#include <QUrl>

#include "engine/atomic_file.h"
#include "engine/layout_color.h"
#include "engine/new_canvas.h"
#include "engine/paint_file.h"
#include "engine/paths.h"

namespace fugue {

namespace {

struct Preset {
    const char *id;
    int width;
    int height;
};

// The sizes the dialog offers; the labels are NewCanvasModel::presets().
constexpr Preset kPresets[] = {{"square", 1024, 1024}, {"landscape", 1024, 768}, {"wide", 1280, 720},
                               {"portrait", 768, 1024}, {"hd", 1920, 1080},      {"4k", 3840, 2160}};

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}

// A whole number of pixels as typed; 0 when it is not one.
int pixels(const QString &text) {
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    return ok && value > 0 ? value : 0;
}

QString sizeText(const QSize &size) { return size.isValid() ? QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()) : QString(); }

QClipboard *clipboard() { return QGuiApplication::instance() ? QGuiApplication::clipboard() : nullptr; }

} // namespace

NewCanvasModel::NewCanvasModel(QObject *parent) : QObject(parent) {
    connect(&m_choices, &CanvasChoices::changed, this, &NewCanvasModel::changed);
    m_name = m_choices.freeName(tr("Untitled"));
}

void NewCanvasModel::setStoreRoot(const QString &root) {
    if (root == m_choices.storeRoot())
        return;
    m_choices.setStoreRoot(root);
    if (!m_nameEdited)
        m_name = m_choices.freeName(tr("Untitled"));
    emit changed();
}

QString NewCanvasModel::stateFile() const { return m_stateFile.isEmpty() ? stateDir() + QStringLiteral("/new-canvas.json") : m_stateFile; }

void NewCanvasModel::setStateFile(const QString &path) {
    m_stateFile = path;
    emit changed();
}

void NewCanvasModel::setName(const QString &name) {
    if (name == m_name)
        return;
    m_name = name;
    m_nameEdited = true;
    emit changed();
}

void NewCanvasModel::setSource(const QString &source) {
    if (source != QLatin1String("blank") && source != QLatin1String("clipboard") && source != QLatin1String("file"))
        return;
    if (source == m_source)
        return;
    m_source = source;
    m_error.clear();
    if (!m_nameEdited)
        m_name = source == QLatin1String("file") && !m_file.isEmpty() ? QFileInfo(localPath(m_file)).completeBaseName() : m_choices.freeName(tr("Untitled"));
    emit changed();
}

QString NewCanvasModel::sizeOf(const QString &which) const {
    const QSize size = m_source == QLatin1String("clipboard") ? m_clipboardPixels : m_source == QLatin1String("file") ? m_fileSize : QSize();
    if (m_source == QLatin1String("blank"))
        return which == QLatin1String("width") ? m_widthText : m_heightText;
    return size.isValid() ? QString::number(which == QLatin1String("width") ? size.width() : size.height()) : QString();
}

QString NewCanvasModel::widthText() const { return sizeOf(QStringLiteral("width")); }
QString NewCanvasModel::heightText() const { return sizeOf(QStringLiteral("height")); }

void NewCanvasModel::setWidthText(const QString &text) {
    if (text == m_widthText)
        return;
    m_widthText = text;
    m_error.clear();
    emit changed();
}

void NewCanvasModel::setHeightText(const QString &text) {
    if (text == m_heightText)
        return;
    m_heightText = text;
    m_error.clear();
    emit changed();
}

QString NewCanvasModel::preset() const {
    if (m_source != QLatin1String("blank"))
        return {};
    const int w = pixels(m_widthText), h = pixels(m_heightText);
    for (const Preset &preset : kPresets)
        if (preset.width == w && preset.height == h)
            return QString::fromLatin1(preset.id);
    return QStringLiteral("custom");
}

void NewCanvasModel::setBackground(const QString &background) {
    if (background != QLatin1String("transparent") && background != QLatin1String("white") && background != QLatin1String("black") && background != QLatin1String("colour"))
        return;
    if (background == m_background)
        return;
    m_background = background;
    emit changed();
}

void NewCanvasModel::setColour(const QString &colour) {
    if (colour == m_colour)
        return;
    m_colour = colour;
    emit changed();
}

void NewCanvasModel::setFile(const QString &file) {
    if (file.isEmpty())
        return;
    m_file = file;
    m_fileSize = QImageReader(localPath(file)).size();
    m_source = QStringLiteral("file");
    m_error.clear();
    if (!m_nameEdited) {
        const QString base = QFileInfo(localPath(file)).completeBaseName();
        m_name = base.isEmpty() ? m_choices.freeName(tr("Untitled")) : base;
    }
    emit changed();
}

QString NewCanvasModel::clipboardSize() const { return m_clipboardSize; }

QString NewCanvasModel::fileLine() const {
    if (m_file.isEmpty())
        return {};
    const QString file = QFileInfo(localPath(m_file)).fileName();
    return m_fileSize.isValid() ? tr("%1 · %2").arg(file, sizeText(m_fileSize)) : file;
}

int NewCanvasModel::width() const { return pixels(widthText()); }
int NewCanvasModel::height() const { return pixels(heightText()); }

QString NewCanvasModel::summary() const {
    const QString cap = tr("At most %1 px a side and %2 megapixels.").arg(mask::paintFileMaxSide).arg(mask::paintFileMaxPixels / 1000000);
    if (width() < 1 || height() < 1)
        return cap;
    const double megapixels = double(width()) * height() / 1e6;
    return tr("%1 × %2 px · %3 megapixels. %4").arg(width()).arg(height()).arg(QLocale().toString(megapixels, 'g', 3), cap);
}

QString NewCanvasModel::problem() const {
    if (m_source == QLatin1String("clipboard")) {
        if (!clipboardAvailable())
            return tr("The clipboard has no picture.");
        return mask::canvasSizeProblem(m_clipboardPixels.width(), m_clipboardPixels.height());
    }
    if (m_source == QLatin1String("file"))
        return m_file.isEmpty() ? tr("Choose a picture or an .ora file.") : QString();
    if (width() < 1 || height() < 1)
        return tr("Type the width and the height in pixels, whole numbers.");
    if (const QString why = mask::canvasSizeProblem(width(), height()); !why.isEmpty())
        return why;
    if (m_background == QLatin1String("colour") && normalizeHex(m_colour).isEmpty())
        return tr("The color is a hex code such as #336699.");
    return {};
}

QVariantList NewCanvasModel::presets() const {
    const auto label = [](const QString &id) {
        if (id == QLatin1String("square"))
            return tr("Square");
        if (id == QLatin1String("landscape"))
            return tr("Landscape 4:3");
        if (id == QLatin1String("wide"))
            return tr("Wide 16:9");
        if (id == QLatin1String("portrait"))
            return tr("Portrait 3:4");
        if (id == QLatin1String("hd"))
            return tr("HD");
        return tr("4K");
    };
    QVariantList list;
    for (const Preset &preset : kPresets) {
        const QString id = QString::fromLatin1(preset.id);
        list.append(QVariantMap{{"id", id}, {"label", label(id)}, {"width", preset.width}, {"height", preset.height}, {"tip", tr("%1 × %2 px").arg(preset.width).arg(preset.height)}});
    }
    list.append(QVariantMap{{"id", "custom"}, {"label", tr("Custom")}, {"width", 0}, {"height", 0}, {"tip", tr("Type any size")}});
    return list;
}

void NewCanvasModel::choosePreset(const QString &id) {
    for (const Preset &preset : kPresets)
        if (QLatin1String(preset.id) == id) {
            m_source = QStringLiteral("blank");
            m_widthText = QString::number(preset.width);
            m_heightText = QString::number(preset.height);
            m_error.clear();
            emit changed();
            return;
        }
    if (m_source != QLatin1String("blank"))
        setSource(QStringLiteral("blank"));
}

void NewCanvasModel::readClipboard() {
    m_clipboardSize.clear();
    m_clipboardPixels = QSize();
    QClipboard *board = clipboard();
    const QMimeData *data = board ? board->mimeData() : nullptr;
    if (data && data->hasImage()) {
        const QImage image = board->image();
        m_clipboardPixels = image.size();
        m_clipboardSize = sizeText(image.size());
    }
}

void NewCanvasModel::refreshClipboard() {
    readClipboard();
    if (m_source == QLatin1String("clipboard") && !clipboardAvailable())
        m_source = QStringLiteral("blank");
    emit changed();
}

void NewCanvasModel::load() {
    QFile file(stateFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject saved = QJsonDocument::fromJson(file.read(1 << 16)).object();
    if (const int w = saved.value("width").toInt(), h = saved.value("height").toInt(); w > 0 && h > 0) {
        m_widthText = QString::number(w);
        m_heightText = QString::number(h);
    }
    const QString background = saved.value("background").toString();
    if (background == QLatin1String("transparent") || background == QLatin1String("white") || background == QLatin1String("black") || background == QLatin1String("colour"))
        m_background = background;
    if (const QString colour = normalizeHex(saved.value("colour").toString()); !colour.isEmpty())
        m_colour = colour;
}

void NewCanvasModel::remember() const {
    const QJsonObject saved{{"preset", preset()}, {"width", pixels(m_widthText)}, {"height", pixels(m_heightText)}, {"background", m_background}, {"colour", m_colour}};
    replaceFile(stateFile(), QJsonDocument(saved).toJson(), QStringLiteral("the New canvas settings")); // a failure only forgets them
}

void NewCanvasModel::reset() {
    m_widthText = QStringLiteral("1024");
    m_heightText = QStringLiteral("1024");
    m_background = QStringLiteral("transparent");
    load();
    m_error.clear();
    m_created.clear();
    m_file.clear();
    m_fileSize = QSize();
    m_nameEdited = false;
    readClipboard();
    m_source = clipboardAvailable() ? QStringLiteral("clipboard") : QStringLiteral("blank");
    m_name = m_choices.freeName(tr("Untitled"));
    emit changed();
}

QString NewCanvasModel::makeFromClipboard(const QString &name) {
    QClipboard *board = clipboard();
    const QMimeData *data = board ? board->mimeData() : nullptr;
    if (!data || !data->hasImage())
        return tr("The clipboard has no picture.");
    QString id;
    if (const QString why = m_choices.makeFromImage(name, board->image(), &id); !why.isEmpty())
        return why;
    m_created = id;
    return {};
}

QString NewCanvasModel::create() {
    m_error.clear();
    if (const QString why = problem(); !why.isEmpty()) {
        m_error = why;
        emit changed();
        return why;
    }
    const QString name = m_name.trimmed().isEmpty() ? m_choices.freeName(tr("Untitled")) : m_name.trimmed();
    QString why, id;
    if (m_source == QLatin1String("clipboard")) {
        why = makeFromClipboard(name);
        id = m_created;
    } else if (m_source == QLatin1String("file")) {
        why = m_choices.makeFromFile(m_file, name, &id);
    } else {
        why = m_choices.makeBlank(name, width(), height(), m_background == QLatin1String("colour") ? normalizeHex(m_colour) : m_background, &id);
    }
    if (!why.isEmpty()) {
        m_error = why;
        emit changed();
        return why;
    }
    m_created = id;
    remember();
    emit changed();
    emit created(id);
    return {};
}

void NewCanvasModel::showError(const QString &why) {
    m_error = why;
    emit changed();
}

QString NewCanvasModel::pasteAsNew() {
    m_error.clear();
    if (const QString why = makeFromClipboard(m_choices.freeName(tr("Pasted"))); !why.isEmpty()) {
        m_error = why;
        emit changed();
        return why;
    }
    emit changed();
    emit created(m_created);
    return {};
}

} // namespace fugue
