#include "gemini_provider.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "engine/server_api.h"

namespace fugue {

using namespace serverapi;

namespace {

constexpr int kDefaultTimeoutS = 300;
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;
// An answer holds the picture as base64; a 4K picture is well under this.
constexpr qint64 kAnswerCap = 128LL << 20;
// Google's limit for one request with inline pictures (text, instructions and inline bytes): 20 MB.
constexpr qint64 kRequestCap = 20LL << 20;
const char *const kCapped = "fugue-capped";

const QString kImageResource = QStringLiteral("fugue/image-model");
const QString kImageGenerate = QStringLiteral("image.generate");
const QString kImageEdit = QStringLiteral("image.edit");

QString oneLine(QString text) {
    text = text.simplified();
    return text.size() > 300 ? text.left(297) + QStringLiteral("...") : text;
}

QString where() { return QStringLiteral("Settings > Providers"); }

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

bool wholeUrl(const QUrl &url) {
    return (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) && !url.host().isEmpty();
}

// Google's error body: {"error": {"code": 429, "message": "...", "status": "RESOURCE_EXHAUSTED", "details": [{"reason": ...}]}}.
QString saidBy(const QJsonObject &answer) {
    const QJsonValue error = answer.value("error");
    if (error.isString())
        return error.toString();
    return error.toObject().value("message").toString();
}

QString errorStatusOf(const QJsonObject &answer) { return answer.value("error").toObject().value("status").toString(); }

bool badKey(const QJsonObject &answer) {
    for (const QJsonValue &detail : answer.value("error").toObject().value("details").toArray())
        if (detail.toObject().value("reason").toString() == QLatin1String("API_KEY_INVALID"))
            return true;
    return saidBy(answer).contains(QLatin1String("API key"), Qt::CaseInsensitive) &&
           (saidBy(answer).contains(QLatin1String("not valid"), Qt::CaseInsensitive) || saidBy(answer).contains(QLatin1String("expired"), Qt::CaseInsensitive));
}

// The nearest entry of `choices` ("16:9", ...) to a width and height, by the ratio on a log scale.
QString nearestAspect(const QStringList &choices, int width, int height) {
    const double want = std::log(double(width) / double(height));
    QString best;
    double distance = 1e9;
    for (const QString &choice : choices) {
        const QStringList parts = choice.split(QLatin1Char(':'));
        if (parts.size() != 2 || parts.at(0).toInt() <= 0 || parts.at(1).toInt() <= 0)
            continue;
        const double d = std::abs(std::log(parts.at(0).toDouble() / parts.at(1).toDouble()) - want);
        if (d < distance) {
            distance = d;
            best = choice;
        }
    }
    return best;
}

// The nearest `imageSize` to a larger side in pixels: "512" is 512, "1K" 1024, "2K" 2048, "4K" 4096.
QString nearestSize(const QStringList &choices, int longSide) {
    QString best;
    double distance = 1e9;
    for (const QString &choice : choices) {
        const double pixels = choice.endsWith(QLatin1Char('K')) ? choice.chopped(1).toDouble() * 1024 : choice.toDouble();
        if (pixels <= 0)
            continue;
        const double d = std::abs(std::log(pixels / double(longSide)));
        if (d < distance) {
            distance = d;
            best = choice;
        }
    }
    return best;
}

} // namespace

QString geminiDefaultAddress() { return QStringLiteral("https://generativelanguage.googleapis.com"); }

QString geminiAddress(const ProviderInstance &instance) {
    const QString address = trimmedAddress(instance.address);
    return address.isEmpty() ? geminiDefaultAddress() : address;
}

QList<GeminiModel> geminiShippedModels() {
    QFile file(QStringLiteral(":/fugue/providers/gemini-models.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<GeminiModel> models;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("models").toArray()) {
        const QJsonObject row = value.toObject();
        GeminiModel model;
        model.id = row.value("id").toString();
        if (model.id.isEmpty())
            continue;
        model.title = row.value("title").toString(model.id);
        model.price = row.value("price").toString();
        model.maxImages = row.value("max_images").toInt(14);
        for (const QJsonValue &aspect : row.value("aspects").toArray())
            model.aspects.append(aspect.toString());
        for (const QJsonValue &size : row.value("sizes").toArray())
            model.sizes.append(size.toString());
        models.append(model);
    }
    return models;
}

struct GeminiProvider::Job {
    JobRequest request;
    QJsonObject handle;
    GeminiModel spec;
    JobStatus status;
    JobResult result;
    qint64 endedAt = 0;
    QPointer<QNetworkReply> reply;
    QJsonObject sent; // the generationConfig sent, for the values used
};

GeminiProvider::GeminiProvider(ProviderInstance instance, QObject *parent) : Provider(parent), m_instance(std::move(instance)) {
    m_instance.address = trimmedAddress(m_instance.address);
    m_instance.key = m_instance.key.trimmed(); // what is sent is what clean() scrubs
}

GeminiProvider::~GeminiProvider() = default;

int GeminiProvider::timeoutMs() const {
    const QJsonValue value = m_instance.settings.value("timeout_s");
    const double seconds = value.isString() ? value.toString().toDouble() : value.toDouble();
    return int(std::clamp(seconds > 0 ? seconds : double(kDefaultTimeoutS), 0.05, 86400.0) * 1000);
}

QString GeminiProvider::host() const { return QUrl(geminiAddress(m_instance)).host(); }

QString GeminiProvider::clean(const QString &text) const {
    QString out = text;
    if (!m_instance.key.isEmpty())
        out.replace(m_instance.key, QStringLiteral("***"));
    return oneLine(out);
}

Error GeminiProvider::failure(QNetworkReply *reply, const QJsonObject &answer) const {
    const QString who = m_instance.name;
    const int code = reply->property(kCapped).toBool() ? 0 : httpStatus(reply);
    const QString said = clean(saidBy(answer));
    const QString status = errorStatusOf(answer);
    const QString tail = said.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + said;
    if (reply->property(kCapped).toBool())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent more data than Fugue reads in one answer.").arg(who),
                         QStringLiteral("Check the address of %1: it should be %2.").arg(who, geminiDefaultAddress()));
    if (code == 0) {
        return makeError(QStringLiteral("provider_unreachable"),
                         QStringLiteral("%1 is not answering at %2 (%3).").arg(who, clean(reply->url().host()), clean(reply->errorString())),
                         QStringLiteral("Check the connection to the internet, then run the node again."));
    }
    // Google answers 400 INVALID_ARGUMENT, not 401, for a key it does not know.
    if (code == 401 || code == 403 || badKey(answer))
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 refused the key (HTTP %2)%3").arg(who).arg(code).arg(tail),
                         QStringLiteral("Check the key of %1 in %2 (aistudio.google.com/apikey).").arg(who, where()));
    if (code >= 300 && code < 400)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 answered with a redirect, which Fugue does not follow.").arg(who),
                         QStringLiteral("Check the address of %1 in %2.").arg(who, where()));
    if (code == 404)
        return makeError(QStringLiteral("model_missing"), QStringLiteral("%1 has no such model: %2").arg(who, clean(reply->url().path().section(QLatin1Char('/'), -1).section(QLatin1Char(':'), 0, 0))),
                         QStringLiteral("Check the model id, such as gemini-3.1-flash-image; Google's models are listed at ai.google.dev/gemini-api/docs/models."));
    if (code == 429) {
        const QString wait = QString::fromLatin1(reply->rawHeader("Retry-After")).trimmed();
        bool seconds = false;
        wait.toInt(&seconds);
        const QString after = seconds ? QStringLiteral("%1 seconds").arg(wait) : clean(wait);
        // Google's image models have no free-tier quota: a key without billing is answered 429 at once.
        return makeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1 has no quota left for this key (HTTP 429)%2%3").arg(who, wait.isEmpty() ? QString() : QStringLiteral("; it asks to wait %1").arg(after), tail),
                         QStringLiteral("The Gemini image models have no free quota: turn on billing for the key's project at aistudio.google.com/apikey, or wait for the limit to reset."));
    }
    if (code == 400 && status == QLatin1String("FAILED_PRECONDITION"))
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 cannot serve this key here%2").arg(who, tail),
                         QStringLiteral("Turn on billing for the key's project at aistudio.google.com/apikey; the free tier is not offered in every country."));
    if (code == 503 || code == 504 || code == 502)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 is not available right now (HTTP %2)%3").arg(who).arg(code).arg(tail),
                         QStringLiteral("Try again in a moment; Google's models are sometimes overloaded."));
    if (code >= 500)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 had an error (HTTP %2)%3").arg(who).arg(code).arg(tail),
                         QStringLiteral("Try again; if it keeps failing, the service may be down."));
    return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 did not accept the job: %2").arg(who, said.isEmpty() ? QStringLiteral("HTTP %1").arg(code) : said),
                     QStringLiteral("Change the prompt, the pictures or the size; the model may not support them."));
}

QList<GeminiModel> GeminiProvider::models() const {
    QList<GeminiModel> all = geminiShippedModels();
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
        if (id.isEmpty() || std::any_of(all.begin(), all.end(), [&](const GeminiModel &model) { return model.id == id; }))
            continue;
        GeminiModel model; // the generic mapping: the aspect ratio, no size
        model.id = id;
        model.title = id;
        model.aspects = {QStringLiteral("1:1"), QStringLiteral("2:3"), QStringLiteral("3:2"), QStringLiteral("3:4"), QStringLiteral("4:3"),
                         QStringLiteral("4:5"), QStringLiteral("5:4"), QStringLiteral("9:16"), QStringLiteral("16:9"), QStringLiteral("21:9")};
        all.append(model);
    }
    return all;
}

Result<Description> GeminiProvider::description() const {
    const QUrl base(geminiAddress(m_instance));
    if (!wholeUrl(base))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no usable address.").arg(m_instance.name),
                                             QStringLiteral("Remove the address of %1 in %2: Google Gemini needs none.").arg(m_instance.name, where())));
    // The key goes with every call: only to https, or to a loopback fake.
    if (!m_instance.key.isEmpty() && base.scheme() != QLatin1String("https") && !loopbackHost(base.host()))
        return Result<Description>(makeError(QStringLiteral("provider_unreachable"),
                                              QStringLiteral("%1 would send its key over plain http to %2.").arg(m_instance.name, base.host()),
                                              QStringLiteral("Use https:// for the address of %1 in %2.").arg(m_instance.name, where())));
    Description description;
    description.engine = QStringLiteral("gemini@") + host();
    const bool keyed = !m_instance.key.isEmpty();
    CapabilityOffer generate{kImageGenerate, {}};
    CapabilityOffer edit{kImageEdit, {}};
    for (const GeminiModel &model : models()) {
        OfferedModel offered;
        offered.handle = QJsonObject{{"resource", kImageResource}, {"name", model.id}};
        offered.title = model.price.isEmpty() ? model.title : QStringLiteral("%1 (%2)").arg(model.title, model.price);
        offered.status = keyed ? ModelStatus::Ready : ModelStatus::Unavailable;
        if (!keyed) {
            offered.reason = QStringLiteral("%1 has no key.").arg(m_instance.name);
            offered.hint = QStringLiteral("Paste your Gemini API key for %1 in %2.").arg(m_instance.name, where());
        }
        offered.properties = QJsonObject{{"edit", true}, {"input", QJsonArray{"text"}}};
        generate.models.append(offered);
        edit.models.append(offered);
    }
    description.capabilities.append(generate);
    description.capabilities.append(edit);
    return Result<Description>(description);
}

void GeminiProvider::describe(Reply<Description> reply) { later(std::move(reply), description()); }

// --- jobs ---

void GeminiProvider::forgetOldJobs() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

void GeminiProvider::change(const QString &id, const JobStatus &status) {
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

void GeminiProvider::fail(const QString &id, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(id, status);
}

void GeminiProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    // A reply never comes inside the call (provider.h): every one goes through the event loop.
    QPointer<GeminiProvider> alive(this);
    reply = [alive, reply = std::move(reply)](const Result<QString> &outcome) {
        if (alive)
            alive->later(reply, outcome);
    };
    const QString who = m_instance.name;
    if (request.capability != kImageGenerate && request.capability != kImageEdit)
        return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"), QStringLiteral("The gemini provider does not offer %1.").arg(request.capability))));
    if (m_instance.key.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 has no key.").arg(who),
                                               QStringLiteral("Paste your Gemini API key for %1 in %2.").arg(who, where()))));
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
    GeminiModel spec;
    for (const GeminiModel &model : models())
        if (model.id == modelId)
            spec = model;

    if (request.inputs.contains("mask"))
        return reply(Result<QString>(makeError(QStringLiteral("mask_unsupported"), QStringLiteral("%1 does not take a mask: the Gemini API has none.").arg(spec.title),
                                               QStringLiteral("Describe the change in the prompt, or use a provider that inpaints."))));
    QJsonArray images = request.inputs.value("images").toArray();
    if (request.inputs.contains("image"))
        images.prepend(request.inputs.value("image"));
    if (request.capability == kImageEdit && images.isEmpty())
        return reply(Result<QString>(makeError(QStringLiteral("missing_input"), QStringLiteral("%1 needs a picture to edit.").arg(spec.title))));
    if (images.size() > spec.maxImages)
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 takes at most %2 pictures, not %3.").arg(spec.title).arg(spec.maxImages).arg(images.size()),
                                               QStringLiteral("Take some pictures off the node."))));

    // The request: the prompt, then each picture inline (Google's limit for the whole request is 20 MB).
    QJsonArray parts{QJsonObject{{"text", request.params.value("prompt").toString()}}};
    qint64 size = parts.first().toObject().value("text").toString().toUtf8().size();
    Errors errors;
    for (const QJsonValue &image : std::as_const(images)) {
        const QString path = image.toObject().value("path").toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            errors.append(makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read the input image %1.").arg(path)));
            continue;
        }
        if (size + file.size() / 3 * 4 > kRequestCap)
            return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("The pictures are too big for one request to %1 (the limit is 20 MB).").arg(who),
                                                   QStringLiteral("Use fewer or smaller pictures."))));
        const QByteArray bytes = file.readAll();
        const QString format = formatOf(bytes);
        if (format.isEmpty()) {
            errors.append(makeError(QStringLiteral("input_invalid"), QStringLiteral("%1 is not a PNG, JPEG or WEBP picture.").arg(path)));
            continue;
        }
        size += bytes.size() / 3 * 4 + 4;
        parts.append(QJsonObject{{"inline_data", QJsonObject{{"mime_type", QStringLiteral("image/") + format}, {"data", QString::fromLatin1(bytes.toBase64())}}}});
    }
    if (hasErrors(errors))
        return reply(Result<QString>(errors));
    if (size > kRequestCap)
        return reply(Result<QString>(makeError(QStringLiteral("input_invalid"), QStringLiteral("The pictures are too big for one request to %1 (the limit is 20 MB).").arg(who),
                                               QStringLiteral("Use fewer or smaller pictures."))));

    QJsonObject config{{"responseModalities", QJsonArray{"TEXT", "IMAGE"}}};
    const int width = request.params.value("width").toInt();
    const int height = request.params.value("height").toInt();
    QJsonObject imageConfig;
    if (width > 0 && height > 0) {
        const QString aspect = nearestAspect(spec.aspects, width, height);
        if (!aspect.isEmpty())
            imageConfig.insert("aspectRatio", aspect);
        const QString imageSize = nearestSize(spec.sizes, std::max(width, height));
        if (!imageSize.isEmpty())
            imageConfig.insert("imageSize", imageSize);
    }
    if (!imageConfig.isEmpty())
        config.insert("imageConfig", imageConfig);
    const QJsonValue seed = request.params.value("seed");
    // Google's seed is an int32 and Fugue's random ones reach 2^53: the same seed always gives the same one.
    if (seed.isDouble() && seed.toDouble() >= 0)
        config.insert("seed", int(qint64(seed.toDouble()) % 2147483648LL));
    const QJsonObject body{{"contents", QJsonArray{QJsonObject{{"role", "user"}, {"parts", parts}}}}, {"generationConfig", config}};

    auto job = std::make_shared<Job>();
    job->request = request;
    job->handle = handle;
    job->spec = spec;
    job->sent = config;
    const QString id = QStringLiteral("gemini/%1").arg(++m_count);
    job->status.state = QStringLiteral("running");
    job->status.stage = QStringLiteral("generating");
    m_jobs.insert(id, job);

    // The key is in the header and in no URL; no redirect is followed (the API answers in place).
    QNetworkRequest http(QUrl(geminiAddress(m_instance) + QStringLiteral("/v1beta/models/%1:generateContent").arg(modelId)));
    http.setTransferTimeout(0);
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    http.setRawHeader("x-goog-api-key", m_instance.key.toUtf8());
    http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *post = m_network.post(http, QJsonDocument(body).toJson(QJsonDocument::Compact));
    job->reply = post;
    connect(post, &QNetworkReply::downloadProgress, post, [post](qint64 got, qint64) {
        if (got > kAnswerCap && !post->property(kCapped).toBool()) {
            post->setProperty(kCapped, true);
            post->abort();
        }
    });
    connect(post, &QNetworkReply::finished, this, [this, id, post] {
        post->deleteLater();
        const auto found = m_jobs.value(id);
        if (found && !found->status.isTerminal())
            finish(id, post);
    });
    // One deadline for the whole job: the model may be silent until the picture is done.
    QTimer::singleShot(timeoutMs(), this, [this, id] {
        const auto found = m_jobs.value(id);
        if (!found || found->status.isTerminal())
            return;
        fail(id, makeError(QStringLiteral("node_timeout"), QStringLiteral("%1 did not finish the job in %2 seconds.").arg(m_instance.name, QString::number(timeoutMs() / 1000.0, 'g', 3)),
                           QStringLiteral("Raise timeout_s for %1 in %2 if the job is only slow.").arg(m_instance.name, where())));
        if (found->reply)
            found->reply->abort();
    });
    reply(Result<QString>(id));
}

void GeminiProvider::finish(const QString &id, QNetworkReply *reply) {
    const int code = reply->property(kCapped).toBool() ? 0 : httpStatus(reply);
    const QJsonObject answer = reply->isOpen() ? jsonBody(reply) : QJsonObject{};
    if (code != 200)
        return fail(id, failure(reply, answer));
    // A body cut off after the 200 is no answer.
    if (reply->error() != QNetworkReply::NoError)
        return fail(id, makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1's answer broke off (%2).").arg(m_instance.name, clean(reply->errorString())),
                                  QStringLiteral("Check the connection to the internet, then run the node again.")));
    const QString who = m_instance.name;
    const QString blocked = answer.value("promptFeedback").toObject().value("blockReason").toString();
    if (!blocked.isEmpty())
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 refused the prompt or the pictures under its safety rules (%2).").arg(who, clean(blocked)),
                                  QStringLiteral("Change the prompt or the pictures and run the node again.")));
    const QJsonObject candidate = firstObject(answer.value("candidates").toArray());
    const QString reason = clean(candidate.value("finishReason").toString());
    QByteArray picture;
    QStringList texts;
    for (const QJsonValue &value : candidate.value("content").toObject().value("parts").toArray()) {
        const QJsonObject part = value.toObject();
        if (part.value("thought").toBool()) // Gemini 3 Pro shows interim pictures as thoughts: not the result
            continue;
        const QJsonObject inlined = part.contains("inlineData") ? part.value("inlineData").toObject() : part.value("inline_data").toObject();
        const QString mime = inlined.contains("mimeType") ? inlined.value("mimeType").toString() : inlined.value("mime_type").toString();
        if (picture.isEmpty() && mime.startsWith(QLatin1String("image/")) && !inlined.value("data").toString().isEmpty())
            picture = QByteArray::fromBase64(inlined.value("data").toString().toLatin1());
        else if (part.contains("text"))
            texts.append(part.value("text").toString());
    }
    if (picture.isEmpty()) {
        static const QStringList refused{QStringLiteral("SAFETY"), QStringLiteral("IMAGE_SAFETY"), QStringLiteral("PROHIBITED_CONTENT"), QStringLiteral("IMAGE_PROHIBITED_CONTENT"),
                                         QStringLiteral("BLOCKLIST"), QStringLiteral("SPII"), QStringLiteral("IMAGE_RECITATION"), QStringLiteral("RECITATION")};
        if (refused.contains(reason))
            return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 held back the picture under its safety rules (%2).").arg(who, reason),
                                      QStringLiteral("Change the prompt or the pictures and run the node again.")));
        const QString said = clean(texts.join(QLatin1Char(' ')));
        return fail(id, makeError(QStringLiteral("node_failed"),
                                  said.isEmpty() ? QStringLiteral("%1 made no picture%2.").arg(who, reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason))
                                                 : QStringLiteral("%1 made no picture, it answered: %2").arg(who, said),
                                  QStringLiteral("Say in the prompt that you want a picture, or change the prompt, and run the node again.")));
    }
    if (formatOf(picture).isEmpty())
        return fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("%1 sent a file that is not a picture.").arg(who)));
    saveImage(id, picture);
}

void GeminiProvider::saveImage(const QString &id, const QByteArray &bytes) {
    const auto job = m_jobs.value(id);
    const QString format = formatOf(bytes);
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail(id, makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();
    // The values used (8.1): the picture's own size when it is a PNG, else the size asked for, and the seed that was sent.
    QJsonObject used{{"model", job->handle}, {"engine", QStringLiteral("gemini@") + job->handle.value("name").toString()}};
    if (job->sent.contains("seed"))
        used.insert("seed", job->sent.value("seed"));
    const QSize size = pngSize(bytes);
    if (size.isValid() && !size.isEmpty()) {
        used.insert("width", size.width());
        used.insert("height", size.height());
    } else {
        for (const char *key : {"width", "height"})
            if (job->request.params.contains(QLatin1String(key)))
                used.insert(QLatin1String(key), job->request.params.value(QLatin1String(key)));
    }
    job->result = JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used};
    JobStatus status;
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

void GeminiProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void GeminiProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    // The call is dropped: Google has no way to cancel, and bills what it already ran.
    JobStatus cancelled;
    cancelled.state = QStringLiteral("cancelled");
    change(job, cancelled);
    if (found->reply)
        found->reply->abort();
    later(std::move(reply), Result<QString>(found->status.state));
}

void GeminiProvider::result(const QString &job, Reply<JobResult> reply) {
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
