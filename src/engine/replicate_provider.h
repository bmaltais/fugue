#pragma once

// The `replicate` provider (spec 8.7, #407): Replicate's official image models for a person who pastes only their
// Replicate API token. An instance of type `replicate` (`{"name": "Replicate", "type": "replicate", "key": "r8_..."}`) serves
// `image.generate` and `image.edit` (and inpainting with a mask where the model takes one) from the models of a table
// shipped with Fugue (share/providers/replicate-models.json) and the `owner/name` ids the `image_models` setting adds.
//
// A job: `POST <address>/v1/models/<owner>/<name>/predictions` ({"input": {...}}, the official-model endpoint), poll the
// `urls.get` the service returned (0.5 s growing to 3 s) until `succeeded`, download the first output URL (no key). Cancel is
// a POST to the returned `urls.cancel`. Only URLs the service returned are used, and only when they are on the API's own origin.
//
// The key is sent as `Authorization: Bearer <key>` to the origin of `address` and to no other: never to an output or
// picture URL, a redirect (none is followed on an API call). No message, status, result or file holds it; a service that
// echoes it in an error is scrubbed.
//
// Pictures: up to 1 MiB each go into the input as a `data:` URI (Replicate does not store those); a larger one is uploaded with
// the files API (`POST /v1/files`, 100 MB at most) and its `urls.get` goes into the input.
//
// Instance settings (all optional):
//   address        the API's root (default https://api.replicate.com); tests point it at a local fake.
//   image_models   more official model ids, `owner/name` (a list, or text separated by commas), with a generic mapping:
//                  prompt, seed, image.
//   timeout_s      how long a request may go silent before the job fails (default 300); a job is given up on after 6 times that.
//   poll_ms        the first wait between polls (default 500; the wait grows to 6 times it).

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

// The API's root when an instance names no address.
QString replicateDefaultAddress();
// The API's address: the instance's, else the default.
QString replicateAddress(const ProviderInstance &instance);

// One model: how Fugue's standard parameters (8.1) become the model's own input fields.
struct ReplicateModel {
    enum class Kind { Generate, Edit, Inpaint };

    QString id;    // owner/name, such as black-forest-labs/flux-schnell: the served name of the handle
    QString title;
    QString price; // Replicate's published price, for the title
    Kind kind = Kind::Generate;
    QString aspectField;          // "aspect_ratio": the nearest of `aspects` to the node's size; empty: not sent
    QStringList aspects;          // "16:9", ...
    QString aspectFollow;         // sent as the aspect when a picture goes in ("match_input_image"); empty: nothing
    QString megapixelsField;      // "megapixels": the nearest of `megapixelValues`
    QStringList megapixelValues;  // "0.25", "1", ... (strings, as the model takes them)
    QString stepsField;
    int stepsMax = 0;             // 0: no limit
    QString guidanceField;
    QString negativeField;
    QString seedField;
    QString imageField;           // a picture; a list of them when `imageList`
    bool imageList = false;
    QString maskField;
    QJsonObject fixed;    // sent as it is
    QJsonObject defaults; // steps, guidance: what the model uses when a node says nothing
};

// The table shipped in the resources.
QList<ReplicateModel> replicateShippedModels();

class ReplicateProvider : public Provider {
    Q_OBJECT

public:
    explicit ReplicateProvider(ProviderInstance instance, QObject *parent = nullptr);
    ~ReplicateProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

private:
    struct Job;
    enum class Phase { Uploading, Submitting, Waiting, Downloading };

    QList<ReplicateModel> models() const;
    Result<Description> description() const;
    int timeoutMs() const;
    QString host() const;
    QUrl apiUrl(const QString &path) const;
    // A URL the key may go to: the origin of the API.
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
    void pollLater(const QString &id, int atLeastMs = 0);
    void giveUp(const QString &id);
    void pollNow(const QString &id);
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
    int m_count = 0;
};

} // namespace fugue
