#pragma once

// Model downloads (spec 8.9, #89): the models a workflow names and lacks, fetched from the
// `source` its `requires.models` records, into the model folder downloads go in.
//
// A model is known by its content hash (2.5), so any https source is safe to take: a file is
// kept only when its sha256 is the handle's. It is written to `<file>.part`, hashed as it
// arrives, and renamed into place, never over a file that is there, only once the hash
// matches; the hash is then remembered (engine/file_hashes), so the model is found at once. A
// mismatch deletes the part. A break (the connection dropped, a silent server) is taken up
// again with a Range request, a few times; a part left by an earlier Fugue is read and hashed,
// then resumed the same way. Cancel deletes the part.
//
// http is taken only to this machine (a loopback address): nothing it sends leaves it.
//
// The window and the CLI run the same DownloadQueue. Not thread-safe: one thread, with an
// event loop.

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <atomic>

#include "engine/error.h"
#include "engine/result.h"

class QNetworkAccessManager;
class QNetworkReply;
class QThread;

namespace fugue {

class FileHashes;
class ModelLibrary;
struct MissingModel;

// One file to download.
struct DownloadTarget {
    QJsonObject handle; // resource, hash, name
    QUrl source;
    QString path;      // the file it becomes, absolute
    qint64 size = -1;  // in bytes, when `requires` records it
};

// Where `model` goes (ModelLibrary::downloadFolder() of its type, then its handle's name) and
// from where. Fails with download_failed when it has no source, the source is not https (or
// http to this machine), its name is not a relative path of a model file of its type (one of
// its extensions; no "..", no hidden part), or another file is at that path already.
Result<DownloadTarget> planDownload(const ModelLibrary &library, const MissingModel &model);

// True for an https URL, or an http one to a loopback address.
bool isAllowedSource(const QUrl &url);

class ModelDownload : public QObject {
    Q_OBJECT
public:
    // `network` and `hashes` must outlive the download.
    ModelDownload(DownloadTarget target, QNetworkAccessManager *network, FileHashes *hashes,
                  QObject *parent = nullptr);
    ~ModelDownload() override;

    const DownloadTarget &target() const { return m_target; }
    qint64 received() const { return m_received; }
    qint64 total() const { return m_total; } // -1 until the server says

    // How many times a break is taken up again, and the wait before the first (it doubles).
    void setRetries(int count, int firstDelayMs);
    // How long the server may send nothing before that is a break.
    void setStallTimeout(int ms) { m_stallMs = ms; }

    void start();
    // Stops it and deletes the part; finished() comes with `cancelled`.
    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    // Once. Empty: the file is in place and its hash remembered.
    void finished(const fugue::Errors &errors);

private:
    void resumePart(qint64 size);
    void request();
    void readHeaders();
    void readData();
    void replyFinished();
    void restartFromZero();
    void verify();
    void fail(const Error &error, bool keepPart);
    void end(const Errors &errors);
    QString partPath() const { return m_target.path + QStringLiteral(".part"); }
    QString shownName() const;

    DownloadTarget m_target;
    QNetworkAccessManager *m_network;
    FileHashes *m_hashes;
    QFile m_part;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QPointer<QNetworkReply> m_reply;
    QThread *m_hasher = nullptr;
    std::atomic<bool> m_stopHashing{false};
    qint64 m_received = 0;
    qint64 m_total = -1;
    int m_retries = 5;
    int m_retryDelayMs = 2000;
    int m_stallMs = 60000;
    int m_attempt = 0;
    bool m_headersRead = false;
    bool m_complete = false; // the server says the part already holds the whole file (416)
    bool m_cancelled = false;
    bool m_ended = false;
};

// Downloads one file after another: a model of 10 GB gets the whole line, and the first ones
// are usable while the rest arrive. A failed file is reported and the next one starts.
class DownloadQueue : public QObject {
    Q_OBJECT
public:
    DownloadQueue(QNetworkAccessManager *network, FileHashes *hashes, QObject *parent = nullptr);

    // Adds files at the end; one already queued (by its path) is not added twice. Starts when idle.
    void add(const QList<DownloadTarget> &targets);
    // Stops the file being fetched (its part is deleted) and drops the rest.
    void cancel();

    bool busy() const { return m_running; }
    const QList<DownloadTarget> &targets() const { return m_targets; } // all added since idle
    int index() const { return m_index; } // of the file being fetched
    qint64 received() const;
    qint64 total() const;
    void setRetries(int count, int firstDelayMs) { m_retries = count, m_retryDelayMs = firstDelayMs; }

signals:
    void started(int index);
    void progress(int index, qint64 received, qint64 total);
    void fileFinished(int index, const fugue::Errors &errors);
    // The line is empty again; `errors` of every file that failed (or `cancelled`).
    void finished(const fugue::Errors &errors);

private:
    void next();

    QNetworkAccessManager *m_network;
    FileHashes *m_hashes;
    QList<DownloadTarget> m_targets;
    int m_index = -1;
    QPointer<ModelDownload> m_current;
    bool m_running = false; // from the first file's start to finished(), between files too
    Errors m_errors;
    int m_retries = 5;
    int m_retryDelayMs = 2000;
};

} // namespace fugue
