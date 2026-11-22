// Generate Video and Generate Song (spec 14.2): the coarse media nodes, served by a provider's
// video.generate and audio.generate (8.1). Like Generate Image they only translate (node_kernel.h):
// inputs to a JobRequest, and the JobResult to outputs with the values actually used (3.3). The
// clip and the song come back as file-backed media handles (2.4), so what reads them later
// (Load-style nodes, Save, the video stages of #105) streams the file and never holds it whole.

#include "nodes/image_cover.h"
#include "nodes/image_size.h"
#include "nodes/kernels.h"

#include <QDir>
#include <QJsonArray>
#include <QSize>

#include <algorithm>
#include <cmath>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/vips.h"

namespace fugue::nodes {

namespace {

const QString kVideo = QStringLiteral("video.generate");
const QString kAudio = QStringLiteral("audio.generate");

// Fantasi's choices (generator.cpp fitVideo, videoFps at 6aa8dc2): a clip is about 0.4 megapixel
// (864 x 480) in steps of 32, at 24 frames a second, unless the model or the node says otherwise.
constexpr double kVideoMegapixels = 0.4;
constexpr int kVideoMultiple = 32;
constexpr QSize kVideoSize{864, 480};
constexpr int kVideoFps = 24;

QString modelName(const Selection &selection) { return selection.model.handle.value("name").toString(); }

// What a node reports of a value the provider may not have said: the provider's own, else the
// fallback.
Value usedNumber(const QJsonObject &used, const QString &key, const Value &otherwise) {
    const QJsonValue value = used.value(key);
    return value.isDouble() ? Value(value) : otherwise;
}

Value usedModel(const QJsonObject &used, const Selection &selection) {
    const QJsonObject model = used.value(QStringLiteral("model")).toObject();
    return Value(model.isEmpty() ? selection.model.handle : model);
}

// The path a provider gave for output `name`, probed as media of `type`.
Result<MediaHandle> outputMedia(MediaType type, const NodeContext &context, const Selection &selection,
                                const JobResult &result) {
    const QString name = mediaTypeName(type);
    const QString path = result.outputs.value(name).toObject().value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return makeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1 finished without %2.").arg(modelName(selection), type == MediaType::Video ? QStringLiteral("a video") : QStringLiteral("audio")));
    return MediaHandle::probe(type, path, *context.hashes);
}

class VideoKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kVideo;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kVideo;
        request.model = selection.model.handle;
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"), context.input(QStringLiteral("prompt")).text());
        const QString negative = context.input(QStringLiteral("negative_prompt")).text();
        if (!negative.isEmpty())
            params.insert(QStringLiteral("negative_prompt"), negative);
        const Value seed = context.input(QStringLiteral("seed"));
        if (!seed.isEmpty())
            params.insert(QStringLiteral("seed"), seed.json().toInteger());

        const QJsonObject &defaults = selection.model.defaults;
        const bool framed = context.inputs.contains(QStringLiteral("image"));
        MediaHandle frame;
        if (framed) {
            frame = context.input(QStringLiteral("image")).media();
            if (frame.width() <= 0 || frame.height() <= 0)
                return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("image"),
                                      QStringLiteral("The first frame has no size."));
        }

        // The size: what the node sets; else, for a first frame, its shape at about 0.4 megapixel
        // (a side set alone keeps the shape); else the model's default, else 864 x 480.
        const auto set = [&](const QString &side) -> int {
            return context.inputs.contains(side) ? int(context.input(side).number()) : 0;
        };
        const auto fallback = [&](const QString &side) {
            const int own = defaults.value(side).toInt(0);
            return own > 0 ? own : (side == QLatin1String("width") ? kVideoSize.width() : kVideoSize.height());
        };
        QSize size(set(QStringLiteral("width")), set(QStringLiteral("height")));
        if (framed) {
            const double aspect = double(frame.width()) / frame.height();
            const auto along = [](double length, double ratio) {
                return std::max(1, int(std::lround(length * ratio / kVideoMultiple))) * kVideoMultiple;
            };
            if (size.width() > 0 && size.height() <= 0)
                size.setHeight(along(size.width(), 1 / aspect));
            else if (size.height() > 0 && size.width() <= 0)
                size.setWidth(along(size.height(), aspect));
            else if (size.width() <= 0)
                size = sizeForMegapixels(aspect, kVideoMegapixels, kVideoMultiple);
        } else {
            if (size.width() <= 0)
                size.setWidth(fallback(QStringLiteral("width")));
            if (size.height() <= 0)
                size.setHeight(fallback(QStringLiteral("height")));
        }
        params.insert(QStringLiteral("width"), size.width());
        params.insert(QStringLiteral("height"), size.height());
        const QString blamed = context.inputs.contains(QStringLiteral("width"))    ? QStringLiteral("width")
                               : context.inputs.contains(QStringLiteral("height")) ? QStringLiteral("height")
                               : framed                                            ? QStringLiteral("image")
                                                                                   : QString();
        if (const Errors outside = outsideSizeLimits(modelName(selection), selection.model.limits, params,
                                                     [&](const QString &side) {
                                                         return context.inputs.contains(side) ? side : blamed;
                                                     });
            hasErrors(outside))
            return outside;

        // The length: seconds at the rate. The model rounds the count to one it can make.
        const Value rate = context.input(QStringLiteral("fps"));
        const int fps = !rate.isEmpty() ? int(rate.number()) : defaults.value(QStringLiteral("fps")).toInt(kVideoFps);
        params.insert(QStringLiteral("fps"), fps);
        params.insert(QStringLiteral("video_frames"),
                      std::max(1, int(std::lround(context.input(QStringLiteral("duration")).number() * fps))));

        for (const QString &key : {QStringLiteral("steps"), QStringLiteral("guidance")}) {
            const Value value = context.input(key);
            if (!value.isEmpty())
                params.insert(key, value.json());
            else if (defaults.contains(key))
                params.insert(key, defaults.value(key));
        }

        if (framed) {
            if (!initVips())
                return vipsUnavailable();
            Result<QString> path = QString();
            try {
                path = coverToFile(frame, size, QDir(context.workDir).filePath(QStringLiteral("first-frame.png")));
            } catch (const vips::VError &error) {
                Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
                unreadable.message = QStringLiteral("Cannot prepare the first frame: %1").arg(unreadable.message);
                return unreadable;
            }
            if (!path)
                return path.errors();
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", path.value()}});
        }
        return request;
    }

    // The values used are the provider's report (8.1); what it leaves out is read from the file
    // (size, rate, length) or is what was asked for (the seed, the model).
    Result<Values> outputs(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        const Result<MediaHandle> video = outputMedia(MediaType::Video, context, selection, result);
        if (!video)
            return video.errors();
        const QJsonObject &used = result.used;
        return Values{{QStringLiteral("video"), Value(video.value())},
                      {QStringLiteral("seed"), usedNumber(used, QStringLiteral("seed"), context.input(QStringLiteral("seed")))},
                      {QStringLiteral("width"), Value(video->width())},
                      {QStringLiteral("height"), Value(video->height())},
                      {QStringLiteral("fps"), Value(video->fps())},
                      {QStringLiteral("duration"), Value(video->duration())},
                      {QStringLiteral("model"), usedModel(used, selection)}};
    }
};

// What every song node can ask of the renderer, one place for the three (Generate, Remix, Repaint Song).
// Each goes only when it is set, so a song that uses none of them is the request it always was (and keeps its
// cache entry). `quality` is the file it is saved as: mp3, mp3-320 or lossless; `reference` is a recording whose
// voice and timbre the song takes (ace-server's ref_audio); `adapter` is a singer or style adapter (LoRA) of the music
// server, with its `adapter_scale`.
Result<bool> addSongOptions(const NodeContext &context, JobRequest &request) {
    const QString quality = context.input(QStringLiteral("quality")).text().trimmed();
    if (!quality.isEmpty() && quality != QLatin1String("mp3"))
        request.params.insert(QStringLiteral("quality"), quality);
    const Value reference = context.input(QStringLiteral("reference"));
    if (!reference.isEmpty()) {
        const MediaHandle audio = reference.media();
        if (audio.path().isEmpty() || audio.duration() <= 0)
            return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("reference"),
                                  QStringLiteral("The style reference cannot be read."));
        request.inputs.insert(QStringLiteral("ref_audio"), QJsonObject{{"path", audio.path()}});
    }
    const QString adapter = context.input(QStringLiteral("adapter")).json().toObject().value(QStringLiteral("name")).toString();
    if (!adapter.isEmpty()) {
        request.params.insert(QStringLiteral("adapter"), adapter);
        request.params.insert(QStringLiteral("adapter_scale"), context.input(QStringLiteral("adapter_scale")).number());
    }
    return true;
}

class SongKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kAudio;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kAudio;
        request.model = selection.model.handle;
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"), context.input(QStringLiteral("prompt")).text().trimmed());
        // ACE-Step's own convention (Fantasi, ace-server docs): "[Instrumental]" sings nothing, an
        // empty text leaves the words to the model.
        const bool instrumental = context.input(QStringLiteral("instrumental")).json().toBool();
        params.insert(QStringLiteral("lyrics"),
                      instrumental ? QStringLiteral("[Instrumental]") : context.input(QStringLiteral("lyrics")).text().trimmed());
        const int seconds = int(context.input(QStringLiteral("duration")).number());
        const int least = selection.model.limits.value(QStringLiteral("min_duration")).toInt(0);
        const int most = selection.model.limits.value(QStringLiteral("max_duration")).toInt(0);
        if ((least > 0 && seconds < least) || (most > 0 && seconds > most))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("duration"),
                                  QStringLiteral("%1 makes songs of %2 to %3 seconds, not %4.")
                                      .arg(modelName(selection))
                                      .arg(least)
                                      .arg(most > 0 ? QString::number(most) : QStringLiteral("any"))
                                      .arg(seconds));
        params.insert(QStringLiteral("duration"), seconds);
        // The model takes a language by its plain code: pt, not pt-BR.
        const QString language = context.input(QStringLiteral("language")).text().trimmed().section(QLatin1Char('-'), 0, 0);
        if (!language.isEmpty())
            params.insert(QStringLiteral("language"), language);
        const Value seed = context.input(QStringLiteral("seed"));
        if (!seed.isEmpty())
            params.insert(QStringLiteral("seed"), seed.json().toInteger());
        // What the model works out itself when it is left out (ace-server, "Metadata (LLM-filled if unset)"):
        // 0 tempo, an empty key or time signature are "unset", so they are not sent.
        const int tempo = int(context.input(QStringLiteral("bpm")).number());
        if (tempo > 0)
            params.insert(QStringLiteral("bpm"), tempo);
        for (const QString &name : {QStringLiteral("key"), QStringLiteral("time_signature")}) {
            const QString text = context.input(name).text().trimmed();
            if (!text.isEmpty())
                params.insert(name, text);
        }
        const Value creativity = context.input(QStringLiteral("creativity"));
        if (!creativity.isEmpty())
            params.insert(QStringLiteral("creativity"), creativity.number());
        if (context.input(QStringLiteral("keep_description")).json().toBool())
            params.insert(QStringLiteral("keep_description"), true);
        if (const Result<bool> options = addSongOptions(context, request); !options)
            return options.errors();
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        const Result<MediaHandle> audio = outputMedia(MediaType::Audio, context, selection, result);
        if (!audio)
            return audio.errors();
        const QJsonObject &used = result.used;
        return Values{{QStringLiteral("audio"), Value(audio.value())},
                      {QStringLiteral("lyrics"), Value(result.outputs.value(QStringLiteral("lyrics")).toString())},
                      {QStringLiteral("seed"), usedNumber(used, QStringLiteral("seed"), context.input(QStringLiteral("seed")))},
                      {QStringLiteral("duration"), Value(audio->duration())},
                      {QStringLiteral("model"), usedModel(used, selection)}};
    }
};

// Remix Song and Repaint Song: a song in, a song out, on the same music models (the provider sends the
// source with the job and skips the model that writes songs, so the description and the lyrics reach the
// renderer as they are). Remix is ace-step's cover (re-played in another style; "stay close" keeps its
// tune and timing), Repaint redraws one stretch of the song and keeps the rest.
class SongEditKernel : public ProviderKernel {
public:
    enum class Mode { Remix, Repaint };
    explicit SongEditKernel(Mode mode) : m_mode(mode) {}

    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kAudio;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kAudio;
        request.model = selection.model.handle;
        const MediaHandle source = context.input(QStringLiteral("audio")).media();
        if (source.path().isEmpty() || source.duration() <= 0)
            return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("audio"),
                                  QStringLiteral("The source song cannot be read."));
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"), context.input(QStringLiteral("prompt")).text().trimmed());
        const bool instrumental = context.input(QStringLiteral("instrumental")).json().toBool();
        params.insert(QStringLiteral("lyrics"),
                      instrumental ? QStringLiteral("[Instrumental]") : context.input(QStringLiteral("lyrics")).text().trimmed());
        const Value seed = context.input(QStringLiteral("seed"));
        if (!seed.isEmpty())
            params.insert(QStringLiteral("seed"), seed.json().toInteger());
        if (m_mode == Mode::Repaint) {
            const double start = context.input(QStringLiteral("start")).number();
            // 0 for the end is the end of the song.
            const double end = context.input(QStringLiteral("end")).number();
            const double stop = end > 0 ? end : source.duration();
            if (stop <= start)
                return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("end"),
                                      QStringLiteral("The part to redraw ends at %1 seconds, before it starts at %2.")
                                          .arg(stop).arg(start));
            params.insert(QStringLiteral("task"), QStringLiteral("repaint"));
            params.insert(QStringLiteral("start"), start);
            params.insert(QStringLiteral("end"), stop);
        } else {
            params.insert(QStringLiteral("task"), context.input(QStringLiteral("stay_close")).json().toBool()
                                                      ? QStringLiteral("cover-nofsq") : QStringLiteral("cover"));
            params.insert(QStringLiteral("strength"), context.input(QStringLiteral("strength")).number());
        }
        request.inputs.insert(QStringLiteral("audio"), QJsonObject{{"path", source.path()}});
        if (const Result<bool> options = addSongOptions(context, request); !options)
            return options.errors();
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        const Result<MediaHandle> audio = outputMedia(MediaType::Audio, context, selection, result);
        if (!audio)
            return audio.errors();
        return Values{{QStringLiteral("audio"), Value(audio.value())},
                      {QStringLiteral("seed"), usedNumber(result.used, QStringLiteral("seed"), context.input(QStringLiteral("seed")))},
                      {QStringLiteral("duration"), Value(audio->duration())},
                      {QStringLiteral("model"), usedModel(result.used, selection)}};
    }

private:
    Mode m_mode;
};

// Describe Song: what the model hears in a song (ace-server's /understand): the description, the words, the tempo,
// the key. Served by the music models, like the song nodes; the provider sends the song with one job and the answer is
// text, so it feeds Remix Song (Song from a song), Enhance Prompt or Preview.
class DescribeSongKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kAudio;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kAudio;
        request.model = selection.model.handle;
        const MediaHandle source = context.input(QStringLiteral("audio")).media();
        if (source.path().isEmpty() || source.duration() <= 0)
            return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("audio"),
                                  QStringLiteral("The song cannot be read."));
        request.params.insert(QStringLiteral("task"), QStringLiteral("understand"));
        request.inputs.insert(QStringLiteral("audio"), QJsonObject{{"path", source.path()}});
        return request;
    }

    Result<Values> outputs(const NodeContext &, const Selection &selection, const JobResult &result) override {
        const QJsonObject &heard = result.outputs;
        return Values{{QStringLiteral("caption"), Value(heard.value(QStringLiteral("caption")).toString())},
                      {QStringLiteral("lyrics"), Value(heard.value(QStringLiteral("lyrics")).toString())},
                      {QStringLiteral("bpm"), Value(heard.value(QStringLiteral("bpm")).toDouble())},
                      {QStringLiteral("key"), Value(heard.value(QStringLiteral("key")).toString())},
                      {QStringLiteral("time_signature"), Value(heard.value(QStringLiteral("time_signature")).toString())},
                      {QStringLiteral("language"), Value(heard.value(QStringLiteral("language")).toString())},
                      {QStringLiteral("model"), usedModel(result.used, selection)}};
    }
};

} // namespace

void addMediaGenerationKernels(Kernels &kernels) {
    kernels.add(QStringLiteral("fugue/generate-video"), std::make_shared<VideoKernel>());
    kernels.add(QStringLiteral("fugue/generate-song"), std::make_shared<SongKernel>());
    kernels.add(QStringLiteral("fugue/remix-song"), std::make_shared<SongEditKernel>(SongEditKernel::Mode::Remix));
    kernels.add(QStringLiteral("fugue/repaint-song"), std::make_shared<SongEditKernel>(SongEditKernel::Mode::Repaint));
    kernels.add(QStringLiteral("fugue/describe-song"), std::make_shared<DescribeSongKernel>());
}

} // namespace fugue::nodes
