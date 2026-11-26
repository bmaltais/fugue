#include "fal_provider.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "engine/http_provider.h"
#include "engine/paths.h"
#include "engine/remote_request.h"
#include "engine/server_api.h"

namespace fugue {

using namespace serverapi;

namespace {

using Header = QPair<QByteArray, QByteArray>;
constexpr int kDefaultTimeoutS = 300;
constexpr int kDefaultPollMs = 500;
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;
// Pictures sent to fal storage are asked to expire in an hour; a link older than 45 minutes is not reused.
constexpr int kStorageLifetimeS = 3600;
constexpr qint64 kUploadReuseMs = 30LL * 60 * 1000;
constexpr qint64 kJsonCap = 64LL << 20;
constexpr qint64 kPictureCap = 512LL << 20;

const QString kImageResource = QStringLiteral("fugue/image-model");
const QString kImageGenerate = QStringLiteral("image.generate");
const QString kImageEdit = QStringLiteral("image.edit");

using remote::statusOf;
using remote::wholeUrl;

QString oneLine(QString text) {
    text = text.simplified();
    return text.size() > 300 ? text.left(297) + QStringLiteral("...") : text;
}

// The JSON a reply holds; nothing from one that was cut off (reading it would warn).
QJsonObject answerOf(QNetworkReply *reply) { return reply->isOpen() ? jsonBody(reply) : QJsonObject{}; }

QString where() { return QStringLiteral("Settings > Providers"); }

bool canEdit(const FalModel &model) { return !model.imageField.isEmpty() && model.kind != FalModel::Kind::Inpaint; }

// What a service says about a failure: `detail` (a text, or a list of {loc, msg, type}), `error`.
QString saidBy(const QJsonObject &answer) {
    const QJsonValue detail = answer.value("detail");
    if (detail.isString())
        return detail.toString();
    QStringList parts;
    if (detail.isArray()) {
        for (const QJsonValue &item : detail.toArray()) {
            const QJsonObject entry = item.toObject();
            const QString msg = entry.value("msg").toString();
            const QString field = entry.value("loc").toArray().last().toString();
            if (!msg.isEmpty())
                parts.append(field.isEmpty() || field == QLatin1String("body") ? msg : field + QLatin1String(": ") + msg);
            if (parts.size() == 3)
                break;
        }
    }
    if (!parts.isEmpty())
        return parts.join(QStringLiteral("; "));
    if (answer.value("error").isString())
        return answer.value("error").toString();
    return serverMessage(answer);
}

QString errorTypeOf(const QJsonObject &answer) {
    const QString type = answer.value("error_type").toString();
    if (!type.isEmpty())
        return type;
    const QJsonArray detail = answer.value("detail").toArray();
    return detail.isEmpty() ? QString() : detail.first().toObject().value("type").toString();
}

} // namespace

QString falDefaultAddress() { return QStringLiteral("https://queue.fal.run"); }
QString falDefaultRestAddress() { return QStringLiteral("https://rest.fal.ai"); }

namespace {
QString trimmedAddress(QString address) {
    address = address.trimmed();
    while (address.endsWith(QLatin1Char('/')))
        address.chop(1);
    return address;
}
} // namespace

QString falQueueAddress(const ProviderInstance &instance) {
    const QString address = trimmedAddress(instance.address);
    return address.isEmpty() ? falDefaultAddress() : address;
}

QString falRestAddress(const ProviderInstance &instance) {
    const QString set = trimmedAddress(instance.settings.value("rest_address").toString());
    if (!set.isEmpty())
        return set;
    // The storage host is fal's own even when the queue's address is typed in; only a loopback fake serves both.
    const QString address = trimmedAddress(instance.address);
    return !address.isEmpty() && remote::loopbackHost(QUrl(address).host()) ? address : falDefaultRestAddress();
}

QList<FalModel> falShippedModels() {
    QFile file(QStringLiteral(":/fugue/providers/fal-models.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<FalModel> models;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("models").toArray()) {
        const QJsonObject row = value.toObject();
        FalModel model;
        model.id = row.value("id").toString();
        if (model.id.isEmpty())
            continue;
        model.title = row.value("title").toString(model.id);
        model.price = row.value("price").toString();
        const QString kind = row.value("capability").toString();
        model.kind = kind == QLatin1String("edit") ? FalModel::Kind::Edit : kind == QLatin1String("inpaint") ? FalModel::Kind::Inpaint : FalModel::Kind::Generate;
        model.sizeField = row.value("size").toString();
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

struct FalProvider::Job {
    struct Upload {
        QString role; // "image" or "mask"
        QString name;
        QString type;
        QByteArray bytes;
        QString hash;
        QString url;
    };
    JobRequest request;
    QJsonObject handle;
    FalModel spec;
    JobStatus status;
    JobResult result;
    qint64 endedAt = 0;
    Phase phase = Phase::Uploading;
    QPointer<QNetworkReply> reply;
    QList<Upload> uploads;
    int uploadAt = 0;
    QString statusUrl, responseUrl, cancelUrl;
    int polls = 0;    // polls in the same state: the wait grows with it
    int failures = 0; // polls that got no usable answer in a row
    int hops = 0;
    QJsonObject sent; // the input sent, for the values used
    QJsonObject answer;
};

FalProvider::FalProvider(ProviderInstance instance, QObject *parent) : Provider(parent), m_instance(std::move(instance)) {
    m_instance.address = m_instance.address.trimmed();
    while (m_instance.address.endsWith(QLatin1Char('/')))
        m_instance.address.chop(1);
}

FalProvider::~FalProvider() = default;

int FalProvider::timeoutMs() const {
    const QJsonValue value = m_instance.settings.value("timeout_s");
    const double seconds = value.isString() ? value.toString().toDouble() : value.toDouble();
    return int(std::clamp(seconds > 0 ? seconds : double(kDefaultTimeoutS), 0.05, 86400.0) * 1000);
}

QUrl FalProvider::queueUrl(const QString &path) const { return QUrl(falQueueAddress(m_instance) + path); }

QUrl FalProvider::restUrl(const QString &path) const { return QUrl(falRestAddress(m_instance) + path); }

QString FalProvider::host() const { return queueUrl({}).host(); }

bool FalProvider::isApiUrl(const QUrl &url) const {
    return wholeUrl(url) && (remote::originOf(url) == remote::originOf(queueUrl({})) || remote::originOf(url) == remote::originOf(restUrl({})));
}

QNetworkReply *FalProvider::send(const QUrl &url, const QByteArray &method, const QByteArray &body, const QByteArray &contentType, bool withKey,
                                 qint64 cap, const QList<QPair<QByteArray, QByteArray>> &headers) {
    // The key goes to fal's own API hosts and nowhere else: a URL the service named is sent it only when it is one.
    remote::Session session;
    session.scheme = "Key";
    session.key = m_instance.key;
    session.origins = {queueUrl({}), restUrl({})};
    session.transferTimeoutMs = timeoutMs();
    remote::Call call;
    call.method = method;
    call.body = body;
    call.contentType = contentType;
    call.cap = cap;
    call.withKey = withKey;
    call.headers = headers;
    return remote::send(m_network, session, url, call);
}

QString FalProvider::clean(const QString &text) const {
    QString out = text;
    if (!m_instance.key.isEmpty())
        out.replace(m_instance.key, QStringLiteral("***"));
    // The id half of an old-style key (<id>:<secret>) is no secret to hide, but the secret half is.
    const qsizetype colon = m_instance.key.indexOf(QLatin1Char(':'));
    if (colon > 0 && m_instance.key.size() - colon > 8)
        out.replace(m_instance.key.mid(colon + 1), QStringLiteral("***"));
    return oneLine(out);
}

Error FalProvider::failure(QNetworkReply *reply, const QJsonObject &answer, bool submitting) const {
    const QString who = m_instance.name;
    const int code = statusOf(reply);
    const QString said = clean(saidBy(answer));
    const QString type = errorTypeOf(answer);
    if (remote::wasCapped(reply))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent more data than Fugue reads in one answer.").arg(who),
                         QStringLiteral("Check the address of %1: it should be fal's queue, https://queue.fal.run.").arg(who));
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
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 refused the key (HTTP %2)%3").arg(who).arg(code).arg(said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Check the key of %1 in %2 (fal.ai/dashboard/keys).").arg(who, where()));
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 answered with a redirect, which Fugue does not follow.").arg(who),
                         QStringLiteral("Check the address of %1 in %2.").arg(who, where()));
    if (code == 404 || code == 410) {
        if (submitting && code == 404)
            return makeError(QStringLiteral("model_missing"), QStringLiteral("%1 has no such model: %2").arg(who, clean(reply->url().path().mid(1))),
                             QStringLiteral("Check the endpoint id, such as fal-ai/flux/dev; the models of fal.ai are listed at fal.ai/models."));
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 no longer has the job (HTTP %2).").arg(who).arg(code), QStringLiteral("Run the node again."));
    }
    if (code == 422) {
        if (type == QLatin1String("content_policy_violation"))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job under its content policy%2").arg(who, said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                             QStringLiteral("Change the prompt or the picture."));
        if (type == QLatin1String("no_media_generated"))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 made no picture%2").arg(who, said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                             QStringLiteral("Run the node again, or change the prompt."));
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 did not accept the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP 422") : said),
                         QStringLiteral("Change the size, the settings or the pictures; the model may not support them."));
    }
    if (code == 429) {
        const QString wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
        bool seconds = false;
        wait.toInt(&seconds);
        const QString after = seconds ? QStringLiteral("%1 seconds").arg(wait) : clean(wait);
        return makeError(QStringLiteral("node_failed"),
                         wait.isEmpty() ? QStringLiteral("%1 is limiting the rate of this key (HTTP 429).").arg(who)
                                        : QStringLiteral("%1 is limiting the rate of this key (HTTP 429); it asks to wait %2.").arg(who, after),
                         wait.isEmpty() ? QStringLiteral("Run the node again in a minute.") : QStringLiteral("Run the node again after %1.").arg(after));
    }
    if (code == 502 || code == 503 || code == 504)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 is not available right now (HTTP %2)%3").arg(who).arg(code).arg(said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Try again in a moment; if it stays, check fal.ai's status page."));
    if (code >= 500 && said.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
        return jobError(QStringLiteral("%1 ran out of memory: %2").arg(who, said));
    if (code >= 500)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 had an error (HTTP %2)%3").arg(who).arg(code).arg(said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Try again; if it keeps failing, the service may be down."));
    return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP %1").arg(code) : said),
                     QStringLiteral("The service did not accept these settings; change the size or the model."));
}

// A job the queue ended with `error` (COMPLETED, error and error_type).
Error FalProvider::failureOfJob(const QJsonObject &answer) const {
    const QString who = m_instance.name;
    const QString said = clean(saidBy(answer));
    const QString type = errorTypeOf(answer);
    if (type == QLatin1String("content_policy_violation"))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the job under its content policy%2").arg(who, said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Change the prompt or the picture."));
    if (type.contains(QLatin1String("timeout")))
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 timed out on the job%2").arg(who, said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said),
                         QStringLiteral("Try again, or use a smaller size."));
    return jobError(QStringLiteral("%1 failed the job: %2").arg(who, said.isEmpty() ? QStringLiteral("no reason given") : said));
}

QList<FalModel> FalProvider::models() const {
    QList<FalModel> all = falShippedModels();
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
        if (id.isEmpty() || std::any_of(all.begin(), all.end(), [&](const FalModel &model) { return model.id == id; }))
            continue;
        FalModel model; // the generic mapping: prompt, image_size, seed, image_url
        model.id = id;
        model.title = id;
        model.sizeField = QStringLiteral("image_size");
        model.seedField = QStringLiteral("seed");
        model.imageField = QStringLiteral("image_url");
        model.fixed = QJsonObject{{"num_images", 1}, {"output_format", "png"}};
        all.append(model);
    }
    return all;
}

Result<Description> FalProvider::description() const {
    const QUrl queue = queueUrl({});
    if (!wholeUrl(queue))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no usable address.").arg(m_instance.name),
                                             QStringLiteral("Remove the address of %1 in %2: fal.ai needs none.").arg(m_instance.name, where())));
    // The key goes with every call: only to https, or to a loopback fake.
    if (!m_instance.key.isEmpty()) {
        for (const QUrl &url : {queue, restUrl({})})
            if (!remote::keyTransportOk(url))
                return Result<Description>(makeError(QStringLiteral("provider_unreachable"),
                                                      QStringLiteral("%1 would send its key over plain http to %2.").arg(m_instance.name, url.host()),
                                                      QStringLiteral("Use https:// for the address of %1 in %2.").arg(m_instance.name, where())));
    }
    if (!wholeUrl(restUrl({})))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no usable storage address.").arg(m_instance.name),
                                             QStringLiteral("Fix rest_address for %1 in %2.").arg(m_instance.name, where())));
    Description description;
    description.engine = QStringLiteral("fal@") + host();
    const bool keyed = !m_instance.key.isEmpty();
    CapabilityOffer generate{kImageGenerate, {}};
    CapabilityOffer edit{kImageEdit, {}};
    for (const FalModel &model : models()) {
        OfferedModel offered;
        offered.handle = QJsonObject{{"resource", kImageResource}, {"name", model.id}};
        offered.title = model.price.isEmpty() ? model.title : QStringLiteral("%1 (%2)").arg(model.title, model.price);
        offered.status = keyed ? ModelStatus::Ready : ModelStatus::Unavailable;
        if (!keyed) {
            offered.reason = QStringLiteral("%1 has no key.").arg(m_instance.name);
            offered.hint = QStringLiteral("Paste your fal.ai key for %1 in %2.").arg(m_instance.name, where());
        }
        offered.properties = QJsonObject{{"edit", canEdit(model)}, {"input", QJsonArray{"text"}}};
        if (model.kind == FalModel::Kind::Inpaint)
            offered.properties.insert("inputs", QJsonArray{"mask"});
        offered.defaults = model.defaults;
        if (model.kind != FalModel::Kind::Edit)
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

void FalProvider::describe(Reply<Description> reply) { later(std::move(reply), description()); }

// --- jobs ---

void FalProvider::forgetOldJobs() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

void FalProvider::change(const QString &id, const JobStatus &status) {
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

void FalProvider::fail(const QString &id, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(id, status);
}

void FalProvider::call(const QString &id, QNetworkReply *reply, std::function<void(QNetworkReply *)> then, bool always) {
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

void FalProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    // A reply never comes inside the call (provider.h): every one goes through the event loop.
    QPointer<FalProvider> alive(this);
    reply = [alive, reply = std::move(reply)](const Result<QString> &outcome) {
        if (alive)
            alive->later(reply, outcome);
    };
    const QString who = m_instance.name;
    if (request.capability != kImageGenerate && request.capability != kImageEdit)
        return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"), QStringLiteral("The fal provider does not offer %1.").arg(request.capability))));
    if (m_instance.key.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no key.").arg(who),
                                               QStringLiteral("Paste your fal.ai key for %1 in %2.").arg(who, where()))));
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
    const QString endpoint = handle.value("name").toString();
    FalModel spec;
    for (const FalModel &model : models())
        if (model.id == endpoint)
            spec = model;

    // The pictures: the start or edit pictures, then the mask.
    QJsonArray images = request.inputs.value("images").toArray();
    if (request.inputs.contains("image"))
        images.prepend(request.inputs.value("image"));
    const bool masked = request.inputs.contains("mask");
    if (masked && spec.maskField.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("mask_unsupported"), QStringLiteral("%1 does not take a mask.").arg(spec.title),
                                               QStringLiteral("Use a model that takes one, such as fal-ai/flux-pro/v1/fill."))));
    if (!images.isEmpty() && spec.imageField.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 does not take a picture, only a prompt.").arg(spec.title),
                                               QStringLiteral("Pick a model that edits pictures, or take the picture off the node."))));
    if ((spec.kind != FalModel::Kind::Generate || request.capability == kImageEdit) && images.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("%1 needs a picture.").arg(spec.title))));
    if (spec.kind == FalModel::Kind::Inpaint && !masked)
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
        upload.hash = QString::fromLatin1(QCryptographicHash::hash(upload.bytes, QCryptographicHash::Sha256).toHex());
        job->uploads.append(upload);
    };
    for (int i = 0; i < images.size() && (spec.imageList || i == 0); ++i)
        collect(QStringLiteral("image"), images.at(i));
    if (masked)
        collect(QStringLiteral("mask"), request.inputs.value("mask"));
    if (hasErrors(errors))
        return reply(Result<QString>(errors));

    const QString id = QStringLiteral("fal/%1").arg(++m_count);
    job->status.state = QStringLiteral("running");
    job->status.stage = job->uploads.isEmpty() ? QStringLiteral("submitting") : QStringLiteral("uploading");
    m_jobs.insert(id, job);
    // One deadline for the whole job, every phase: a transfer timeout restarts with each byte, so a trickle never trips it.
    QTimer::singleShot(remote::jobDeadlineMs(timeoutMs()), this, [this, id] { giveUp(id); });
    reply(Result<QString>(id));
    uploadNext(id);
}

void FalProvider::uploadNext(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    job->phase = Phase::Uploading;
    const bool store = m_instance.settings.value("store_io").toBool();
    // A picture sent in this session already is not sent again.
    while (job->uploadAt < job->uploads.size()) {
        Job::Upload &upload = job->uploads[job->uploadAt];
        const Uploaded kept = m_uploaded.value(upload.hash);
        if (kept.url.isEmpty() || QDateTime::currentMSecsSinceEpoch() - kept.at > kUploadReuseMs)
            break;
        upload.url = kept.url;
        ++job->uploadAt;
    }
    if (job->uploadAt >= job->uploads.size())
        return sendJob(id);
    const Job::Upload &upload = job->uploads.at(job->uploadAt);
    QList<QPair<QByteArray, QByteArray>> headers;
    if (!store)
        headers.append(Header{"X-Fal-Object-Lifecycle", QJsonDocument(QJsonObject{{"expiration_duration_seconds", kStorageLifetimeS}}).toJson(QJsonDocument::Compact)});
    const QByteArray body = QJsonDocument(QJsonObject{{"content_type", upload.type}, {"file_name", upload.name}}).toJson(QJsonDocument::Compact);
    call(id, send(restUrl(QStringLiteral("/storage/upload/initiate?storage_type=fal-cdn-v3")), "POST", body, "application/json", true, kJsonCap, headers),
         [this, id, job](QNetworkReply *initiated) {
             const int code = statusOf(initiated);
             const QJsonObject answer = answerOf(initiated);
             if (code != 200)
                 return fail(id, failure(initiated, answer));
             const QUrl fileUrl(answer.value("file_url").toString());
             const QUrl uploadUrl(answer.value("upload_url").toString());
             if (!downloadUrlAllowed(fileUrl, host()) || !downloadUrlAllowed(uploadUrl, host()))
                 return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 named a place for the picture that Fugue does not use.").arg(m_instance.name),
                                           QStringLiteral("Only http and https links, and no local address of another machine, are used.")));
             const Job::Upload &upload = job->uploads.at(job->uploadAt);
             // The storage URL is presigned: it takes no key.
             call(id, send(uploadUrl, "PUT", upload.bytes, upload.type.toUtf8(), false, kJsonCap), [this, id, job, fileUrl](QNetworkReply *put) {
                 const int status = statusOf(put);
                 if (status == 0)
                     return fail(id, failure(put, {}));
                 if (status < 200 || status >= 300)
                     return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1's storage refused the picture (HTTP %2).").arg(m_instance.name).arg(status),
                                               QStringLiteral("Run the node again.")));
                 Job::Upload &done = job->uploads[job->uploadAt];
                 done.url = fileUrl.toString();
                 const qint64 now = QDateTime::currentMSecsSinceEpoch();
                 for (auto it = m_uploaded.begin(); it != m_uploaded.end();)
                     it = now - it->at > kUploadReuseMs ? m_uploaded.erase(it) : std::next(it);
                 m_uploaded.insert(done.hash, Uploaded{done.url, QDateTime::currentMSecsSinceEpoch()});
                 ++job->uploadAt;
                 uploadNext(id);
             });
         });
}

// The input of the model: the endpoint's fixed fields, then what the node asked for, each in the model's own field.
QJsonObject FalProvider::bodyOf(const Job &job) const {
    const FalModel &spec = job.spec;
    const QJsonObject &params = job.request.params;
    QJsonObject body = spec.fixed;
    body.insert("prompt", params.value("prompt").toString());
    const int width = params.value("width").toInt();
    const int height = params.value("height").toInt();
    if (!spec.sizeField.isEmpty() && width > 0 && height > 0)
        body.insert(spec.sizeField, QJsonObject{{"width", width}, {"height", height}});
    if (!spec.stepsField.isEmpty() && params.value("steps").toInt() > 0) {
        const int steps = params.value("steps").toInt();
        body.insert(spec.stepsField, spec.stepsMax > 0 ? std::min(steps, spec.stepsMax) : steps);
    }
    if (!spec.guidanceField.isEmpty() && params.value("guidance").isDouble())
        body.insert(spec.guidanceField, params.value("guidance"));
    if (!spec.negativeField.isEmpty() && !params.value("negative_prompt").toString().trimmed().isEmpty())
        body.insert(spec.negativeField, params.value("negative_prompt"));
    if (!spec.seedField.isEmpty() && params.value("seed").isDouble() && params.value("seed").toDouble() >= 0)
        body.insert(spec.seedField, params.value("seed"));
    QJsonArray images;
    for (const Job::Upload &upload : job.uploads) {
        if (upload.role == QLatin1String("mask") && !spec.maskField.isEmpty())
            body.insert(spec.maskField, upload.url);
        else if (upload.role == QLatin1String("image"))
            images.append(upload.url);
    }
    if (!images.isEmpty() && !spec.imageField.isEmpty()) {
        if (spec.imageList)
            body.insert(spec.imageField, images);
        else
            body.insert(spec.imageField, images.first());
    }
    return body;
}

void FalProvider::sendJob(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    job->phase = Phase::Submitting;
    JobStatus status = job->status;
    status.stage = QStringLiteral("submitting");
    change(id, status);
    job->sent = bodyOf(*job);
    // fal keeps a job's inputs and outputs 90 days unless asked not to; the picture is downloaded at once, so it may go sooner.
    QList<QPair<QByteArray, QByteArray>> headers;
    if (!m_instance.settings.value("store_io").toBool()) {
        headers.append(Header{"X-Fal-Store-IO", "0"});
        headers.append(Header{"X-Fal-Object-Lifecycle-Preference",
                        QJsonDocument(QJsonObject{{"expiration_duration_seconds", kStorageLifetimeS}}).toJson(QJsonDocument::Compact)});
    }
    call(id, send(queueUrl(QStringLiteral("/") + job->handle.value("name").toString()), "POST", QJsonDocument(job->sent).toJson(QJsonDocument::Compact),
                  "application/json", true, kJsonCap, headers),
         [this, id, job](QNetworkReply *posted) {
             const int code = statusOf(posted);
             const QJsonObject answer = answerOf(posted);
             const QString statusUrl = answer.value("status_url").toString();
             const QString responseUrl = answer.value("response_url").toString();
             const QString cancelUrl = answer.value("cancel_url").toString();
             if (code != 200 && code != 201 && code != 202) {
                 if (!job->status.isTerminal())
                     fail(id, failure(posted, answer, true));
                 return;
             }
             // Only the URLs the service returned are used, and only when they are on fal's own hosts: the key goes with them.
             if (!isApiUrl(QUrl(statusUrl)) || !isApiUrl(QUrl(responseUrl)) || (!cancelUrl.isEmpty() && !isApiUrl(QUrl(cancelUrl)))) {
                 if (!job->status.isTerminal())
                     fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 took the job but named no usable place to follow it.").arg(m_instance.name),
                                        QStringLiteral("Run the node again; if it repeats, the service has changed its API.")));
                 return;
             }
             job->statusUrl = statusUrl;
             job->responseUrl = responseUrl;
             job->cancelUrl = cancelUrl;
             if (job->status.isTerminal()) // cancelled while the job was being sent
                 return sendCancel(job);
             job->phase = Phase::Waiting;
             pollLater(id);
         },
         true);
}

void FalProvider::sendCancel(const std::shared_ptr<Job> &job) {
    if (job->cancelUrl.isEmpty())
        return;
    QNetworkReply *put = send(QUrl(job->cancelUrl), "PUT");
    connect(put, &QNetworkReply::finished, put, &QObject::deleteLater);
}

void FalProvider::pollLater(const QString &id, int atLeastMs) {
    const auto job = m_jobs.value(id);
    if (!job)
        return;
    const int base = std::max(5, m_instance.settings.value("poll_ms").toInt(kDefaultPollMs));
    // 0.5 s growing to 3 s; it starts over when the job's state changes.
    const int wait = int(std::min(double(base) * std::pow(1.5, job->polls), double(base) * 6));
    ++job->polls;
    QTimer::singleShot(std::max(wait, atLeastMs), this, [this, id] { pollNow(id); });
}

void FalProvider::giveUp(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    fail(id, makeError(QStringLiteral("node_timeout"), QStringLiteral("%1 did not finish the job in %2 seconds.").arg(m_instance.name).arg(remote::jobDeadlineMs(timeoutMs()) / 1000),
                       QStringLiteral("Raise timeout_s for %1 in %2 if the job is only slow.").arg(m_instance.name, where())));
    if (job->reply && job->phase != Phase::Submitting)
        job->reply->abort();
    sendCancel(job);
}

void FalProvider::pollNow(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    call(id, send(QUrl(job->statusUrl), "GET"), [this, id, job](QNetworkReply *reply) {
        const int code = statusOf(reply);
        // No answer, a rate limit or a gateway hiccup: try again a few times, waiting longer each time.
        if (remote::transientStatus(code)) {
            if (remote::pollsExhausted(job->failures))
                return fail(id, failure(reply, answerOf(reply)));
            return pollLater(id, remote::retryAfterMs(reply));
        }
        job->failures = 0;
        // fal answers 202 while the job is queued or running, 200 when it is done.
        if (code != 200 && code != 202)
            return fail(id, failure(reply, answerOf(reply)));
        const QJsonObject answer = answerOf(reply);
        const QString state = answer.value("status").toString();
        const QJsonValue error = answer.value("error");
        if (state == QLatin1String("COMPLETED")) {
            if (!error.isNull() && !error.isUndefined() && !(error.isString() && error.toString().isEmpty()))
                return fail(id, failureOfJob(answer));
            return fetchResult(id);
        }
        JobStatus status = job->status;
        if (state == QLatin1String("IN_QUEUE")) {
            status.state = QStringLiteral("queued");
            status.stage = answer.value("queue_position").isDouble() ? QStringLiteral("in queue, position %1").arg(answer.value("queue_position").toInt() + 1)
                                                                     : QStringLiteral("in queue");
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

void FalProvider::fetchResult(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    job->phase = Phase::Fetching;
    JobStatus status = job->status;
    status.state = QStringLiteral("running");
    status.stage = QStringLiteral("fetching");
    change(id, status);
    call(id, send(QUrl(job->responseUrl), "GET"), [this, id, job](QNetworkReply *reply) {
        const int code = statusOf(reply);
        const QJsonObject answer = answerOf(reply);
        if (code != 200)
            return fail(id, failure(reply, answer));
        job->answer = answer;
        QJsonObject first = firstObject(answer.value("images").toArray());
        if (first.isEmpty())
            first = answer.value("image").toObject();
        const QString url = first.value("url").toString();
        if (url.isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 finished without an image.").arg(m_instance.name),
                                      QStringLiteral("Run the node again, or change the prompt.")));
        // A picture its safety checker blanked is a black one: not a result.
        if (answer.value("has_nsfw_concepts").toArray().contains(true))
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1's safety checker held back this picture.").arg(m_instance.name),
                                      QStringLiteral("Change the prompt or the picture and run the node again.")));
        job->hops = 0;
        download(id, QUrl(url));
    });
}

// The picture, from fal's file server, with no key.
void FalProvider::download(const QString &id, const QUrl &url) {
    const auto job = m_jobs.value(id);
    if (!job || job->status.isTerminal())
        return;
    if (!downloadUrlAllowed(url, host()))
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 named a picture at %2, which Fugue does not fetch.").arg(m_instance.name, clean(url.toString())),
                                  QStringLiteral("Only http and https links, and no local address of another machine, are fetched.")));
    job->phase = Phase::Downloading;
    JobStatus status = job->status;
    status.stage = QStringLiteral("downloading");
    change(id, status);
    call(id, send(url, "GET", {}, {}, false, kPictureCap), [this, id, job](QNetworkReply *reply) {
        const int code = statusOf(reply);
        if (remote::isRedirect(code)) {
            const QUrl next = remote::redirectTarget(reply);
            if (!remote::followHop(job->hops))
                return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent the picture link on too many times.").arg(m_instance.name),
                                          QStringLiteral("Run the node again.")));
            return download(id, next);
        }
        if (code >= 400)
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1's picture link was refused (HTTP %2); it may have expired.").arg(m_instance.name).arg(code),
                                      QStringLiteral("Run the node again.")));
        if (code == 0 || code >= 300)
            return fail(id, failure(reply, {}));
        const QByteArray raw = reply->readAll();
        if (formatOf(raw).isEmpty())
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(m_instance.name)));
        saveImage(id, raw);
    });
}

void FalProvider::saveImage(const QString &id, const QByteArray &bytes) {
    const auto job = m_jobs.value(id);
    const QString format = formatOf(bytes);
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(id, makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();
    // The values used (8.1): the service's seed, the picture's own size, and the steps and guidance that were sent.
    QJsonObject used{{"model", job->handle}, {"engine", QStringLiteral("fal@") + job->handle.value("name").toString()}};
    const QJsonValue seed = seedOf(job->answer.value("seed"));
    if (!seed.isUndefined())
        used.insert("seed", seed);
    else if (job->sent.contains(job->spec.seedField) && !job->spec.seedField.isEmpty())
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

void FalProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void FalProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    // Cancelled at once: whatever fal makes of the request, nothing more is waited for. A job whose
    // URLs are not known yet (it is being sent) is cancelled when its answer comes.
    JobStatus cancelled;
    cancelled.state = QStringLiteral("cancelled");
    change(job, cancelled);
    if (found->reply && found->phase != Phase::Submitting)
        found->reply->abort();
    sendCancel(found);
    later(std::move(reply), Result<QString>(found->status.state));
}

void FalProvider::result(const QString &job, Reply<JobResult> reply) {
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
