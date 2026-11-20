#pragma once

// The window's model downloads (#89): one line of files (engine/model_download.h's
// DownloadQueue), what the banner and the Templates dialog show of it, and cancel. Workflows
// fills it (the missing models of a workflow or a template) and tells the banner when it ends.
//
// Progress reaches QML at most ten times a second: a fast download reads many small pieces.

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/error.h"
#include "engine/file_hashes.h"
#include "engine/model_download.h"

namespace fugue {

class ModelDownloads : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by Workflows")
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Who asked, as add() was told: "template:<id>", or "workflow" for the banner's.
    Q_PROPERTY(QString what READ what NOTIFY changed)
    // "flux-2-klein-9b-Q8_0.gguf, 2 of 3"
    Q_PROPERTY(QString label READ label NOTIFY changed)
    // "4.2 of 10.2 GB"; empty before the server says how large.
    Q_PROPERTY(QString progressText READ progressText NOTIFY changed)
    // 0 to 1; -1 while the size is not known.
    Q_PROPERTY(double fraction READ fraction NOTIFY changed)

public:
    explicit ModelDownloads(QObject *parent = nullptr);

    bool busy() const { return m_queue.busy(); }
    QString what() const { return busy() ? m_what : QString(); }
    QString label() const;
    QString progressText() const;
    double fraction() const;

    // Queues `targets`. While another's files are fetched, these wait after them, and `what`
    // stays the first asker's until the line is empty.
    void add(const QList<DownloadTarget> &targets, const QString &what);
    Q_INVOKABLE void cancel() { m_queue.cancel(); }

    // "12.3 GB", "450 MB"
    static QString sizeText(qint64 bytes);

signals:
    void changed();
    // The line is empty again: how many files arrived, and what failed (or `cancelled`).
    void finished(int arrived, const fugue::Errors &errors);

private:
    void tell(bool now);

    QNetworkAccessManager m_network;
    FileHashes m_hashes{FileHashes::defaultPath()};
    DownloadQueue m_queue{&m_network, &m_hashes};
    QString m_what;
    int m_arrived = 0;
    QElapsedTimer m_told;
};

} // namespace fugue
