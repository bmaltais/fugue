#pragma once

// The `gemini` provider (spec 8.7, #407): Google's Gemini image models (Nano Banana) for a person who pastes only a Gemini API
// key. An instance of type `gemini` (`{"name": "Google Gemini", "type": "gemini", "key": "..."}`) serves `image.generate` and
// `image.edit` from the models of a table shipped with Fugue (share/providers/gemini-models.json) and the model ids the
// `image_models` setting adds. Every model takes the prompt and the node's pictures in one request, so a picture is an edit
// or a reference, and a mask is `mask_unsupported`: the API takes none.
//
// A job is one synchronous call: POST <address>/v1beta/models/<model>:generateContent, with the pictures as base64
// `inline_data` parts, and the first non-thought `inlineData` image of the answer is the result. The key is sent as
// `x-goog-api-key` to the origin of `address` and nowhere else: it is in no URL (never `?key=`), and no redirect is followed.
// No message, status or result holds it; a service that echoes it in an error is scrubbed.
//
// Instance settings (all optional):
//   address        the API's root (default https://generativelanguage.googleapis.com); tests point it at a local fake.
//   image_models   more model ids (a list, or text separated by commas), with the generic mapping: aspectRatio from the size,
//                  up to 14 pictures.
//   timeout_s      the whole job's deadline (default 300); the job fails with `node_timeout` after it.

#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>

#include <memory>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

QString geminiDefaultAddress();
// The API's address (the instance's, else the default), without a trailing slash.
QString geminiAddress(const ProviderInstance &instance);

// One model: what it accepts.
struct GeminiModel {
    QString id; // the served name of the handle, such as gemini-3.1-flash-image
    QString title;
    QString price; // Google's published price, for the title
    int maxImages = 14;
    QStringList aspects; // aspectRatio values; empty: not sent
    QStringList sizes;   // imageSize values; empty: not sent
};

// The table shipped in the resources.
QList<GeminiModel> geminiShippedModels();

class GeminiProvider : public Provider {
    Q_OBJECT

public:
    explicit GeminiProvider(ProviderInstance instance, QObject *parent = nullptr);
    ~GeminiProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

private:
    struct Job;

    QList<GeminiModel> models() const;
    Result<Description> description() const;
    int timeoutMs() const;
    QString host() const;
    QString clean(const QString &text) const;
    // The failure of an HTTP answer (code 0: no answer), in the spec's codes (8.7, 13.3).
    Error failure(QNetworkReply *reply, const QJsonObject &answer) const;
    void finish(const QString &id, QNetworkReply *reply);
    void saveImage(const QString &id, const QByteArray &bytes);
    void change(const QString &id, const JobStatus &status);
    void fail(const QString &id, const Error &error);
    void forgetOldJobs();

    ProviderInstance m_instance;
    QNetworkAccessManager m_network;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    int m_count = 0;
};

} // namespace fugue
