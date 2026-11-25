#pragma once

// What the providers that talk to model servers over HTTP share: the omarchy adapter and the
// generic `http` provider (spec 8.7). Request bodies for the stable-diffusion.cpp native API
// (api.md "sdcpp API") and the OpenAI-compatible chat API, reading a job's status, a streamed
// chat answer, and the result of a finished image job. All of it is stateless: no network, no
// provider state.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "engine/provider.h"

class QNetworkReply;

namespace fugue::serverapi {

// The native API's root: the endpoint without its trailing /v1. Not the URL's origin, because a
// paired host's endpoint has a path (/local-ai/<recipe>/v1).
QString baseOf(const QString &endpoint);

QJsonObject jsonBody(QNetworkReply *reply);
int httpStatus(QNetworkReply *reply);

// Whether the server's img_gen has a feature (`mask_image`, `ref_images`):
// `features_by_mode.img_gen.<feature>`, else the deprecated `features` mirror, a flag that is
// true or a list that holds its name.
bool featureOf(const QJsonObject &capabilities, const char *feature);
// ... takes a mask.
bool maskImageOf(const QJsonObject &capabilities);
// Defaults (8.1 names) from sd-server's capabilities: defaults_by_mode.<mode> (img_gen or
// vid_gen), or the deprecated `defaults` mirror.
QJsonObject defaultsOf(const QJsonObject &capabilities, const char *modeName = "img_gen");
QJsonObject limitsOf(const QJsonObject &capabilities);

// A server's error text: {"error": "..."} or {"error": {"message": "..."}}.
QString serverMessage(const QJsonObject &body);
// `text`: a text model's job, whose way to need less memory is a shorter prompt or answer.
Error jobError(const QString &message, bool text = false);
// One sd-server job (async_jobs.cpp make_async_job_json) as a status (8.8). A completed job is
// reported as running until its result is on disk.
JobStatus statusOf(const QJsonObject &job);

// The file's bytes as base64, which every sd-server image field accepts. False, and an error
// in `errors`, when the file cannot be read.
bool encodeInput(const QJsonValue &input, QString *encoded, Errors *errors);
// The img_gen body for the standard parameters of 8.1. Parameters left out get the server's defaults.
QJsonObject imgGenBody(const JobRequest &request, Errors *errors);
// A seed in generation_info is a decimal string; a number is accepted too. Undefined when neither.
QJsonValue seedOf(const QJsonValue &seed);
// The chat request for text.generate (8.1) for the served model `model`.
QJsonObject chatBody(const QString &model, const JobRequest &request, Errors *errors);

// The first object of an array, an empty one for an empty array.
QJsonObject firstObject(const QJsonArray &array);

// A multipart/form-data body. A part with a file name is a file; one without is a plain field.
struct FormPart {
    QByteArray name;
    QByteArray fileName;
    QByteArray type;
    QByteArray data;
};
// The body of `parts`; `boundary` is the one it uses (for the Content-Type header).
QByteArray multipartBody(const QList<FormPart> &parts, QByteArray *boundary);

// A streamed chat answer (server-sent events): feed it the bytes as they arrive.
class ChatStream {
public:
    // Reads the whole lines so far (the last line too when `atEnd`). Returns false when the server
    // sent an error event; `error` then says it.
    bool feed(const QByteArray &bytes, bool atEnd);

    // Each `data:` line is a chunk whose delta adds to the answer. Reasoning is not part of it.
    QString text;
    int chunks = 0;
    bool finished = false; // [DONE] or a finish_reason came
    QString stage;         // "thinking" or "writing" once there is something to say
    QString error;         // the server's own words, when it sent an error event

private:
    QByteArray m_unread;
};

// A finished sd-server image job (its whole answer): the first image written to
// `<request.outputDir>/image.<format>` and the values used (8.1). A value the server does not
// report is the one that was asked for. `who` names the server in messages.
Result<JobResult> imageJobResult(const QJsonObject &answer, const JobRequest &request, const QJsonObject &model,
                                 const QString &engine, const QString &who);

} // namespace fugue::serverapi
