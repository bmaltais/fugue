#include "replicate_provider.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "engine/http_provider.h"
#include "engine/paths.h"
#include "engine/server_api.h"

namespace fugue {

using namespace serverapi;

namespace {

using Header = QPair<QByteArray, QByteArray>;
constexpr int kDefaultTimeoutS = 300;
constexpr int kDefaultPollMs = 500;
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;
constexpr qint64 kJsonCap = 64LL << 20;
constexpr qint64 kPictureCap = 512LL << 20;
// Replicate's guide: a data URI is for files under 1 MB; a larger one goes through the files API.
constexpr qint64 kDataUriMax = 1LL << 20;
constexpr int kMaxHops = 3;
constexpr int kMaxTransientPolls = 5;
const char *const kCapped = "fugue-capped";

const QString kImageResource = QStringLiteral("fugue/image-model");
const QString kImageGenerate = QStringLiteral("image.generate");
const QString kImageEdit = QStringLiteral("image.edit");

int codeOf(QNetworkReply *reply) { return reply->property(kCapped).toBool() ? 0 : httpStatus(reply); }

QString oneLine(QString text) {
    text = text.simplified();
    return text.size() > 300 ? text.left(297) + QStringLiteral("...") : text;
}

// The JSON a reply holds; nothing from one that was cut off (reading it would warn).
QJsonObject answerOf(QNetworkReply *reply) { return reply->isOpen() ? jsonBody(reply) : QJsonObject{}; }

QString where() { return QStringLiteral("Settings > Providers"); }

bool canEdit(const ReplicateModel &model) { return !model.imageField.isEmpty() && model.kind != ReplicateModel::Kind::Inpaint; }

QString withSaid(const QString &said) { return said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said; }

// What a service says about a failure: `detail` (a text), `invalid_fields` ({field, description}), `error`.
QString saidBy(const QJsonObject &answer) {
    QStringList parts;
    for (const QJsonValue &item : answer.value("invalid_fields").toArray()) {
        const QJsonObject entry = item.toObject();
        const QString description = entry.value("description").toString();
        const QString field = entry.value("field").toString();
        if (!description.isEmpty())
            parts.append(field.isEmpty() ? description : field + QLatin1String(": ") + description);
        if (parts.size() == 3)
            break;
    }
    if (!parts.isEmpty())
        return parts.join(QStringLiteral("; "));
    if (answer.value("detail").isString())
        return answer.value("detail").toString();
    if (answer.value("error").isString())
        return answer.value("error").toString();
    if (answer.value("title").isString())
        return answer.value("title").toString();
    return serverMessage(answer);
}

bool wholeUrl(const QUrl &url) {
    return (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) && !url.host().isEmpty();
}

QString originOf(const QUrl &url) {
    return QStringLiteral("%1://%2:%3").arg(url.scheme(), url.host().toLower()).arg(url.port(url.scheme() == QLatin1String("https") ? 443 : 80));
}

QString trimmedAddress(QString address) {
    address = address.trimmed();
    while (address.endsWith(QLatin1Char('/')))
        address.chop(1);
    return address;
}

bool loopbackHost(const QString &host) {
    QHostAddress address;
    return host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive) ||
           (address.setAddress(host) && address.isLoopback());
}

// The aspect ratio of `values` ("16:9") nearest to width/height, on a log scale.
QString nearestAspect(const QStringList &values, int width, int height) {
    const double want = std::log(double(width) / double(height));
    QString best;
    double bestGap = 1e9;
    for (const QString &value : values) {
        const QStringList sides = value.split(QLatin1Char(':'));
        if (sides.size() != 2 || sides.at(0).toDouble() <= 0 || sides.at(1).toDouble() <= 0)
            continue;
        const double gap = std::abs(std::log(sides.at(0).toDouble() / sides.at(1).toDouble()) - want);
        if (gap < bestGap) {
            bestGap = gap;
            best = value;
        }
    }
    return best;
}

// The megapixel value of `values` ("0.25") nearest to width x height, on a log scale.
QString nearestMegapixels(const QStringList &values, int width, int height) {
    const double want = std::log(double(width) * double(height) / 1e6);
    QString best;
    double bestGap = 1e9;
    for (const QString &value : values) {
        if (value.toDouble() <= 0)
            continue;
        const double gap = std::abs(std::log(value.toDouble()) - want);
        if (gap < bestGap) {
            bestGap = gap;
            best = value;
        }
    }
    return best;
}

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list.append(item.toString());
    return list;
}

} // namespace

QString replicateDefaultAddress() { return QStringLiteral("https://api.replicate.com"); }

QString replicateAddress(const ProviderInstance &instance) {
    const QString address = trimmedAddress(instance.address);
    return address.isEmpty() ? replicateDefaultAddress() : address;
}

QList<ReplicateModel> replicateShippedModels() {
    QFile file(QStringLiteral(":/fugue/providers/replicate-models.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<ReplicateModel> models;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("models").toArray()) {
        const QJsonObject row = value.toObject();
        ReplicateModel model;
        model.id = row.value("id").toString();
        if (model.id.isEmpty())
            continue;
        model.title = row.value("title").toString(model.id);
        model.price = row.value("price").toString();
        const QString kind = row.value("capability").toString();
        model.kind = kind == QLatin1String("edit") ? ReplicateModel::Kind::Edit
                     : kind == QLatin1String("inpaint") ? ReplicateModel::Kind::Inpaint : ReplicateModel::Kind::Generate;
        model.aspectField = row.value("aspect").toString();
        model.aspects = stringList(row.value("aspects"));
        model.aspectFollow = row.value("aspect_follow").toString();
        model.megapixelsField = row.value("megapixels").toString();
        model.megapixelValues = stringList(row.value("megapixel_values"));
        model.stepsField = row.value("steps").toString();
        model.stepsMax = row.value("steps_max").toInt();
        model.guidanceField = row.value("guidance").toString();
        model.negativeField = row.value("negative_prompt").toString();
        model.seedField = row.value("seed").toString();
        model.imageField = row.value("image").toString();
        model.imageList = row.value("image_list").toBool();
        model.maskField = row.value("mask").toString();
        model.fixed = row.value("fixed").toObject();
        model.defaults = row.value("defaults").toObject();
        models.append(model);
    }
    return models;
}

struct ReplicateProvider::Job {
    struct Upload {
        QString role; // "image" or "mask"
        QString name;
        QString type;
        QByteArray bytes;
        QString url; // a data URI, or the files API's link
    };
    JobRequest request;
    QJsonObject handle;
    ReplicateModel spec;
    JobStatus status;
    JobResult result;
    qint64 endedAt = 0;
    Phase phase = Phase::Uploading;
    QPointer<QNetworkReply> reply;
    QList<Upload> uploads;
    int uploadAt = 0;
    QString getUrl, cancelUrl;
    int polls = 0;    // polls in the same state: the wait grows with it
    int failures = 0; // polls that got no usable answer in a row
    int hops = 0;
    QJsonObject sent; // the input sent, for the values used
};

ReplicateProvider::ReplicateProvider(ProviderInstance instance, QObject *parent) : Provider(parent), m_instance(std::move(instance)) {
    m_instance.address = trimmedAddress(m_instance.address);
}

ReplicateProvider::~ReplicateProvider() = default;

int ReplicateProvider::timeoutMs() const {
    const QJsonValue value = m_instance.settings.value("timeout_s");
    const double seconds = value.isString() ? value.toString().toDouble() : value.toDouble();
    return int(std::clamp(seconds > 0 ? seconds : double(kDefaultTimeoutS), 0.05, 86400.0) * 1000);
}

QUrl ReplicateProvider::apiUrl(const QString &path) const { return QUrl(replicateAddress(m_instance) + path); }

QString ReplicateProvider::host() const { return apiUrl({}).host(); }

bool ReplicateProvider::isApiUrl(const QUrl &url) const { return wholeUrl(url) && originOf(url) == originOf(apiUrl({})); }

QNetworkReply *ReplicateProvider::send(const QUrl &url, const QByteArray &method, const QByteArray &body, const QByteArray &contentType, bool withKey,
                                       qint64 cap, const QList<QPair<QByteArray, QByteArray>> &headers) {
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs());
    // No redirect is followed by Qt: a picture's is checked by download(), hop by hop, and an API call's is an error.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    // The key goes to Replicate's API origin and nowhere else: a URL the service named is sent it only when it is that.
    if (withKey && !m_instance.key.isEmpty() && isApiUrl(url))
        request.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);
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

QString ReplicateProvider::clean(const QString &text) const {
    QString out = text;
    if (!m_instance.key.isEmpty())
        out.replace(m_instance.key, QStringLiteral("***"));
    return oneLine(out);
}

Error ReplicateProvider::failure(QNetworkReply *reply, const QJsonObject &answer, bool submitting) const {
    const QString who = m_instance.name;
    const int code = codeOf(reply);
    const QString said = clean(saidBy(answer));
    if (reply->property(kCapped).toBool())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent more data than Fugue reads in one answer.").arg(who),
                         QStringLiteral("Check the address of %1: it should be Replicate's API, https://api.replicate.com.").arg(who));
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
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 refused the key (HTTP %2)%3").arg(who).arg(code).arg(withSaid(said)),
                         QStringLiteral("Check the key of %1 in %2 (replicate.com/account/api-tokens).").arg(who, where()));
    if (code == 402)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no credit left for this account (HTTP 402)%2").arg(who, withSaid(said)),
                         QStringLiteral("Add credit at replicate.com/account/billing, then run the node again."));
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 answered with a redirect, which Fugue does not follow.").arg(who),
                         QStringLiteral("Check the address of %1 in %2.").arg(who, where()));
    if (code == 404 || code == 410) {
        if (submitting && code == 404)
            return makeError(QStringLiteral("model_missing"), QStringLiteral("%1 has no such model: %2").arg(who, clean(reply->url().path().mid(11).section(QLatin1String("/predictions"), 0, 0))),
                             QStringLiteral("Check the model id, such as black-forest-labs/flux-schnell: only official models are run by name; they are listed at replicate.com/collections/official."));
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 no longer has the job (HTTP %2).").arg(who).arg(code), QStringLiteral("Run the node again."));
    }
    if (code == 422)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 did not accept the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP 422") : said),
                         QStringLiteral("Change the size, the settings or the pictures; the model may not support them."));
    if (code == 429) {
        const QString wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
        bool seconds = false;
        wait.toInt(&seconds);
        const QString after = seconds ? QStringLiteral("%1 seconds").arg(wait) : clean(wait);
        return makeError(QStringLiteral("node_failed"),
                         wait.isEmpty() ? QStringLiteral("%1 is limiting the rate of this key (HTTP 429)%2").arg(who, withSaid(said))
                                        : QStringLiteral("%1 is limiting the rate of this key (HTTP 429); it asks to wait %2.").arg(who, after),
                         wait.isEmpty() ? QStringLiteral("Run the node again in a minute. An account with little credit is given a lower rate.")
                                        : QStringLiteral("Run the node again after %1.").arg(after));
    }
    if (code == 502 || code == 503 || code == 504)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 is not available right now (HTTP %2)%3").arg(who).arg(code).arg(withSaid(said)),
                         QStringLiteral("Try again in a moment; if it stays, check status.replicate.com."));
    if (code >= 500 && said.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
        return jobError(QStringLiteral("%1 ran out of memory: %2").arg(who, said));
    if (code >= 500)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 had an error (HTTP %2)%3").arg(who).arg(code).arg(withSaid(said)),
                         QStringLiteral("Try again; if it keeps failing, the service may be down."));
    return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP %1").arg(code) : said),
                     QStringLiteral("The service did not accept these settings; change the size or the model."));
}

// A prediction that ended `failed` (its `error` text, and Replicate's E-codes in it).
Error ReplicateProvider::failureOfJob(const QJsonObject &answer) const {
    const QString who = m_instance.name;
    const QString said = clean(answer.value("error").isString() ? answer.value("error").toString() : QString());
    if (said.contains(QLatin1String("NSFW"), Qt::CaseInsensitive) || said.contains(QLatin1String("safety"), Qt::CaseInsensitive) ||
        said.contains(QLatin1String("sensitive"), Qt::CaseInsensitive))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job under its content policy%2").arg(who, withSaid(said)),
                         QStringLiteral("Change the prompt or the picture."));
    if (said.contains(QLatin1String("E1001")) || said.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
        return jobError(QStringLiteral("%1 ran out of memory: %2").arg(who, said));
    if (said.contains(QLatin1String("E6716")) || said.contains(QLatin1String("E9243")) || said.contains(QLatin1String("E8765")))
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 could not start the model%2").arg(who, withSaid(said)),
                         QStringLiteral("Try again, or pick another model."));
    return jobError(QStringLiteral("%1 failed the job: %2").arg(who, said.isEmpty() ? QStringLiteral("no reason given") : said));
}

QList<ReplicateModel> ReplicateProvider::models() const {
    QList<ReplicateModel> all = replicateShippedModels();
    QStringList extra;
    const QJsonValue value = m_instance.settings.value(QStringLiteral("image_models"));
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            extra.append(item.toString().trimmed());
    } else {
        for (const QString &item : value.toString().split(QRegularExpression(QStringLiteral("[,\\n]"))))
            extra.append(item.trimmed());
    }
    static const QRegularExpression official(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    for (const QString &id : std::as_const(extra)) {
        if (!official.match(id).hasMatch() || std::any_of(all.begin(), all.end(), [&](const ReplicateModel &model) { return model.id == id; }))
            continue;
        ReplicateModel model; // the generic mapping: prompt, seed, image
        model.id = id;
        model.title = id;
        model.seedField = QStringLiteral("seed");
        model.imageField = QStringLiteral("image");
        all.append(model);
    }
    return all;
}

Result<Description> ReplicateProvider::description() const {
    const QUrl api = apiUrl({});
    if (!wholeUrl(api))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no usable address.").arg(m_instance.name),
                                             QStringLiteral("Remove the address of %1 in %2: Replicate needs none.").arg(m_instance.name, where())));
    // The key goes with every call: only to https, or to a loopback fake.
    if (!m_instance.key.isEmpty() && api.scheme() != QLatin1String("https") && !loopbackHost(api.host()))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"),
                                             QStringLiteral("%1 would send its key over plain http to %2.").arg(m_instance.name, api.host()),
                                             QStringLiteral("Use https:// for the address of %1 in %2.").arg(m_instance.name, where())));
    Description description;
    description.engine = QStringLiteral("replicate@") + host();
    const bool keyed = !m_instance.key.isEmpty();
    CapabilityOffer generate{kImageGenerate, {}};
    CapabilityOffer edit{kImageEdit, {}};
    for (const ReplicateModel &model : models()) {
        OfferedModel offered;
        offered.handle = QJsonObject{{"resource", kImageResource}, {"name", model.id}};
        offered.title = model.price.isEmpty() ? model.title : QStringLiteral("%1 (%2)").arg(model.title, model.price);
        offered.status = keyed ? ModelStatus::Ready : ModelStatus::Unavailable;
        if (!keyed) {
            offered.reason = QStringLiteral("%1 has no key.").arg(m_instance.name);
            offered.hint = QStringLiteral("Paste your Replicate API token for %1 in %2.").arg(m_instance.name, where());
        }
        offered.properties = QJsonObject{{"edit", canEdit(model)}, {"input", QJsonArray{"text"}}};
        if (model.kind == ReplicateModel::Kind::Inpaint)
            offered.properties.insert("inputs", QJsonArray{"mask"});
        offered.defaults = model.defaults;
        if (model.kind != ReplicateModel::Kind::Edit)
            generate.models.append(offered);
        if (canEdit(model))
            edit.models.append(offered);
    }
    if (!generate.models.isEmpty())
        description.capabilities.append(generate);
    if (!edit.models.isEmpty())
        description.capabilities.append(edit);
    return Result<Description>(description);
}

void ReplicateProvider::describe(Reply<Description> reply) { later(std::move(reply), description()); }

// --- jobs ---

void ReplicateProvider::forgetOldJobs() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

void ReplicateProvider::change(const QString &id, const JobStatus &status) {
    const auto job = m_jobs.value(id);
    if (!job)
        return;
    const JobStatus old = job->status;
    if (old.state == status.state && old.progress == status.progress && old.stage == status.stage && old.error.code == status.error.code)
        return;
    job->status = status;
    if (status.isTerminal())
        job->endedAt = QDateTime::currentMSecsSinceEpoch();
    emit statusChanged(id, status);
}

void ReplicateProvider::fail(const QString &id, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(id, status);
}

void ReplicateProvider::call(const QString &id, QNetworkReply *reply, std::function<void(QNetworkReply *)> then, bool always) {
    const auto job = m_jobs.value(id);
    if (job)
        job->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, then = std::move(then), always] {
        reply->deleteLater();
        const auto found = m_jobs.value(id);
        if (!found || (found->status.isTerminal() && !always))
            return;
        then(reply);
    });
}

void ReplicateProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    // A reply never comes inside the call (provider.h): every one goes through the event loop.
    QPointer<ReplicateProvider> alive(this);
    reply = [alive, reply = std::move(reply)](const Result<QString> &outcome) {
        if (alive)
            alive->later(reply, outcome);
    };
    const QString who = m_instance.name;
    if (request.capability != kImageGenerate && request.capability != kImageEdit)
        return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"), QStringLiteral("The Replicate provider does not offer %1.").arg(request.capability))));
    if (m_instance.key.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no key.").arg(who),
                                               QStringLiteral("Paste your Replicate API token for %1 in %2.").arg(who, where()))));
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
    ReplicateModel spec;
    for (const ReplicateModel &model : models())
        if (model.id == modelId)
            spec = model;

    // The pictures: the start or edit pictures, then the mask.
    QJsonArray images = request.inputs.value("images").toArray();
    if (request.inputs.contains("image"))
        images.prepend(request.inputs.value("image"));
    const bool masked = request.inputs.contains("mask");
    if (masked && spec.maskField.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("mask_unsupported"), QStringLiteral("%1 does not take a mask.").arg(spec.title),
                                               QStringLiteral("Use a model that takes one, such as black-forest-labs/flux-fill-dev."))));
    if (!images.isEmpty() && spec.imageField.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 does not take a picture, only a prompt.").arg(spec.title),
                                               QStringLiteral("Pick a model that edits pictures, or take the picture off the node."))));
    if ((spec.kind != ReplicateModel::Kind::Generate || request.capability == kImageEdit) && images.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("%1 needs a picture.").arg(spec.title))));
    if (spec.kind == ReplicateModel::Kind::Inpaint && !masked)
        return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("%1 needs a mask.").arg(spec.title),
                                               QStringLiteral("Paint the part to redraw, white, in the mask."))));

    auto job = std::make_shared<Job>();
    job->request = request;
    job->handle = handle;
    job->spec = spec;
    Errors errors;
    const auto collect = [&](const QString &role, const QJsonValue &image) {
        const QString path = image.toObject().value("path").toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            errors.append(makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read the input image %1.").arg(path)));
            return;
        }
        Job::Upload upload;
        upload.role = role;
        upload.bytes = file.readAll();
        const QString format = formatOf(upload.bytes);
        upload.type = format == QLatin1String("jpeg") ? QStringLiteral("image/jpeg") : format == QLatin1String("webp") ? QStringLiteral("image/webp") : QStringLiteral("image/png");
        upload.name = QStringLiteral("%1.%2").arg(role, format == QLatin1String("jpeg") ? QStringLiteral("jpg") : format.isEmpty() ? QStringLiteral("png") : format);
        if (upload.bytes.size() <= kDataUriMax)
            upload.url = QStringLiteral("data:%1;base64,%2").arg(upload.type, QString::fromLatin1(upload.bytes.toBase64()));
        job->uploads.append(upload);
    };
    for (int i = 0; i < images.size() && (spec.imageList || i == 0); ++i)
        collect(QStringLiteral("image"), images.at(i));
    if (masked)
        collect(QStringLiteral("mask"), request.inputs.value("mask"));
    if (hasErrors(errors))
        return reply(Result<QString>(errors));

    const QString id = QStringLiteral("replicate/%1").arg(++m_count);
    job->status.state = QStringLiteral("running");
    const bool needsUpload = std::any_of(job->uploads.begin(), job->uploads.end(), [](const Job::Upload &upload) { return upload.url.isEmpty(); });
    job->status.stage = needsUpload ? QStringLiteral("uploading") : QStringLiteral("submitting");
    m_jobs.insert(id, job);
    // One deadline for the whole job, every phase: a transfer timeout restarts with each byte, so a trickle never trips it.
    QTimer::singleShot(timeoutMs() * 6, this, [this, id] { giveUp(id); });
    reply(Result<QString>(id));
    uploadNext(id);
}

// A picture over 1 MiB goes to the files API (multipart, `content`); the others already hold their data URI.
void ReplicateProvider::uploadNext(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    job->phase = Phase::Uploading;
    while (job->uploadAt < job->uploads.size() && !job->uploads.at(job->uploadAt).url.isEmpty())
        ++job->uploadAt;
    if (job->uploadAt >= job->uploads.size())
        return sendJob(id);
    const Job::Upload &upload = job->uploads.at(job->uploadAt);
    const QByteArray boundary = "fugue" + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    QByteArray body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"content\"; filename=\"" + upload.name.toUtf8() + "\"\r\nContent-Type: " +
                      upload.type.toUtf8() + "\r\n\r\n" + upload.bytes + "\r\n--" + boundary + "--\r\n";
    call(id, send(apiUrl(QStringLiteral("/v1/files")), "POST", body, "multipart/form-data; boundary=" + boundary, true, kJsonCap),
         [this, id, job](QNetworkReply *posted) {
             const int code = codeOf(posted);
             const QJsonObject answer = answerOf(posted);
             if (code != 200 && code != 201)
                 return fail(id, failure(posted, answer));
             const QString url = answer.value("urls").toObject().value("get").toString();
             // The link goes to the model's input as it is: it must be on Replicate's API, never another host.
             if (!isApiUrl(QUrl(url)))
                 return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 named a place for the picture that Fugue does not use.").arg(m_instance.name),
                                           QStringLiteral("Run the node again; if it repeats, the service has changed its API.")));
             job->uploads[job->uploadAt].url = url;
             ++job->uploadAt;
             uploadNext(id);
         });
}

// The input of the model: the model's fixed fields, then what the node asked for, each in the model's own field.
QJsonObject ReplicateProvider::bodyOf(const Job &job) const {
    const ReplicateModel &spec = job.spec;
    const QJsonObject &params = job.request.params;
    QJsonObject input = spec.fixed;
    input.insert("prompt", params.value("prompt").toString());
    const int width = params.value("width").toInt();
    const int height = params.value("height").toInt();
    const bool pictured = std::any_of(job.uploads.begin(), job.uploads.end(), [](const Job::Upload &upload) { return upload.role == QLatin1String("image"); });
    if (!spec.aspectField.isEmpty()) {
        if (pictured && !spec.aspectFollow.isEmpty())
            input.insert(spec.aspectField, spec.aspectFollow);
        else if (width > 0 && height > 0 && !nearestAspect(spec.aspects, width, height).isEmpty())
            input.insert(spec.aspectField, nearestAspect(spec.aspects, width, height));
    }
    if (!spec.megapixelsField.isEmpty() && !(pictured && !spec.aspectFollow.isEmpty()) && width > 0 && height > 0 &&
        !nearestMegapixels(spec.megapixelValues, width, height).isEmpty())
        input.insert(spec.megapixelsField, nearestMegapixels(spec.megapixelValues, width, height));
    if (!spec.stepsField.isEmpty() && params.value("steps").toInt() > 0) {
        const int steps = params.value("steps").toInt();
        input.insert(spec.stepsField, spec.stepsMax > 0 ? std::min(steps, spec.stepsMax) : steps);
    }
    if (!spec.guidanceField.isEmpty() && params.value("guidance").isDouble())
        input.insert(spec.guidanceField, params.value("guidance"));
    if (!spec.negativeField.isEmpty() && !params.value("negative_prompt").toString().trimmed().isEmpty())
        input.insert(spec.negativeField, params.value("negative_prompt"));
    if (!spec.seedField.isEmpty() && params.value("seed").isDouble() && params.value("seed").toDouble() >= 0)
        input.insert(spec.seedField, params.value("seed"));
    QJsonArray images;
    for (const Job::Upload &upload : job.uploads) {
        if (upload.role == QLatin1String("mask") && !spec.maskField.isEmpty())
            input.insert(spec.maskField, upload.url);
        else if (upload.role == QLatin1String("image"))
            images.append(upload.url);
    }
    if (!images.isEmpty() && !spec.imageField.isEmpty()) {
        if (spec.imageList)
            input.insert(spec.imageField, images);
        else
            input.insert(spec.imageField, images.first());
    }
    return input;
}

void ReplicateProvider::sendJob(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    job->phase = Phase::Submitting;
    JobStatus status = job->status;
    status.stage = QStringLiteral("submitting");
    change(id, status);
    job->sent = bodyOf(*job);
    // The prediction is cancelled by Replicate at the job's own deadline, should Fugue go away: no bill for a job nobody waits for.
    const int deadlineS = std::max(5, timeoutMs() * 6 / 1000);
    const QList<QPair<QByteArray, QByteArray>> headers{Header{"Cancel-After", QByteArray::number(deadlineS) + "s"}};
    call(id, send(apiUrl(QStringLiteral("/v1/models/") + job->handle.value("name").toString() + QStringLiteral("/predictions")), "POST",
                  QJsonDocument(QJsonObject{{"input", job->sent}}).toJson(QJsonDocument::Compact), "application/json", true, kJsonCap, headers),
         [this, id, job](QNetworkReply *posted) {
             const int code = codeOf(posted);
             const QJsonObject answer = answerOf(posted);
             if (code != 200 && code != 201 && code != 202) {
                 if (!job->status.isTerminal())
                     fail(id, failure(posted, answer, true));
                 return;
             }
             const QJsonObject urls = answer.value("urls").toObject();
             const QString getUrl = urls.value("get").toString();
             const QString cancelUrl = urls.value("cancel").toString();
             // Only the URLs the service returned are used, and only when they are on its API origin: the key goes with them.
             if (!isApiUrl(QUrl(getUrl)) || (!cancelUrl.isEmpty() && !isApiUrl(QUrl(cancelUrl)))) {
                 if (!job->status.isTerminal())
                     fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 took the job but named no usable place to follow it.").arg(m_instance.name),
                                        QStringLiteral("Run the node again; if it repeats, the service has changed its API.")));
                 return;
             }
             job->getUrl = getUrl;
             job->cancelUrl = cancelUrl;
             if (job->status.isTerminal()) // cancelled while the job was being sent
                 return sendCancel(job);
             job->phase = Phase::Waiting;
             pollLater(id);
         },
         true);
}

void ReplicateProvider::sendCancel(const std::shared_ptr<Job> &job) {
    if (job->cancelUrl.isEmpty())
        return;
    QNetworkReply *post = send(QUrl(job->cancelUrl), "POST");
    connect(post, &QNetworkReply::finished, post, &QObject::deleteLater);
}

void ReplicateProvider::pollLater(const QString &id, int atLeastMs) {
    const auto job = m_jobs.value(id);
    if (!job)
        return;
    const int base = std::max(5, m_instance.settings.value("poll_ms").toInt(kDefaultPollMs));
    // 0.5 s growing to 3 s; it starts over when the job's state changes.
    const int wait = int(std::min(double(base) * std::pow(1.5, job->polls), double(base) * 6));
    ++job->polls;
    QTimer::singleShot(std::max(wait, atLeastMs), this, [this, id] { pollNow(id); });
}

void ReplicateProvider::giveUp(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    fail(id, makeError(QStringLiteral("node_timeout"), QStringLiteral("%1 did not finish the job in %2 seconds.").arg(m_instance.name).arg(timeoutMs() * 6 / 1000),
                       QStringLiteral("Raise timeout_s for %1 in %2 if the job is only slow.").arg(m_instance.name, where())));
    if (job->reply && job->phase != Phase::Submitting)
        job->reply->abort();
    sendCancel(job);
}

void ReplicateProvider::pollNow(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    call(id, send(QUrl(job->getUrl), "GET"), [this, id, job](QNetworkReply *reply) {
        const int code = codeOf(reply);
        // No answer, a rate limit or a gateway hiccup: try again a few times, waiting longer each time.
        if (code == 0 || code == 429 || code == 500 || code == 502 || code == 503 || code == 504) {
            if (++job->failures >= kMaxTransientPolls)
                return fail(id, failure(reply, answerOf(reply)));
            bool seconds = false;
            const int retryAfter = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed().toInt(&seconds);
            return pollLater(id, seconds ? std::clamp(retryAfter, 0, 30) * 1000 : 0);
        }
        job->failures = 0;
        if (code != 200)
            return fail(id, failure(reply, answerOf(reply)));
        const QJsonObject answer = answerOf(reply);
        const QString state = answer.value("status").toString();
        if (state == QLatin1String("succeeded")) {
            // The output is a list of URLs (or one), on Replicate's delivery host.
            const QJsonValue output = answer.value("output");
            const QString url = output.isArray() ? output.toArray().first().toString() : output.toString();
            if (url.isEmpty())
                return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 finished without an image.").arg(m_instance.name),
                                          QStringLiteral("Run the node again, or change the prompt.")));
            job->hops = 0;
            return download(id, QUrl(url));
        }
        if (state == QLatin1String("failed"))
            return fail(id, failureOfJob(answer));
        if (state == QLatin1String("canceled") || state == QLatin1String("aborted"))
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 stopped the job (%2).").arg(m_instance.name, state),
                                      QStringLiteral("Run the node again.")));
        JobStatus status = job->status;
        if (state == QLatin1String("starting")) {
            status.state = QStringLiteral("queued");
            status.stage = QStringLiteral("starting");
        } else {
            status.state = QStringLiteral("running");
            status.stage = QStringLiteral("running");
        }
        if (status.state != job->status.state)
            job->polls = 0;
        change(id, status);
        pollLater(id);
    });
}

// The picture, from Replicate's delivery host, with no key.
void ReplicateProvider::download(const QString &id, const QUrl &url) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    if (!downloadUrlAllowed(url, host()))
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 named a picture at %2, which Fugue does not fetch.").arg(m_instance.name, clean(url.toString())),
                                  QStringLiteral("Only http and https links, and no local address of another machine, are fetched.")));
    job->phase = Phase::Downloading;
    JobStatus status = job->status;
    status.state = QStringLiteral("running");
    status.stage = QStringLiteral("downloading");
    change(id, status);
    call(id, send(url, "GET", {}, {}, false, kPictureCap), [this, id, job](QNetworkReply *reply) {
        const int code = codeOf(reply);
        if (code >= 300 && code < 400 && code != 304) {
            const QUrl next = reply->url().resolved(QUrl(QString::fromLatin1(reply->rawHeader("Location"))));
            if (++job->hops > kMaxHops)
                return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent the picture link on too many times.").arg(m_instance.name),
                                          QStringLiteral("Run the node again.")));
            return download(id, next);
        }
        if (code >= 400)
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1's picture link was refused (HTTP %2); it may have expired (they last an hour).").arg(m_instance.name).arg(code),
                                      QStringLiteral("Run the node again.")));
        if (code == 0 || code >= 300)
            return fail(id, failure(reply, {}));
        const QByteArray raw = reply->readAll();
        if (formatOf(raw).isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(m_instance.name)));
        saveImage(id, raw);
    });
}

void ReplicateProvider::saveImage(const QString &id, const QByteArray &bytes) {
    const auto job = m_jobs.value(id);
    const QString format = formatOf(bytes);
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(id, makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();
    // The values used (8.1): the seed that was sent (Replicate does not return one), the picture's own size, and the steps and guidance sent.
    QJsonObject used{{"model", job->handle}, {"engine", QStringLiteral("replicate@") + job->handle.value("name").toString()}};
    if (!job->spec.seedField.isEmpty() && job->sent.contains(job->spec.seedField))
        used.insert("seed", job->sent.value(job->spec.seedField));
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
    if (!job->spec.stepsField.isEmpty() && job->sent.contains(job->spec.stepsField))
        used.insert("steps", job->sent.value(job->spec.stepsField));
    if (!job->spec.guidanceField.isEmpty() && job->sent.contains(job->spec.guidanceField))
        used.insert("guidance", job->sent.value(job->spec.guidanceField));
    job->result = JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used};
    JobStatus status;
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

void ReplicateProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void ReplicateProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    // Cancelled at once: whatever Replicate makes of the request, nothing more is waited for. A job whose
    // URLs are not known yet (it is being sent) is cancelled when its answer comes.
    JobStatus cancelled;
    cancelled.state = QStringLiteral("cancelled");
    change(job, cancelled);
    if (found->reply && found->phase != Phase::Submitting)
        found->reply->abort();
    sendCancel(found);
    later(std::move(reply), Result<QString>(found->status.state));
}

void ReplicateProvider::result(const QString &job, Reply<JobResult> reply) {
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
