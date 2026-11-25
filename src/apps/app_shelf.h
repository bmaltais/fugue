#pragma once

// The apps shelf's store (Fugue Apps, #388, epic #397): the installed apps as cards for the person who lands in Fugue with
// nothing open, and for File > Apps…. No window and no QML in it: value in (the templates, landing), change signal out.
//
//  - The catalog is Workflows::templates(): one list for the Templates window and the shelf, filtered to the apps (`app`),
//    never a second catalog. Each app is a card: {id, title, summary, preview, ready, needs, detail, local, hosts, offlineHosts}.
//    `ready` is the one word the card shows; `needs` says what is missing in few words ("Needs 2.1 GB and 1 pack"), empty when
//    ready; `kind`, `video` (the moving preview), `own`, `inherited`, `canRename`, `bookmarked`; `detail` is the Templates window's own longer line, for a tooltip. Ready apps come first, the catalog's order kept.
//  - Search, bookmarks, Duplicate and Rename (#470): `query` narrows `shown` live (every word must be in the title, the summary
//    or the kind, any case); a bookmarked app (toggleBookmark, kept per user in a small file of the state folder) comes first
//    in `apps`, with a `bookmarked` flag on its card; Duplicate makes the app the person's own copy (Workflows::duplicateTemplate)
//    and opens it in the App view; Rename changes the title of one of the person's own apps only (`canRename`).
//  - The list is read only by refresh(), which the shelf's window calls when it is made or shown. A window that is given a
//    workflow on the command line, or reopens its last tabs, never makes the shelf, so it costs nothing there.
//  - Landing: the window tells whether it has nothing open (setLanding). `atHome` is true while it has nothing open and the
//    person has not said "Start blank" (dismiss) in this session: the shelf takes the canvas's place until then.
//  - Choosing an app opens it as its template is opened anywhere (Workflows::openTemplateById): an app that is ready lands
//    in the App view; one that is not opens the same tab and the one Get ready sheet (#379), and on its yes the same tab is
//    shown ready to Run. The shelf holds no readiness logic of its own: it only says what the sheet will ask for.

#include <QObject>
#include <QString>
#include <QStringList>
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
    // The search words; `shown` is `apps` narrowed by them (all of them when empty), in the same order.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList shown READ shown NOTIFY shownChanged)
    // The window has nothing open and the person has not asked for a blank graph: the shelf is shown in the canvas's place.
    Q_PROPERTY(bool atHome READ atHome NOTIFY homeChanged)
    // The shown tab is pristine (empty, untitled): the shelf's tab, whether it shows the shelf or, after Start blank, the empty graph.
    Q_PROPERTY(bool landing READ landing NOTIFY homeChanged)

public:
    using Source = std::function<QVariantList()>; // Workflows::templates(): every template as a map
    using Open = std::function<bool(const QString &id)>; // Workflows::openTemplateById
    using Duplicate = std::function<QString(const QString &id, const QString &title)>; // Workflows::duplicateTemplate: the new id
    using Rename = std::function<bool(const QString &id, const QString &title)>;       // Workflows::renameTemplate

    AppShelf(Source source, Open open, QObject *parent = nullptr);

    // The apps among `templates`, as cards, ready ones first; the `bookmarks` (ids) before the rest.
    static QVariantList cardsFrom(const QVariantList &templates, const QStringList &bookmarks = {});
    // One template's card; empty for a template that is not an app.
    static QVariantMap cardFor(const QVariantMap &templ);
    // Whether the card has every word of `query` (any case) in its title, summary or kind; an empty query matches.
    static bool matches(const QString &query, const QVariantMap &card);

    QVariantList apps() const { return m_apps; }
    QVariantList shown() const { return m_shown; }
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    // Where the bookmarks are kept ({"bookmarks": [ids]}); read now, written on each change. Without it they last the session.
    void setBookmarksFile(const QString &path);
    void setActions(Duplicate duplicate, Rename rename);
    Q_INVOKABLE bool isBookmarked(const QString &id) const { return m_bookmarks.contains(id); }
    Q_INVOKABLE void toggleBookmark(const QString &id);
    // A copy of the app as the person's own, opened in the App view; false when it could not be made (the window says why).
    Q_INVOKABLE bool duplicate(const QString &id);
    // Gives one of the person's own apps a new title; false for any other, or a blank title.
    Q_INVOKABLE bool rename(const QString &id, const QString &title);
    bool atHome() const { return m_landing && !m_dismissed; }
    bool landing() const { return m_landing; }

    // Reads the catalog again (models arrive, folders change outside Fugue).
    Q_INVOKABLE void refresh();
    // Opens the app with this id; false when it could not be opened (the window says why). `chosen` follows on success.
    Q_INVOKABLE bool choose(const QString &id);
    // "Start blank": the person wants an empty graph, not the shelf; for the rest of the session.
    Q_INVOKABLE void dismiss();
    // The window bar's App segment on the empty graph Start blank gave: the shelf comes back. Undoes dismiss() as a whole (the flag is
    // the session's, not a tab's), so every pristine tab shows the shelf again until the next Start blank.
    Q_INVOKABLE void showAgain();
    // The window has nothing open (one empty, untitled workflow) or has something.
    void setLanding(bool landing);

signals:
    void appsChanged();
    void shownChanged();
    void queryChanged();
    void homeChanged();
    void chosen(const QString &id);

private:
    void rebuild(const QVariantList &templates);
    void narrow();
    void saveBookmarks() const;

    Source m_source;
    Open m_open;
    Duplicate m_duplicate;
    Rename m_rename;
    QVariantList m_templates; // as the source gave them at the last refresh
    QVariantList m_apps;
    QVariantList m_shown;
    QString m_query;
    QStringList m_bookmarks;
    QString m_bookmarksFile;
    bool m_landing = false;
    bool m_dismissed = false;
};

} // namespace fugue
