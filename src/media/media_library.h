#pragma once

// What the window gives a media browser (#133): the results of a workflow as the maps
// MediaBrowser.qml reads, the details of one, the actions on a file, and the thumbnails. The
// browser itself knows none of this (it takes a list and says what a person chose); the host
// (the workflows panel, later a node's face) asks here and hands the answer over.
//
// A row of resultsOf is
//   {path, url (file URL), kind ("image"|"video"|"audio"), title (the file name), group (the run's
//    heading: its start in local time), run, node, thumb (an image:// source; for a video a frame, for a sound its waveform, drawn by
//    the provider off the UI thread and an image load error when it cannot, so the cell keeps its glyph)}
// and a row of detailsOf is {label, value}. A row has also `favorite` (the file is one of the favorites)
// and, for an image, `width` and `height` (0 when unread). What the browser keeps between sessions
// (the favorites, the mode, the view, the favorites-only filter) is MediaState's.

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

#include "media/media_items.h"
#include "media/media_state.h"

namespace fugue {

class MediaLibrary : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // What the browser remembers (media_state.h). `onlyFavorites` narrows resultsOf and resultsOfRun.
    Q_PROPERTY(QString resultsMode READ resultsMode WRITE setResultsMode NOTIFY stateChanged)
    Q_PROPERTY(QString resultsView READ resultsView WRITE setResultsView NOTIFY stateChanged)
    Q_PROPERTY(bool onlyFavorites READ onlyFavorites WRITE setOnlyFavorites NOTIFY resultsChanged)

public:
    // Empty folders mean the defaults (the run folders, the thumbnail cache, the state folder).
    explicit MediaLibrary(QString runsRoot = {}, QString thumbDir = {}, QString statePath = {}, QObject *parent = nullptr);

    // What opens a file or folder; the default is the desktop's. Tests set their own.
    void setOpenUrl(std::function<void(const QUrl &)> openUrl) { m_openUrl = std::move(openUrl); }

    Q_INVOKABLE QVariantList resultsOf(const QString &workflow) const;
    // The results of one run alone (the queue's finished rows open the browser on it).
    Q_INVOKABLE QVariantList resultsOfRun(const QString &run) const;
    Q_INVOKABLE QVariantList detailsOf(const QString &path, const QString &run, const QString &node) const;
    Q_INVOKABLE void openFile(const QString &path) const;
    Q_INVOKABLE void openFolder(const QString &path) const;
    Q_INVOKABLE void copyPath(const QString &path) const;

    // Favorites (#133). toggleFavorite returns the new state; both announce resultsChanged.
    Q_INVOKABLE bool isFavorite(const QString &path) const { return m_state.isFavorite(path); }
    Q_INVOKABLE bool toggleFavorite(const QString &path);
    // What can be done with a file: {"workflow": it holds an embedded workflow (open it, re-run it),
    // "loadImage": it is an image a Load Image node can take}.
    Q_INVOKABLE QVariantMap capabilitiesOf(const QString &path) const;
    // Moves a regular file to the trash (never deletes it) and forgets it as a favorite. False when
    // it is no file or the trash would not take it. The browser asks the person first.
    Q_INVOKABLE bool trash(const QString &path);
    // What moves a file to the trash; the default is the desktop's (QFile::moveToTrash). Tests set their own.
    void setTrash(std::function<bool(const QString &)> trash) { m_trash = std::move(trash); }

    QString resultsMode() const { return m_state.mode(); }
    void setResultsMode(const QString &mode);
    QString resultsView() const { return m_state.view(); }
    void setResultsView(const QString &view);
    bool onlyFavorites() const { return m_state.onlyFavorites(); }
    void setOnlyFavorites(bool on);

    // The image:// source of a thumbnail of `path` that fits `box` pixels; the provider reads the
    // same table. A token stands for the path, so no file name is ever parsed out of a URL.
    Q_INVOKABLE QString thumbSource(const QString &path, int box = 256) const;
    QString pathOfToken(const QString &token) const;
    QQuickImageProvider *makeProvider() const;

    static constexpr const char *providerName = "fuguemedia";

signals:
    // The rows a list shows would be different now (a favorite, the filter, a file in the trash).
    void resultsChanged();
    void stateChanged();

private:
    QVariantList rowsOf(const QList<MediaItem> &items) const;

    QString m_runsRoot;
    QString m_thumbDir;
    MediaState m_state;
    std::function<void(const QUrl &)> m_openUrl;
    std::function<bool(const QString &)> m_trash;
    mutable QMutex m_lock;
    mutable QHash<QString, QString> m_tokens;
};

} // namespace fugue
