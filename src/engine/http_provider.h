#pragma once

// The `http` provider (spec 8.7, #401): a generic endpoint, an instance of type `http` with an
// `address`, an optional `key` and a few settings. It serves
//
// - `image.generate` and `image.edit` from an OpenAI-compatible API (POST <address>/images/generations,
//   POST <address>/images/edits, a synchronous request) or from the stable-diffusion.cpp native
//   server (the job API under <address minus /v1>/sdcpp/v1: submit, poll, cancel), and
// - `text.generate` (with an image) from an OpenAI-compatible chat API (POST <address>/chat/completions,
//   streamed).
//
// Like the omarchy provider it is compiled into Fugue and shares its request and result code
// (engine/server_api.h), but it reads nothing from Omarchy's state: where it is and how it is
// reached is the instance. The key is sent as `Authorization: Bearer <key>` and goes nowhere
// else: no message, status, result or file this provider makes holds it (a server that echoes it
// in an error is scrubbed).
//
// Instance settings (all optional):
//   api            "auto" (default), "openai" or "sdcpp". Auto asks <root>/sdcpp/v1/capabilities:
//                  an answer means the native server, else the OpenAI dialect.
//   image_models   the served names of the image models (a list, or text separated by commas).
//   text_models    the same for text models. With neither, GET <address>/models lists them and
//                  a name that looks like an image model is one (see looksLikeImageModel); a
//                  setting replaces the guess.
//   timeout_s      how long a request may go silent before the job fails (default 300).
//   poll_ms        how often a native job is polled (default 500).

#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

#include "engine/providers.h"
#include "engine/server_api.h"

class QNetworkReply;

namespace fugue {

// Whether a model id from an OpenAI-style /models list is one that draws pictures (dall-e-3,
// gpt-image-1, flux, ...): the guess used when the instance names no image_models.
bool looksLikeImageModel(const QString &id);
// Whether a model id is not a chat model at all (embeddings, speech): never offered as text.
bool looksLikeOtherModel(const QString &id);

class HttpProvider : public Provider {
    Q_OBJECT

public:
    explicit HttpProvider(ProviderInstance instance, QObject *parent = nullptr);
    ~HttpProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

private:
    enum class Dialect { Unknown, OpenAI, Sdcpp };
    struct Found {
        Description description;
        Dialect dialect = Dialect::OpenAI;
    };
    struct Job;

    // The setting as a list: a JSON array, or text separated by commas or lines.
    QStringList listSetting(const char *name) const;
    int timeoutMs() const;
    QString host() const;
    // <address>/<path>, and the native server's <root>/<path> (the address without its /v1).
    QUrl openaiUrl(const QString &path) const;
    QUrl nativeUrl(const QString &path) const;
    QNetworkReply *send(const QUrl &url, const QByteArray &method, const QByteArray &body = {},
                        const QByteArray &contentType = QByteArrayLiteral("application/json"), bool withKey = true);
    // `text` with the key and its first lines' worth of noise removed: what a message may carry.
    QString clean(const QString &text) const;
    // The failure of an HTTP answer (code 0: no answer), in the spec's codes (8.8, 13.3).
    Error failure(QNetworkReply *reply, const QJsonObject &answer, bool text = false) const;
    Error badJson() const;

    void discover(std::function<void(const Result<Found> &)> done);
    void discoverOpenAI(std::function<void(const Result<Found> &)> done);
    void discoverNative(const QJsonObject &capabilities, std::function<void(const Result<Found> &)> done);

    void submitNative(const Found &found, const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    void submitOpenAI(const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    void submitImage(const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    void submitChat(const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    std::shared_ptr<Job> newJob(const JobRequest &request, const QJsonObject &model, QString *id);
    void finishImage(const QString &id);
    void saveImage(const QString &id, const QByteArray &bytes, const QString &format);
    void readChat(const QString &id, bool atEnd = false);
    void finishChat(const QString &id);
    void poll();
    void pollJob(const QString &id);
    void change(const QString &id, const JobStatus &status);
    void fail(const QString &id, const Error &error);
    void forgetOldJobs();

    ProviderInstance m_instance;
    QNetworkAccessManager m_network;
    QTimer m_poll;
    Dialect m_dialect = Dialect::Unknown; // what the last discovery found
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    int m_count = 0;
};

} // namespace fugue
