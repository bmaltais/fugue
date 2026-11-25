#include "http_provider.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSize>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "engine/paths.h"

namespace fugue {

using namespace serverapi;

namespace {

constexpr int kDefaultTimeoutS = 300;
constexpr int kDefaultPollMs = 500;
// How long a finished job is kept when nobody takes its result.
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;

const QString kImageResource = QStringLiteral("fugue/image-model");
const QString kTextResource = QStringLiteral("fugue/text-model");
const QString kImageGenerate = QStringLiteral("image.generate");
const QString kImageEdit = QStringLiteral("image.edit");
const QString kTextGenerate = QStringLiteral("text.generate");

// A response is read whole, so it is cut off past a size: a few hundred MB for a picture (or a native
// job, whose result holds one), far less for anything else.
constexpr qint64 kJsonCap = 64LL << 20;
constexpr qint64 kPictureCap = 512LL << 20;
const char *const kCapped = "fugue-capped";
constexpr int kMaxHops = 3;
constexpr int kMaxTransientPolls = 5;

// The status of a reply: 0 when it was cut off for its size, as if nothing had answered.
int codeOf(QNetworkReply *reply) { return reply->property(kCapped).toBool() ? 0 : httpStatus(reply); }

QString oneLine(QString text) {
    text = text.simplified();
    return text.size() > 300 ? text.left(297) + QStringLiteral("...") : text;
}

QByteArray mimeOf(const QJsonValue &image) {
    const QString format = image.toObject().value("format").toString();
    return format == QLatin1String("jpeg") ? "image/jpeg" : "image/png";
}

} // namespace

bool isLocalHost(const QString &host) {
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive) ||
        host.endsWith(QLatin1String(".local"), Qt::CaseInsensitive) || host.endsWith(QLatin1String(".internal"), Qt::CaseInsensitive))
        return true;
    QHostAddress address;
    if (!address.setAddress(host))
        return false;
    return address.isLoopback() || address.isLinkLocal() || address.isPrivateUse() || address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7) || address.isNull() ||
           address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
}

bool downloadUrlAllowed(const QUrl &url, const QString &instanceHost) {
    if ((url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) || url.host().isEmpty())
        return false;
    return !isLocalHost(url.host()) || isLocalHost(instanceHost);
}

bool looksLikeImageModel(const QString &id) {
    static const QRegularExpression image(
        QStringLiteral("dall-?e|gpt-image|imagen|flux|stable[-_ ]?diffusion|sdxl|\\bsd[-_]?\\d|sd-cpp|qwen-image|z-image|image-gen|midjourney|ideogram|recraft"),
        QRegularExpression::CaseInsensitiveOption);
    return image.match(id).hasMatch();
}

bool looksLikeOtherModel(const QString &id) {
    static const QRegularExpression other(QStringLiteral("embed|whisper|tts|moderation|rerank|transcribe|realtime|audio"),
                                          QRegularExpression::CaseInsensitiveOption);
    return other.match(id).hasMatch();
}

struct HttpProvider::Job {
    JobRequest request;
    QJsonObject model;
    JobStatus status;
    JobResult result;
    qint64 endedAt = 0;
    // An OpenAI request is one HTTP request, whose answer is the whole job: `reply` is the one in
    // flight (the request, then the download of a picture it named by URL).
    QPointer<QNetworkReply> reply;
    // A native job is polled on the server instead.
    QString serverId;
    qint64 deadline = 0;  // a native job gives up here (6 times timeout_s, 30 minutes by default)
    qint64 notBefore = 0; // the next poll waits until then, after an answer worth retrying
    int hops = 0;         // redirects followed by the picture download
    bool downloading = false; // the picture a service named by URL is being fetched
    bool asking = false;
    int failures = 0;
    bool chat = false;
    ChatStream reader;
};

HttpProvider::HttpProvider(ProviderInstance instance, QObject *parent)
    : Provider(parent), m_instance(std::move(instance)) {
    m_instance.address = m_instance.address.trimmed();
    while (m_instance.address.endsWith(QLatin1Char('/')))
        m_instance.address.chop(1);
    m_poll.setInterval(std::max(50, m_instance.settings.value("poll_ms").toInt(kDefaultPollMs)));
    connect(&m_poll, &QTimer::timeout, this, &HttpProvider::poll);
}

HttpProvider::~HttpProvider() = default;

QStringList HttpProvider::listSetting(const char *name) const {
    const QJsonValue value = m_instance.settings.value(QLatin1String(name));
    QStringList names;
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            names.append(item.toString().trimmed());
    } else {
        for (const QString &item : value.toString().split(QRegularExpression(QStringLiteral("[,\\n]"))))
            names.append(item.trimmed());
    }
    names.removeAll(QString());
    names.removeDuplicates();
    return names;
}

int HttpProvider::timeoutMs() const {
    const QJsonValue value = m_instance.settings.value("timeout_s");
    const double seconds = value.isString() ? value.toString().toDouble() : value.toDouble();
    return int(std::clamp(seconds > 0 ? seconds : double(kDefaultTimeoutS), 0.05, 86400.0) * 1000);
}

QString HttpProvider::host() const { return QUrl(m_instance.address).host(); }

QUrl HttpProvider::openaiUrl(const QString &path) const { return QUrl(m_instance.address + path); }
QUrl HttpProvider::nativeUrl(const QString &path) const { return QUrl(baseOf(m_instance.address) + path); }

QNetworkReply *HttpProvider::send(const QUrl &url, const QByteArray &method, const QByteArray &body,
                                  const QByteArray &contentType, bool withKey, qint64 cap) {
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs());
    // The key goes to the address it was given for and nowhere else: never to a redirect, never
    // to a file the service names by URL.
    // A redirect is never followed by Qt: a picture's is checked by finishImage, hop by hop.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (withKey && !m_instance.key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
    if (method != "GET")
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    QNetworkReply *reply = m_network.sendCustomRequest(request, method, body);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, cap](qint64 got, qint64) {
        if (got > cap && !reply->property(kCapped).toBool()) {
            reply->setProperty(kCapped, true);
            reply->abort();
        }
    });
    return reply;
}

QString HttpProvider::clean(const QString &text) const {
    QString out = text;
    if (!m_instance.key.isEmpty())
        out.replace(m_instance.key, QStringLiteral("***"));
    return oneLine(out);
}

Error HttpProvider::badJson() const {
    return makeError(QStringLiteral("node_failed"),
                     QStringLiteral("%1 answered something that is not JSON.").arg(m_instance.name),
                     QStringLiteral("Check the address: it should be the service's API root, such as https://api.example.com/v1."));
}

Error HttpProvider::failure(QNetworkReply *reply, const QJsonObject &answer, bool text) const {
    const QString who = m_instance.name;
    const int code = codeOf(reply);
    const QString said = clean(serverMessage(answer));
    const QString errorCode = answer.value("error").toObject().value("code").toString();
    const QString address = clean(m_instance.address);
    if (reply->property(kCapped).toBool())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent more data than Fugue reads in one answer.").arg(who),
                         QStringLiteral("Check the address of %1: it should be the API root, not a file or a page.").arg(who));
    if (code == 0) {
        const QNetworkReply::NetworkError error = reply->error();
        if (error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError)
            return makeError(QStringLiteral("provider_unreachable"),
                             QStringLiteral("%1 said nothing for %2 seconds.").arg(who).arg(timeoutMs() / 1000),
                             QStringLiteral("Raise timeout_s for this provider in %1 if the service is only slow.").arg(providersFilePath()));
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 is not answering at %2 (%3).").arg(who, address, clean(reply->errorString())),
                         QStringLiteral("Check the address of %1 in %2 and that the service is running.").arg(who, providersFilePath()));
    }
    if (code == 401 || code == 403)
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 refused the key (HTTP %2).").arg(who).arg(code),
                         m_instance.key.isEmpty()
                             ? QStringLiteral("This service needs a key: set the key of %1 in %2.").arg(who, providersFilePath())
                             : QStringLiteral("Check the key of %1 in %2.").arg(who, providersFilePath()));
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
        const QString where = clean(QString::fromLatin1(reply->rawHeader("Location")));
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 answered with a redirect%2.").arg(who, where.isEmpty() ? QString() : QStringLiteral(" to ") + where),
                         where.isEmpty() ? QStringLiteral("Check the address of %1 in %2 (https instead of http?).").arg(who, providersFilePath())
                                         : QStringLiteral("Use that address for %1 in %2.").arg(who, providersFilePath()));
    }
    if (code == 404) {
        const bool model = errorCode == QLatin1String("model_not_found") || said.contains(QLatin1String("model"), Qt::CaseInsensitive);
        if (model)
            return makeError(QStringLiteral("model_missing"), QStringLiteral("%1 has no such model: %2").arg(who, said),
                             QStringLiteral("Pick a model the service lists, or fix the model name."));
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 has nothing at %2 (HTTP 404).").arg(who, clean(reply->url().toString())),
                         QStringLiteral("Check the address of %1: it is the API root, usually ending in /v1.").arg(who));
    }
    if (code == 429) {
        const QString wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
        if (errorCode == QLatin1String("insufficient_quota"))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 has no quota left for this key: %2").arg(who, said),
                             QStringLiteral("Check the plan and billing of the account behind the key."));
        bool seconds = false;
        wait.toInt(&seconds);
        return makeError(QStringLiteral("node_failed"),
                         wait.isEmpty() ? QStringLiteral("%1 is limiting the rate of this key (HTTP 429).").arg(who)
                                        : QStringLiteral("%1 is limiting the rate of this key (HTTP 429); it asks to wait %2.")
                                              .arg(who, seconds ? QStringLiteral("%1 seconds").arg(wait) : clean(wait)),
                         wait.isEmpty() ? QStringLiteral("Run the node again in a minute.")
                                        : QStringLiteral("Run the node again after %1.").arg(seconds ? QStringLiteral("%1 seconds").arg(wait) : clean(wait)));
    }
    if (code == 503 && said.contains(QLatin1String("loading"), Qt::CaseInsensitive))
        return makeError(QStringLiteral("model_not_running"), QStringLiteral("%1 is loading its model.").arg(who),
                         QStringLiteral("Wait for it to finish loading."));
    if (code == 502 || code == 503 || code == 504)
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 is not available right now (HTTP %2).").arg(who).arg(code),
                         QStringLiteral("Try again in a moment; if it stays, check the service's status."));
    if (code >= 500 && said.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
        return jobError(QStringLiteral("%1 ran out of memory: %2").arg(who, said), text);
    if (code >= 500)
        return makeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1 had an error (HTTP %2)%3").arg(who).arg(code).arg(said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Try again; if it keeps failing, the service may be down."));
    Error error = jobError(QStringLiteral("%1 refused the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP %1").arg(code) : said), text);
    if (error.hint.isEmpty() && code == 400)
        error.hint = QStringLiteral("The service did not accept these settings; change the size or the model, or check what it supports.");
    return error;
}

// --- discovery ---

void HttpProvider::discover(std::function<void(const Result<Found> &)> done) {
    const QUrl url(m_instance.address);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
        return QTimer::singleShot(0, this, [this, done] {
            done(Result<Found>(makeError(QStringLiteral("provider_unreachable"),
                                         QStringLiteral("%1 has no usable address.").arg(m_instance.name),
                                         QStringLiteral("Set its address in %1 to the service's API root, such as https://api.example.com/v1.")
                                             .arg(providersFilePath()))));
        });
    const QString api = m_instance.settings.value("api").toString(QStringLiteral("auto"));
    if (api != QLatin1String("auto") && api != QLatin1String("openai") && api != QLatin1String("sdcpp")) {
        Error error = makeError(QStringLiteral("invalid_document"),
                                QStringLiteral("%1: api must be auto, openai or sdcpp (got \"%2\").").arg(m_instance.name, api),
                                QStringLiteral("Fix it in %1.").arg(providersFilePath()));
        error.valid = QJsonArray{"auto", "openai", "sdcpp"};
        return QTimer::singleShot(0, this, [done, error] { done(Result<Found>(error)); });
    }
    if (api == QLatin1String("openai"))
        return discoverOpenAI(done);
    QNetworkReply *reply = send(nativeUrl(QStringLiteral("/sdcpp/v1/capabilities")), "GET");
    connect(reply, &QNetworkReply::finished, this, [this, reply, api, done] {
        reply->deleteLater();
        const int code = codeOf(reply);
        const QJsonObject body = jsonBody(reply);
        if (code == 200 && !body.isEmpty())
            return discoverNative(body, done);
        // A server that is not there, or that refuses the key, is the same for either dialect.
        if (api == QLatin1String("sdcpp") || code == 0 || code == 401 || code == 403 || code >= 500)
            return done(Result<Found>(failure(reply, body)));
        discoverOpenAI(done);
    });
}

void HttpProvider::discoverNative(const QJsonObject &capabilities, std::function<void(const Result<Found> &)> done) {
    Found found;
    found.dialect = Dialect::Sdcpp;
    m_dialect = Dialect::Sdcpp;
    found.description.engine = QStringLiteral("sdcpp@") + host();
    const QJsonObject loaded = capabilities.value("model").toObject();
    QString name = loaded.value("name").toString();
    if (name.isEmpty())
        name = loaded.value("stem").toString();
    if (name.isEmpty())
        name = QStringLiteral("sd-cpp-local");
    const QJsonArray modes = capabilities.value("supported_modes").toArray();
    if (modes.isEmpty() || modes.contains(QStringLiteral("img_gen"))) {
        OfferedModel model;
        model.handle = QJsonObject{{"resource", kImageResource}, {"name", name}};
        model.title = name;
        model.status = ModelStatus::Ready;
        const bool edits = featureOf(capabilities, "ref_images");
        model.properties = QJsonObject{{"edit", edits}, {"input", QJsonArray{"text"}}};
        if (maskImageOf(capabilities))
            model.properties.insert("inputs", QJsonArray{"mask"});
        model.defaults = defaultsOf(capabilities);
        model.limits = limitsOf(capabilities);
        found.description.capabilities.append(CapabilityOffer{kImageGenerate, {model}});
        if (edits)
            found.description.capabilities.append(CapabilityOffer{kImageEdit, {model}});
    }
    done(Result<Found>(found));
}

void HttpProvider::discoverOpenAI(std::function<void(const Result<Found> &)> done) {
    QNetworkReply *reply = send(openaiUrl(QStringLiteral("/models")), "GET");
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        const int code = codeOf(reply);
        const QJsonObject body = jsonBody(reply);
        QStringList listed;
        for (const QJsonValue &entry : body.value("data").toArray())
            if (!entry.toObject().value("id").toString().isEmpty())
                listed.append(entry.toObject().value("id").toString());
        QStringList images = listSetting("image_models");
        QStringList texts = listSetting("text_models");
        // A service without a model list is usable when the instance names its models; one that
        // is not there, refuses the key or is down is not.
        const bool answered = code == 200 || (code > 0 && code < 500 && code != 401 && code != 403);
        if (!answered || (code != 200 && images.isEmpty() && texts.isEmpty()))
            return done(Result<Found>(failure(reply, body)));
        if (code == 200 && listed.isEmpty() && images.isEmpty() && texts.isEmpty())
            return done(Result<Found>(badJson()));
        if (images.isEmpty() && !m_instance.settings.contains("image_models"))
            for (const QString &id : std::as_const(listed))
                if (looksLikeImageModel(id))
                    images.append(id);
        if (texts.isEmpty() && !m_instance.settings.contains("text_models"))
            for (const QString &id : std::as_const(listed))
                if (!looksLikeImageModel(id) && !looksLikeOtherModel(id))
                    texts.append(id);

        Found found;
        found.dialect = Dialect::OpenAI;
        m_dialect = Dialect::OpenAI;
        found.description.engine = QStringLiteral("openai@") + host();
        const auto offered = [](const QStringList &names, const QString &resource, const QJsonObject &properties) {
            QList<OfferedModel> models;
            for (const QString &name : names) {
                OfferedModel model;
                model.handle = QJsonObject{{"resource", resource}, {"name", name}};
                model.title = name;
                model.status = ModelStatus::Ready;
                model.properties = properties;
                models.append(model);
            }
            return models;
        };
        // Every image model is offered for edits too: the service says no, in its words, when one cannot.
        const QList<OfferedModel> pictures = offered(images, kImageResource, QJsonObject{{"edit", true}, {"input", QJsonArray{"text"}}});
        if (!pictures.isEmpty()) {
            found.description.capabilities.append(CapabilityOffer{kImageGenerate, pictures});
            found.description.capabilities.append(CapabilityOffer{kImageEdit, pictures});
        }
        const QList<OfferedModel> chats = offered(texts, kTextResource, {});
        if (!chats.isEmpty())
            found.description.capabilities.append(CapabilityOffer{kTextGenerate, chats});
        done(Result<Found>(found));
    });
}

void HttpProvider::describe(Reply<Description> reply) {
    discover([reply = std::move(reply)](const Result<Found> &found) {
        reply(found.ok() ? Result<Description>(found->description) : Result<Description>(found.errors()));
    });
}

// --- jobs ---

void HttpProvider::forgetOldJobs() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

std::shared_ptr<HttpProvider::Job> HttpProvider::newJob(const JobRequest &request, const QJsonObject &model, QString *id) {
    auto job = std::make_shared<Job>();
    job->request = request;
    job->model = model;
    job->status.state = QStringLiteral("running");
    job->status.stage = QStringLiteral("waiting");
    *id = QStringLiteral("http/%1").arg(++m_count);
    m_jobs.insert(*id, job);
    return job;
}

void HttpProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    // A reply never comes inside the call (provider.h): every one goes through the event loop.
    QPointer<HttpProvider> alive(this);
    reply = [alive, reply = std::move(reply)](const Result<QString> &outcome) {
        if (alive)
            alive->later(reply, outcome);
    };
    if (request.capability != kImageGenerate && request.capability != kImageEdit && request.capability != kTextGenerate)
        return later(std::move(reply), Result<QString>(makeError(QStringLiteral("capability_unavailable"),
                                                                 QStringLiteral("The http provider does not offer %1.").arg(request.capability))));
    // A model that is named goes straight to an OpenAI service. Otherwise (no model chosen, or a
    // native server, whose capabilities say whether it takes a mask) the instance is asked first.
    const QString model = request.model.value("name").toString();
    if (m_dialect == Dialect::OpenAI && !model.isEmpty())
        return submitOpenAI(request, request.model, std::move(reply));
    QPointer<HttpProvider> self(this);
    discover([self, request, reply = std::move(reply)](const Result<Found> &found) {
        if (!self)
            return;
        if (!found.ok())
            return reply(Result<QString>(found.errors()));
        Requirement requirement;
        requirement.capability = request.capability;
        requirement.model = request.model;
        requirement.nodePin = self->m_instance.name;
        const auto chosen = selectProvider({Candidate{self->m_instance, Result<Description>(found->description), std::nullopt}}, requirement);
        if (!chosen.ok())
            return reply(Result<QString>(chosen.errors()));
        const QJsonObject handle = chosen->model.handle;
        if (found->dialect == Dialect::Sdcpp) {
            // A mask goes only to a server that lists mask_image, or the whole picture would be redrawn (14.4).
            if (request.inputs.contains("mask") &&
                !chosen->model.properties.value("inputs").toArray().contains(QStringLiteral("mask")))
                return reply(Result<QString>(makeError(
                    QStringLiteral("mask_unsupported"),
                    QStringLiteral("%1 does not take a mask: its server does not list mask_image.").arg(self->m_instance.name),
                    QStringLiteral("Use a model whose server lists mask_image, or redraw the part staged with the Fugue helper."))));
            return self->submitNative(*found, request, handle, reply);
        }
        self->submitOpenAI(request, handle, reply);
    });
}

void HttpProvider::submitOpenAI(const JobRequest &request, const QJsonObject &model, Reply<QString> reply) {
    if (request.capability == kTextGenerate)
        return submitChat(request, model, std::move(reply));
    // An OpenAI edit's mask marks the part to redraw by transparency, not by white: not the mask of 14.4.
    if (request.inputs.contains("mask"))
        return reply(Result<QString>(makeError(
            QStringLiteral("mask_unsupported"),
            QStringLiteral("%1 does not take a mask: an OpenAI-compatible service draws masks differently.").arg(m_instance.name),
            QStringLiteral("Use a provider whose server lists mask_image, or redraw the part staged with the Fugue helper."))));
    submitImage(request, model, std::move(reply));
}

// The native server: POST /sdcpp/v1/img_gen, then poll the job (api.md "sdcpp API").
void HttpProvider::submitNative(const Found &, const JobRequest &request, const QJsonObject &model, Reply<QString> reply) {
    Errors errors;
    const QJsonObject body = imgGenBody(request, &errors);
    if (hasErrors(errors))
        return reply(Result<QString>(errors));
    QPointer<HttpProvider> self(this);
    QNetworkReply *posted = send(nativeUrl(QStringLiteral("/sdcpp/v1/img_gen")), "POST", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(posted, &QNetworkReply::finished, this, [self, posted, request, model, reply] {
        posted->deleteLater();
        if (!self)
            return;
        const int code = codeOf(posted);
        const QJsonObject answer = jsonBody(posted);
        const QString serverId = answer.value("id").toString();
        if ((code == 202 || code == 200) && !serverId.isEmpty()) {
            QString id;
            const auto job = self->newJob(request, model, &id);
            job->serverId = serverId;
            job->deadline = QDateTime::currentMSecsSinceEpoch() + qint64(self->timeoutMs()) * 6;
            job->status.state = QStringLiteral("queued");
            job->status.stage.clear();
            if (!self->m_poll.isActive())
                self->m_poll.start();
            return reply(Result<QString>(id));
        }
        reply(Result<QString>(code == 200 || code == 202
                                  ? jobError(QStringLiteral("%1 took the job but gave no id.").arg(self->m_instance.name))
                                  : self->failure(posted, answer)));
    });
}

// An OpenAI image request: generations (JSON) without pictures, edits (multipart) with them. The
// answer is the finished picture.
void HttpProvider::submitImage(const JobRequest &request, const QJsonObject &model, Reply<QString> reply) {
    const QJsonObject &params = request.params;
    const QString name = model.value("name").toString();
    const QString prompt = params.value("prompt").toString();
    const bool edit = request.capability == kImageEdit || request.inputs.contains("image");
    QString size;
    if (params.value("width").toInt() > 0 && params.value("height").toInt() > 0)
        size = QStringLiteral("%1x%2").arg(params.value("width").toInt()).arg(params.value("height").toInt());

    QByteArray body;
    QByteArray contentType = "application/json";
    QUrl url = openaiUrl(QStringLiteral("/images/generations"));
    if (!edit) {
        QJsonObject json{{"model", name}, {"prompt", prompt}, {"n", 1}};
        if (!size.isEmpty())
            json.insert("size", size);
        body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    } else {
        url = openaiUrl(QStringLiteral("/images/edits"));
        QList<FormPart> parts{{"model", {}, {}, name.toUtf8()}, {"prompt", {}, {}, prompt.toUtf8()}, {"n", {}, {}, "1"}};
        if (!size.isEmpty())
            parts.append({"size", {}, {}, size.toUtf8()});
        QJsonArray images = request.inputs.value("images").toArray();
        if (request.inputs.contains("image"))
            images.prepend(request.inputs.value("image"));
        Errors errors;
        int index = 0;
        for (const QJsonValue &image : std::as_const(images)) {
            const QString path = image.toObject().value("path").toString();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                errors.append(makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read the input image %1.").arg(path)));
                break;
            }
            const QByteArray type = mimeOf(image);
            parts.append({"image[]", "image" + QByteArray::number(++index) + (type == "image/jpeg" ? ".jpg" : ".png"), type, file.readAll()});
        }
        if (hasErrors(errors))
            return reply(Result<QString>(errors));
        if (images.isEmpty())
            return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("An edit needs at least one picture."))));
        QByteArray boundary;
        body = multipartBody(parts, &boundary);
        contentType = "multipart/form-data; boundary=" + boundary;
    }
    QString id;
    const auto job = newJob(request, model, &id);
    job->reply = send(url, "POST", body, contentType, true, kPictureCap);
    connect(job->reply, &QNetworkReply::finished, this, [this, id] { finishImage(id); });
    reply(Result<QString>(id));
}

void HttpProvider::fail(const QString &id, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(id, status);
}

void HttpProvider::finishImage(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || !job->reply)
        return;
    QNetworkReply *reply = job->reply;
    reply->deleteLater();
    if (job->status.isTerminal())
        return; // cancelled
    const int code = codeOf(reply);
    const QString who = m_instance.name;
    // The second step: the picture a service named by URL, from its own file server (no key).
    if (job->downloading) {
        if (code >= 300 && code < 400 && code != 304) {
            const QUrl next = reply->url().resolved(QUrl(QString::fromLatin1(reply->rawHeader("Location"))));
            if (++job->hops > kMaxHops || !downloadUrlAllowed(next, host()))
                return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent the picture link on to a place Fugue does not fetch.").arg(who),
                                          QStringLiteral("Only http and https links, and no local address of another machine, are fetched.")));
            job->reply = send(next, "GET", {}, {}, false, kPictureCap);
            connect(job->reply, &QNetworkReply::finished, this, [this, id] { finishImage(id); });
            return;
        }
        if (code >= 400)
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1's picture link was refused (HTTP %2); it may have expired.").arg(who).arg(code),
                                      QStringLiteral("Run the node again.")));
        if (code == 0 || code >= 300)
            return fail(id, failure(reply, {}));
        const QByteArray raw = reply->readAll();
        const QString format = formatOf(raw);
        if (format.isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(who)));
        return saveImage(id, raw, format);
    }
    if (code == 0 || code >= 300)
        return fail(id, failure(reply, jsonBody(reply)));
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (!document.isObject())
        return fail(id, badJson());
    const QJsonObject first = firstObject(document.object().value("data").toArray());
    const QString encoded = first.value("b64_json").toString();
    if (!encoded.isEmpty()) {
        const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
        const QString format = formatOf(bytes);
        if (format.isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(who)));
        return saveImage(id, bytes, format);
    }
    const QString url = first.value("url").toString();
    if (!url.isEmpty()) {
        // The link may be relative to the address; it is only fetched when it is a place a remote service may name.
        const QUrl target = QUrl(m_instance.address + QLatin1Char('/')).resolved(QUrl(url));
        if (!downloadUrlAllowed(target, host()))
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 named a picture at %2, which Fugue does not fetch.").arg(who, clean(url)),
                                      QStringLiteral("Only http and https links, and no local address of another machine, are fetched.")));
        job->downloading = true;
        job->reply = send(target, "GET", {}, {}, false, kPictureCap);
        connect(job->reply, &QNetworkReply::finished, this, [this, id] { finishImage(id); });
        return;
    }
    fail(id, jobError(QStringLiteral("%1 finished without an image.").arg(who)));
}

void HttpProvider::saveImage(const QString &id, const QByteArray &bytes, const QString &format) {
    const auto job = m_jobs.value(id);
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    if (bytes.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(id, bytes.isEmpty() ? jobError(QStringLiteral("%1 finished without an image.").arg(m_instance.name))
                                        : makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();
    // The values used (8.1): the picture's own size when it can be read, else the one asked for.
    // The seed, steps and guidance are not the service's to take, so none is reported.
    QJsonObject used{{"model", job->model}, {"engine", QStringLiteral("openai@") + host()}};
    const QSize size = pngSize(bytes);
    const QJsonObject &params = job->request.params;
    if (size.isValid() && !size.isEmpty()) {
        used.insert("width", size.width());
        used.insert("height", size.height());
    } else {
        for (const char *key : {"width", "height"})
            if (params.contains(QLatin1String(key)))
                used.insert(QLatin1String(key), params.value(QLatin1String(key)));
    }
    job->result = JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used};
    JobStatus status;
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

// --- text ---

void HttpProvider::submitChat(const JobRequest &request, const QJsonObject &model, Reply<QString> reply) {
    Errors errors;
    const QJsonObject body = chatBody(model.value("name").toString(), request, &errors);
    if (hasErrors(errors))
        return reply(Result<QString>(errors));
    QString id;
    const auto job = newJob(request, model, &id);
    job->chat = true;
    job->status.stage = QStringLiteral("reading");
    job->reply = send(openaiUrl(QStringLiteral("/chat/completions")), "POST", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(job->reply, &QNetworkReply::readyRead, this, [this, id] { readChat(id); });
    connect(job->reply, &QNetworkReply::finished, this, [this, id] { finishChat(id); });
    reply(Result<QString>(id));
}

void HttpProvider::readChat(const QString &id, bool atEnd) {
    const auto job = m_jobs.value(id);
    if (!job || !job->reply || job->status.isTerminal())
        return;
    const QByteArray type = job->reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    if (!type.contains("event-stream") || codeOf(job->reply) >= 400)
        return; // a whole JSON answer, read when it is over
    const bool fine = job->reader.feed(job->reply->readAll(), atEnd);
    JobStatus status = job->status;
    if (!job->reader.stage.isEmpty())
        status.stage = job->reader.stage;
    if (!fine) {
        status.state = QStringLiteral("error");
        status.error = jobError(QStringLiteral("%1 failed the job: %2").arg(m_instance.name, clean(job->reader.error)), true);
    }
    const int longest = job->request.params.value("max_tokens").toInt(0);
    if (longest > 0)
        status.progress = std::min(0.99, std::floor(100.0 * job->reader.chunks / longest) / 100.0);
    change(id, status);
    if (status.state == QLatin1String("error"))
        job->reply->abort();
}

void HttpProvider::finishChat(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || !job->reply)
        return;
    QNetworkReply *reply = job->reply;
    reply->deleteLater();
    if (job->status.isTerminal())
        return; // cancelled, or failed by an error event
    readChat(id, true);
    if (job->status.isTerminal())
        return;
    const int code = codeOf(reply);
    const bool streamed = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().contains("event-stream");
    if (code == 0 || code >= 300)
        return fail(id, failure(reply, QJsonDocument::fromJson(reply->readAll()).object(), true));
    // Headers came, then the stream broke off: half an answer is not an answer.
    if (reply->error() != QNetworkReply::NoError || (streamed && !job->reader.finished))
        return fail(id, makeError(QStringLiteral("provider_unreachable"),
                                  QStringLiteral("%1 stopped in the middle of its answer.").arg(m_instance.name),
                                  QStringLiteral("Run the node again; if it happens again, check the service.")));
    if (!streamed) {
        // A server that ignored `stream` answers in one JSON object.
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject())
            return fail(id, badJson());
        job->reader.text = firstObject(document.object().value("choices").toArray()).value("message").toObject().value("content").toString();
    }
    QJsonObject used{{"model", job->model}, {"engine", QStringLiteral("openai@") + host()}};
    if (job->request.params.contains("seed"))
        used.insert("seed", job->request.params.value("seed"));
    job->result = JobResult{QJsonObject{{"text", job->reader.text}}, used};
    JobStatus status;
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

// --- native job polling ---

void HttpProvider::poll() {
    bool active = false;
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it.value()->status.isTerminal() || it.value()->serverId.isEmpty() || it.value()->reply)
            continue; // an OpenAI job is followed by its request
        active = true;
        if (it.value()->deadline && QDateTime::currentMSecsSinceEpoch() > it.value()->deadline) {
            // The job is asked to stop (best effort) and given up on: a poll that never ends would hold the lane.
            connect(send(nativeUrl(QStringLiteral("/sdcpp/v1/jobs/%1/cancel").arg(it.value()->serverId)), "POST"), &QNetworkReply::finished,
                    this, [this] { sender()->deleteLater(); });
            fail(it.key(), makeError(QStringLiteral("node_timeout"), QStringLiteral("%1 did not finish the job in %2 seconds.").arg(m_instance.name).arg(timeoutMs() * 6 / 1000),
                                     QStringLiteral("Raise timeout_s for %1 in %2 if the job is only slow.").arg(m_instance.name, providersFilePath())));
            continue;
        }
        if (!it.value()->asking && QDateTime::currentMSecsSinceEpoch() >= it.value()->notBefore)
            pollJob(it.key());
    }
    if (!active)
        m_poll.stop();
}

void HttpProvider::pollJob(const QString &id) {
    const auto job = m_jobs.value(id);
    job->asking = true;
    QNetworkReply *reply = send(nativeUrl(QStringLiteral("/sdcpp/v1/jobs/%1").arg(job->serverId)), "GET", {}, {}, true, kPictureCap);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, job] {
        reply->deleteLater();
        job->asking = false;
        if (job->status.isTerminal())
            return;
        const int code = codeOf(reply);
        // No answer, a rate limit or a gateway hiccup: try again a few times, waiting longer each time.
        if (code == 0 || code == 429 || code == 502 || code == 503 || code == 504) {
            if (++job->failures >= kMaxTransientPolls)
                fail(id, code == 0 || code >= 500
                             ? makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 stopped answering during the job.").arg(m_instance.name),
                                         QStringLiteral("Check that the service is running, then run the node again."))
                             : failure(reply, jsonBody(reply)));
            else
                job->notBefore = QDateTime::currentMSecsSinceEpoch() + (200LL << job->failures);
            return;
        }
        job->failures = 0;
        if (code == 401 || code == 403)
            return fail(id, failure(reply, jsonBody(reply)));
        if (code != 200)
            // 404 or 410: the server no longer has the job, so its result is lost.
            return fail(id, makeError(QStringLiteral("node_failed"),
                                      QStringLiteral("%1 no longer has the job (HTTP %2).").arg(m_instance.name).arg(code),
                                      QStringLiteral("Run the node again.")));
        const QJsonObject answer = jsonBody(reply);
        if (answer.value("status").toString() == QLatin1String("completed")) {
            // The result is inline in the completed job and expires with it, so it is written now (8.6).
            const Result<JobResult> made =
                imageJobResult(answer, job->request, job->model, QStringLiteral("sdcpp@") + host(), m_instance.name);
            if (!made)
                return fail(id, made.error());
            job->result = made.value();
            JobStatus done;
            done.state = QStringLiteral("done");
            done.progress = 1;
            return change(id, done);
        }
        JobStatus status = statusOf(answer);
        status.error.message = clean(status.error.message);
        change(id, status);
    });
}

void HttpProvider::change(const QString &id, const JobStatus &status) {
    const auto job = m_jobs.value(id);
    if (!job)
        return;
    const JobStatus &old = job->status;
    if (old.state == status.state && old.progress == status.progress && old.stage == status.stage &&
        old.preview == status.preview && old.error.code == status.error.code)
        return;
    job->status = status;
    if (status.isTerminal())
        job->endedAt = QDateTime::currentMSecsSinceEpoch();
    emit statusChanged(id, status);
}

void HttpProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void HttpProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    if (found->reply) {
        // The request is the job: closing it ends it (a chat's stream stops the server's generation).
        JobStatus cancelled;
        cancelled.state = QStringLiteral("cancelled");
        change(job, cancelled);
        found->reply->abort();
        return later(std::move(reply), Result<QString>(found->status.state));
    }
    // A native job: cooperative (api.md "POST /sdcpp/v1/jobs/{id}/cancel"), so the poller keeps
    // following it to its terminal state (9.6).
    QNetworkReply *posted = send(nativeUrl(QStringLiteral("/sdcpp/v1/jobs/%1/cancel").arg(found->serverId)), "POST");
    connect(posted, &QNetworkReply::finished, this, [this, posted, job, found, reply] {
        posted->deleteLater();
        const int code = codeOf(posted);
        if (code == 0)
            return reply(Result<QString>(failure(posted, {})));
        const QJsonObject answer = jsonBody(posted);
        if (code == 200 && answer.value("status").toString() == QLatin1String("cancelled") && !found->status.isTerminal())
        {
            JobStatus cancelled = statusOf(answer);
            cancelled.error.message = clean(cancelled.error.message);
            change(job, cancelled);
        }
        reply(Result<QString>(found->status.state));
    });
}

void HttpProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobResult>(makeError("internal", "No job " + job + ".")));
    if (found->status.state != QLatin1String("done"))
        return later(std::move(reply), Result<JobResult>(makeError("internal", QStringLiteral("Job %1 is %2, not done.").arg(job, found->status.state))));
    // Handed over once: the files are in the caller's outputDir.
    m_jobs.remove(job);
    later(std::move(reply), Result<JobResult>(found->result));
}

} // namespace fugue
