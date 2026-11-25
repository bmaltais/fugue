#include "server_api.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

namespace fugue::serverapi {

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

// Whether the server's img_gen takes a mask: `features_by_mode.img_gen.mask_image`, else the
// deprecated `features` mirror (api.md "GET /sdcpp/v1/capabilities"), a flag that is true. A list of
// feature names that holds "mask_image" counts too (the recipes' `features`, api.md "Recipes").
bool featureOf(const QJsonObject &capabilities, const char *feature) {
    const auto lists = [feature](const QJsonValue &features) {
        if (features.isObject())
            return features.toObject().value(QLatin1String(feature)).toBool(false);
        return features.isArray() && features.toArray().contains(QLatin1String(feature));
    };
    const QJsonValue byMode = capabilities.value("features_by_mode").toObject().value("img_gen");
    return byMode.isUndefined() ? lists(capabilities.value("features")) : lists(byMode);
}

// Defaults (8.1 names) from sd-server's capabilities: defaults_by_mode.<mode> (img_gen or
// vid_gen), or the deprecated `defaults` mirror (api.md "GET /sdcpp/v1/capabilities").
bool maskImageOf(const QJsonObject &capabilities) { return featureOf(capabilities, "mask_image"); }

QJsonObject defaultsOf(const QJsonObject &capabilities, const char *modeName) {
    QJsonObject mode = capabilities.value("defaults_by_mode").toObject().value(QLatin1String(modeName)).toObject();
    if (mode.isEmpty())
        mode = capabilities.value("defaults").toObject();
    const QJsonObject sample = mode.value("sample_params").toObject();
    QJsonObject defaults;
    // A size of 0 is sd-server's "the model's own": no default, not a size (#72).
    for (const char *key : {"width", "height"})
        if (mode.value(QLatin1String(key)).toDouble() > 0)
            defaults.insert(QLatin1String(key), mode.value(QLatin1String(key)));
    if (sample.contains("sample_steps"))
        defaults.insert("steps", sample.value("sample_steps"));
    const QJsonObject guidance = sample.value("guidance").toObject();
    if (guidance.contains("txt_cfg"))
        defaults.insert("guidance", guidance.value("txt_cfg"));
    if (mode.value("fps").toDouble() > 0)
        defaults.insert("fps", mode.value("fps"));
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

// A server's error text: {"error": "..."} or {"error": {"message": "..."}}.
QString serverMessage(const QJsonObject &body) {
    const QJsonValue error = body.value("error");
    return error.isObject() ? error.toObject().value("message").toString() : error.toString();
}

// `text`: a text model's job, whose way to need less memory is a shorter prompt or answer.
Error jobError(const QString &message, bool text) {
    const bool memory = message.contains(QLatin1String("out of memory"), Qt::CaseInsensitive);
    return makeError(memory ? QStringLiteral("out_of_memory") : QStringLiteral("node_failed"),
                         message.isEmpty() ? QStringLiteral("The model server failed the job.") : message,
                         !memory ? QString()
                         : text  ? QStringLiteral("Stop other models to free memory, or ask for a shorter answer.")
                                 : QStringLiteral("Stop other models to free memory, or ask for a smaller size."));
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
        // One channel, white redrawn (api.md "Channel expectations"). The caller has checked that the
        // server lists the feature (maskImageOf); a request that has none sends no key at all.
        if (request.inputs.contains("mask") && encodeInput(request.inputs.value("mask"), &encoded, errors))
            body.insert("mask_image", encoded);
    }
    return body;
}

// A seed in generation_info is a decimal string (generation_metadata.md "Reproduction
// parameters"); accept a number too.
QJsonValue seedOf(const QJsonValue &seed) {
    if (seed.isString()) {
        bool ok = false;
        const qint64 value = seed.toString().toLongLong(&ok);
        return ok ? QJsonValue(value) : QJsonValue(QJsonValue::Undefined);
    }
    return seed.isDouble() ? seed : QJsonValue(QJsonValue::Undefined);
}

// The chat request for text.generate (8.1): the system prompt, then the user's prompt with
// its image as a data URL (OpenAI's image_url content part, which llama.cpp-style servers
// take). Parameters left out are the server's own.
QJsonObject chatBody(const QString &recipe, const JobRequest &request, Errors *errors) {
    const QJsonObject &params = request.params;
    QJsonArray messages;
    const QString system = params.value("system").toString();
    if (!system.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", system}});
    const QString prompt = params.value("prompt").toString();
    if (request.inputs.contains("image")) {
        QString encoded;
        if (!encodeInput(request.inputs.value("image"), &encoded, errors))
            return {};
        // The node sends a PNG, or a JPEG as it is, and says which (MediaHandle::format, from
        // the bytes, not the name).
        const bool jpeg = request.inputs.value("image").toObject().value("format").toString() == QLatin1String("jpeg");
        const QString mime = jpeg ? QStringLiteral("image/jpeg") : QStringLiteral("image/png");
        messages.append(QJsonObject{
            {"role", "user"},
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", prompt}},
                                   QJsonObject{{"type", "image_url"},
                                               {"image_url", QJsonObject{{"url", QStringLiteral("data:%1;base64,%2").arg(mime, encoded)}}}}}}});
    } else {
        messages.append(QJsonObject{{"role", "user"}, {"content", prompt}});
    }
    QJsonObject body{{"model", recipe}, {"messages", messages}, {"stream", true}};
    for (const char *key : {"seed", "max_tokens", "temperature"})
        if (params.contains(QLatin1String(key)))
            body.insert(QLatin1String(key), params.value(QLatin1String(key)));
    // The chat template's switch (Qwen3 templates; llama.cpp, vLLM and tensorfold take it). Not
    // OpenAI's reasoning_effort, whose "none" a strict server refuses. Unsaid: the model's way.
    if (params.value("thinking").isBool())
        body.insert("chat_template_kwargs", QJsonObject{{"enable_thinking", params.value("thinking").toBool()}});
    return body;
}

QByteArray multipartBody(const QList<FormPart> &parts, QByteArray *boundary) {
    *boundary = "----fugue" + QUuid::createUuid().toByteArray(QUuid::Id128);
    QByteArray body;
    for (const FormPart &part : parts) {
        body += "--" + *boundary + "\r\nContent-Disposition: form-data; name=\"" + part.name + "\"";
        if (!part.fileName.isEmpty())
            body += "; filename=\"" + part.fileName + "\"";
        if (!part.type.isEmpty())
            body += "\r\nContent-Type: " + part.type;
        body += "\r\n\r\n" + part.data + "\r\n";
    }
    return body + "--" + *boundary + "--\r\n";
}

bool ChatStream::feed(const QByteArray &bytes, bool atEnd) {
    m_unread += bytes;
    if (atEnd && !m_unread.isEmpty())
        m_unread += '\n';
    qsizetype end;
    while ((end = m_unread.indexOf('\n')) >= 0) {
        const QByteArray line = m_unread.left(end).trimmed();
        m_unread.remove(0, end + 1);
        if (!line.startsWith("data:"))
            continue;
        const QByteArray data = line.mid(5).trimmed();
        if (data == "[DONE]") {
            finished = true;
            continue;
        }
        const QJsonObject chunk = QJsonDocument::fromJson(data).object();
        if (chunk.contains("error")) {
            error = serverMessage(chunk);
            return false;
        }
        const QJsonObject choice = chunk.value("choices").toArray().first().toObject();
        if (choice.value("finish_reason").isString())
            finished = true;
        const QJsonObject delta = choice.value("delta").toObject();
        const QString content = delta.value("content").toString();
        if (!content.isEmpty()) {
            text += content;
            ++chunks;
            stage = QStringLiteral("writing");
        } else if (!delta.value("reasoning_content").toString().isEmpty() && text.isEmpty()) {
            stage = QStringLiteral("thinking");
        }
    }
    return true;
}

Result<JobResult> imageJobResult(const QJsonObject &answer, const JobRequest &request, const QJsonObject &model,
                                 const QString &engine, const QString &who) {
    const QJsonObject result = answer.value("result").toObject();
    const QString format = result.value("output_format").toString(QStringLiteral("png"));
    const QJsonArray images = result.value("images").toArray();
    if (images.isEmpty())
        return Result<JobResult>(jobError(QStringLiteral("%1 finished without an image.").arg(who)));
    const QJsonObject first = images.first().toObject();
    const QString path = QDir(request.outputDir).absoluteFilePath(QStringLiteral("image.") + format);
    QFile file(path);
    const QByteArray bytes = QByteArray::fromBase64(first.value("b64_json").toString().toLatin1());
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return Result<JobResult>(makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path)));
    file.close();

    // The values actually used (8.1) come from the server's generation record.
    const QJsonObject info = first.value("generation_info").toObject();
    const QJsonObject &params = request.params;
    QJsonObject used{{"model", model}, {"engine", engine}};
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
    return Result<JobResult>(JobResult{QJsonObject{{"image", QJsonObject{{"path", path}, {"format", format}}}}, used});
}

} // namespace fugue::serverapi
