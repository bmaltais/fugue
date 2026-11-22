#pragma once

// The New canvas dialog's model (#232, epic #206, spec 14.5, 6.2 widget `canvas`): what Photoshop's New
// Document asks, for a canvas of this root's store. A name (Untitled n, never one a canvas of this root
// has), a source (a blank canvas, the picture on the clipboard, or a picture file), for a blank one a
// size from a preset or typed and a background (transparent, white, black or a color), and the
// reasons it cannot be made, in plain words from the engine (engine/new_canvas.h). The dialog
// (NewCanvasDialog.qml) only draws this; the work is CanvasChoices' (canvas_choices.h).
//
// `reset()` is what opening the dialog does: the last settings come back (the preset, size and
// background of the last canvas made, in `stateFile`), the name is the first free Untitled n, and when
// the clipboard holds a picture the source is the clipboard and the size fields show its size, as Photoshop
// does. Settings are remembered only when a canvas was made. `create()` returns why not (the dialog
// stays open) or empty and says `created(id)`; `pasteAsNew()` is the same with no dialog, from the
// clipboard (the command canvas.paste-new).

#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_choices.h"

namespace fugue {

class NewCanvasModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The folder of the canvas store; empty is the session's (a project's own under FUGUE_HOME).
    Q_PROPERTY(QString storeRoot READ storeRoot WRITE setStoreRoot NOTIFY changed)
    // Where the last settings are kept; empty is `<stateDir>/new-canvas.json`. A test sets its own, before reset().
    Q_PROPERTY(QString stateFile READ stateFile WRITE setStateFile NOTIFY changed)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    // "blank", "clipboard" or "file".
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY changed)
    // The size shown: what was typed for a blank canvas, the picture's size for the other sources.
    Q_PROPERTY(QString widthText READ widthText WRITE setWidthText NOTIFY changed)
    Q_PROPERTY(QString heightText READ heightText WRITE setHeightText NOTIFY changed)
    // The id of the preset the typed size is, or "custom".
    Q_PROPERTY(QString preset READ preset NOTIFY changed)
    // "transparent", "white", "black" or "colour" (then `colour`, "#rrggbb").
    Q_PROPERTY(QString background READ background WRITE setBackground NOTIFY changed)
    Q_PROPERTY(QString colour READ colour WRITE setColour NOTIFY changed)
    // The picture file (a path or a file: URL) of the source "file".
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY changed)
    Q_PROPERTY(bool clipboardAvailable READ clipboardAvailable NOTIFY changed)
    // "1920 × 1080" of the clipboard's picture, "" with none.
    Q_PROPERTY(QString clipboardSize READ clipboardSize NOTIFY changed)
    // "harbour.png · 300 × 120", "" with no file.
    Q_PROPERTY(QString fileLine READ fileLine NOTIFY changed)
    // The size to be made, 0 when it cannot be read.
    Q_PROPERTY(int width READ width NOTIFY changed)
    Q_PROPERTY(int height READ height NOTIFY changed)
    // "1024 × 1024 px · 1.05 megapixels · ..." and the cap.
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    // Why a canvas cannot be made now, "" when it can (live); `error` is why the last create() failed.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY changed)

public:
    explicit NewCanvasModel(QObject *parent = nullptr);

    QString storeRoot() const { return m_choices.storeRoot(); }
    void setStoreRoot(const QString &root);
    QString stateFile() const;
    void setStateFile(const QString &path);
    QString name() const { return m_name; }
    void setName(const QString &name);
    QString source() const { return m_source; }
    void setSource(const QString &source);
    QString widthText() const;
    void setWidthText(const QString &text);
    QString heightText() const;
    void setHeightText(const QString &text);
    QString preset() const;
    QString background() const { return m_background; }
    void setBackground(const QString &background);
    QString colour() const { return m_colour; }
    void setColour(const QString &colour);
    QString file() const { return m_file; }
    void setFile(const QString &file);
    bool clipboardAvailable() const { return !m_clipboardSize.isEmpty(); }
    QString clipboardSize() const;
    QString fileLine() const;
    int width() const;
    int height() const;
    QString summary() const;
    QString problem() const;
    QString error() const { return m_error; }
    bool canCreate() const { return problem().isEmpty(); }

    // [{id, label, width, height, tip}], Custom last: Square, Landscape 4:3, Wide 16:9, Portrait 3:4, HD, 4K.
    Q_INVOKABLE QVariantList presets() const;
    // Takes a preset's size (Custom keeps the numbers as they are).
    Q_INVOKABLE void choosePreset(const QString &id);
    // The clipboard read again (it is also read when it changes).
    Q_INVOKABLE void refreshClipboard();
    // What opening the dialog does (see above).
    Q_INVOKABLE void reset();
    // Makes the canvas; returns why not, or empty. Remembers the settings when it is made.
    Q_INVOKABLE QString create();
    // The clipboard's picture as a canvas named "Pasted n", no dialog; returns why not, or empty.
    Q_INVOKABLE QString pasteAsNew();
    // Shows `why` as the reason the last attempt failed (a paste made before the dialog opened).
    Q_INVOKABLE void showError(const QString &why);
    // The canvas just made.
    Q_INVOKABLE QString createdId() const { return m_created; }

signals:
    void changed();
    void created(const QString &id);

private:
    QString sizeOf(const QString &which) const;
    void load();
    void remember() const;
    QString makeFromClipboard(const QString &name);
    void readClipboard();

    CanvasChoices m_choices;
    QString m_stateFile;
    QString m_name;
    bool m_nameEdited = false;
    QString m_source = QStringLiteral("blank");
    QString m_widthText = QStringLiteral("1024");
    QString m_heightText = QStringLiteral("1024");
    QString m_background = QStringLiteral("transparent");
    QString m_colour = QStringLiteral("#336699");
    QString m_file;
    QSize m_fileSize;
    QString m_clipboardSize;
    QSize m_clipboardPixels;
    QString m_error;
    QString m_created;
};

} // namespace fugue
