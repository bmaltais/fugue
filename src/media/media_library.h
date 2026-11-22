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
// and a row of detailsOf is {label, value}.

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

namespace fugue {

class MediaLibrary : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")

public:
    // Empty folders mean the defaults (the run folders, the thumbnail cache).
    explicit MediaLibrary(QString runsRoot = {}, QString thumbDir = {}, QObject *parent = nullptr);

    // What opens a file or folder; the default is the desktop's. Tests set their own.
    void setOpenUrl(std::function<void(const QUrl &)> openUrl) { m_openUrl = std::move(openUrl); }

    Q_INVOKABLE QVariantList resultsOf(const QString &workflow) const;
    // The results of one run alone (the queue's finished rows open the browser on it).
    Q_INVOKABLE QVariantList resultsOfRun(const QString &run) const;
    Q_INVOKABLE QVariantList detailsOf(const QString &path, const QString &run, const QString &node) const;
    Q_INVOKABLE void openFile(const QString &path) const;
    Q_INVOKABLE void openFolder(const QString &path) const;
    Q_INVOKABLE void copyPath(const QString &path) const;

    // The image:// source of a thumbnail of `path` that fits `box` pixels; the provider reads the
    // same table. A token stands for the path, so no file name is ever parsed out of a URL.
    Q_INVOKABLE QString thumbSource(const QString &path, int box = 256) const;
    QString pathOfToken(const QString &token) const;
    QQuickImageProvider *makeProvider() const;

    static constexpr const char *providerName = "fuguemedia";

private:
    QVariantList rowsOf(const QList<MediaItem> &items) const;

    QString m_runsRoot;
    QString m_thumbDir;
    std::function<void(const QUrl &)> m_openUrl;
    mutable QMutex m_lock;
    mutable QHash<QString, QString> m_tokens;
};

} // namespace fugue
