#pragma once

// The apps shelf's store (Fugue Apps, #388, epic #397): the installed apps as cards for the person who lands in Fugue with
// nothing open, and for File > Apps…. No window and no QML in it: value in (the templates, landing), change signal out.
//
//  - The catalog is Workflows::templates(): one list for the Templates window and the shelf, filtered to the apps (`app`),
//    never a second catalog. Each app is a card: {id, title, summary, preview, ready, needs, detail, local, hosts, offlineHosts}.
//    `ready` is the one word the card shows; `needs` says what is missing in few words ("Needs 2.1 GB and 1 pack"), empty when
//    ready; `detail` is the Templates window's own longer line, for a tooltip. Ready apps come first, the catalog's order kept.
//  - The list is read only by refresh(), which the shelf's window calls when it is made or shown. A window that is given a
//    workflow on the command line, or reopens its last tabs, never makes the shelf, so it costs nothing there.
//  - Landing: the window tells whether it has nothing open (setLanding). `atHome` is true while it has nothing open and the
//    person has not said "Start blank" (dismiss) in this session: the shelf takes the canvas's place until then.
//  - Choosing an app opens it as its template is opened anywhere (Workflows::openTemplateById): an app that is ready lands
//    in the App view; one that is not opens the same tab and the one Get ready sheet (#379), and on its yes the same tab is
//    shown ready to Run. The shelf holds no readiness logic of its own: it only says what the sheet will ask for.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

namespace fugue {

class AppShelf : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    // The window has nothing open and the person has not asked for a blank graph: the shelf is shown in the canvas's place.
    Q_PROPERTY(bool atHome READ atHome NOTIFY homeChanged)

public:
    using Source = std::function<QVariantList()>; // Workflows::templates(): every template as a map
    using Open = std::function<bool(const QString &id)>; // Workflows::openTemplateById

    AppShelf(Source source, Open open, QObject *parent = nullptr);

    // The apps among `templates`, as cards, ready ones first.
    static QVariantList cardsFrom(const QVariantList &templates);
    // One template's card; empty for a template that is not an app.
    static QVariantMap cardFor(const QVariantMap &templ);

    QVariantList apps() const { return m_apps; }
    bool atHome() const { return m_landing && !m_dismissed; }

    // Reads the catalog again (models arrive, folders change outside Fugue).
    Q_INVOKABLE void refresh();
    // Opens the app with this id; false when it could not be opened (the window says why). `chosen` follows on success.
    Q_INVOKABLE bool choose(const QString &id);
    // "Start blank": the person wants an empty graph, not the shelf; for the rest of the session.
    Q_INVOKABLE void dismiss();
    // The window has nothing open (one empty, untitled workflow) or has something.
    void setLanding(bool landing);

signals:
    void appsChanged();
    void homeChanged();
    void chosen(const QString &id);

private:
    Source m_source;
    Open m_open;
    QVariantList m_apps;
    bool m_landing = false;
    bool m_dismissed = false;
};

} // namespace fugue
