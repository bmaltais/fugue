#include "omarchy_provider.h"

#include "engine/fugue_home.h"
#include "engine/paths.h"

#include "engine/json.h"
#include "engine/server_api.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fugue {

using namespace serverapi;

namespace {

// Fantasi gives each request 15 seconds (generator.cpp setTransferTimeout).
constexpr int kTransferTimeoutMs = 15000;
constexpr int kDefaultPollMs = 500;

const QString kEngine = QStringLiteral("sdcpp");
const QString kModelResource = QStringLiteral("fugue/image-model");
const QString kTextResource = QStringLiteral("fugue/text-model");
const QString kVideoResource = QStringLiteral("fugue/video-model");
const QString kMusicResource = QStringLiteral("fugue/music-model");
const QString kTextGenerate = QStringLiteral("text.generate");
const QString kVideoGenerate = QStringLiteral("video.generate");
const QString kAudioGenerate = QStringLiteral("audio.generate");
// A music server's adapters (LoRA files of its --adapters folder, listed by GET /props) are offered as
// models of a capability of their own, so a picker lists them and a job never takes one as its music model.
const QString kAudioAdapter = QStringLiteral("audio.adapter");
const QString kAdapterResource = QStringLiteral("fugue/music-adapter");
// Engine ids of a row that names no runtime.
const QString kMusicEngine = QStringLiteral("acestep");

// A chat answer streams a chunk per token, but the first can be long in coming: the prompt
// (and an image) is read first, and a thinking model reasons before it answers. Silence this
// long ends the job; a server that streams nothing at all (stream ignored) answers in one go.
constexpr int kChatTransferTimeoutMs = 300 * 1000;

// A remote host's models come from network.json, which only a tailnet survey writes, and a survey
// lists the models running at that moment (a unit that is restarting is missing). A listing finding
// the file older than this runs the survey itself, at most once per window.
constexpr qint64 kSurveyFreshMs = 2 * 60 * 1000;
// `omarchy-local-ai network --json` asks every online host over the tailnet and rewrites network.json (its listing is
// thrown away: the file is what is read); it takes seconds. `--json`, not the hidden `--write`, which is not part of its interface.
constexpr int kSurveyTimeoutMs = 10 * 1000;


QByteArray readKey(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

} // namespace

struct OmarchyProvider::Job {
    Server server;
    QString serverId; // the sd-server's job id; empty for a chat (text) job
    JobRequest request;
    QJsonObject model; // the handle of the model that runs it
    JobStatus status;
    JobResult result;
    bool asking = false; // a poll is in flight
    int failures = 0;    // polls in a row that got no answer
    qint64 endedAt = 0;  // when it reached a terminal state, in ms
    // A chat job (text.generate) is one streamed request, not a job polled on the server.
    bool chat = false;
    QPointer<QNetworkReply> stream;
    ChatStream reader;     // the answer so far, and whether the stream said it is over
    // A music job (audio.generate) is two ace-server jobs in turn: the song written (/lm), then
    // rendered (/synth). `serverId` is the one in flight.
    bool music = false;
    bool rendering = false; // the second job is the one in flight
    bool understanding = false; // a Describe Song job: one /understand job, whose answer is text, not audio
    QJsonObject song;       // the written song (an AceRequest), sent to /synth as it is
};

namespace {

// Polls in a row without an answer before the job counts as lost.
constexpr int kMaxPollFailures = 3;

// How long a finished job is kept when nobody takes its result: as long as the server
// keeps its own (--completed-ttl-seconds, 8.6).
constexpr qint64 kFinishedJobTtlMs = 600 * 1000;

// A server's answer to a job it did not take: not there, the key refused, still loading, or
// the job refused with the server's own words.
Error refusal(const QString &recipe, const QString &base, int code, const QJsonObject &answer, bool text) {
    if (code == 0)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 is not answering at %2.").arg(recipe, base),
                         QStringLiteral("Check that it is running: omarchy-agent-local status."));
    if (code == 401 || code == 403)
        return makeError(QStringLiteral("provider_unreachable"), QStringLiteral("%1 refused the key.").arg(recipe),
                         QStringLiteral("Check the key, or pair again."));
    if (code == 503)
        return makeError(QStringLiteral("model_not_running"), QStringLiteral("%1 has no model loaded yet.").arg(recipe),
                         QStringLiteral("Wait for it to finish loading."));
    const QString said = serverMessage(answer);
    return jobError(QStringLiteral("%1 refused the job: %2").arg(recipe, said.isEmpty() ? QStringLiteral("HTTP %1").arg(code) : said),
                    text);
}

bool sameStatus(const JobStatus &a, const JobStatus &b) {
    return a.state == b.state && a.progress == b.progress && a.stage == b.stage &&
           a.preview == b.preview && a.error.code == b.error.code;
}

// The vid_gen body for the parameters of video.generate (8.1; api.md "vid_gen"). Always WebM, and
// the VAE tiled as Fantasi does for video. Every LoRA the server lists goes with the job: it
// applies none by itself, and a model such as MiniMax H3 ships its few-step LoRA that way (8.7).
QJsonObject vidGenBody(const JobRequest &request, const QJsonArray &loras, Errors *errors) {
    const QJsonObject &params = request.params;
    QJsonObject body{{"prompt", params.value("prompt").toString()},
                     {"seed", params.contains("seed") ? params.value("seed") : QJsonValue(-1)},
                     {"output_format", "webm"},
                     {"vae_tiling_params", QJsonObject{{"enabled", true}}}};
    for (const char *key : {"negative_prompt", "width", "height", "video_frames", "fps"})
        if (params.contains(QLatin1String(key)))
            body.insert(QLatin1String(key), params.value(QLatin1String(key)));
    QJsonObject sample;
    if (params.contains("steps"))
        sample.insert("sample_steps", params.value("steps"));
    if (params.contains("guidance"))
        sample.insert("guidance", QJsonObject{{"txt_cfg", params.value("guidance")}});
    if (!sample.isEmpty())
        body.insert("sample_params", sample);
    QJsonArray lora;
    for (const QJsonValue &each : loras)
        lora.append(QJsonObject{{"path", each.toObject().value("path").toString()}, {"multiplier", 1.0}});
    if (!lora.isEmpty())
        body.insert("lora", lora);
    QString encoded;
    if (request.inputs.contains("image") && encodeInput(request.inputs.value("image"), &encoded, errors))
        body.insert("init_image", encoded);
    return body;
}

// The /lm body for audio.generate (8.1): ace-server's AceRequest fields caption, lyrics, duration,
// vocal_language and seed (docs/ARCHITECTURE.md, "Request JSON reference"), and the ones a node may set:
// bpm, keyscale, timesignature (the LM fills what is left out), lm_temperature and use_cot_caption
// (false keeps the caption as written). A seed of -1 is random.
QJsonObject aceBody(const JobRequest &request) {
    const QJsonObject &params = request.params;
    QJsonObject body{{"caption", params.value("prompt").toString()},
                     {"lyrics", params.value("lyrics").toString()},
                     {"duration", params.value("duration")}};
    if (params.contains("language"))
        body.insert("vocal_language", params.value("language"));
    if (params.contains("seed"))
        body.insert("seed", params.value("seed"));
    for (const auto &[from, to] : {std::pair{"bpm", "bpm"}, {"key", "keyscale"}, {"time_signature", "timesignature"},
                                   {"creativity", "lm_temperature"}, {"keep_description", "use_cot_caption"}}) {
        if (!params.contains(QLatin1String(from)))
            continue;
        // keep_description is the opposite of the server's use_cot_caption.
        body.insert(QLatin1String(to), QLatin1String(from) == QLatin1String("keep_description")
                                           ? QJsonValue(!params.value("keep_description").toBool())
                                           : params.value(QLatin1String(from)));
    }
    return body;
}

// How a song is saved: `quality` of the node is mp3 (the server's own 128 kb/s, nothing sent), mp3-320, or
// lossless, a 24-bit WAV (ace-server's output_format wav24: the DiT's float output kept to 24 bits; wav16
// throws away what a studio file keeps, wav32 is float that some players cannot read).
struct SongFile {
    QString extension;  // the file's extension, and its format's name
    QJsonObject fields; // output_format and, for a high mp3, mp3_bitrate
};

SongFile songFile(const QJsonObject &params) {
    const QString quality = params.value("quality").toString();
    if (quality == QLatin1String("lossless"))
        return {QStringLiteral("wav"), QJsonObject{{"output_format", "wav24"}}};
    if (quality == QLatin1String("mp3-320"))
        return {QStringLiteral("mp3"), QJsonObject{{"output_format", "mp3"}, {"mp3_bitrate", 320}}};
    return {QStringLiteral("mp3"), QJsonObject{{"output_format", "mp3"}}};
}

// What a /synth request takes from the node whichever way it is made (a written song, or a cover or a repaint):
// the file's format, and an adapter with its strength. An adapter is merged into the DiT when it loads, so the
// first song with a new one makes the server reload the DiT; none leaves the server's own defaults (no adapter).
void addSynthOptions(QJsonObject &body, const QJsonObject &params) {
    const QJsonObject file = songFile(params).fields;
    for (auto it = file.begin(); it != file.end(); ++it)
        body.insert(it.key(), it.value());
    if (params.contains("adapter")) {
        body.insert("adapter", params.value("adapter"));
        body.insert("adapter_scale", params.value("adapter_scale").toDouble(1.0));
    }
}

// The /synth body of Remix Song and Repaint Song: the LM is skipped, caption and lyrics reach the DiT as
// they are (docs/ARCHITECTURE.md, "Generation modes"). `task` is cover, cover-nofsq or repaint.
QJsonObject aceEditBody(const JobRequest &request) {
    const QJsonObject &params = request.params;
    QJsonObject body{{"task_type", params.value("task")},
                     {"caption", params.value("prompt").toString()},
                     {"lyrics", params.value("lyrics").toString()}};
    addSynthOptions(body, params);
    for (const auto &[from, to] : {std::pair{"seed", "seed"}, {"strength", "audio_cover_strength"},
                                   {"start", "repainting_start"}, {"end", "repainting_end"}, {"steps", "inference_steps"}})
        if (params.contains(QLatin1String(from)))
            body.insert(QLatin1String(to), params.value(QLatin1String(from)));
    return body;
}

// The files a /synth job may carry beside its request: the song to work on (`audio`) and a recording to take
// the voice and timbre from (`ref_audio`), named as ace-server's multipart form names them (docs/ARCHITECTURE.md,
// "Endpoints": request + audio|src_latents + ref_audio|ref_latents).
const char *const kSynthFiles[][2] = {{"audio", "source audio"}, {"ref_audio", "style reference"}};

// The files of the job that /synth takes, as the multipart form's parts: the request as JSON, then each file with
// its own name (every part is sent with a file name, as the server reads them). False, and `error`, when one cannot
// be read.
bool synthForm(const QJsonObject &body, const QJsonObject &inputs, QByteArray *form, QByteArray *boundary, Error *error) {
    QList<FormPart> parts{{"request", "request.json", "application/json", QJsonDocument(body).toJson(QJsonDocument::Compact)}};
    for (const auto &file : kSynthFiles) {
        if (!inputs.contains(QLatin1String(file[0])))
            continue;
        const QString path = inputs.value(QLatin1String(file[0])).toObject().value("path").toString();
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            *error = makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read the %1 %2.").arg(QLatin1String(file[1]), path));
            return false;
        }
        parts.append({file[0], QFileInfo(path).fileName().toUtf8(), "application/octet-stream", source.readAll()});
    }
    *form = multipartBody(parts, boundary);
    return true;
}

// The audio of a /synth result: multipart/mixed, whose first part is the song (the others are
// latents). The boundary is the first line; parts are bodies between its lines.
QByteArray firstPart(const QByteArray &body) {
    const qsizetype lineEnd = body.indexOf("\r\n");
    if (lineEnd <= 0)
        return {};
    const QByteArray boundary = "\r\n" + body.left(lineEnd);
    const qsizetype start = body.indexOf("\r\n\r\n");
    if (start < 0)
        return {};
    const qsizetype end = body.indexOf(boundary, start + 4);
    return end < 0 ? QByteArray() : body.mid(start + 4, end - start - 4);
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
                                     const QString &path, const QByteArray &body, int timeoutMs,
                                     const QByteArray &contentType) {
    QNetworkRequest request(QUrl(server.base + path));
    if (timeoutMs > 0)
        request.setTransferTimeout(timeoutMs);
    request.setRawHeader("Authorization", "Bearer " + server.key);
    if (!body.isEmpty() || method == "POST")
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    return m_network.sendCustomRequest(request, method, body);
}

void OmarchyProvider::runSurvey(std::function<void(bool)> finished) {
    if (m_surveyRunner)
        return m_surveyRunner(std::move(finished));
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    // finished fires once: a timeout kills the process, which then reports through the same signal.
    auto done = std::make_shared<std::function<void(bool)>>(std::move(finished));
    const auto report = [process, done](bool ok) {
        if (*done)
            std::exchange(*done, nullptr)(ok);
        process->deleteLater();
    };
    connect(process, &QProcess::finished, this, [process, report](int code, QProcess::ExitStatus status) {
        report(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [report](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            report(false);
    });
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    process->start(QStringLiteral("omarchy-local-ai"), {QStringLiteral("network"), QStringLiteral("--json")});
    timer->start(kSurveyTimeoutMs);
}

// True when a remote listing should run the survey before it reads network.json.
bool OmarchyProvider::surveyDue() const {
    if (m_instance.address.isEmpty())
        return false;
    const Result<QJsonObject> network = readJsonObject(QDir(m_paths.stateDir).filePath("network.json"));
    if (!network) // none means no paired host: today's message; unreadable is said by discover()
        return false;
    const qint64 at = now();
    if (m_surveyedMs && at - m_surveyedMs < kSurveyFreshMs)
        return false;
    return at - qint64(network->value("updated_ms").toDouble()) > kSurveyFreshMs;
}

void OmarchyProvider::discover(std::function<void(const Result<Discovery> &)> done) {
    if (m_surveying) {
        // A survey is running: this listing waits for it, so none reads the old file meanwhile.
        m_surveyWaiters.append([this, done = std::move(done)]() mutable { discover(std::move(done)); });
        return;
    }
    if (surveyDue()) {
        m_surveying = true;
        m_surveyedMs = now(); // a failure counts too: one try per window
        runSurvey([this, done = std::move(done)](bool) mutable {
            // On failure the file as it is serves, as it did before the survey was asked for.
            m_surveying = false;
            discover(std::move(done));
            const auto waiters = std::exchange(m_surveyWaiters, {});
            for (const auto &waiter : waiters)
                waiter();
        });
        return;
    }
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
        const Result<QJsonObject> status = readJsonObject(state.filePath("status.json"));
        if (!status && status.error().code == QLatin1String("file_not_found"))
            return fail(QStringLiteral("Omarchy's local models are not set up on this machine (no %1).")
                            .arg(state.filePath("status.json")),
                        QStringLiteral("Install omarchy-local-ai, then set up an image model: "
                                       "omarchy-agent-local use <recipe>."));
        // There but unreadable is not "no models": say so, and it is usually worth another try.
        if (!status)
            return fail(status.error().message, QStringLiteral("Try again; omarchy-agent-local may be rewriting it."));
        const QJsonObject models = status->value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it)
            rows.append(it.value().toObject());
        if (key.isEmpty())
            key = readKey(config.filePath("key"));
        if (key.isEmpty())
            return fail(QStringLiteral("No key to talk to Omarchy's model servers (no %1).").arg(config.filePath("key")),
                        QStringLiteral("Set up a model with omarchy-agent-local, which writes the key, "
                                       "or set this provider's key in %1.").arg(providersFilePath()));
    } else {
        // No network.json means no paired host; one that cannot be read is said as such.
        const Result<QJsonObject> network = readJsonObject(state.filePath("network.json"));
        if (!network && network.error().code != QLatin1String("file_not_found"))
            return fail(network.error().message, QStringLiteral("Try again; omarchy-agent-local may be rewriting it."));
        const QJsonObject models = network ? network->value("models").toObject() : QJsonObject();
        for (auto it = models.begin(); it != models.end(); ++it) {
            const QJsonObject row = it.value().toObject();
            if (row.value("dns").toString() == dns && row.value("paired").toBool())
                rows.append(row);
        }
        const QString pairHint = QStringLiteral("Pair with it: omarchy-agent-local pair %1").arg(host);
        // A paired host with no model to offer is said as such, not as unpaired: its state
        // is in `hosts` (agent_local.py survey_network), and an online host lists only the
        // models it runs.
        const QJsonObject hostRow = network ? network->value("hosts").toObject().value(host).toObject() : QJsonObject();
        if (rows.isEmpty() && hostRow.value("paired").toBool()) {
            const QString away = hostRow.value("state").toString();
            if (away == QLatin1String("offline") || away == QLatin1String("unreachable"))
                return fail(QStringLiteral("%1 is %2.").arg(host, away),
                            QStringLiteral("Turn on %1, or check its tailnet connection.").arg(host));
            return fail(QStringLiteral("%1 runs no model it shares with this machine.").arg(host),
                        QStringLiteral("Start a model on %1 and share it there: omarchy-agent-local share <recipe>").arg(host));
        }
        if (rows.isEmpty())
            return fail(QStringLiteral("This machine is not paired with %1.").arg(dns), pairHint);
        if (key.isEmpty())
            key = readKey(config.filePath(QStringLiteral("remotes/%1.key").arg(host)));
        if (key.isEmpty())
            return fail(QStringLiteral("No key for %1 (no %2).").arg(dns, config.filePath("remotes/" + host + ".key")),
                        pairHint);
    }

    // One model per image or text row; running servers are asked what they can do.
    struct Pending {
        Discovery discovery;
        QList<OfferedModel> models;
        QList<bool> edits;
        QStringList kinds; // each row's kind: image, video, music or text
        // Models a server offers for a mode its row's kind does not name (supported_modes of its
        // capabilities): a video row that also serves img_gen, an image row that also serves vid_gen.
        QList<OfferedModel> extraImages;
        QList<bool> extraImageEdits;
        QList<OfferedModel> extraVideos;
        QList<OfferedModel> adapters; // the music servers' (GET /props), a capability of their own
        int waiting = 0;
        std::function<void(const Result<Discovery> &)> done;
    };
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    const auto assemble = [pending] {
        Description &description = pending->discovery.description;
        description.engine = kEngine;
        CapabilityOffer generate{QStringLiteral("image.generate"), {}};
        CapabilityOffer edit{QStringLiteral("image.edit"), {}};
        CapabilityOffer text{kTextGenerate, {}};
        CapabilityOffer video{kVideoGenerate, {}};
        CapabilityOffer music{kAudioGenerate, {}};
        for (qsizetype i = 0; i < pending->models.size(); ++i) {
            const QString &kind = pending->kinds.at(i);
            if (kind == QLatin1String("text")) {
                text.models.append(pending->models.at(i));
            } else if (kind == QLatin1String("video")) {
                video.models.append(pending->models.at(i));
            } else if (kind == QLatin1String("music")) {
                music.models.append(pending->models.at(i));
            } else {
                generate.models.append(pending->models.at(i));
                if (pending->edits.at(i))
                    edit.models.append(pending->models.at(i));
            }
        }
        for (qsizetype i = 0; i < pending->extraImages.size(); ++i) {
            generate.models.append(pending->extraImages.at(i));
            if (pending->extraImageEdits.at(i))
                edit.models.append(pending->extraImages.at(i));
        }
        video.models.append(pending->extraVideos);
        const CapabilityOffer adapters{kAudioAdapter, pending->adapters};
        for (const CapabilityOffer &offer : {generate, edit, text, video, music, adapters})
            if (!offer.models.isEmpty())
                description.capabilities.append(offer);
        pending->done(Result<Discovery>(pending->discovery));
    };

    const QString where = remote ? QStringLiteral(" on %1").arg(host) : QString();
    for (const QJsonObject &row : rows) {
        const QString kind = row.value("kind").toString();
        const bool text = kind == QLatin1String("text");
        const bool music = kind == QLatin1String("music");
        if (kind != QLatin1String("image") && kind != QLatin1String("video") && !text && !music)
            continue;
        const QString recipe = row.value("recipe").toString();
        const QString unit = row.value("unit").toString();
        const Server server{recipe, baseOf(row.value("endpoint").toString()), key,
                            row.value("runtime").toString(), {}, {}};
        pending->discovery.servers.insert(recipe, server);

        OfferedModel model;
        model.handle = QJsonObject{{"resource", text    ? kTextResource
                                                : music ? kMusicResource
                                                : kind == QLatin1String("video") ? kVideoResource
                                                                                 : kModelResource},
                                   {"name", recipe}};
        model.title = row.value("name").toString();
        if (text) {
            // What it takes is known for a paired host's row only (network.json); a local row
            // says nothing (agent_local.py model_record), so nothing is claimed for it.
            if (row.value("input").isArray())
                model.properties.insert("input", row.value("input"));
            if (row.value("max_tokens").toInt() > 0)
                model.limits.insert("max_tokens", row.value("max_tokens"));
        } else if (music) {
            // ace-server makes 10 to 600 seconds (docs/ARCHITECTURE.md, "Request JSON reference").
            model.limits = QJsonObject{{"min_duration", 10}, {"max_duration", 600}};
        } else if (kind == QLatin1String("video")) {
            model.properties = QJsonObject{{"input", row.value("input").isArray() ? row.value("input") : QJsonArray{"text"}}};
        } else {
            model.properties = QJsonObject{{"edit", row.value("edit").toBool()},
                                           {"input", row.value("input").isArray() ? row.value("input")
                                                                                   : QJsonArray{"text"}}};
        }
        const QString command = QStringLiteral("omarchy-agent-local start ") + recipe;
        const QString offline = row.value("offline").toString();
        if (!offline.isEmpty()) {
            model.status = ModelStatus::Unavailable;
            model.reason = QStringLiteral("%1 is %2.").arg(host, offline);
            model.hint = QStringLiteral("Turn on %1, or check its tailnet connection.").arg(host);
        } else if (unit == QLatin1String("active") || remote) {
            // A paired host's unit in network.json is only as fresh as the agents panel's last
            // survey, so the server's own answer to the probe below decides.
            model.status = ModelStatus::Ready;
        } else if (unit == QLatin1String("activating")) {
            model.status = ModelStatus::Loading;
            model.reason = QStringLiteral("%1 is starting.").arg(recipe);
            model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
        } else if (unit == QLatin1String("installing")) {
            model.status = ModelStatus::Unavailable;
            model.reason = QStringLiteral("%1 is still being set up.").arg(recipe);
            model.hint = QStringLiteral("Wait for its setup to finish, then start it.");
        } else {
            model.status = ModelStatus::Unavailable;
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
        pending->kinds.append(kind);
        if (model.status != ModelStatus::Ready)
            continue;

        // An sd-server's capabilities, or a text server's model list (the OpenAI-compatible API
        // omarchy-local-ai checks after a start, docs/engine.md).
        ++pending->waiting;
        const bool video = kind == QLatin1String("video");
        QNetworkReply *reply = send(server, "GET",
                                    text    ? QStringLiteral("/v1/models")
                                    : music ? QStringLiteral("/health")
                                            : QStringLiteral("/sdcpp/v1/capabilities"));
        connect(reply, &QNetworkReply::finished, this, [pending, reply, index, recipe, server, assemble, where, command, text, music, video, this] {
            reply->deleteLater();
            OfferedModel &model = pending->models[index];
            const int code = httpStatus(reply);
            const QJsonObject body = jsonBody(reply);
            if (code == 200) {
                if (!text && !music) {
                    model.defaults = defaultsOf(body, video ? "vid_gen" : "img_gen");
                    model.limits = limitsOf(body);
                    // `inputs` (8.1): what the model takes beyond the prompt that not every provider does.
                    if (!video && maskImageOf(body))
                        model.properties.insert("inputs", QJsonArray{"mask"});
                }
                if (video)
                    pending->discovery.servers[recipe].loras = body.value("loras").toArray();
                if (music) {
                    // ace-server lists the files of its --adapters folder in /props (a server of an older
                    // build has no such call: no adapters). Asked before this row counts as probed.
                    ++pending->waiting;
                    QNetworkReply *props = send(server, "GET", QStringLiteral("/props"));
                    connect(props, &QNetworkReply::finished, this, [pending, props, recipe, assemble] {
                        props->deleteLater();
                        if (httpStatus(props) == 200)
                            for (const QJsonValue &entry : jsonBody(props).value("adapters").toArray()) {
                                const QString name = entry.isObject() ? entry.toObject().value("name").toString() : entry.toString();
                                if (name.isEmpty())
                                    continue;
                                pending->discovery.servers[recipe].adapters.append(name);
                                OfferedModel adapter;
                                adapter.handle = QJsonObject{{"resource", kAdapterResource}, {"name", name}};
                                adapter.title = name;
                                adapter.status = ModelStatus::Ready;
                                adapter.properties = QJsonObject{{"model", recipe}};
                                pending->adapters.append(adapter);
                            }
                        if (--pending->waiting == 0)
                            assemble();
                    });
                }
                if (!text && !music) {
                    // A model is offered for every mode its server says it serves, not only the
                    // one its row's kind names. Absent supported_modes, the kind decides alone.
                    const QJsonArray modes = body.value("supported_modes").toArray();
                    const bool images = modes.contains(QStringLiteral("img_gen"));
                    const bool videos = modes.contains(QStringLiteral("vid_gen"));
                    OfferedModel other = model;
                    if (video && images) {
                        other.handle.insert("resource", kModelResource);
                        other.defaults = defaultsOf(body, "img_gen");
                        const QJsonObject features = body.value("features_by_mode").toObject().value("img_gen").toObject();
                        // Reference images are what image.edit sends (8.1); no ref_images, no edit.
                        const bool edits = features.value("ref_images").toBool();
                        other.properties = QJsonObject{{"edit", edits}, {"input", model.properties.value("input")}};
                        pending->extraImages.append(other);
                        pending->extraImageEdits.append(edits);
                    } else if (!video && videos) {
                        other.handle.insert("resource", kVideoResource);
                        other.defaults = defaultsOf(body, "vid_gen");
                        other.properties = QJsonObject{{"input", model.properties.value("input")}};
                        pending->extraVideos.append(other);
                    }
                }
            } else if (code == 503 && (text || music || body.value("code").toString() == QLatin1String("model_loading"))) {
                // llama.cpp-style servers answer 503 while the weights load.
                model.status = ModelStatus::Loading;
                model.reason = QStringLiteral("%1 is loading its model.").arg(recipe);
                model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
            } else if (code == 401 || code == 403) {
                model.status = ModelStatus::Unavailable;
                model.reason = QStringLiteral("%1 refused the key.").arg(recipe);
                model.hint = m_instance.key.isEmpty()
                                 ? QStringLiteral("Check Omarchy's key file, or pair again.")
                                 : QStringLiteral("Check this provider's key in %1.").arg(providersFilePath());
            } else if (code == 502 || code == 404) {
                // The share proxy's answers for a model whose port refuses (stopped) or that is not
                // shared (agent_local.py make_share_proxy _forward).
                model.status = ModelStatus::Unavailable;
                model.reason = QStringLiteral("%1 is not running%2.").arg(recipe, where);
                model.hint = QStringLiteral("Start it%1: %2").arg(where, command);
            } else {
                model.status = ModelStatus::Unavailable;
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
            request.capability != QLatin1String("image.edit") && request.capability != kTextGenerate &&
            request.capability != kVideoGenerate && request.capability != kAudioGenerate)
            return reply(Result<QString>(makeError(QStringLiteral("capability_unavailable"),
                                                   QStringLiteral("The omarchy provider does not offer %1.").arg(request.capability))));
        // The same rules as choosing among instances (8.3, 8.4), within this one.
        Requirement requirement;
        requirement.capability = request.capability;
        requirement.model = request.model;
        requirement.nodePin = self->m_instance.name;
        const auto chosen = selectProvider({Candidate{self->m_instance, Result<Description>(found->description), std::nullopt}}, requirement);
        if (!chosen.ok())
            return reply(Result<QString>(chosen.errors()));
        if (request.capability == kTextGenerate) {
            const QJsonObject model = chosen->model.handle;
            return self->submitChat(found->servers.value(model.value("name").toString()), request, model, reply);
        }
        if (request.capability == kAudioGenerate) {
            const QJsonObject model = chosen->model.handle;
            return self->submitSong(found->servers.value(model.value("name").toString()), request, model, reply);
        }
        const bool video = request.capability == kVideoGenerate;
        const QJsonObject model = chosen->model.handle;
        const Server server = found->servers.value(model.value("name").toString());
        // A mask goes only to a server that lists mask_image: otherwise it would be dropped and the
        // whole picture redrawn (14.4). The node checks this before the job; so does the provider.
        if (!video && request.inputs.contains("mask") &&
            !chosen->model.properties.value("inputs").toArray().contains(QStringLiteral("mask")))
            return reply(Result<QString>(makeError(
                QStringLiteral("mask_unsupported"),
                QStringLiteral("%1 does not take a mask: its server does not list mask_image.").arg(server.recipe),
                QStringLiteral("Use a model whose server lists mask_image, or redraw the part staged with the Fugue helper."))));
        Errors errors;
        const QJsonObject body = video ? vidGenBody(request, server.loras, &errors) : imgGenBody(request, &errors);
        if (hasErrors(errors))
            return reply(Result<QString>(errors));

        QNetworkReply *posted = self->send(server, "POST", video ? QStringLiteral("/sdcpp/v1/vid_gen") : QStringLiteral("/sdcpp/v1/img_gen"),
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
            reply(Result<QString>(refusal(server.recipe, server.base, code, answer, false)));
        });
    });
}

void OmarchyProvider::submitChat(const Server &server, const JobRequest &request, const QJsonObject &model,
                                 Reply<QString> reply) {
    Errors errors;
    const QJsonObject body = chatBody(server.recipe, request, &errors);
    if (hasErrors(errors))
        return reply(Result<QString>(errors));
    auto job = std::make_shared<Job>();
    job->chat = true;
    job->server = server;
    job->request = request;
    job->model = model;
    job->status.state = QStringLiteral("running");
    job->status.stage = QStringLiteral("reading");
    const QString id = QStringLiteral("%1/chat-%2").arg(server.recipe).arg(++m_chats);
    m_jobs.insert(id, job);
    job->stream = send(server, "POST", QStringLiteral("/v1/chat/completions"),
                       QJsonDocument(body).toJson(QJsonDocument::Compact), kChatTransferTimeoutMs);
    connect(job->stream, &QNetworkReply::readyRead, this, [this, id] { readChat(id); });
    connect(job->stream, &QNetworkReply::finished, this, [this, id] { finishChat(id); });
    reply(Result<QString>(id));
}

// The stream's whole lines so far (server-sent events): each `data:` line is a chunk whose
// delta adds to the answer. Reasoning (`reasoning_content`) is not part of it. At the end the
// last line counts without its line end.
void OmarchyProvider::readChat(const QString &id, bool atEnd) {
    const auto job = m_jobs.value(id);
    if (!job || !job->stream || job->status.isTerminal())
        return;
    const QByteArray type = job->stream->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    if (!type.contains("event-stream") || httpStatus(job->stream) >= 400)
        return; // a whole JSON answer, read when it is over
    const bool fine = job->reader.feed(job->stream->readAll(), atEnd);
    JobStatus status = job->status;
    if (!job->reader.stage.isEmpty())
        status.stage = job->reader.stage;
    if (!fine) {
        status.state = QStringLiteral("error");
        status.error = jobError(QStringLiteral("%1 failed the job: %2").arg(job->server.recipe, job->reader.error), true);
    }
    // Progress against the longest answer allowed, when one is known, in whole percent.
    const int longest = job->request.params.value("max_tokens").toInt(0);
    if (longest > 0)
        status.progress = std::min(0.99, std::floor(100.0 * job->reader.chunks / longest) / 100.0);
    change(id, status);
    if (status.state == QLatin1String("error"))
        job->stream->abort();
}

void OmarchyProvider::finishChat(const QString &id) {
    const auto job = m_jobs.value(id);
    if (!job || !job->stream)
        return;
    QNetworkReply *reply = job->stream;
    reply->deleteLater();
    if (job->status.isTerminal())
        return; // cancelled, or failed by an error event
    readChat(id, true);
    if (job->status.isTerminal())
        return;
    const Server &server = job->server;
    const int code = httpStatus(reply);
    const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
    const bool streamed = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().contains("event-stream");
    JobStatus status;
    status.state = QStringLiteral("error");
    if (code == 0 || code >= 300) {
        const bool silent = reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError;
        status.error = code == 0 && silent
                           ? makeError(QStringLiteral("provider_unreachable"),
                                       QStringLiteral("%1 said nothing for %2 seconds.").arg(server.recipe).arg(kChatTransferTimeoutMs / 1000),
                                       QStringLiteral("Check that it is running: omarchy-agent-local status."))
                           : refusal(server.recipe, server.base, code, answer, true);
        return change(id, status);
    }
    // Headers came, then the stream broke off (the server died, the proxy dropped it, or it
    // went silent): half an answer is not an answer.
    if (reply->error() != QNetworkReply::NoError || (streamed && !job->reader.finished)) {
        status.error = makeError(QStringLiteral("provider_unreachable"),
                                 QStringLiteral("%1 stopped in the middle of its answer.").arg(server.recipe),
                                 QStringLiteral("Run the node again; if it happens again, check the model: omarchy-agent-local status."));
        return change(id, status);
    }
    // A server that ignored `stream` answers in one JSON object.
    if (!streamed)
        job->reader.text = firstObject(answer.value("choices").toArray()).value("message").toObject().value("content").toString();

    QJsonObject used{{"model", job->model}};
    if (!server.runtime.isEmpty())
        used.insert("engine", server.runtime);
    if (job->request.params.contains("seed"))
        used.insert("seed", job->request.params.value("seed"));
    job->result = JobResult{QJsonObject{{"text", job->reader.text}}, used};
    status = JobStatus{};
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
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
    if (found->chat) {
        // A chat job ends with its request: closing the stream stops the server's generation.
        JobStatus cancelled;
        cancelled.state = QStringLiteral("cancelled");
        change(job, cancelled);
        if (found->stream)
            found->stream->abort();
        return later(std::move(reply), Result<QString>(found->status.state));
    }
    if (found->music) {
        // ace-server: POST /job?id=N&cancel=1. A job it has finished stays finished, so the poller
        // follows the answer to the end (9.6).
        QNetworkReply *posted = send(found->server, "POST", QStringLiteral("/job?id=%1&cancel=1").arg(found->serverId));
        connect(posted, &QNetworkReply::finished, this, [this, posted, job, found, reply] {
            posted->deleteLater();
            const int code = httpStatus(posted);
            if (code == 0)
                return reply(Result<QString>(makeError(QStringLiteral("provider_unreachable"),
                                                       QStringLiteral("%1 is not answering at %2.").arg(found->server.recipe, found->server.base))));
            if (code == 200 && jsonBody(posted).value("status").toString() == QLatin1String("cancelled") && !found->status.isTerminal()) {
                JobStatus cancelled;
                cancelled.state = QStringLiteral("cancelled");
                change(job, cancelled);
            }
            reply(Result<QString>(found->status.state));
        });
        return;
    }
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
        if (it.value()->status.isTerminal() || it.value()->chat)
            continue; // a chat job is followed by its stream, not polled
        active = true;
        if (!it.value()->asking)
            it.value()->music ? pollSong(it.key()) : pollJob(it.key());
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
    if (job->request.capability == kVideoGenerate)
        return finishVideo(id, result);
    JobStatus status;
    const Result<JobResult> made = imageJobResult(answer, job->request, job->model, kEngine, job->server.recipe);
    if (!made) {
        status.state = QStringLiteral("error");
        status.error = made.error();
        return change(id, status);
    }
    job->result = made.value();
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

// A finished vid_gen job's result is the whole container as base64 (api.md "Completion Result":
// output_format, mime_type, fps, frame_count, b64_json), written to video.<format> now (8.6).
void OmarchyProvider::finishVideo(const QString &id, const QJsonObject &result) {
    const auto job = m_jobs.value(id);
    const QString format = result.value("output_format").toString(QStringLiteral("webm"));
    const QByteArray bytes = QByteArray::fromBase64(result.value("b64_json").toString().toLatin1());
    JobStatus status;
    if (bytes.isEmpty()) {
        status.state = QStringLiteral("error");
        status.error = jobError(QStringLiteral("%1 finished without a video.").arg(job->server.recipe));
        return change(id, status);
    }
    const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("video.") + format);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        status.state = QStringLiteral("error");
        status.error = makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path));
        return change(id, status);
    }
    file.close();

    // The model may have changed the length (MiniMax H3: 17n+5 frames), so the file's own are the
    // truth: the node reads size, rate and length from it. The seed is the server's record, else
    // the one asked for.
    const QJsonObject &params = job->request.params;
    QJsonObject used{{"model", job->model}, {"engine", kEngine}};
    const QJsonValue seed = seedOf(result.value("generation_info").toObject().value("seed"));
    if (!seed.isUndefined())
        used.insert("seed", seed);
    else if (params.contains("seed"))
        used.insert("seed", params.value("seed"));
    if (result.value("fps").toDouble() > 0)
        used.insert("fps", result.value("fps"));
    if (result.value("frame_count").toInt() > 0)
        used.insert("frames", result.value("frame_count"));
    job->result = JobResult{QJsonObject{{"video", QJsonObject{{"path", path}, {"format", format}}}}, used};
    status.state = QStringLiteral("done");
    status.progress = 1;
    change(id, status);
}

// A song is two ace-server jobs in turn (Fantasi's makeSong; docs/ARCHITECTURE.md): /lm writes it
// out (lyrics, tempo, key, audio codes) and /synth renders that. The server has no queue state,
// progress counter or preview, so progress is the half each job is.
void OmarchyProvider::submitSong(const Server &server, const JobRequest &request, const QJsonObject &model,
                                 Reply<QString> reply) {
    // An adapter the server does not list would fail after the song was written, or be ignored: say so first.
    const QString adapter = request.params.value("adapter").toString();
    if (!adapter.isEmpty() && !server.adapters.contains(adapter)) {
        Error missing = makeError(QStringLiteral("model_missing"),
                                  QStringLiteral("%1 does not have the adapter %2.").arg(server.recipe, adapter),
                                  server.adapters.isEmpty()
                                      ? QStringLiteral("%1 lists no adapters: put the file in its adapters folder and restart it.").arg(server.recipe)
                                      : QStringLiteral("Pick one of: %1.").arg(server.adapters.join(QStringLiteral(", "))));
        missing.valid = QJsonArray::fromStringList(server.adapters);
        return reply(Result<QString>(missing));
    }
    // A file the job cannot send is refused before the song is written, not after.
    for (const auto &file : kSynthFiles) {
        const QString path = request.inputs.value(QLatin1String(file[0])).toObject().value("path").toString();
        if (request.inputs.contains(QLatin1String(file[0])) && !QFileInfo(path).isReadable())
            return reply(Result<QString>(makeError(QStringLiteral("file_not_found"),
                                                   QStringLiteral("Cannot read the %1 %2.").arg(QLatin1String(file[1]), path))));
    }
    if (request.params.value("task").toString() == QLatin1String("understand"))
        return submitUnderstand(server, request, model, std::move(reply));
    if (request.params.contains("task"))
        return submitSongEdit(server, request, model, std::move(reply));
    QNetworkReply *posted = send(server, "POST", QStringLiteral("/lm"),
                                 QJsonDocument(aceBody(request)).toJson(QJsonDocument::Compact));
    connect(posted, &QNetworkReply::finished, this, [this, posted, server, request, model, reply] {
        posted->deleteLater();
        const int code = httpStatus(posted);
        const QJsonObject answer = jsonBody(posted);
        const QString serverId = answer.value("id").toString();
        if (code != 200 && code != 202)
            return reply(Result<QString>(refusal(server.recipe, server.base, code, answer, false)));
        if (serverId.isEmpty())
            return reply(Result<QString>(jobError(QStringLiteral("%1 took the job but gave no id.").arg(server.recipe))));
        auto job = std::make_shared<Job>();
        job->music = true;
        job->server = server;
        job->serverId = serverId;
        job->request = request;
        job->model = model;
        job->status.state = QStringLiteral("running");
        job->status.stage = QStringLiteral("writing");
        const QString id = QStringLiteral("%1/song-%2").arg(server.recipe).arg(++m_chats);
        m_jobs.insert(id, job);
        if (!m_poll.isActive())
            m_poll.start();
        reply(Result<QString>(id));
    });
}

// POST /synth: JSON, or a multipart form when the job carries files. Null (and `error`) when a file cannot be read.
QNetworkReply *OmarchyProvider::sendSynth(const Server &server, const QJsonObject &body, const QJsonObject &inputs, Error *error) {
    const bool form = std::any_of(std::begin(kSynthFiles), std::end(kSynthFiles),
                                  [&](const auto &file) { return inputs.contains(QLatin1String(file[0])); });
    if (!form)
        return send(server, "POST", QStringLiteral("/synth"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    return sendForm(server, QStringLiteral("/synth"), body, inputs, error);
}

// A multipart form to a job endpoint (/synth, /understand): the request as JSON and the job's files.
QNetworkReply *OmarchyProvider::sendForm(const Server &server, const QString &endpoint, const QJsonObject &body,
                                         const QJsonObject &inputs, Error *error) {
    QByteArray data, boundary;
    if (!synthForm(body, inputs, &data, &boundary, error))
        return nullptr;
    return send(server, "POST", endpoint, data, 0, "multipart/form-data; boundary=" + boundary);
}

// Describe Song: one /understand job with the song as the multipart part `audio` (ace-server: the LM reads the
// song's FSQ codes and answers with its caption, lyrics, tempo and key). No second job: the answer is text.
void OmarchyProvider::submitUnderstand(const Server &server, const JobRequest &request, const QJsonObject &model,
                                       Reply<QString> reply) {
    Error problem;
    QNetworkReply *posted = sendForm(server, QStringLiteral("/understand"), QJsonObject(), request.inputs, &problem);
    if (!posted)
        return reply(Result<QString>(problem));
    connect(posted, &QNetworkReply::finished, this, [this, posted, server, request, model, reply] {
        posted->deleteLater();
        const int code = httpStatus(posted);
        const QJsonObject answer = jsonBody(posted);
        const QString serverId = answer.value("id").toString();
        if (code != 200 && code != 202)
            return reply(Result<QString>(refusal(server.recipe, server.base, code, answer, false)));
        if (serverId.isEmpty())
            return reply(Result<QString>(jobError(QStringLiteral("%1 took the job but gave no id.").arg(server.recipe))));
        auto job = std::make_shared<Job>();
        job->music = true;
        job->rendering = true;
        job->understanding = true;
        job->server = server;
        job->serverId = serverId;
        job->request = request;
        job->model = model;
        job->status.state = QStringLiteral("running");
        job->status.stage = QStringLiteral("listening");
        job->status.progress = 0.5;
        const QString id = QStringLiteral("%1/song-%2").arg(server.recipe).arg(++m_chats);
        m_jobs.insert(id, job);
        if (!m_poll.isActive())
            m_poll.start();
        reply(Result<QString>(id));
    });
}

// Remix Song and Repaint Song: one /synth job with the request and the source audio as a multipart
// form (the parts `request` and `audio`, ace-server's own). There is no first job, so the job starts
// at rendering and the song's lyrics are the ones that were sent.
void OmarchyProvider::submitSongEdit(const Server &server, const JobRequest &request, const QJsonObject &model,
                                     Reply<QString> reply) {
    Error problem;
    QNetworkReply *posted = sendSynth(server, aceEditBody(request), request.inputs, &problem);
    if (!posted)
        return reply(Result<QString>(problem));
    connect(posted, &QNetworkReply::finished, this, [this, posted, server, request, model, reply] {
        posted->deleteLater();
        const int code = httpStatus(posted);
        const QJsonObject answer = jsonBody(posted);
        const QString serverId = answer.value("id").toString();
        if (code != 200 && code != 202)
            return reply(Result<QString>(refusal(server.recipe, server.base, code, answer, false)));
        if (serverId.isEmpty())
            return reply(Result<QString>(jobError(QStringLiteral("%1 took the job but gave no id.").arg(server.recipe))));
        auto job = std::make_shared<Job>();
        job->music = true;
        job->rendering = true;
        job->server = server;
        job->serverId = serverId;
        job->request = request;
        job->model = model;
        job->song = QJsonObject{{"lyrics", request.params.value("lyrics").toString()}};
        if (request.params.contains("seed"))
            job->song.insert("seed", request.params.value("seed"));
        job->status.state = QStringLiteral("running");
        job->status.stage = QStringLiteral("rendering");
        job->status.progress = 0.5;
        const QString id = QStringLiteral("%1/song-%2").arg(server.recipe).arg(++m_chats);
        m_jobs.insert(id, job);
        if (!m_poll.isActive())
            m_poll.start();
        reply(Result<QString>(id));
    });
}

// ace-server's job states: running, done, failed, cancelled (GET /job?id=N).
void OmarchyProvider::pollSong(const QString &id) {
    const auto job = m_jobs.value(id);
    job->asking = true;
    QNetworkReply *reply = send(job->server, "GET", QStringLiteral("/job?id=%1").arg(job->serverId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, job] {
        reply->deleteLater();
        const auto over = [&] { job->asking = false; };
        if (job->status.isTerminal())
            return over();
        const int code = httpStatus(reply);
        if (code == 0) {
            over();
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
            over();
            JobStatus gone;
            gone.state = QStringLiteral("error");
            gone.error = makeError(QStringLiteral("node_failed"),
                                   QStringLiteral("%1 no longer has the job (HTTP %2).").arg(job->server.recipe).arg(code),
                                   QStringLiteral("Run the node again."));
            return change(id, gone);
        }
        const QString state = jsonBody(reply).value("status").toString();
        JobStatus status;
        if (state == QLatin1String("done"))
            return fetchSong(id); // keeps `asking` until the result is in hand
        over();
        if (state == QLatin1String("failed")) {
            status.state = QStringLiteral("error");
            status.error = jobError(QStringLiteral("%1 could not make the song.").arg(job->server.recipe));
        } else if (state == QLatin1String("cancelled")) {
            status.state = QStringLiteral("cancelled");
        } else {
            status.state = QStringLiteral("running");
            status.stage = job->understanding ? QStringLiteral("listening")
                           : job->rendering   ? QStringLiteral("rendering")
                                              : QStringLiteral("writing");
            status.progress = job->rendering ? 0.5 : 0.0;
        }
        change(id, status);
    });
}

void OmarchyProvider::fetchSong(const QString &id) {
    const auto job = m_jobs.value(id);
    QNetworkReply *reply = send(job->server, "GET", QStringLiteral("/job?id=%1&result=1").arg(job->serverId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, job] {
        reply->deleteLater();
        job->asking = false;
        if (job->status.isTerminal())
            return;
        const auto fail = [&](const QString &message) {
            JobStatus failed;
            failed.state = QStringLiteral("error");
            failed.error = jobError(message);
            change(id, failed);
        };
        const int code = httpStatus(reply);
        if (code != 200)
            return fail(QStringLiteral("%1 gave no result (HTTP %2).").arg(job->server.recipe).arg(code));
        const QByteArray body = reply->readAll();
        if (!job->rendering) {
            // The song written out: it goes back to be rendered as it is, in the file format asked for.
            QJsonObject song = QJsonDocument::fromJson(body).array().first().toObject();
            if (song.isEmpty())
                return fail(QStringLiteral("%1 wrote no song.").arg(job->server.recipe));
            job->song = song;
            addSynthOptions(song, job->request.params);
            Error problem;
            QNetworkReply *posted = sendSynth(job->server, song, job->request.inputs, &problem);
            if (!posted)
                return fail(problem.message);
            job->asking = true;
            connect(posted, &QNetworkReply::finished, this, [this, posted, id, job] {
                posted->deleteLater();
                job->asking = false;
                if (job->status.isTerminal())
                    return;
                const int status = httpStatus(posted);
                const QString serverId = jsonBody(posted).value("id").toString();
                if ((status != 200 && status != 202) || serverId.isEmpty()) {
                    JobStatus failed;
                    failed.state = QStringLiteral("error");
                    failed.error = refusal(job->server.recipe, job->server.base, status, jsonBody(posted), false);
                    return change(id, failed);
                }
                job->serverId = serverId;
                job->rendering = true;
                JobStatus running;
                running.state = QStringLiteral("running");
                running.stage = QStringLiteral("rendering");
                running.progress = 0.5;
                change(id, running);
            });
            return;
        }
        const QByteArray song = firstPart(body);
        if (job->understanding) {
            // The answer is a JSON array of one AceRequest (the song as the model hears it), first part of a multipart.
            const QJsonDocument parsed = QJsonDocument::fromJson(song);
            const QJsonObject heard = parsed.isArray() ? parsed.array().first().toObject() : parsed.object();
            if (heard.isEmpty())
                return fail(QStringLiteral("%1 could not tell what the song is.").arg(job->server.recipe));
            job->result = JobResult{QJsonObject{{"caption", heard.value("caption").toString()},
                                                {"lyrics", heard.value("lyrics").toString()},
                                                {"bpm", heard.value("bpm").toDouble(0)},
                                                {"key", heard.value("keyscale").toString()},
                                                {"time_signature", heard.value("timesignature").toString()},
                                                {"language", heard.value("vocal_language").toString()}},
                                    QJsonObject{{"model", job->model},
                                                {"engine", job->server.runtime.isEmpty() ? kMusicEngine : job->server.runtime}}};
            JobStatus done;
            done.state = QStringLiteral("done");
            done.progress = 1;
            return change(id, done);
        }
        if (song.isEmpty())
            return fail(QStringLiteral("%1 finished without a song.").arg(job->server.recipe));
        const QString format = songFile(job->request.params).extension;
        const QString path = QDir(job->request.outputDir).absoluteFilePath(QStringLiteral("audio.") + format);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(song) != song.size()) {
            JobStatus failed;
            failed.state = QStringLiteral("error");
            failed.error = makeError(QStringLiteral("internal"), QStringLiteral("Cannot write %1.").arg(path));
            return change(id, failed);
        }
        file.close();
        QJsonObject used{{"model", job->model},
                         {"engine", job->server.runtime.isEmpty() ? kMusicEngine : job->server.runtime}};
        const QJsonValue seed = job->song.contains("seed") ? seedOf(job->song.value("seed")) : QJsonValue(QJsonValue::Undefined);
        if (!seed.isUndefined() && seed.toDouble(-1) >= 0)
            used.insert("seed", seed);
        else if (job->request.params.contains("seed"))
            used.insert("seed", job->request.params.value("seed"));
        job->result = JobResult{QJsonObject{{"audio", QJsonObject{{"path", path}, {"format", format}}},
                                            {"lyrics", job->song.value("lyrics").toString()}},
                                used};
        JobStatus done;
        done.state = QStringLiteral("done");
        done.progress = 1;
        change(id, done);
    });
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
