#include "remote_helper_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include "engine/helper_provider.h"

namespace fugue {

namespace {

constexpr int kDefaultPollMs = 500;
constexpr int kTransferTimeoutMs = 120000; // of silence: a large upload or download is not one
constexpr int kLostPolls = 3;              // status requests in a row that may fail before the job does

Error hostError(const QJsonObject &error) {
    const QString code = error.value("code").toString();
    const QString message = error.value("message").toString(QStringLiteral("The helper on the paired host failed."));
    // These keep their codes (8.5, 8.8, 9.5); everything else the host says is about this job's work.
    if (code == QLatin1String("out_of_memory") || code == QLatin1String("cancelled") ||
        code == QLatin1String("model_missing"))
        return makeError(code, message);
    return makeError(QStringLiteral("node_failed"), message);
}

// How a file goes to the host: a stage file by the input it is (their names are the host's to
// read back), an image by its extension.
QString formatOf(const QString &input, const QString &path) {
    if (input == QLatin1String("conditioning"))
        return input;
    if (input == QLatin1String("latent") || input == QLatin1String("init_latent"))
        return QStringLiteral("latent");
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jpeg") || suffix == QLatin1String("jpg"))
        return QStringLiteral("jpg");
    return suffix.isEmpty() ? QStringLiteral("png") : suffix;
}

// The file as {"data", "format"}, or why it cannot be read.
Result<QJsonObject> asData(const QString &input, const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("node_failed"),
                         QStringLiteral("Cannot read %1 to send it to the paired host: %2.").arg(path, file.errorString()));
    return QJsonObject{{"data", QString::fromLatin1(file.readAll().toBase64())}, {"format", formatOf(input, path)}};
}

// `value` with every {"path": p} replaced by the file's data; a list item by item.
Result<QJsonValue> dataOf(const QString &input, const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray()) {
            const Result<QJsonValue> one = dataOf(input, item);
            if (!one)
                return one.errors();
            items.append(one.value());
        }
        return QJsonValue(items);
    }
    const QJsonObject object = value.toObject();
    if (!object.contains(QStringLiteral("path")))
        return value;
    const Result<QJsonObject> data = asData(input, object.value(QStringLiteral("path")).toString());
    if (!data)
        return data.errors();
    return QJsonValue(data.value());
}

QString extensionOf(const QString &format) {
    if (format == QLatin1String("conditioning"))
        return QStringLiteral("conditioning");
    return format.isEmpty() ? QStringLiteral("bin") : format;
}

} // namespace

RemoteHelperProvider::RemoteHelperProvider(const ProviderInstance &instance, int pollMs, QObject *parent)
    : Provider(parent), m_instance(instance) {
    m_network.setTransferTimeout(kTransferTimeoutMs);
    m_poll.setInterval(pollMs > 0 ? pollMs : kDefaultPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RemoteHelperProvider::poll);
}

RemoteHelperProvider::~RemoteHelperProvider() = default;

Error RemoteHelperProvider::unreachable(const QString &why) const {
    return makeError(QStringLiteral("provider_unreachable"),
                     QStringLiteral("%1 (%2): %3").arg(m_instance.name, m_instance.address, why),
                     QStringLiteral("Check that the Fugue helper is running on the host and that its address and key in "
                                    "providers.json are right."));
}

QNetworkReply *RemoteHelperProvider::request(const QString &method, const QString &path, const QByteArray &body) {
    QNetworkRequest request(QUrl(m_instance.address + path));
    request.setRawHeader("Authorization", "Bearer " + m_instance.key.toUtf8());
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    return m_network.sendCustomRequest(request, method.toLatin1(), body);
}

Result<QJsonObject> RemoteHelperProvider::parse(QNetworkReply *reply) const {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    if (status == 401 || status == 403)
        return unreachable(QStringLiteral("the host refused the key (HTTP %1).").arg(status));
    if (object.value("error").isObject())
        return hostError(object.value("error").toObject());
    if (status == 0)
        return unreachable(QStringLiteral("cannot reach it: %1.").arg(reply->errorString()));
    if (status >= 400 || reply->error() != QNetworkReply::NoError)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("The paired host answered HTTP %1.").arg(status));
    return object;
}

void RemoteHelperProvider::post(const QString &op, const QJsonObject &body, Answer answer) {
    QNetworkReply *reply = request(QStringLiteral("POST"), QStringLiteral("/fugue/v1/") + op,
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, answer = std::move(answer)] {
        const Result<QJsonObject> parsed = parse(reply);
        reply->deleteLater();
        answer(parsed);
    });
}

void RemoteHelperProvider::describe(Reply<Description> reply) {
    post(QStringLiteral("describe"), {}, [reply](const Result<QJsonObject> &answer) {
        if (!answer)
            return reply(Result<Description>(answer.errors()));
        Description description;
        description.protocol = answer->value("protocol").toInt(1);
        // Per host: results are deterministic per machine and build (spec 8.10), so a stage run on the
        // host never answers from, or for, an entry made by another machine's helper (the cache key has the engine).
        description.engine = answer->value("engine").toString(QStringLiteral("sdcpp"));
        if (const QString host = answer->value("host").toString(); !host.isEmpty())
            description.engine += QLatin1Char('@') + host;
        description.sharesFilesystem = answer->value("shares_filesystem").toBool(false);
        if (answer->value("free_memory_bytes").isDouble())
            description.freeMemoryBytes = qint64(answer->value("free_memory_bytes").toDouble());
        QStringList names;
        for (const QJsonValue &name : answer->value("capabilities").toArray())
            names.append(name.toString());
        if (names.isEmpty())
            names = helperCapabilities();
        for (const QString &capability : std::as_const(names)) {
            OfferedModel files;
            files.title = QStringLiteral("Model files on the paired host");
            files.status = ModelStatus::Ready;
            description.capabilities.append(CapabilityOffer{capability, {files}});
        }
        reply(Result<Description>(description));
    });
}

void RemoteHelperProvider::submit(const JobRequest &request, Reply<QString> reply) {
    // The files go with the job, read now: a stage file of an earlier node is already in the run folder.
    QJsonObject inputs;
    for (auto it = request.inputs.begin(); it != request.inputs.end(); ++it) {
        const Result<QJsonValue> data = dataOf(it.key(), it.value());
        if (!data)
            return later(std::move(reply), Result<QString>(data.errors()));
        inputs.insert(it.key(), data.value());
    }
    // Edit references are files in the generation options (ref_images: [{"image": PATH}]).
    QJsonObject params = request.params;
    if (params.value("ref_images").isArray()) {
        QJsonArray references;
        for (const QJsonValue &item : params.value("ref_images").toArray()) {
            QJsonObject reference = item.toObject();
            if (reference.value("image").isString()) {
                const Result<QJsonObject> data = asData(QStringLiteral("ref_images"), reference.value("image").toString());
                if (!data)
                    return later(std::move(reply), Result<QString>(data.errors()));
                reference.insert("image", data.value());
            }
            references.append(reference);
        }
        params.insert("ref_images", references);
    }
    const QString outputDir = request.outputDir;
    post(QStringLiteral("submit"),
         QJsonObject{{"capability", request.capability}, {"params", params}, {"inputs", inputs}},
         [this, reply, outputDir](const Result<QJsonObject> &answer) {
             if (!answer)
                 return reply(Result<QString>(answer.errors()));
             const QString hostJob = answer->value("job").toString();
             Job job;
             job.hostJob = hostJob;
             job.outputDir = outputDir;
             job.status.state = QStringLiteral("queued");
             m_jobs.insert(hostJob, job);
             if (!m_poll.isActive())
                 m_poll.start();
             reply(Result<QString>(hostJob));
         });
}

void RemoteHelperProvider::status(const QString &job, Reply<JobStatus> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd())
        return later(std::move(reply),
                     Result<JobStatus>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    later(std::move(reply), Result<JobStatus>(found->status));
}

void RemoteHelperProvider::cancel(const QString &job, Reply<QString> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd())
        return later(std::move(reply),
                     Result<QString>(makeError(QStringLiteral("unknown_run"), QStringLiteral("No job %1.").arg(job))));
    if (found->status.isTerminal())
        return later(std::move(reply), Result<QString>(found->status.state));
    post(QStringLiteral("cancel"), QJsonObject{{"job", job}}, [this, job, reply](const Result<QJsonObject> &) {
        reply(Result<QString>(m_jobs.value(job).status.state));
    });
}

void RemoteHelperProvider::result(const QString &job, Reply<JobResult> reply) {
    const auto found = m_jobs.constFind(job);
    if (found == m_jobs.constEnd() || found->status.state != QLatin1String("done"))
        return later(std::move(reply), Result<JobResult>(makeError(QStringLiteral("unknown_run"),
                                                                   QStringLiteral("Job %1 has no result.").arg(job))));
    later(std::move(reply), Result<JobResult>(found->result));
}

void RemoteHelperProvider::poll() {
    bool active = false;
    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids) {
        Job &job = m_jobs[id];
        if (job.status.isTerminal())
            continue;
        active = true;
        if (job.polling)
            continue;
        job.polling = true;
        post(QStringLiteral("status"), QJsonObject{{"job", id}}, [this, id](const Result<QJsonObject> &answer) {
            if (!m_jobs.contains(id) || m_jobs.value(id).status.isTerminal())
                return;
            Job &job = m_jobs[id];
            if (!answer) {
                job.polling = false;
                // A host that cannot be reached for a few polls in a row has lost the job; an answer
                // that is an error is the job's (it ended).
                if (answer.error().code == QLatin1String("provider_unreachable") && ++job.failures < kLostPolls)
                    return;
                return fail(id, answer.error());
            }
            job.failures = 0;
            JobStatus status;
            status.state = answer->value("state").toString();
            status.progress = answer->value("progress").toDouble();
            status.stage = answer->value("stage").toString();
            if (status.state == QLatin1String("error"))
                status.error = hostError(answer->value("error").toObject());
            if (status.state == QLatin1String("done"))
                return fetch(id); // polling stays set until the files are here
            job.polling = false;
            if (status.state != job.status.state || status.progress != job.status.progress || status.stage != job.status.stage)
                change(id, status);
        });
    }
    if (!active)
        m_poll.stop();
}

void RemoteHelperProvider::fail(const QString &job, const Error &error) {
    JobStatus status;
    status.state = QStringLiteral("error");
    status.error = error;
    change(job, status);
}

void RemoteHelperProvider::change(const QString &job, const JobStatus &status) {
    Job &held = m_jobs[job];
    held.status = status;
    held.polling = false;
    emit statusChanged(job, status);
}

namespace {

// What the result's files are to be fetched as: each {"file": i, "format", "size"} in `outputs`.
struct Download {
    QString name;  // the output, with -k for the k-th of a list
    int file = 0;
    QString format;
    qint64 size = -1;
    QStringList where; // the path in outputs: {name} or {name, index}
};

} // namespace

void RemoteHelperProvider::fetch(const QString &id) {
    post(QStringLiteral("result"), QJsonObject{{"job", id}}, [this, id](const Result<QJsonObject> &answer) {
        if (!answer)
            return fail(id, answer.error());
        const QString outputDir = m_jobs.value(id).outputDir;
        const QJsonObject outputs = answer->value("outputs").toObject();
        // The files, in order, then each is downloaded one after the other.
        struct Pending {
            QJsonObject outputs;
            QJsonObject used;
            QList<Download> files;
            QJsonObject placed; // outputs with the paths
            int next = 0;
        };
        auto pending = std::make_shared<Pending>();
        pending->outputs = outputs;
        pending->used = answer->value("used").toObject();
        pending->placed = outputs;
        const auto isFile = [](const QJsonValue &value) { return value.isObject() && value.toObject().contains("file"); };
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (isFile(it.value())) {
                const QJsonObject file = it.value().toObject();
                pending->files.append({it.key(), file.value("file").toInt(), file.value("format").toString(),
                                       qint64(file.value("size").toDouble(-1)), {it.key()}});
            } else if (it.value().isArray()) {
                const QJsonArray list = it.value().toArray();
                for (qsizetype k = 0; k < list.size(); ++k)
                    if (isFile(list.at(k))) {
                        const QJsonObject file = list.at(k).toObject();
                        pending->files.append({QStringLiteral("%1-%2").arg(it.key()).arg(k + 1), file.value("file").toInt(),
                                               file.value("format").toString(), qint64(file.value("size").toDouble(-1)),
                                               {it.key(), QString::number(k)}});
                    }
            }
        }
        auto step = std::make_shared<std::function<void()>>();
        QPointer<RemoteHelperProvider> self(this);
        std::weak_ptr<std::function<void()>> again = step; // not owned by itself
        *step = [self, id, outputDir, pending, again] {
            if (!self)
                return;
            if (pending->next >= pending->files.size()) {
                if (!self->m_jobs.contains(id))
                    return;
                self->m_jobs[id].result = JobResult{pending->placed, pending->used};
                JobStatus done;
                done.state = QStringLiteral("done");
                done.progress = 1;
                return self->change(id, done);
            }
            const Download &download = pending->files.at(pending->next);
            QNetworkReply *reply = self->request(QStringLiteral("GET"),
                                                 QStringLiteral("/fugue/v1/result/%1/%2").arg(id).arg(download.file), {});
            const std::shared_ptr<std::function<void()>> next = again.lock();
            connect(reply, &QNetworkReply::finished, self.data(), [self, id, outputDir, pending, next, reply, download] {
                if (!self)
                    return;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray bytes = reply->readAll();
                const QString failed = status == 401 || status == 403 ? QStringLiteral("the host refused the key")
                                       : status == 0                  ? reply->errorString()
                                       : status >= 400                ? QStringLiteral("HTTP %1").arg(status)
                                       : download.size >= 0 && bytes.size() != download.size
                                           ? QStringLiteral("the file came short (%1 of %2 bytes)").arg(bytes.size()).arg(download.size)
                                           : QString();
                reply->deleteLater();
                if (!failed.isEmpty())
                    return self->fail(id, status == 0 || status == 401 || status == 403
                                              ? self->unreachable(QStringLiteral("cannot fetch %1: %2.").arg(download.name, failed))
                                              : makeError(QStringLiteral("node_failed"),
                                                          QStringLiteral("Cannot fetch %1 from the paired host: %2.").arg(download.name, failed)));
                const QString path = QDir(outputDir).filePath(download.name + QLatin1Char('.') + extensionOf(download.format));
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
                    return self->fail(id, makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path)));
                file.close();
                const QJsonObject placed{{"path", path}, {"format", download.format}};
                if (download.where.size() == 1) {
                    pending->placed.insert(download.where.first(), placed);
                } else {
                    QJsonArray list = pending->placed.value(download.where.first()).toArray();
                    list.replace(download.where.last().toInt(), placed);
                    pending->placed.insert(download.where.first(), list);
                }
                ++pending->next;
                (*next)();
            });
        };
        (*step)();
    });
}

} // namespace fugue
