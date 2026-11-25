#pragma once

// The `fal` provider (spec 8.7, #407): fal.ai's image models for a person who pastes only their fal.ai
// key. An instance of type `fal` (`{"name": "fal.ai", "type": "fal", "key": "..."}`) serves
// `image.generate` and `image.edit` (and inpainting with a mask where the model takes one) from the
// models of a table shipped with Fugue (share/providers/fal-models.json) and the endpoint ids the
// `image_models` setting adds.
//
// A job is fal's queue: upload the pictures to fal storage (initiate, then PUT), POST the input to
// <address>/<endpoint id>, poll the `status_url` the service returned (0.5 s growing to 3 s), GET its
// `response_url`, download the picture (no key). Cancel is a PUT to the returned `cancel_url`. Only
// the URLs the service returned are used: building them from the endpoint id is a 405.
//
// The key is sent as `Authorization: Key <key>` to the queue host and the storage host (the origins of
// `address` and `rest_address`) and to no other: never to the upload URL, a picture's URL or a
// redirect (none is followed on an API call). No message, status, result or file holds it; a service
// that echoes it in an error is scrubbed.
//
// Instance settings (all optional):
//   address        the queue's root (default https://queue.fal.run); tests point it at a local fake.
//   rest_address   fal's storage host (default https://rest.fal.ai, or `address` when that is set).
//   image_models   more endpoint ids (a list, or text separated by commas), with a generic mapping:
//                  prompt, image_size, seed, image_url.
//   store_io       true lets fal keep the job's inputs and outputs for 90 days, its default; unset, the
//                  requests carry `X-Fal-Store-IO: 0` and the files a short lifetime (below).
//   timeout_s      how long a request may go silent before the job fails (default 300); a job is given
//                  up on after 6 times that.
//   poll_ms        the first wait between polls (default 500; the wait grows to 6 times it).

#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

// The queue's root when an instance names no address, and fal's storage host.
QString falDefaultAddress();
QString falDefaultRestAddress();

// One endpoint: how Fugue's standard parameters (8.1) become the model's own input fields.
struct FalModel {
    enum class Kind { Generate, Edit, Inpaint };

    QString id;    // the endpoint id, such as fal-ai/flux/dev: the served name of the handle
    QString title;
    QString price; // fal.ai's published price, for the title
    Kind kind = Kind::Generate;
    QString sizeField; // "image_size" ({width, height}); empty: not sent
    QString stepsField;
    int stepsMax = 0;  // 0: no limit
    QString guidanceField;
    QString negativeField;
    QString seedField;
    QString imageField; // image_url; image_urls when `imageList`
    bool imageList = false;
    QString maskField;
    QJsonObject fixed;    // sent as it is
    QJsonObject defaults; // steps, guidance: what the model uses when a node says nothing
};

// The table shipped in the resources.
QList<FalModel> falShippedModels();

class FalProvider : public Provider {
    Q_OBJECT

public:
    explicit FalProvider(ProviderInstance instance, QObject *parent = nullptr);
    ~FalProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

private:
    struct Job;
    enum class Phase { Uploading, Submitting, Waiting, Fetching, Downloading };

    QList<FalModel> models() const;
    Result<Description> description() const;
    int timeoutMs() const;
    QString host() const;
    QUrl queueUrl(const QString &path) const;
    QUrl restUrl(const QString &path) const;
    // A URL the key may go to: the origin of the queue or of the storage host.
    bool isApiUrl(const QUrl &url) const;
    QNetworkReply *send(const QUrl &url, const QByteArray &method, const QByteArray &body = {},
                        const QByteArray &contentType = QByteArrayLiteral("application/json"), bool withKey = true,
                        qint64 cap = 64LL << 20, const QList<QPair<QByteArray, QByteArray>> &headers = {});
    QString clean(const QString &text) const;
    // The failure of an HTTP answer (code 0: no answer), in the spec's codes (8.7, 13.3).
    Error failure(QNetworkReply *reply, const QJsonObject &answer, bool submitting = false) const;
    Error failureOfJob(const QJsonObject &answer) const;

    // `reply` is the call in flight for the job; `then` runs when it ends, unless the job ended
    // meanwhile (a cancel), or always for `always`.
    void call(const QString &id, QNetworkReply *reply, std::function<void(QNetworkReply *)> then, bool always = false);
    void uploadNext(const QString &id);
    void sendJob(const QString &id);
    void pollLater(const QString &id);
    void pollNow(const QString &id);
    void fetchResult(const QString &id);
    void download(const QString &id, const QUrl &url);
    void saveImage(const QString &id, const QByteArray &bytes);
    void sendCancel(const std::shared_ptr<Job> &job);
    QJsonObject bodyOf(const Job &job) const;
    void change(const QString &id, const JobStatus &status);
    void fail(const QString &id, const Error &error);
    void forgetOldJobs();

    ProviderInstance m_instance;
    QNetworkAccessManager m_network;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    // Pictures already uploaded this session: content hash -> file URL and when it was made.
    struct Uploaded {
        QString url;
        qint64 at = 0;
    };
    QHash<QString, Uploaded> m_uploaded;
    int m_count = 0;
};

} // namespace fugue
