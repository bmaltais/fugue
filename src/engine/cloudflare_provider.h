#pragma once

// The `cloudflare` provider (spec 8.7, #407): Cloudflare Workers AI's image models for a person who pastes an API
// token and their account id. An instance of type `cloudflare`
// (`{"name": "Cloudflare Workers AI", "type": "cloudflare", "key": "<token>", "account_id": "..."}`) serves
// `image.generate` and, for the models that take reference pictures, `image.edit`, from the models of a table shipped
// with Fugue (share/providers/cloudflare-models.json) and the ids the `image_models` setting adds.
//
// A job is one synchronous call: POST <address>/client/v4/accounts/<account_id>/ai/run/<model id> (JSON, or
// multipart/form-data for the FLUX.2 models, which also carry the reference pictures as input_image_0..3). The answer
// holds the picture as base64 (result.image); a raw picture body is taken too. No second request, no download.
//
// The token is sent as `Authorization: Bearer <token>` to the origin of `address` and to no other; no redirect is
// followed. No message, status or result holds it; a service that echoes it in an error is scrubbed. A token is not
// sent over plain http to a machine other than this one.
//
// Instance settings:
//   account_id     the Cloudflare account the calls are made for (required; the dashboard shows it).
//   address        the API's root (default https://api.cloudflare.com); tests point it at a local fake.
//   image_models   more model ids (@cf/..., a list, or text separated by commas), with a generic mapping:
//                  prompt, width, height, seed as JSON.
//   timeout_s      how long the call may go silent before the job fails (default 120); a job is given up on
//                  after 6 times that.

#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

#include <memory>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

// The API's root when an instance names no address.
QString cloudflareDefaultAddress();
// The API's address: the instance's, else the default.
QString cloudflareAddress(const ProviderInstance &instance);

// One model: how Fugue's standard parameters (8.1) become the model's own request fields.
struct CloudflareModel {
    QString id;    // @cf/black-forest-labs/flux-1-schnell: the served name of the handle
    QString title;
    QString price; // Cloudflare's published price, for the title
    bool multipart = false;
    bool size = false; // width and height are sent (clamped to sizeMin..sizeMax)
    int sizeMin = 0, sizeMax = 0;
    bool steps = false;
    int stepsMax = 0; // 0: no limit
    bool guidance = false;
    bool seed = false;
    int images = 0; // reference pictures (input_image_N) the model takes; 0: none
    QJsonObject defaults;
};

// The table shipped in the resources.
QList<CloudflareModel> cloudflareShippedModels();

class CloudflareProvider : public Provider {
    Q_OBJECT

public:
    explicit CloudflareProvider(ProviderInstance instance, QObject *parent = nullptr);
    ~CloudflareProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

private:
    struct Job;

    QList<CloudflareModel> models() const;
    QString accountId() const;
    Result<Description> description() const;
    int timeoutMs() const;
    QUrl runUrl(const QString &model) const;
    QString clean(const QString &text) const;
    // The failure of an HTTP answer (code 0: no answer), in the spec's codes (8.7, 13.3).
    Error failure(QNetworkReply *reply, const QJsonObject &answer) const;
    void change(const QString &id, const JobStatus &status);
    void fail(const QString &id, const Error &error);
    void forgetOldJobs();
    void finish(const QString &id, QNetworkReply *reply);
    void saveImage(const QString &id, const QByteArray &bytes);

    ProviderInstance m_instance;
    QNetworkAccessManager m_network;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    int m_count = 0;
};

} // namespace fugue
