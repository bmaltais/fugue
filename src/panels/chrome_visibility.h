#pragma once

// The registry of the window's bars and panels (#129, as Photoshop's Window menu): which of them is
// shown, what focus mode hides and brings back, and what is kept between runs. Each bar or panel
// registers an id with it and the window shows the item while `isVisible(id)`; the commands
// (chrome_commands.h) and the menus read and write the same ids. Plain C++ with no QML, so the
// rules are tested without a window (tests/panels/test_chrome_visibility.cpp).
//
// An element has a *wanted* state, the user's choice, which is kept in a small file of the state
// folder: {"visible": {"<id>": true|false}}. An auto-hide bar (setAutoHide, the menu bar: #160) is
// not removed when it is not shown: it collapses to a slim edge and slides in when wanted
// (panels/AutoHide.qml). Its entry is a mode word, "shown" or "auto-hide"; the older false reads as
// auto-hide and true as shown, and the same ids are written back as words. What is shown is the wanted state seen through focus
// mode: while it is on, nothing is shown except what was asked for since it began (a panel opened
// on purpose, the menu bar brought back with F10). Leaving it restores the wanted states exactly,
// plus what was asked for meanwhile. Focus mode is not kept: a window always opens with its bars.
//
// Ids this version does not register are kept in the file untouched, so a newer or older build
// does not lose a choice. A damaged file gives the defaults.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class ChromeVisibility : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // Id to shown (bool), for bindings: `App.chrome.shown["menu-bar"]` follows `changed`.
    Q_PROPERTY(QVariantMap shown READ shown NOTIFY changed)
    Q_PROPERTY(bool focusMode READ focusMode WRITE setFocusMode NOTIFY focusModeChanged)

public:
    // `path` is the file it is kept in (empty: not kept).
    explicit ChromeVisibility(QString path, QObject *parent = nullptr);

    // Registers an element, shown or not at first. What the file holds for the id wins.
    void add(const QString &id, bool shownByDefault = true);
    // For an element that used to keep its own state elsewhere: `shown` is that old state, used
    // only when this registry's file has nothing for the id.
    void seed(const QString &id, bool shown);
    // The bar `id` auto-hides instead of disappearing when it is not shown: only its saved form
    // and `mode` differ; the window draws the slim edge (`isVisible` is false then, as ever).
    void setAutoHide(const QString &id);
    bool autoHides(const QString &id) const { return m_autoHide.contains(id); }
    // "shown" or "auto-hide" for the wanted state of an id; empty for an unknown id.
    Q_INVOKABLE QString mode(const QString &id) const;
    // At most one of these is shown: showing one hides the others. When the file shows several,
    // the first stays.
    void setExclusive(const QStringList &ids);

    QStringList ids() const { return m_order; }
    bool contains(const QString &id) const { return m_wanted.contains(id); }
    // What the window shows: the wanted state, seen through focus mode. False for an unknown id.
    Q_INVOKABLE bool isVisible(const QString &id) const;
    // The user's choice, whatever focus mode does.
    bool wanted(const QString &id) const { return m_wanted.value(id, false); }
    QVariantMap shown() const;

    void setVisible(const QString &id, bool visible);
    void toggle(const QString &id) { setVisible(id, !isVisible(id)); }

    bool focusMode() const { return m_focus; }
    void setFocusMode(bool on);
    void toggleFocusMode() { setFocusMode(!m_focus); }

    // Every element to its default and focus mode off: the way back from any state.
    void reset();

signals:
    // `id` is now shown or hidden (focus mode included).
    void visibilityChanged(const QString &id, bool visible);
    // After any of the above, once: for the `shown` map.
    void changed();
    void focusModeChanged();
    // `id` was hidden by a request (a menu item, a key), not by focus mode or a reset: the window
    // says how to bring it back.
    void hiddenByRequest(const QString &id);

private:
    QHash<QString, bool> snapshot() const;
    // Calls `edit`, then says what changed in what is shown, and saves what was chosen.
    template <typename Edit> void apply(Edit edit, bool save);
    void save() const;

    QString m_path;
    QStringList m_order;
    QHash<QString, bool> m_wanted;
    QHash<QString, bool> m_default;
    QHash<QString, bool> m_stored; // every choice made, also of ids not registered now
    QList<QStringList> m_groups;
    QSet<QString> m_autoHide;
    QSet<QString> m_asked; // in focus mode: what was asked for since it began
    bool m_focus = false;
};

} // namespace fugue
