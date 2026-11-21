#include "model_download.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_library.h"
#include "engine/port_type.h"

namespace fugue {

namespace {

constexpr qint64 kReadChunk = 4 * 1024 * 1024;

Error failed(const QString &message, const QString &hint = {}) {
    return makeError(QStringLiteral("download_failed"), message, hint);
}

// Something is at `path`: a file, a folder, or a link (even one pointing nowhere).
bool occupied(const QString &path) { return QFileInfo::exists(path) || QFileInfo(path).isSymLink(); }

// Renames `from` to `to` unless something is at `to`, atomically where the file system can
// (renameat2 RENAME_NOREPLACE). 0, or the errno.
int renameNoReplace(const QString &from, const QString &to) {
    const QByteArray a = QFile::encodeName(from), b = QFile::encodeName(to);
    if (::renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return errno;
    // A file system without the flag: as close as it gets.
    if (occupied(to))
        return EEXIST;
    return ::rename(a.constData(), b.constData()) == 0 ? 0 : errno;
}

} // namespace

QString sizeText(qint64 bytes) {
    return bytes >= 1000000000 ? QStringLiteral("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 1)
                               : QStringLiteral("%1 MB").arg(std::max<qint64>(1, (bytes + 500000) / 1000000));
}

QString DownloadTarget::fileName() const {
    const QString name = handle.value(QStringLiteral("name")).toString();
    return name.isEmpty() ? QFileInfo(path).fileName() : name;
}

bool isAllowedSource(const QUrl &url) {
    if (!url.isValid() || url.host().isEmpty())
        return false;
    if (url.scheme() == QLatin1String("https"))
        return true;
    if (url.scheme() != QLatin1String("http"))
        return false;
    if (url.host() == QLatin1String("localhost"))
        return true;
    const QHostAddress address(url.host());
    return !address.isNull() && address.isLoopback();
}

Result<DownloadTarget> planDownload(const ModelLibrary &library, const MissingModel &model) {
    const QString name = model.handle.value(QStringLiteral("name")).toString();
    const QString resource = model.handle.value(QStringLiteral("resource")).toString();
    const QString shown = name.isEmpty() ? resource : name;
    if (model.source.isEmpty())
        return failed(QStringLiteral("%1 has no source to download it from.").arg(shown),
                      QStringLiteral("Get it from its maker and put it in %1.").arg(model.folder));
    const QUrl source(model.source, QUrl::StrictMode);
    if (!isAllowedSource(source))
        return failed(QStringLiteral("%1 cannot be downloaded from %2: Fugue downloads over https only.")
                          .arg(shown, model.source),
                      QStringLiteral("Download it yourself and put it in %1.").arg(model.folder));
    const std::optional<PortType> type = PortType::parse(resource);
    const QString folder = type ? library.downloadFolder(*type) : QString();
    if (folder.isEmpty())
        return failed(QStringLiteral("%1 is not a model type of an installed pack.").arg(resource),
                      QStringLiteral("Install the pack that declares %1.").arg(resource));
    // A relative name inside the type's folder; a part starting with "." would never be listed.
    const QStringList parts = name.split(QLatin1Char('/'));
    bool safe = !name.isEmpty() && !name.startsWith(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
    for (const QString &part : parts)
        safe = safe && !part.isEmpty() && !part.startsWith(QLatin1Char('.'));
    if (!safe || !library.takesFile(*type, parts.last()))
        return failed(QStringLiteral("The model name %1 is not a file name Fugue can download to.").arg(shown),
                      QStringLiteral("Download it yourself and put it in %1.").arg(folder));
    const QString path = QDir::cleanPath(folder + QLatin1Char('/') + name);
    if (occupied(path))
        return failed(QStringLiteral("%1 is in %2 already, but it is another file than the one the workflow names.")
                          .arg(name, folder),
                      QStringLiteral("Move or rename that file, then download again."));
    return DownloadTarget{model.handle, source, path, model.size};
}

// ---- ModelDownload ----

ModelDownload::ModelDownload(DownloadTarget target, QNetworkAccessManager *network, FileHashes *hashes,
                             QObject *parent)
    : QObject(parent), m_target(std::move(target)), m_network(network), m_hashes(hashes) {}

ModelDownload::~ModelDownload() {
    if (m_hasher) {
        m_stopHashing = true;
        m_hasher->wait();
        delete m_hasher;
    }
    dropReply();
}

void ModelDownload::dropReply() {
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this); // abort() says finished at once: not to this download
    reply->abort();
    reply->deleteLater();
}

void ModelDownload::setRetries(int count, int firstDelayMs) {
    m_retries = count;
    m_retryDelayMs = firstDelayMs;
}


void ModelDownload::start() {
    const QString folder = QFileInfo(m_target.path).absolutePath();
    if (!QDir().mkpath(folder))
        return fail(makeError(QStringLiteral("permission_denied"),
                              QStringLiteral("Fugue cannot make the folder %1.").arg(folder),
                              QStringLiteral("Check that the model folder is writable, or download into another.")),
                    true);
    if (occupied(m_target.path))
        return fail(failed(QStringLiteral("%1 is there already.").arg(m_target.path),
                           QStringLiteral("Move or rename that file, then download again.")),
                    true);
    // One writer of a part: another Fugue (the window, `fugue model get`) fetching the same file.
    m_lock = std::make_unique<QLockFile>(partPath() + QStringLiteral(".lock"));
    m_lock->setStaleLockTime(0); // a lock whose process is gone is taken over, however old
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        return fail(failed(QStringLiteral("%1 is being downloaded by another Fugue already.").arg(m_target.fileName()),
                           QStringLiteral("Wait for that download to end.")),
                    true);
    }
    // A link at the part's name would have the download write into whatever it points to.
    if (QFileInfo(partPath()).isSymLink())
        QFile::remove(partPath());
    m_part.setFileName(partPath());
    const qint64 left = QFileInfo(partPath()).size();
    if (QFileInfo::exists(partPath()) && left > 0)
        return resumePart(left);
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(makeError(QStringLiteral("permission_denied"),
                              QStringLiteral("Fugue cannot write %1: %2.").arg(partPath(), m_part.errorString()),
                              QStringLiteral("Check that the model folder is writable, or download into another.")),
                    true);
    request();
}

// A part an earlier download left: its bytes are hashed (off this thread: it may be many GB),
// then the rest is asked for.
void ModelDownload::resumePart(qint64 size) {
    const QString path = partPath();
    m_hasher = QThread::create([this, path, size] {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return;
        qint64 read = 0;
        while (read < size && !m_stopHashing) {
            const QByteArray chunk = file.read(std::min(kReadChunk, size - read));
            if (chunk.isEmpty())
                break;
            m_hash.addData(chunk);
            read += chunk.size();
        }
        m_hashedBytes = read;
    });
    connect(m_hasher, &QThread::finished, this, [this] {
        m_hasher->deleteLater();
        m_hasher = nullptr;
        if (m_cancelled)
            return;
        m_received = m_hashedBytes; // the thread has ended: m_hash is this thread's again
        // Exactly the bytes hashed: a part that grew or shrank meanwhile is cut to them.
        if (!m_part.open(QIODevice::ReadWrite) || !m_part.resize(m_received) || !m_part.seek(m_received))
            return fail(makeError(QStringLiteral("permission_denied"),
                                  QStringLiteral("Fugue cannot write %1: %2.").arg(partPath(), m_part.errorString())),
                        true);
        emit progress(m_received, m_total);
        request();
    });
    m_hasher->start();
}

void ModelDownload::request() {
    if (m_cancelled || m_ended)
        return;
    m_headersRead = false;
    ++m_requests;
    QNetworkRequest request(m_target.source);
    // Follows redirects (a model host hands off to its storage) to what a source may be, never
    // from https to http: from http on this machine only to this machine.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setTransferTimeout(m_stallMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Fugue"));
    // The file's own bytes: a compressed answer's length is not the file's, nor can a range of it
    // be asked for.
    request.setRawHeader("Accept-Encoding", "identity");
    if (m_received > 0)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_received) + '-');
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::redirected, this, [this](const QUrl &to) {
        const QUrl from = m_reply ? m_reply->url() : QUrl();
        if (m_reply && isAllowedSource(to) && !(from.scheme() == QLatin1String("https") && to.scheme() != from.scheme())) {
            emit m_reply->redirectAllowed();
            return;
        }
        fail(failed(QStringLiteral("%1 sent the download of %2 on to %3, which Fugue does not download from.")
                        .arg(from.host(), m_target.fileName(), to.toString()),
                    QStringLiteral("Download it yourself and put it in %1.").arg(QFileInfo(m_target.path).absolutePath())),
             true);
    });
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelDownload::readData);
    connect(m_reply, &QNetworkReply::finished, this, &ModelDownload::replyFinished);
}

void ModelDownload::readHeaders() {
    m_headersRead = true;
    QNetworkReply *reply = m_reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).isValid()
                              ? reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()
                              : -1;
    if (status == 416 && m_received > 0) {
        // Nothing past the part's end: it may hold the whole file. The hash tells.
        m_partWhole = true;
        m_total = m_received;
        return;
    }
    if (status == 206) {
        static const QRegularExpression range(QStringLiteral("^bytes (\\d+)-(\\d+)/(\\d+|\\*)$"));
        const QRegularExpressionMatch match = range.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
        if (!match.hasMatch() || match.captured(1).toLongLong() != m_received)
            return restartFromZero();
        if (match.captured(3) != QLatin1String("*"))
            m_total = match.captured(3).toLongLong();
        else
            m_total = length >= 0 ? m_received + length : -1; // told by the end of the stream
    } else if (status == 200) {
        if (m_received > 0) {
            // The server sent the whole file again: start the part over.
            m_part.resize(0);
            m_part.seek(0);
            m_hash.reset();
            m_received = 0;
        }
        m_total = length;
    } else {
        const QString host = m_target.source.host();
        QString hint = QStringLiteral("Try again later, or download it yourself and put it in %1.")
                           .arg(QFileInfo(m_target.path).absolutePath());
        if (status == 401 || status == 403)
            hint = QStringLiteral("The model may need a sign-in or a licence accepted at %1: download it in a "
                                  "browser and put it in %2.")
                       .arg(host, QFileInfo(m_target.path).absolutePath());
        return fail(failed(QStringLiteral("%1 answered %2 for %3.").arg(host).arg(status).arg(m_target.fileName()), hint), true);
    }
    if (m_target.size >= 0 && m_total >= 0 && m_total != m_target.size) {
        return fail(makeError(QStringLiteral("download_mismatch"),
                              QStringLiteral("The file at %1 is %2 bytes; the workflow names one of %3 bytes, so it is "
                                             "another file than %4.")
                                  .arg(m_target.source.toString())
                                  .arg(m_total)
                                  .arg(m_target.size)
                                  .arg(m_target.fileName()),
                              QStringLiteral("The source may hold a newer build of the model: get the one the workflow "
                                             "was made with from its maker.")),
                    false);
    }
    if (m_total >= 0) {
        const QStorageInfo storage(QFileInfo(m_target.path).absolutePath());
        const qint64 needed = m_total - m_received;
        if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < needed)
            return fail(failed(QStringLiteral("%1 needs %2 more, and %3 has %4 free.")
                                   .arg(m_target.fileName(), sizeText(needed), storage.rootPath(),
                                        sizeText(storage.bytesAvailable())),
                               QStringLiteral("Make room there, or download into another model folder.")),
                        true);
    }
}

void ModelDownload::readData() {
    if (!m_reply || m_ended)
        return;
    if (!m_headersRead)
        readHeaders();
    if (m_ended || !m_reply || m_partWhole)
        return;
    const QByteArray data = m_reply->readAll();
    if (data.isEmpty())
        return;
    if (m_part.write(data) != data.size())
        return fail(failed(QStringLiteral("Fugue could not write %1: %2.").arg(partPath(), m_part.errorString()),
                           QStringLiteral("Make room in the model folder, then download again to go on.")),
                    true);
    m_hash.addData(data);
    m_received += data.size();
    m_attempt = 0; // it moves again: a later break has its retries anew
    emit progress(m_received, m_total);
}

void ModelDownload::replyFinished() {
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    reply->deleteLater();
    if (m_ended || m_cancelled)
        return;
    // Each step may end the download or start the request over (m_reply is then another).
    if (!m_headersRead && (reply->error() == QNetworkReply::NoError
                           || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()))
        readHeaders(); // an error status (a 404 says so in error() too) is read as one
    if (m_ended || m_reply != reply)
        return;
    if (m_headersRead && !m_partWhole && reply->isReadable())
        readData();
    if (m_ended || m_reply != reply)
        return;
    m_reply = nullptr;
    const bool whole = m_partWhole || (reply->error() == QNetworkReply::NoError && (m_total < 0 || m_received == m_total));
    if (whole)
        return verify();
    // A break: the connection dropped or the server fell silent. Go on from here.
    m_part.flush();
    if (m_attempt < m_retries) {
        const int delay = m_retryDelayMs << std::min(m_attempt, 6);
        ++m_attempt;
        QTimer::singleShot(delay, this, &ModelDownload::request);
        return;
    }
    const QString host = m_target.source.host();
    if (!m_headersRead && m_received == 0)
        return fail(failed(QStringLiteral("%1 could not be reached for %2: %3.").arg(host, m_target.fileName(), reply->errorString()),
                           QStringLiteral("Check the network, then download again.")),
                    true);
    fail(failed(QStringLiteral("The download of %1 broke off at %2 of %3: %4.")
                    .arg(m_target.fileName(), sizeText(m_received), m_total >= 0 ? sizeText(m_total) : QStringLiteral("?"),
                         reply->errorString()),
                QStringLiteral("Download again to go on from there.")),
         true);
}

void ModelDownload::restartFromZero() {
    dropReply();
    m_part.resize(0);
    m_part.seek(0);
    m_hash.reset();
    m_received = 0;
    m_total = -1;
    QTimer::singleShot(0, this, &ModelDownload::request);
}

void ModelDownload::verify() {
    m_part.close();
    const QString got = QStringLiteral("sha256:") + QString::fromLatin1(m_hash.result().toHex());
    const QString wanted = m_target.handle.value(QStringLiteral("hash")).toString();
    if (got != wanted)
        return fail(makeError(QStringLiteral("download_mismatch"),
                              QStringLiteral("The file from %1 is not %2: its content hash is %3, the workflow names "
                                             "%4. It was deleted.")
                                  .arg(m_target.source.toString(), m_target.fileName(), got, wanted),
                              QStringLiteral("The source may hold a newer build of the model: get the one the workflow "
                                             "was made with from its maker.")),
                    false);
    if (const int error = renameNoReplace(partPath(), m_target.path); error != 0)
        return fail(failed(error == EEXIST ? QStringLiteral("A file came to %1 while %2 was downloading; it was left "
                                                            "as it is.")
                                                 .arg(m_target.path, m_target.fileName())
                                           : QStringLiteral("Fugue could not move %1 into place: %2.")
                                                 .arg(partPath(), QString::fromLocal8Bit(std::strerror(error))),
                           error == EEXIST ? QStringLiteral("Check which of the two is the model.") : QString()),
                    error != EEXIST);
    m_hashes->remember(m_target.path, got);
    m_hashes->save();
    end({});
}

void ModelDownload::cancel() {
    if (m_ended)
        return;
    m_cancelled = true;
    m_stopHashing = true;
    dropReply();
    if (m_hasher)
        m_hasher->wait(); // its file is the part, removed below
    m_part.close();
    QFile::remove(partPath());
    end({makeError(QStringLiteral("cancelled"), QStringLiteral("The download of %1 was cancelled.").arg(m_target.fileName()))});
}

void ModelDownload::fail(const Error &error, bool keepPart) {
    if (m_ended)
        return;
    dropReply();
    m_part.close();
    if (!keepPart)
        QFile::remove(partPath());
    end({error});
}

void ModelDownload::end(const Errors &errors) {
    if (m_ended)
        return;
    m_ended = true;
    m_lock.reset(); // unlocks
    emit finished(errors);
}

// ---- DownloadQueue ----

DownloadQueue::DownloadQueue(QNetworkAccessManager *network, FileHashes *hashes, QObject *parent)
    : QObject(parent), m_network(network), m_hashes(hashes) {}

void DownloadQueue::add(const QList<DownloadTarget> &targets) {
    for (const DownloadTarget &target : targets) {
        bool queued = false;
        for (qsizetype i = std::max(0, m_index); i < m_targets.size(); ++i)
            queued = queued || m_targets.at(i).path == target.path;
        if (!queued)
            m_targets.append(target);
    }
    if (!m_running && m_index + 1 < m_targets.size()) {
        m_running = true;
        next();
    }
}

void DownloadQueue::cancel() {
    if (!m_running)
        return;
    m_targets = m_targets.mid(0, m_index + 1); // the rest are dropped
    if (m_current)
        m_current->cancel();
    else // between two files: next() finds none left
        m_errors << makeError(QStringLiteral("cancelled"), QStringLiteral("The downloads were cancelled."));
}

qint64 DownloadQueue::received() const { return m_current ? m_current->received() : 0; }
qint64 DownloadQueue::total() const { return m_current ? m_current->total() : -1; }

void DownloadQueue::next() {
    ++m_index;
    if (m_index >= m_targets.size()) {
        const Errors errors = m_errors;
        m_targets.clear();
        m_errors.clear();
        m_index = -1;
        m_running = false;
        emit finished(errors);
        return;
    }
    auto *download = new ModelDownload(m_targets.at(m_index), m_network, m_hashes, this);
    download->setRetries(m_retries, m_retryDelayMs);
    m_current = download;
    const int index = m_index;
    connect(download, &ModelDownload::progress, this,
            [this, index](qint64 received, qint64 total) { emit progress(index, received, total); });
    connect(download, &ModelDownload::finished, this, [this, index, download](const Errors &errors) {
        m_errors += errors;
        m_current = nullptr;
        download->deleteLater();
        emit fileFinished(index, errors);
        // After this call returns: a slot may add to the line, and the part's file is closed.
        QTimer::singleShot(0, this, &DownloadQueue::next);
    });
    emit started(m_index);
    download->start();
}

} // namespace fugue
