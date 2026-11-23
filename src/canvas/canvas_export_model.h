#pragma once

// The model of the Canvas view's two export dialogs (#247 Export as…, #248 Export layers as files; epic #206): what
// Photoshop's Export As / Save for Web and Layers to Files ask, for one revision of a canvas. The work is
// engine/canvas_export.h's, the same code `fugue canvas export` runs; this keeps the choices, makes the picture in the
// background whenever one changes (a large canvas takes a moment to encode), and says what the file will be: its size
// in bytes, its pixels, a small preview of the encoded picture (a JPEG shows its artifacts).
//
// `open(canvasId, revision, mode)` loads the revision. mode "picture" is one file: format png, jpg, webp or ora (the
// layers kept, which is the "not flattened" choice), quality (jpg and a lossy webp), a size (a percentage, or a width or
// a height, always in the canvas's proportions) and a background for the transparency. mode "layers" is one picture
// per layer into a folder: png or webp, hidden layers or not, layer masks applied or not, a name prefix.
//
// `exportTo(file)` and `exportLayersTo(folder)` write what is shown (a path or a file: URL) and never replace a file;
// `error` says why not, `notice` what was written. A file dialog (FilePicker) asks where; this never opens one.

#include <QObject>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

#include "engine/canvas_export.h"
#include "engine/canvas_store.h"

namespace fugue {

class CanvasExportModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The folder of the canvas store; empty is the session's.
    Q_PROPERTY(QString storeRoot READ storeRoot WRITE setStoreRoot NOTIFY storeChanged)
    // "picture" or "layers".
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString canvasName READ canvasName NOTIFY changed)
    // "png", "jpg", "webp" or "ora" (picture); "png" or "webp" (layers).
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY changed)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY changed)
    // Whether the format has a quality (jpg, and webp when not lossless), and whether it can be lossless (webp).
    Q_PROPERTY(bool hasQuality READ hasQuality NOTIFY changed)
    Q_PROPERTY(bool lossless READ lossless WRITE setLossless NOTIFY changed)
    Q_PROPERTY(bool canBeLossless READ canBeLossless NOTIFY changed)
    // The canvas's size in pixels, and the picture's as typed: width and height keep its proportions.
    Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY changed)
    Q_PROPERTY(QString widthText READ widthText WRITE setWidthText NOTIFY changed)
    Q_PROPERTY(QString heightText READ heightText WRITE setHeightText NOTIFY changed)
    Q_PROPERTY(int percent READ percent WRITE setPercent NOTIFY changed)
    // "auto" (transparent, white for a jpg), "transparent", "white", "black" or "colour" (then `colour`, "#rrggbb").
    Q_PROPERTY(QString background READ background WRITE setBackground NOTIFY changed)
    Q_PROPERTY(QString colour READ colour WRITE setColour NOTIFY changed)
    Q_PROPERTY(bool hasBackground READ hasBackground NOTIFY changed)
    // Layers to files.
    Q_PROPERTY(bool visibleOnly READ visibleOnly WRITE setVisibleOnly NOTIFY changed)
    Q_PROPERTY(bool applyMasks READ applyMasks WRITE setApplyMasks NOTIFY changed)
    Q_PROPERTY(QString prefix READ prefix WRITE setPrefix NOTIFY changed)
    // A file name to offer ("Harbour.webp"), "" in layers mode.
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    // True while a picture is being made; the numbers below are of the last one made.
    Q_PROPERTY(bool busy READ busy NOTIFY resultChanged)
    // "1.4 MB · 1920 × 1080 px", or for layers "5 files · 3.2 MB"; "" before the first is made.
    Q_PROPERTY(QString estimate READ estimate NOTIFY resultChanged)
    // A small PNG of the picture as encoded (a file: URL), "" for none (layers).
    Q_PROPERTY(QUrl preview READ preview NOTIFY resultChanged)
    // Layers: [{name, fileName, size ("12 KB")}] of the files that would be written, and the layers left out.
    Q_PROPERTY(QVariantList files READ files NOTIFY resultChanged)
    Q_PROPERTY(QString skipped READ skipped NOTIFY resultChanged)
    // Why nothing can be written now (a size past the limit, no layer to write), "" when it can; live.
    Q_PROPERTY(QString problem READ problem NOTIFY resultChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY resultChanged)

public:
    explicit CanvasExportModel(QObject *parent = nullptr);
    ~CanvasExportModel() override;

    QString storeRoot() const { return m_root; }
    void setStoreRoot(const QString &root);
    QString mode() const { return m_layers ? QStringLiteral("layers") : QStringLiteral("picture"); }
    QString canvasName() const { return m_name; }
    QString format() const { return m_format; }
    void setFormat(const QString &format);
    int quality() const { return m_quality; }
    void setQuality(int quality);
    bool hasQuality() const;
    bool lossless() const { return m_lossless; }
    void setLossless(bool lossless);
    bool canBeLossless() const { return m_format == QLatin1String("webp"); }
    QSize sourceSize() const { return m_source; }
    QString widthText() const { return QString::number(m_width); }
    void setWidthText(const QString &text);
    QString heightText() const { return QString::number(m_height); }
    void setHeightText(const QString &text);
    int percent() const;
    void setPercent(int percent);
    QString background() const { return m_background; }
    void setBackground(const QString &background);
    QString colour() const { return m_colour; }
    void setColour(const QString &colour);
    bool hasBackground() const { return !m_layers && m_format != QLatin1String("ora"); }
    bool visibleOnly() const { return m_visibleOnly; }
    void setVisibleOnly(bool on);
    bool applyMasks() const { return m_applyMasks; }
    void setApplyMasks(bool on);
    QString prefix() const { return m_prefix; }
    void setPrefix(const QString &prefix);
    QString fileName() const;
    bool busy() const { return m_busy; }
    QString estimate() const { return m_estimate; }
    QUrl preview() const { return m_preview; }
    QVariantList files() const { return m_files; }
    QString skipped() const { return m_skipped; }
    QString problem() const { return m_problem; }
    bool canExport() const { return !m_busy && m_problem.isEmpty() && m_ready; }
    QString error() const { return m_error; }
    QString notice() const { return m_notice; }

    // Loads `revision` (a full or short hash, or "head"; "" is the head) of the canvas and starts from the defaults of
    // `mode`: a png at the canvas's size, or png files of every layer. Returns why not, or empty.
    Q_INVOKABLE QString open(const QString &canvasId, const QString &revision, const QString &mode);
    // Writes the picture (mode "picture") or the layer files into the folder (mode "layers"). True when written.
    Q_INVOKABLE bool exportTo(const QString &target);

signals:
    void storeChanged();
    void changed();
    void resultChanged();

private:
    struct Job;
    void touch();      // an option changed: the numbers are stale; make them again
    void start();
    void finish(quint64 generation, std::shared_ptr<Job> job);
    void setError(const QString &text);
    canvas::PictureExport pictureOptions() const;
    canvas::LayersExport layerOptions() const;
    QString backgroundText() const;
    void clamp();

    QString m_root;
    QString m_canvasId;
    QString m_revision;
    QString m_name;
    bool m_layers = false;
    std::shared_ptr<const mask::PaintDocument> m_document;
    QSize m_source;
    QString m_format = QStringLiteral("png");
    int m_quality = 92;
    bool m_lossless = true;
    int m_width = 0;
    int m_height = 0;
    QString m_background = QStringLiteral("auto");
    QString m_colour = QStringLiteral("#ffffff");
    bool m_visibleOnly = false;
    bool m_applyMasks = true;
    QString m_prefix;

    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_ready = false;
    QString m_estimate;
    QUrl m_preview;
    QVariantList m_files;
    QString m_skipped;
    QString m_problem;
    QString m_error;
    QString m_notice;
    std::shared_ptr<Job> m_last; // the files of the last finished job, written by exportTo when still current
    QTemporaryDir m_folder;
};

} // namespace fugue
