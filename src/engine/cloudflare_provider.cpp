#include "cloudflare_provider.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "engine/http_provider.h"
#include "engine/server_api.h"
#include "engine/vips.h"

namespace fugue {

using namespace serverapi;

namespace {

constexpr int kDefaultTimeoutS = 120;
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;
constexpr qint64 kAnswerCap = 32LL << 20;
// Cloudflare documents no seed range; a seed is kept inside a signed 32-bit integer, as other services require, and the one sent is recorded.
constexpr qint64 kSeedRange = 2147483647;
// The documentation asks for reference pictures under 512x512.
constexpr int kPictureSide = 511;
const char *const kCapped = "fugue-capped";

const QString kImageResource = QStringLiteral("fugue/image-model");
const QString kImageGenerate = QStringLiteral("image.generate");
const QString kImageEdit = QStringLiteral("image.edit");

int codeOf(QNetworkReply *reply) { return reply->property(kCapped).toBool() ? 0 : httpStatus(reply); }

QString oneLine(QString text) {
    text = text.simplified();
    return text.size() > 300 ? text.left(297) + QStringLiteral("...") : text;
}

QString where() { return QStringLiteral("Settings > Providers"); }

bool wholeUrl(const QUrl &url) {
    return (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) && !url.host().isEmpty();
}

bool loopbackHost(const QString &host) {
    QHostAddress address;
    return host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive) ||
           (address.setAddress(host) && address.isLoopback());
}

// The first of Cloudflare's `errors` ({code, message}), else a plain `error` or `message`.
QString saidBy(const QJsonObject &answer) {
    QStringList parts;
    for (const QJsonValue &item : answer.value("errors").toArray()) {
        const QString message = item.toObject().value("message").toString();
        if (!message.isEmpty())
            parts.append(message);
        if (parts.size() == 3)
            break;
    }
    if (!parts.isEmpty())
        return parts.join(QStringLiteral("; "));
    return serverMessage(answer);
}

int errorCodeOf(const QJsonObject &answer) { return answer.value("errors").toArray().first().toObject().value("code").toInt(); }

// A model id such as @cf/black-forest-labs/flux-1-schnell: the path of a URL, so nothing else is let in.
bool modelIdOk(const QString &id) {
    static const QRegularExpression ok(QStringLiteral("^@[a-z]{2}/[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)+$"));
    return ok.match(id).hasMatch() && !id.contains(QLatin1String(".."));
}

bool accountIdOk(const QString &id) {
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return ok.match(id).hasMatch();
}

} // namespace

QString cloudflareDefaultAddress() { return QStringLiteral("https://api.cloudflare.com"); }

QString cloudflareAddress(const ProviderInstance &instance) {
    QString address = instance.address.trimmed();
    while (address.endsWith(QLatin1Char('/')))
        address.chop(1);
    return address.isEmpty() ? cloudflareDefaultAddress() : address;
}

QList<CloudflareModel> cloudflareShippedModels() {
    QFile file(QStringLiteral(":/fugue/providers/cloudflare-models.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<CloudflareModel> models;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("models").toArray()) {
        const QJsonObject row = value.toObject();
        CloudflareModel model;
        model.id = row.value("id").toString();
        if (model.id.isEmpty())
            continue;
        model.title = row.value("title").toString(model.id);
        model.price = row.value("price").toString();
        model.multipart = row.value("format").toString() == QLatin1String("multipart");
        model.size = row.value("size").toBool();
        model.sizeMin = row.value("size_min").toInt();
        model.sizeMax = row.value("size_max").toInt();
        model.steps = row.value("steps").toBool();
        model.stepsMax = row.value("steps_max").toInt();
        model.guidance = row.value("guidance").toBool();
        model.seed = row.value("seed").toBool();
        model.images = row.value("images").toInt();
        model.defaults = row.value("defaults").toObject();
        models.append(model);
    }
    return models;
}

struct CloudflareProvider::Job {
    JobRequest request;
    QJsonObject handle;
    CloudflareModel spec;
    JobStatus status;
    JobResult result;
    qint64 endedAt = 0;
    QPointer<QNetworkReply> reply;
    QJsonObject sent; // the values sent, for the values used
};

CloudflareProvider::CloudflareProvider(ProviderInstance instance, QObject *parent) : Provider(parent), m_instance(std::move(instance)) {}

CloudflareProvider::~CloudflareProvider() = default;

int CloudflareProvider::timeoutMs() const {
    const QJsonValue value = m_instance.settings.value("timeout_s");
    const double seconds = value.isString() ? value.toString().toDouble() : value.toDouble();
    return int(std::clamp(seconds > 0 ? seconds : double(kDefaultTimeoutS), 0.05, 86400.0) * 1000);
}

QString CloudflareProvider::accountId() const {
    const QJsonValue value = m_instance.settings.value("account_id");
    return (value.isDouble() ? QString::number(value.toDouble(), 'f', 0) : value.toString()).trimmed();
}

QUrl CloudflareProvider::runUrl(const QString &model) const {
    return QUrl(cloudflareAddress(m_instance) + QStringLiteral("/client/v4/accounts/") + accountId() + QStringLiteral("/ai/run/") + model);
}

QString CloudflareProvider::clean(const QString &text) const {
    QString out = text;
    if (!m_instance.key.isEmpty())
        out.replace(m_instance.key, QStringLiteral("***"));
    return oneLine(out);
}

Error CloudflareProvider::failure(QNetworkReply *reply, const QJsonObject &answer) const {
    const QString who = m_instance.name;
    const int code = codeOf(reply);
    const QString said = clean(saidBy(answer));
    const QString after = said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said;
    if (reply->property(kCapped).toBool())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent more data than Fugue reads in one answer.").arg(who),
                         QStringLiteral("Check the address of %1: it should be Cloudflare's API, https://api.cloudflare.com.").arg(who));
    if (code == 0) {
        const QNetworkReply::NetworkError error = reply->error();
        if (error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError)
            return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 said nothing for %2 seconds.").arg(who).arg(timeoutMs() / 1000),
                             QStringLiteral("Raise timeout_s for %1 in %2 if the service is only slow.").arg(who, where()));
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 is not answering at %2 (%3).").arg(who, clean(reply->url().host()), clean(reply->errorString())),
                         QStringLiteral("Check the connection to the internet, then run the node again."));
    }
    if (code == 401 || code == 403)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 refused the token (HTTP %2)%3").arg(who).arg(code).arg(after),
                         QStringLiteral("Check the API token and the account id of %1 in %2; the token needs the Workers AI permission.").arg(who, where()));
    if (code >= 300 && code < 400)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 answered with a redirect, which Fugue does not follow.").arg(who),
                         QStringLiteral("Check the address of %1 in %2.").arg(who, where()));
    if (code == 404) {
        if (errorCodeOf(answer) == 5007 || said.contains(QLatin1String("no such model"), Qt::CaseInsensitive))
            return makeError(QStringLiteral("model_missing"), QStringLiteral("%1 has no such model: %2").arg(who, clean(reply->url().path().section(QStringLiteral("/ai/run/"), 1))),
                             QStringLiteral("Check the model id, such as @cf/black-forest-labs/flux-1-schnell; the models are listed at developers.cloudflare.com/workers-ai/models."));
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 does not know this account or model (HTTP 404)%2").arg(who, after),
                         QStringLiteral("Check the account id of %1 in %2.").arg(who, where()));
    }
    if (code == 413)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 refused the request as too large%2").arg(who, after),
                         QStringLiteral("Use smaller pictures: the models take reference pictures under 512x512."));
    if (code == 429 || (said.contains(QLatin1String("neurons"), Qt::CaseInsensitive) && code < 500)) {
        const QString wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
        bool seconds = false;
        wait.toInt(&seconds);
        if (said.contains(QLatin1String("neurons"), Qt::CaseInsensitive))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 has no neurons left to run this%2").arg(who, after),
                             QStringLiteral("The free allocation resets at 00:00 UTC; Workers Paid has no daily limit."));
        return makeError(QStringLiteral("node_failed"),
                         wait.isEmpty() || !seconds ? QStringLiteral("%1 is limiting the rate of this token (HTTP 429).").arg(who)
                                                    : QStringLiteral("%1 is limiting the rate of this token (HTTP 429); it asks to wait %2 seconds.").arg(who, wait),
                         QStringLiteral("Run the node again in a minute."));
    }
    if (code == 502 || code == 503 || code == 504)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 is not available right now (HTTP %2)%3").arg(who).arg(code).arg(after),
                         QStringLiteral("Try again in a moment; if it stays, check Cloudflare's status page."));
    if (said.contains(QLatin1String("nsfw"), Qt::CaseInsensitive) || said.contains(QLatin1String("flagged"), Qt::CaseInsensitive) ||
        said.contains(QLatin1String("policy"), Qt::CaseInsensitive))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job under its content policy%2").arg(who, after),
                         QStringLiteral("Change the prompt or the picture."));
    if (code >= 500 && said.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
        return jobError(QStringLiteral("%1 ran out of memory: %2").arg(who, said));
    if (code >= 500)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 had an error (HTTP %2)%3").arg(who).arg(code).arg(after),
                         QStringLiteral("Try again; if it keeps failing, the service may be down."));
    return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job (HTTP %2)%3").arg(who).arg(code).arg(after),
                     QStringLiteral("The service did not accept these settings; change the size, the pictures or the model."));
}

QList<CloudflareModel> CloudflareProvider::models() const {
    QList<CloudflareModel> all = cloudflareShippedModels();
    QStringList extra;
    const QJsonValue value = m_instance.settings.value(QStringLiteral("image_models"));
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            extra.append(item.toString().trimmed());
    } else {
        for (const QString &item : value.toString().split(QRegularExpression(QStringLiteral("[,\\n]"))))
            extra.append(item.trimmed());
    }
    for (const QString &id : std::as_const(extra)) {
        if (!modelIdOk(id) || std::any_of(all.begin(), all.end(), [&](const CloudflareModel &model) { return model.id == id; }))
            continue;
        CloudflareModel model; // the generic mapping: prompt, width, height, seed as JSON
        model.id = id;
        model.title = id;
        model.size = true;
        model.seed = true;
        all.append(model);
    }
    return all;
}

Result<Description> CloudflareProvider::description() const {
    const QUrl api = QUrl(cloudflareAddress(m_instance));
    if (!wholeUrl(api))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no usable address.").arg(m_instance.name),
                                             QStringLiteral("Remove the address of %1 in %2: Cloudflare needs none.").arg(m_instance.name, where())));
    // The token goes with every call: only to https, or to a loopback fake.
    if (!m_instance.key.isEmpty() && api.scheme() != QLatin1String("https") && !loopbackHost(api.host()))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"),
                                              QStringLiteral("%1 would send its token over plain http to %2.").arg(m_instance.name, api.host()),
                                              QStringLiteral("Use https:// for the address of %1 in %2.").arg(m_instance.name, where())));
    Description description;
    description.engine = QStringLiteral("cloudflare@") + api.host();
    const bool keyed = !m_instance.key.isEmpty();
    const bool account = accountIdOk(accountId());
    CapabilityOffer generate{kImageGenerate, {}};
    CapabilityOffer edit{kImageEdit, {}};
    for (const CloudflareModel &model : models()) {
        OfferedModel offered;
        offered.handle = QJsonObject{{"resource", kImageResource}, {"name", model.id}};
        offered.title = model.price.isEmpty() ? model.title : QStringLiteral("%1 (%2)").arg(model.title, model.price);
        offered.status = keyed && account ? ModelStatus::Ready : ModelStatus::Unavailable;
        if (!keyed) {
            offered.reason = QStringLiteral("%1 has no API token.").arg(m_instance.name);
            offered.hint = QStringLiteral("Paste your Cloudflare API token for %1 in %2.").arg(m_instance.name, where());
        } else if (!account) {
            offered.reason = accountId().isEmpty() ? QStringLiteral("%1 has no account id.").arg(m_instance.name)
                                                   : QStringLiteral("The account id of %1 is not an id.").arg(m_instance.name);
            offered.hint = QStringLiteral("Paste your Cloudflare account id for %1 in %2 (it is on the dash.cloudflare.com overview page).").arg(m_instance.name, where());
        }
        offered.properties = QJsonObject{{"edit", model.images > 0}, {"input", QJsonArray{"text"}}};
        offered.defaults = model.defaults;
        generate.models.append(offered);
        if (model.images > 0)
            edit.models.append(offered);
    }
    if (!generate.models.isEmpty())
        description.capabilities.append(generate);
    if (!edit.models.isEmpty())
        description.capabilities.append(edit);
    return Result<Description>(description);
}

void CloudflareProvider::describe(Reply<Description> reply) { later(std::move(reply), description()); }

// --- jobs ---

void CloudflareProvider::forgetOldJobs() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

void CloudflareProvider::change(const QString &id, const JobStatus &status) {
    const auto job = m_jobs.value(id);
    if (!job)
        return;
    const JobStatus &old = job->status;
    if (old.state == status.state && old.progress == status.progress && old.stage == status.stage && old.error.code == status.error.code)
        return;
    job->status = status;
    if (status.isTerminal())
        job->endedAt = QDateTime::currentMSecsSinceEpoch();
    emit statusChanged(id, status);
}

void CloudflareProvider::fail(const QString &id, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(id, status);
}

void CloudflareProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    // A reply never comes inside the call (provider.h): every one goes through the event loop.
    QPointer<CloudflareProvider> alive(this);
    reply = [alive, reply = std::move(reply)](const Result<QString> &outcome) {
        if (alive)
            alive->later(reply, outcome);
    };
    const QString who = m_instance.name;
    if (request.capability != kImageGenerate && request.capability != kImageEdit)
        return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"), QStringLiteral("The cloudflare provider does not offer %1.").arg(request.capability))));
    if (m_instance.key.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no API token.").arg(who),
                                               QStringLiteral("Paste your Cloudflare API token for %1 in %2.").arg(who, where()))));
    if (!accountIdOk(accountId()))
        return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"),
                                               accountId().isEmpty() ? QStringLiteral("%1 has no account id.").arg(who) : QStringLiteral("The account id of %1 is not an id.").arg(who),
                                               QStringLiteral("Paste your Cloudflare account id for %1 in %2 (it is on the dash.cloudflare.com overview page).").arg(who, where()))));
    const Result<Description> described = description();
    if (!described.ok())
        return reply(Result<QString>(described.errors()));
    Requirement requirement;
    requirement.capability = request.capability;
    requirement.model = request.model;
    requirement.nodePin = who;
    const auto chosen = selectProvider({Candidate{m_instance, described, std::nullopt}}, requirement);
    if (!chosen.ok())
        return reply(Result<QString>(chosen.errors()));
    const QJsonObject handle = chosen->model.handle;
    const QString modelId = handle.value("name").toString();
    CloudflareModel spec;
    for (const CloudflareModel &model : models())
        if (model.id == modelId)
            spec = model;

    QJsonArray images = request.inputs.value("images").toArray();
    if (request.inputs.contains("image"))
        images.prepend(request.inputs.value("image"));
    if (request.inputs.contains("mask"))
        return reply(Result<QString>(makeError(QStringLiteral("mask_unsupported"), QStringLiteral("%1 does not take a mask.").arg(spec.title),
                                               QStringLiteral("Cloudflare's image models edit with reference pictures, not a mask."))));
    if (!images.isEmpty() && spec.images == 0)
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 does not take a picture, only a prompt.").arg(spec.title),
                                               QStringLiteral("Pick a model that edits pictures, such as FLUX.2 [klein], or take the picture off the node."))));
    if (request.capability == kImageEdit && images.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("%1 needs a picture.").arg(spec.title))));

    // What is sent: the prompt, then what the node asked for in the fields the model takes.
    const QJsonObject &params = request.params;
    QJsonObject sent{{"prompt", params.value("prompt").toString()}};
    const int width = params.value("width").toInt();
    const int height = params.value("height").toInt();
    if (spec.size && width > 0 && height > 0) {
        const auto fit = [&](int value) { return spec.sizeMax > 0 ? std::clamp(value, spec.sizeMin, spec.sizeMax) : value; };
        sent.insert("width", fit(width));
        sent.insert("height", fit(height));
    }
    if (spec.steps && params.value("steps").toInt() > 0) {
        const int steps = params.value("steps").toInt();
        sent.insert("steps", spec.stepsMax > 0 ? std::min(steps, spec.stepsMax) : steps);
    }
    if (spec.guidance && params.value("guidance").isDouble())
        sent.insert("guidance", params.value("guidance"));
    if (spec.seed && params.value("seed").isDouble() && params.value("seed").toDouble() >= 0)
        sent.insert("seed", double(qint64(params.value("seed").toDouble()) % kSeedRange));

    struct Picture {
        QByteArray bytes;
        QString type;
        QString name;
    };
    QList<Picture> pictures;
    Errors errors;
    if (images.size() > spec.images && spec.images > 0)
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 takes at most %2 pictures, not %3.").arg(spec.title).arg(spec.images).arg(images.size()),
                                               QStringLiteral("Take some pictures off the node."))));
    for (int i = 0; i < images.size(); ++i) {
        const QString path = images.at(i).toObject().value("path").toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            errors.append(makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read the input image %1.").arg(path)));
            continue;
        }
        // Every picture is sent as a PNG within 511x511: the models take none of 512x512 or more. The aspect is kept, a smaller
        // picture is not enlarged and the EXIF rotation is applied.
        if (!initVips()) {
            errors.append(vipsUnavailable());
            break;
        }
        try {
            const QByteArray raw = file.readAll();
            vips::VImage small = vips::VImage::thumbnail_buffer(const_cast<char *>(raw.constData()), size_t(raw.size()), kPictureSide,
                                                                vips::VImage::option()->set("height", kPictureSide)->set("size", VIPS_SIZE_DOWN));
            void *memory = nullptr;
            size_t length = 0;
            small.write_to_buffer(".png", &memory, &length);
            Picture picture;
            picture.bytes = QByteArray(static_cast<const char *>(memory), qsizetype(length));
            g_free(memory);
            picture.type = QStringLiteral("image/png");
            picture.name = QStringLiteral("input_image_%1.png").arg(i);
            pictures.append(picture);
        } catch (const vips::VError &error) {
            errors.append(vipsError(error, QStringLiteral("media_unreadable")));
        }
    }
    if (hasErrors(errors))
        return reply(Result<QString>(errors));

    auto job = std::make_shared<Job>();
    job->request = request;
    job->handle = handle;
    job->spec = spec;
    job->sent = sent;
    const QString id = QStringLiteral("cloudflare/%1").arg(++m_count);
    job->status.state = QStringLiteral("running");
    job->status.stage = QStringLiteral("running");
    m_jobs.insert(id, job);

    QNetworkRequest http(runUrl(modelId));
    http.setTransferTimeout(timeoutMs());
    // No redirect is followed: an answer that is one is an error, and the token goes nowhere else.
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    http.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
    QNetworkReply *posted = nullptr;
    if (spec.multipart) {
        auto *form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (auto it = sent.begin(); it != sent.end(); ++it) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(it.key()));
            const double number = it.value().toDouble();
            part.setBody(it.value().isString() ? it.value().toString().toUtf8()
                         : number == std::floor(number) ? QByteArray::number(qint64(number)) : QByteArray::number(number, 'g', 10));
            form->append(part);
        }
        for (int i = 0; i < pictures.size(); ++i) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentTypeHeader, pictures.at(i).type);
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"input_image_%1\"; filename=\"%2\"").arg(i).arg(pictures.at(i).name));
            part.setBody(pictures.at(i).bytes);
            form->append(part);
        }
        posted = m_network.post(http, form);
        form->setParent(posted);
    } else {
        http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        posted = m_network.post(http, QJsonDocument(sent).toJson(QJsonDocument::Compact));
    }
    job->reply = posted;
    connect(posted, &QNetworkReply::downloadProgress, posted, [posted](qint64 got, qint64) {
        if (got > kAnswerCap && !posted->property(kCapped).toBool()) {
            posted->setProperty(kCapped, true);
            posted->abort();
        }
    });
    connect(posted, &QNetworkReply::finished, this, [this, id, posted] {
        posted->deleteLater();
        const auto found = m_jobs.value(id);
        if (!found || found->status.isTerminal())
            return;
        finish(id, posted);
    });
    // One deadline for the whole job: a transfer timeout restarts with each byte, so a trickle never trips it.
    QTimer::singleShot(timeoutMs() * 6, this, [this, id] {
        const auto found = m_jobs.value(id);
        if (!found || found->status.isTerminal())
            return;
        fail(id, makeError(QStringLiteral("node_timeout"), QStringLiteral("%1 did not finish the job in %2 seconds.").arg(m_instance.name).arg(timeoutMs() * 6 / 1000),
                           QStringLiteral("Raise timeout_s for %1 in %2 if the job is only slow.").arg(m_instance.name, where())));
        if (found->reply)
            found->reply->abort();
    });
    reply(Result<QString>(id));
}

void CloudflareProvider::finish(const QString &id, QNetworkReply *reply) {
    const int code = codeOf(reply);
    const QByteArray raw = reply->isOpen() ? reply->readAll() : QByteArray{};
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (code == 200 && type.startsWith(QLatin1String("image/"))) {
        if (formatOf(raw).isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(m_instance.name)));
        return saveImage(id, raw);
    }
    const QJsonObject answer = QJsonDocument::fromJson(raw).object();
    if (code != 200 || answer.value("success").toBool(true) == false)
        return fail(id, failure(reply, answer));
    // The envelope {result: {image}, success, errors, messages}; an answer without it is read too.
    const QJsonValue result = answer.value("result");
    QString encoded = result.isObject() ? result.toObject().value("image").toString() : result.toString();
    if (encoded.isEmpty())
        encoded = answer.value("image").toString();
    const qsizetype comma = encoded.indexOf(QLatin1Char(','));
    if (encoded.startsWith(QLatin1String("data:")) && comma > 0)
        encoded = encoded.mid(comma + 1);
    if (encoded.isEmpty())
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 finished without an image.").arg(m_instance.name),
                                  QStringLiteral("Run the node again, or change the prompt.")));
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (formatOf(bytes).isEmpty())
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(m_instance.name)));
    saveImage(id, bytes);
}

void CloudflareProvider::saveImage(const QString &id, const QByteArray &bytes) {
    const auto job = m_jobs.value(id);
    const QString format = formatOf(bytes);
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(id, makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();
    // The values used (8.1): the picture's own size, and the seed, steps and guidance that were sent.
    QJsonObject used{{"model", job->handle}, {"engine", QStringLiteral("cloudflare@") + job->handle.value("name").toString()}};
    for (const char *key : {"seed", "steps", "guidance"})
        if (job->sent.contains(QLatin1String(key)))
            used.insert(QLatin1String(key), job->sent.value(QLatin1String(key)));
    const QSize size = pngSize(bytes);
    if (size.isValid() && !size.isEmpty()) {
        used.insert("width", size.width());
        used.insert("height", size.height());
    } else {
        for (const char *key : {"width", "height"})
            if (job->sent.contains(QLatin1String(key)))
                used.insert(QLatin1String(key), job->sent.value(QLatin1String(key)));
    }
    job->result = JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used};
    JobStatus status;
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

void CloudflareProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void CloudflareProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    // The call is one request: aborting it is the cancel, and nothing more is waited for.
    JobStatus cancelled;
    cancelled.state = QStringLiteral("cancelled");
    change(job, cancelled);
    if (found->reply)
        found->reply->abort();
    later(std::move(reply), Result<QString>(found->status.state));
}

void CloudflareProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobResult>(makeError("internal", "No job " + job + ".")));
    if (found->status.state != QLatin1String("done"))
        return later(std::move(reply), Result<JobResult>(makeError("internal", QStringLiteral("Job %1 is %2, not done.").arg(job, found->status.state))));
    // Handed over once: the file is in the caller's outputDir.
    m_jobs.remove(job);
    later(std::move(reply), Result<JobResult>(found->result));
}

} // namespace fugue
