#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "look.h"

// Follows the Omarchy theme live: its whole palette, whether it is light or
// dark, Hyprland's window rounding as radius, and the desktop text size.
//
// QML reads it as App.theme (qml_app.h). For all QML (the canvas
// included), use only these names and never a literal color or size:
//   background, panel, border, text, muted, accent, accentForeground, error
//                         semantic colors (panel is a surface raised from
//                         background, border a hairline on either, muted
//                         secondary text; all readable on light and dark)
//   portColor(type)       color of a port type ("text", "number", "boolean",
//                         "image", "video", "audio"; anything else, a declared
//                         type, gets "declared"); portColors is the whole map.
//                         Color is never the only signal (spec 12.6)
//   radius                corner radius in px (Hyprland rounding)
//   textSize              desktop base text size in px (12 by default);
//                         derive other sizes from it, e.g. textSize * 1.2
//   light                 true on a light theme
//   colors                the raw palette (red, green, selection, ...)
//   look                  the canvas look (look.h): node, wire, popup and overlay
//                         colors, sizes and shadows, for the canvas, node search
//                         and the `?` overlay
class Theme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(int textSize READ textSize NOTIFY changed)
    Q_PROPERTY(QColor text READ foreground NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor error READ error NOTIFY changed)
    Q_PROPERTY(QVariantMap portColors READ portColors NOTIFY changed)
    Q_PROPERTY(CanvasLook *look READ look CONSTANT)

public:
    // Tests pass a temp directory, a fake hyprctl, and their own event socket.
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &hyprlandEvents = {});
    ~Theme() override;

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const;
    QColor background() const { return color("background"); }
    QColor foreground() const { return color("foreground"); }
    QVariantMap colors() const;
    bool light() const { return m_light; }
    int radius() const { return m_radius; }
    int textSize() const { return m_textSize; }
    QColor text() const { return foreground(); }
    QColor panel() const;
    QColor border() const;
    QColor muted() const;
    QColor error() const { return color("red"); }
    QVariantMap portColors() const;
    // A port type's color: the theme's, made distinct from the others and readable on a node
    // (docs/canvas-look.md 6.2).
    Q_INVOKABLE QColor portColor(const QString &type) const;
    CanvasLook *look() const { return m_look; }
    // A straight sRGB blend, `toB` of the way from a to b.
    static QColor mix(const QColor &a, const QColor &b, double toB);

    struct Palette {
        QHash<QString, QColor> colors;
        bool light = false;
    };
    static Palette readThemeColors(const QString &path);
    // `[font] base-size` of a shell.toml in px, or 0 when absent or not sane.
    static int readTextSize(const QString &path);

signals:
    void changed();

private:
    QColor color(const QString &key) const;
    void reload();
    void refreshRounding();
    void refreshLook();

    QString m_directory;
    QString m_shellFile;
    int m_textSize = 12;
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    bool m_roundingAgain = false;
    QProcess m_roundingProcess;
    QTimer m_roundingTimeout;
    QLocalSocket m_events;
    QTimer m_reconnect;
    QFileSystemWatcher m_eventsFolder;
    CanvasLook *m_look;
};
