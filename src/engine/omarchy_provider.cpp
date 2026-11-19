#include "omarchy_provider.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace fugue {

namespace {

// Fantasi gives each request 15 seconds (generator.cpp setTransferTimeout).
constexpr int kTransferTimeoutMs = 15000;
constexpr int kDefaultPollMs = 500;

const QString kEngine = QStringLiteral("sdcpp");
const QString kModelResource = QStringLiteral("fugue/image-model");

QJsonObject readObject(const QString &path, bool *exists) {
    QFile file(path);
    *exists = file.open(QIODevice::ReadOnly);
    return *exists ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

QByteArray readKey(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

// The native API's root: the endpoint without its trailing /v1. Not the URL's origin, because a
// paired host's endpoint has a path (/local-ai/<recipe>/v1).
QString baseOf(const QString &endpoint) {
    QString base = endpoint;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.endsWith(QLatin1String("/v1")))
        base.chop(3);
    return base;
}

QJsonObject jsonBody(QNetworkReply *reply) { return QJsonDocument::fromJson(reply->readAll()).object(); }

int httpStatus(QNetworkReply *reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Defaults (8.1 names) from sd-server's capabilities: defaults_by_mode.img_gen, or the
// deprecated `defaults` mirror (api.md "GET /sdcpp/v1/capabilities").
QJsonObject defaultsOf(const QJsonObject &capabilities) {
    QJsonObject mode = capabilities.value("defaults_by_mode").toObject().value("img_gen").toObject();
    if (mode.isEmpty())
        mode = capabilities.value("defaults").toObject();
    const QJsonObject sample = mode.value("sample_params").toObject();
    QJsonObject defaults;
    for (const char *key : {"width", "height"})
        if (mode.contains(QLatin1String(key)))
            defaults.insert(QLatin1String(key), mode.value(QLatin1String(key)));
    if (sample.contains("sample_steps"))
        defaults.insert("steps", sample.value("sample_steps"));
    const QJsonObject guidance = sample.value("guidance").toObject();
    if (guidance.contains("txt_cfg"))
        defaults.insert("guidance", guidance.value("txt_cfg"));
    return defaults;
}

QJsonObject limitsOf(const QJsonObject &capabilities) {
    const QJsonObject all = capabilities.value("limits").toObject();
    QJsonObject limits;
    for (const char *key : {"min_width", "max_width", "min_height", "max_height"})
        if (all.contains(QLatin1String(key)))
            limits.insert(QLatin1String(key), all.value(QLatin1String(key)));
    return limits;
}

} // namespace

struct OmarchyProvider::Job {
    Server server;
    QString serverId; // the sd-server's job id
    JobRequest request;
    QJsonObject model; // the handle of the model that runs it
    JobStatus status;
    JobResult result;
    bool asking = false; // a poll is in flight
    int failures = 0;    // polls in a row that got no answer
    qint64 endedAt = 0;  // when it reached a terminal state, in ms
};

namespace {

// Polls in a row without an answer before the job counts as lost.
constexpr int kMaxPollFailures = 3;

// How long a finished job is kept when nobody takes its result: as long as the server
// keeps its own (--completed-ttl-seconds, 8.6).
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;

// A server's error text: {"error": "..."} or {"error": {"message": "..."}}.
QString serverMessage(const QJsonObject &body) {
    const QJsonValue error = body.value("error");
    return error.isObject() ? error.toObject().value("message").toString() : error.toString();
}

Error jobError(const QString &message) {
    const bool memory = message.contains(QLatin1String("out of memory"), Qt::CaseInsensitive);
    return makeError(memory ? QStringLiteral("out_of_memory") : QStringLiteral("node_failed"),
                         message.isEmpty() ? QStringLiteral("The model server failed the job.") : message,
                         memory ? QStringLiteral("Stop other models to free memory, or ask for a smaller image.")
                                : QString());
}

// One sd-server job (async_jobs.cpp make_async_job_json) as a status (8.8). A completed job is
// reported as running until its result is on disk; finishJob() then makes it done.
JobStatus statusOf(const QJsonObject &job) {
    JobStatus status;
    const QString state = job.value("status").toString();
    const QJsonObject progress = job.value("progress").toObject();
    status.stage = progress.value("stage").toString();
    if (state == QLatin1String("queued")) {
        status.state = QStringLiteral("queued");
    } else if (state == QLatin1String("generating") || state == QLatin1String("completed")) {
        status.state = QStringLiteral("running");
        // Per sampling pass, not the whole job (api.md "Job Model").
        const double total = progress.value("total").toDouble();
        if (progress.value("unit").toString() == QLatin1String("steps") && total > 0)
            status.progress = progress.value("completed").toDouble() / total;
        const QString preview = job.value("preview").toObject().value("b64_json").toString();
        if (!preview.isEmpty())
            status.preview = QByteArray::fromBase64(preview.toLatin1());
    } else if (state == QLatin1String("cancelled")) {
        status.state = QStringLiteral("cancelled");
    } else {
        status.state = QStringLiteral("error");
        status.error = jobError(job.value("error").toObject().value("message").toString());
    }
    return status;
}

bool sameStatus(const JobStatus &a, const JobStatus &b) {
    return a.state == b.state && a.progress == b.progress && a.stage == b.stage &&
           a.preview == b.preview && a.error.code == b.error.code;
}

// The file's bytes as base64, which every sd-server image field accepts (api.md "Image
// Encoding Rules"). The servers have no network and no shared filesystem.
bool encodeInput(const QJsonValue &input, QString *encoded, Errors *errors) {
    const QString path = input.toObject().value("path").toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        errors->append(makeError(QStringLiteral("file_not_found"),
                                     QStringLiteral("Cannot read the input image %1.").arg(path)));
        return false;
    }
    *encoded = QString::fromLatin1(file.readAll().toBase64());
    return true;
}

// The img_gen body for the standard parameters of 8.1 (api.md "Request Body"). Parameters
// left out get the server's defaults.
QJsonObject imgGenBody(const JobRequest &request, Errors *errors) {
    const QJsonObject &params = request.params;
    QJsonObject body{{"prompt", params.value("prompt").toString()},
                     {"seed", params.contains("seed") ? params.value("seed") : QJsonValue(-1)},
                     {"batch_count", 1},
                     {"output_format", "png"},
                     {"preview", "proj"},
                     {"preview_interval", 1}};
    for (const char *key : {"negative_prompt", "width", "height"})
        if (params.contains(QLatin1String(key)))
            body.insert(QLatin1String(key), params.value(QLatin1String(key)));
    QJsonObject sample;
    if (params.contains("steps"))
        sample.insert("sample_steps", params.value("steps"));
    if (params.contains("guidance"))
        sample.insert("guidance", QJsonObject{{"txt_cfg", params.value("guidance")}});
    if (!sample.isEmpty())
        body.insert("sample_params", sample);

    QString encoded;
    if (request.capability == QLatin1String("image.edit")) {
        QJsonArray refs;
        for (const QJsonValue &image : request.inputs.value("images").toArray())
            if (encodeInput(image, &encoded, errors))
                refs.append(encoded);
        body.insert("ref_images", refs);
    } else if (request.inputs.contains("image")) {
        if (encodeInput(request.inputs.value("image"), &encoded, errors))
            body.insert("init_image", encoded);
        if (params.contains("strength"))
            body.insert("strength", params.value("strength"));
    }
    return body;
}

// A seed in generation_info is a decimal string (generation_metadata.md "Reproduction
// parameters"); accept a number too.
QJsonValue seedOf(const QJsonValue &seed) {
    if (seed.isString()) {
        bool ok = false;
        const qint64 value = seed.toString().toLongLong(&ok);
        return ok ? QJsonValue(value) : QJsonValue();
    }
    return seed.isDouble() ? seed : QJsonValue();
}

} // namespace

OmarchyPaths OmarchyPaths::standard() {
    return OmarchyPaths{QDir::home().filePath(QStringLiteral(".local/state/omarchy/agent-local")),
                        QDir::home().filePath(QStringLiteral(".config/omarchy/agent-local"))};
}

OmarchyProvider::OmarchyProvider(ProviderInstance instance, OmarchyPaths paths, QObject *parent)
    : Provider(parent), m_instance(std::move(instance)), m_paths(std::move(paths)) {
    m_network.setTransferTimeout(kTransferTimeoutMs);
    m_poll.setInterval(m_instance.settings.value("poll_ms").toInt(kDefaultPollMs));
    connect(&m_poll, &QTimer::timeout, this, &OmarchyProvider::poll);
}

OmarchyProvider::~OmarchyProvider() = default;

QNetworkReply *OmarchyProvider::send(const Server &server, const QByteArray &method,
                                     const QString &path, const QByteArray &body) {
    QNetworkRequest request(QUrl(server.base + path));
    request.setRawHeader("Authorization", "Bearer " + server.key);
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    return m_network.sendCustomRequest(request, method, body);
}

void OmarchyProvider::discover(std::function<void(const Result<Discovery> &)> done) {
    const QDir state(m_paths.stateDir);
    const QDir config(m_paths.configDir);
    const bool remote = !m_instance.address.isEmpty();
    const QString dns = QUrl(m_instance.address).host();
    const QString host = dns.section(QLatin1Char('.'), 0, 0);
    // Replies come from the event loop, never inside the call (provider.h).
    const auto fail = [this, &done](const QString &message, const QString &hint) {
        const Error error = makeError(QStringLiteral("provider_unreachable"), message, hint);
        QTimer::singleShot(0, this, [done = std::move(done), error] { done(Result<Discovery>(error)); });
    };

    // The rows to serve from, and the key for them.
    QList<QJsonObject> rows;
    QByteArray key = m_instance.key.toUtf8();
    if (!remote) {
        bool exists = false;
        const QJsonObject status = readObject(state.filePath("status.json"), &exists);
        if (!exists)
            return fail(QStringLiteral("Omarchy's local models are not set up on this machine (no %1).")
                            .arg(state.filePath("status.json")),
                        QStringLiteral("Install omarchy-local-ai, then set up an image model: "
                                       "omarchy-agent-local use <recipe>."));
        const QJsonObject models = status.value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it)
            rows.append(it.value().toObject());
        if (key.isEmpty())
            key = readKey(config.filePath("key"));
        if (key.isEmpty())
            return fail(QStringLiteral("No key to talk to Omarchy's model servers (no %1).").arg(config.filePath("key")),
                        QStringLiteral("Set up a model with omarchy-agent-local, which writes the key, "
                                       "or set this provider's key in ~/.config/fugue/providers.json."));
    } else {
        bool exists = false;
        const QJsonObject network = readObject(state.filePath("network.json"), &exists);
        const QJsonObject models = network.value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it) {
            const QJsonObject row = it.value().toObject();
            if (row.value("dns").toString() == dns && row.value("paired").toBool())
                rows.append(row);
        }
        const QString pairHint = QStringLiteral("Pair with it: omarchy-agent-local pair %1").arg(host);
        if (rows.isEmpty())
            return fail(QStringLiteral("This machine is not paired with %1.").arg(dns), pairHint);
        if (key.isEmpty())
            key = readKey(config.filePath(QStringLiteral("remotes/%1.key").arg(host)));
        if (key.isEmpty())
            return fail(QStringLiteral("No key for %1 (no %2).").arg(dns, config.filePath("remotes/" + host + ".key")),
                        pairHint);
    }

    // One model per image row; running servers are asked for their capabilities.
    struct Pending {
        Discovery discovery;
        QList<OfferedModel> models;
        QList<bool> edits;
        int waiting = 0;
        std::function<void(const Result<Discovery> &)> done;
    };
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    const auto assemble = [pending] {
        Description &description = pending->discovery.description;
        description.engine = kEngine;
        CapabilityOffer generate{QStringLiteral("image.generate"), pending->models};
        CapabilityOffer edit{QStringLiteral("image.edit"), {}};
        for (qsizetype i = 0; i < pending->models.size(); ++i)
            if (pending->edits.at(i))
                edit.models.append(pending->models.at(i));
        if (!generate.models.isEmpty())
            description.capabilities.append(generate);
        if (!edit.models.isEmpty())
            description.capabilities.append(edit);
        pending->done(Result<Discovery>(pending->discovery));
    };

    const QString where = remote ? QStringLiteral(" on %1").arg(host) : QString();
    for (const QJsonObject &row : rows) {
        if (row.value("kind").toString() != QLatin1String("image"))
            continue;
        const QString recipe = row.value("recipe").toString();
        const QString unit = row.value("unit").toString();
        const Server server{recipe, baseOf(row.value("endpoint").toString()), key};
        pending->discovery.servers.insert(recipe, server);

        OfferedModel model;
        model.handle = QJsonObject{{"resource", kModelResource}, {"name", recipe}};
        model.properties = QJsonObject{{"edit", row.value("edit").toBool()},
                                       {"input", row.value("input").isArray() ? row.value("input")
                                                                               : QJsonArray{"text"}}};
        const QString command = QStringLiteral("omarchy-agent-local start ") + recipe;
        const QString offline = row.value("offline").toString();
        if (!offline.isEmpty()) {
            model.status = QStringLiteral("unavailable");
            model.reason = QStringLiteral("%1 is %2.").arg(host, offline);
            model.hint = QStringLiteral("Turn on %1, or check its tailnet connection.").arg(host);
        } else if (unit == QLatin1String("active") || remote) {
            // A paired host's unit in network.json is only as fresh as the agents panel's last
            // survey, so the server's own answer to the probe below decides.
            model.status = QStringLiteral("ready");
        } else if (unit == QLatin1String("activating")) {
            model.status = QStringLiteral("loading");
            model.reason = QStringLiteral("%1 is starting.").arg(recipe);
            model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
        } else if (unit == QLatin1String("installing")) {
            model.status = QStringLiteral("unavailable");
            model.reason = QStringLiteral("%1 is still being set up.").arg(recipe);
            model.hint = QStringLiteral("Wait for its setup to finish, then start it.");
        } else {
            model.status = QStringLiteral("unavailable");
            const QString error = row.value("error").toString();
            const QString fit = row.value("fits").toBool(true) ? QString() : row.value("fit_reason").toString();
            model.reason = !error.isEmpty() ? error
                           : !fit.isEmpty() ? fit
                                            : QStringLiteral("%1 is stopped%2.").arg(recipe, where);
            model.hint = (fit.isEmpty() ? QStringLiteral("Start it%1: %2") : QStringLiteral("Free memory, then start it%1: %2"))
                             .arg(where, command);
        }
        const qsizetype index = pending->models.size();
        pending->models.append(model);
        pending->edits.append(row.value("edit").toBool());
        if (model.status != QLatin1String("ready"))
            continue;

        ++pending->waiting;
        QNetworkReply *reply = send(server, "GET", QStringLiteral("/sdcpp/v1/capabilities"));
        connect(reply, &QNetworkReply::finished, this, [pending, reply, index, recipe, server, assemble, where, command, this] {
            reply->deleteLater();
            OfferedModel &model = pending->models[index];
            const int code = httpStatus(reply);
            const QJsonObject body = jsonBody(reply);
            if (code == 200) {
                model.defaults = defaultsOf(body);
                model.limits = limitsOf(body);
            } else if (code == 503 && body.value("code").toString() == QLatin1String("model_loading")) {
                model.status = QStringLiteral("loading");
                model.reason = QStringLiteral("%1 is loading its model.").arg(recipe);
                model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
            } else if (code == 401 || code == 403) {
                model.status = QStringLiteral("unavailable");
                model.reason = QStringLiteral("%1 refused the key.").arg(recipe);
                model.hint = m_instance.key.isEmpty()
                                 ? QStringLiteral("Check Omarchy's key file, or pair again.")
                                 : QStringLiteral("Check this provider's key in ~/.config/fugue/providers.json.");
            } else if (code == 502 || code == 404) {
                // The share proxy's answers for a model whose port refuses (stopped) or that is not
                // shared (agent_local.py make_share_proxy _forward).
                model.status = QStringLiteral("unavailable");
                model.reason = QStringLiteral("%1 is not running%2.").arg(recipe, where);
                model.hint = QStringLiteral("Start it%1: %2").arg(where, command);
            } else {
                model.status = QStringLiteral("unavailable");
                model.reason = code == 0 ? QStringLiteral("%1 is not answering at %2.").arg(recipe, server.base)
                                         : QStringLiteral("%1 answered HTTP %2.").arg(recipe).arg(code);
                model.hint = QStringLiteral("Restart it%1: omarchy-agent-local stop %2, then start %2.").arg(where, recipe);
            }
            if (--pending->waiting == 0)
                assemble();
        });
    }
    if (pending->waiting == 0)
        QTimer::singleShot(0, this, assemble);
}

void OmarchyProvider::describe(Reply<Description> reply) {
    discover([reply = std::move(reply)](const Result<Discovery> &found) {
        reply(found.ok() ? Result<Description>(found->description) : Result<Description>(found.errors()));
    });
}

qint64 OmarchyProvider::now() const {
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

// A job leaves m_jobs when its result is taken (result()). One that ended with nothing
// to take, or whose result nobody asked for, leaves here once it is old.
void OmarchyProvider::forgetOldJobs() {
    const qint64 oldest = now() - kFinishedJobTtlMs;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->status.isTerminal() && it.value()->endedAt < oldest)
            it = m_jobs.erase(it);
        else
            ++it;
    }
}

void OmarchyProvider::submit(const JobRequest &request, Reply<QString> reply) {
    forgetOldJobs();
    QPointer<OmarchyProvider> self(this);
    discover([self, request, reply = std::move(reply)](const Result<Discovery> &found) {
        if (!self)
            return;
        if (!found.ok())
            return reply(Result<QString>(found.errors()));
        if (request.capability != QLatin1String("image.generate") &&
            request.capability != QLatin1String("image.edit"))
            return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"),
                                                   QStringLiteral("The omarchy provider does not offer %1.").arg(request.capability))));
        // The same rules as choosing among instances (8.3, 8.4), within this one.
        Requirement requirement;
        requirement.capability = request.capability;
        requirement.model = request.model;
        requirement.nodePin = self->m_instance.name;
        const auto chosen = selectProvider({Candidate{self->m_instance, Result<Description>(found->description)}}, requirement);
        if (!chosen.ok())
            return reply(Result<QString>(chosen.errors()));
        Errors errors;
        const QJsonObject body = imgGenBody(request, &errors);
        if (hasErrors(errors))
            return reply(Result<QString>(errors));

        const QJsonObject model = chosen->model.handle;
        const Server server = found->servers.value(model.value("name").toString());
        QNetworkReply *posted = self->send(server, "POST", QStringLiteral("/sdcpp/v1/img_gen"),
                                           QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(posted, &QNetworkReply::finished, self, [self, posted, server, request, model, reply] {
            posted->deleteLater();
            const int code = httpStatus(posted);
            const QJsonObject answer = jsonBody(posted);
            const QString serverId = answer.value("id").toString();
            if ((code == 202 || code == 200) && !serverId.isEmpty()) {
                auto job = std::make_shared<Job>();
                job->server = server;
                job->serverId = serverId;
                job->request = request;
                job->model = model;
                job->status.state = QStringLiteral("queued");
                const QString id = server.recipe + QLatin1Char('/') + serverId;
                self->m_jobs.insert(id, job);
                if (!self->m_poll.isActive())
                    self->m_poll.start();
                return reply(Result<QString>(id));
            }
            Error error;
            if (code == 0)
                error = makeError(QStringLiteral("provider_unreachable"),
                                      QStringLiteral("%1 is not answering at %2.").arg(server.recipe, server.base),
                                      QStringLiteral("Check that it is running: omarchy-agent-local status."));
            else if (code == 401 || code == 403)
                error = makeError(QStringLiteral("provider_unreachable"),
                                      QStringLiteral("%1 refused the key.").arg(server.recipe),
                                      QStringLiteral("Check the key, or pair again."));
            else if (code == 503)
                error = makeError(QStringLiteral("model_not_running"),
                                      QStringLiteral("%1 has no model loaded yet.").arg(server.recipe),
                                      QStringLiteral("Wait for it to finish loading."));
            else
                error = jobError(QStringLiteral("%1 refused the job: %2")
                                     .arg(server.recipe, serverMessage(answer).isEmpty()
                                                             ? QStringLiteral("HTTP %1").arg(code)
                                                             : serverMessage(answer)));
            reply(Result<QString>(error));
        });
    });
}

void OmarchyProvider::status(const QString &job, Reply<JobStatus> reply) {
    forgetOldJobs();
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobStatus>(makeError("internal", "No job " + job + ".")));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void OmarchyProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<QString>(makeError("internal", "No job " + job + ".")));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    // Cooperative (api.md "POST /sdcpp/v1/jobs/{id}/cancel"): a generating job stays generating
    // until a checkpoint, so the poller keeps following it to its terminal state (9.6).
    QNetworkReply *posted = send(found->server, "POST", QStringLiteral("/sdcpp/v1/jobs/%1/cancel").arg(found->serverId));
    connect(posted, &QNetworkReply::finished, this, [this, posted, job, found, reply] {
        posted->deleteLater();
        const int code = httpStatus(posted);
        if (code == 0)
            return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"),
                                                   QStringLiteral("%1 is not answering at %2.").arg(found->server.recipe, found->server.base))));
        const QJsonObject answer = jsonBody(posted);
        if (code == 200 && answer.value("status").toString() == QLatin1String("cancelled") && !found->status.isTerminal())
            change(job, statusOf(answer));
        reply(Result<QString>(found->status.state));
    });
}

void OmarchyProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.value(job);
    if (!found)
        return later(std::move(reply), Result<JobResult>(makeError("internal", "No job " + job + ".")));
    if (found->status.state != QLatin1String("done"))
        return later(std::move(reply),
                     Result<JobResult>(makeError("internal", QStringLiteral("Job %1 is %2, not done.")
                                                                 .arg(job, found->status.state))));
    // Handed over once: the files are in the caller's outputDir, and nothing else is
    // kept for a job that is over.
    m_jobs.remove(job);
    later(std::move(reply), Result<JobResult>(found->result));
}

void OmarchyProvider::poll() {
    bool active = false;
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it.value()->status.isTerminal())
            continue;
        active = true;
        if (!it.value()->asking)
            pollJob(it.key());
    }
    if (!active)
        m_poll.stop();
}

void OmarchyProvider::pollJob(const QString &id) {
    const auto job = m_jobs.value(id);
    job->asking = true;
    QNetworkReply *reply = send(job->server, "GET", QStringLiteral("/sdcpp/v1/jobs/%1").arg(job->serverId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, job] {
        reply->deleteLater();
        job->asking = false;
        if (job->status.isTerminal())
            return;
        const int code = httpStatus(reply);
        if (code == 0) {
            if (++job->failures >= kMaxPollFailures) {
                JobStatus lost;
                lost.state = QStringLiteral("error");
                lost.error = makeError(QStringLiteral("provider_unreachable"),
                                           QStringLiteral("%1 stopped answering during the job.").arg(job->server.recipe));
                change(id, lost);
            }
            return;
        }
        job->failures = 0;
        if (code != 200) {
            // 404 or 410: the server no longer has the job, so its result is lost.
            JobStatus gone;
            gone.state = QStringLiteral("error");
            gone.error = makeError(QStringLiteral("node_failed"),
                                       QStringLiteral("%1 no longer has the job (HTTP %2).").arg(job->server.recipe).arg(code),
                                       QStringLiteral("Run the node again."));
            return change(id, gone);
        }
        const QJsonObject answer = jsonBody(reply);
        if (answer.value("status").toString() == QLatin1String("completed"))
            return finishJob(id, answer);
        change(id, statusOf(answer));
    });
}

void OmarchyProvider::finishJob(const QString &id, const QJsonObject &answer) {
    // The result is inline in the completed job (api.md "Completion Result") and expires with it
    // (--completed-ttl-seconds, 600 by default), so it is written out now (8.6).
    const auto job = m_jobs.value(id);
    const QJsonObject result = answer.value("result").toObject();
    const QString format = result.value("output_format").toString(QStringLiteral("png"));
    const QJsonArray images = result.value("images").toArray();
    const QDir dir(job->request.outputDir);
    JobStatus status;
    if (images.isEmpty()) {
        status.state = QStringLiteral("error");
        status.error = jobError(QStringLiteral("%1 finished without an image.").arg(job->server.recipe));
        return change(id, status);
    }
    const QJsonObject first = images.first().toObject();
    const QString path = dir.absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    const QByteArray bytes = QByteArray::fromBase64(first.value("b64_json").toString().toLatin1());
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        status.state = QStringLiteral("error");
        status.error = makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path));
        return change(id, status);
    }
    file.close();

    // The values actually used (8.1) come from the server's generation record; a value it does
    // not report is the one that was asked for.
    const QJsonObject info = first.value("generation_info").toObject();
    const QJsonObject &params = job->request.params;
    QJsonObject used{{"model", job->model}, {"engine", kEngine}};
    const QJsonValue seed = seedOf(info.value("seed"));
    if (!seed.isUndefined())
        used.insert("seed", seed);
    else if (params.contains("seed"))
        used.insert("seed", params.value("seed"));
    for (const char *key : {"width", "height"}) {
        const QJsonValue value = info.contains(QLatin1String(key)) ? info.value(QLatin1String(key)) : params.value(QLatin1String(key));
        if (!value.isUndefined())
            used.insert(QLatin1String(key), value);
    }
    const QJsonValue steps = info.value("sample_params").toObject().value("sample_steps");
    if (!steps.isUndefined())
        used.insert("steps", steps);
    else if (params.contains("steps"))
        used.insert("steps", params.value("steps"));

    job->result = JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used};
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

void OmarchyProvider::change(const QString &id, const JobStatus &status) {
    const auto job = m_jobs.value(id);
    if (!job || sameStatus(job->status, status))
        return;
    job->status = status;
    if (status.isTerminal())
        job->endedAt = now();
    emit statusChanged(id, status);
}

} // namespace fugue
